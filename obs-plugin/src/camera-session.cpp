#include "camera-session.hpp"

#include <rtc/rtc.hpp>

#include <QJsonArray>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>

namespace tazzio {

CameraSession::CameraSession(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), transport_(std::make_unique<PeerTransport>())
{
    transport_->on_state([this](const std::string &state) {
        QMetaObject::invokeMethod(this, [this, state] { emit statusChanged(QString::fromStdString(state)); });
    });
    transport_->on_local_description([this](const std::string &type, const std::string &sdp) {
        if (type != "answer")
            return;
        QMetaObject::invokeMethod(this, [this, type, sdp] {
            send_signal(QStringLiteral("answer"),
                        {{QStringLiteral("type"), QString::fromStdString(type)},
                         {QStringLiteral("sdp"), QString::fromStdString(sdp)}});
        });
    });
    transport_->on_local_candidate([this](const std::string &candidate, const std::string &mid) {
        QMetaObject::invokeMethod(this, [this, candidate, mid] {
            send_signal(QStringLiteral("ice-candidate"),
                        {{QStringLiteral("candidate"), QString::fromStdString(candidate)},
                         {QStringLiteral("mid"), QString::fromStdString(mid)}});
        });
    });
}

CameraSession::~CameraSession()
{
    disconnect_session();
}

void CameraSession::set_server(QUrl server)
{
    if (!server.scheme().startsWith(QStringLiteral("http")))
        server.setScheme(QStringLiteral("https"));
    server.setPath(QString{});
    server.setQuery(QString{});
    server.setFragment(QString{});
    server_ = std::move(server);
}

QUrl CameraSession::api_url(const QString &path) const
{
    auto url = server_;
    url.setPath(QStringLiteral("/api") + path);
    return url;
}

void CameraSession::check_for_updates()
{
    auto url = api_url(QStringLiteral("/camera/update"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("current"), QStringLiteral(TAZZIO_PLUGIN_VERSION));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("Cache-Control", "no-cache");
    auto *reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto object = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit updateCheckFailed(reply->errorString());
            reply->deleteLater();
            return;
        }
        const auto latest = object.value(QStringLiteral("latest_version")).toString();
        const auto available = object.value(QStringLiteral("update_available")).toBool();
        const QUrl download_url(object.value(QStringLiteral("download_url")).toString());
        const auto sha256 = object.value(QStringLiteral("sha256")).toString().toLower();
        const auto size = object.value(QStringLiteral("size")).toVariant().toLongLong();
        const auto trusted_download = download_url.scheme() == QStringLiteral("https") &&
                                      (download_url.host() == QStringLiteral("tazzio.pl") ||
                                       download_url.host().endsWith(QStringLiteral(".tazzio.pl")));
        const auto valid_version = QRegularExpression(QStringLiteral(R"(^\d+(?:\.\d+){1,3}$)"))
                                       .match(latest).hasMatch();
        const auto valid_hash = QRegularExpression(QStringLiteral(R"(^[0-9a-f]{64}$)"))
                                    .match(sha256).hasMatch();
        if (!valid_version || download_url.isEmpty() || !download_url.isValid() || !trusted_download ||
            !valid_hash || size <= 0 || size > 95LL * 1024LL * 1024LL)
            emit updateCheckFailed(QStringLiteral("Serwer aktualizacji zwrócił niepełne dane."));
        else
            emit updateCheckFinished(latest, available, download_url, sha256, size);
        reply->deleteLater();
    });
}

void CameraSession::download_update(const QString &version, const QUrl &download_url,
                                    const QString &sha256, qint64 expected_size)
{
#ifndef _WIN32
    emit updateInstallFailed(QStringLiteral("Automatyczna aktualizacja jest dostępna tylko w Windows."));
    return;
#else
    const auto trusted_download = download_url.scheme() == QStringLiteral("https") &&
                                  (download_url.host() == QStringLiteral("tazzio.pl") ||
                                   download_url.host().endsWith(QStringLiteral(".tazzio.pl")));
    const auto valid_version = QRegularExpression(QStringLiteral(R"(^\d+(?:\.\d+){1,3}$)"))
                                   .match(version).hasMatch();
    const auto valid_hash = QRegularExpression(QStringLiteral(R"(^[0-9a-fA-F]{64}$)"))
                                .match(sha256).hasMatch();
    if (!trusted_download || !valid_version || !valid_hash || expected_size <= 0 ||
        expected_size > 95LL * 1024LL * 1024LL) {
        emit updateInstallFailed(QStringLiteral("Dane instalatora są nieprawidłowe."));
        return;
    }

    QNetworkRequest request(download_url);
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = network_->get(request);
    auto bytes = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) { emit updateDownloadProgress(received, total); });
    connect(reply, &QIODevice::readyRead, this, [reply, bytes, oversized, expected_size] {
        const auto chunk = reply->readAll();
        if (*oversized)
            return;
        if (bytes->size() + chunk.size() > expected_size) {
            *oversized = true;
            bytes->clear();
            reply->abort();
            return;
        }
        bytes->append(chunk);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, oversized, version, sha256, expected_size] {
        if (!*oversized)
            bytes->append(reply->readAll());
        const auto final_url = reply->url();
        const auto trusted_final = final_url.scheme() == QStringLiteral("https") &&
                                   (final_url.host() == QStringLiteral("tazzio.pl") ||
                                    final_url.host().endsWith(QStringLiteral(".tazzio.pl")));
        if (*oversized || reply->error() != QNetworkReply::NoError || !trusted_final ||
            bytes->size() != expected_size) {
            emit updateInstallFailed(*oversized
                                         ? QStringLiteral("Instalator przekroczył oczekiwany rozmiar.")
                                         : QStringLiteral("Nie udało się pobrać instalatora: %1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }
        const auto actual_hash = QString::fromLatin1(
            QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex());
        if (actual_hash.compare(sha256, Qt::CaseInsensitive) != 0) {
            emit updateInstallFailed(QStringLiteral("Suma kontrolna instalatora jest nieprawidłowa."));
            reply->deleteLater();
            return;
        }

        const auto directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                               QStringLiteral("/TazzioIPhoneCameraUpdater");
        if (!QDir().mkpath(directory)) {
            emit updateInstallFailed(QStringLiteral("Nie można przygotować katalogu aktualizacji."));
            reply->deleteLater();
            return;
        }
        const auto path = directory + QStringLiteral("/Tazzio-iPhone-Camera-Setup-") + version +
                          QStringLiteral(".exe");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(*bytes) != bytes->size() || !file.commit()) {
            emit updateInstallFailed(QStringLiteral("Nie można zapisać instalatora aktualizacji."));
            reply->deleteLater();
            return;
        }
        const QStringList arguments{QStringLiteral("/SILENT"), QStringLiteral("/CLOSEAPPLICATIONS"),
                                    QStringLiteral("/RESTARTAPPLICATIONS")};
        if (!QProcess::startDetached(path, arguments)) {
            emit updateInstallFailed(QStringLiteral("Nie można uruchomić instalatora aktualizacji."));
            reply->deleteLater();
            return;
        }
        emit updateInstallerStarted(version);
        reply->deleteLater();
    });
#endif
}

void CameraSession::create_pairing(const QString &mode)
{
    disconnect_session();
    emit statusChanged(QStringLiteral("Tworzenie prywatnej sesji kamery…"));
    QNetworkRequest request(api_url(QStringLiteral("/camera/sessions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const auto requested_mode = mode == QStringLiteral("internet") ? QStringLiteral("internet") : QStringLiteral("lan");
    const auto body = QJsonDocument(QJsonObject{{QStringLiteral("mode"), requested_mode},
                                                 {QStringLiteral("client_version"), QStringLiteral(TAZZIO_PLUGIN_VERSION)}})
                          .toJson(QJsonDocument::Compact);
    auto *reply = network_->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto object = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(object.value(QStringLiteral("message")).toString(reply->errorString()));
            reply->deleteLater();
            return;
        }
        socket_token_ = object.value(QStringLiteral("control_token")).toString();
        pairing_url_ = object.value(QStringLiteral("pairing_url")).toString();
        const auto receiver_url = object.value(QStringLiteral("receiver_url")).toString();
        const auto response_mode = object.value(QStringLiteral("mode")).toString();
        ice_servers_.clear();
        for (const auto server_value : object.value(QStringLiteral("ice_servers")).toArray()) {
            const auto server = server_value.toObject();
            const auto username = server.value(QStringLiteral("username")).toString().toStdString();
            const auto credential = server.value(QStringLiteral("credential")).toString().toStdString();
            for (const auto url : server.value(QStringLiteral("urls")).toArray())
                ice_servers_.push_back({url.toString().toStdString(), username, credential});
        }
        if (socket_token_.isEmpty() || pairing_url_.isEmpty() || receiver_url.isEmpty() || response_mode.isEmpty()) {
            emit errorOccurred(QStringLiteral("Serwer nie zwrócił kompletnej konfiguracji kamery."));
            reply->deleteLater();
            return;
        }
        emit pairingReady(pairing_url_, receiver_url, response_mode,
                          object.value(QStringLiteral("pair_expires_in")).toInt(120));
        open_signaling();
        emit statusChanged(QStringLiteral("Zeskanuj kod QR i uruchom kamerę w Safari."));
        reply->deleteLater();
    });
}

void CameraSession::open_signaling()
{
    auto url = server_;
    url.setScheme(server_.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/camera-ws"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("token"), socket_token_);
    url.setQuery(query);

    websocket_ = std::make_shared<rtc::WebSocket>();
    websocket_->onOpen([this] {
        QMetaObject::invokeMethod(this, [this] { emit statusChanged(QStringLiteral("Kod QR aktywny — oczekiwanie na iPhone'a.")); });
    });
    websocket_->onMessage([this](rtc::message_variant message) {
        if (std::holds_alternative<std::string>(message)) {
            const auto text = std::get<std::string>(std::move(message));
            QMetaObject::invokeMethod(this, [this, text] { handle_message(text); });
        }
    });
    websocket_->onError([this](std::string error) {
        QMetaObject::invokeMethod(this, [this, error] { emit errorOccurred(QString::fromStdString(error)); });
    });
    websocket_->onClosed([this] {
        QMetaObject::invokeMethod(this, [this] {
            emit peerReady(false);
            emit statusChanged(QStringLiteral("Sesja sygnalizacji została zamknięta."));
        });
    });
    websocket_->open(url.toString(QUrl::FullyEncoded).toStdString());
}

void CameraSession::handle_message(const std::string &message)
{
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(message)).object();
    const auto type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("peer")) {
        const auto ready = object.value(QStringLiteral("ready")).toBool();
        emit peerReady(ready);
        if (ready)
            emit statusChanged(QStringLiteral("iPhone połączony — sterowanie kamerą aktywne."));
        return;
    }
    if (type == QStringLiteral("error")) {
        emit errorOccurred(object.value(QStringLiteral("code")).toString());
        return;
    }
    const auto payload = object.value(QStringLiteral("payload")).toObject();
    if (type == QStringLiteral("camera-capabilities"))
        emit cameraCapabilitiesChanged(payload);
    else if (type == QStringLiteral("camera-state"))
        emit cameraStateChanged(payload);
    else if (type == QStringLiteral("camera-command-result"))
        emit cameraCommandFinished(payload.value(QStringLiteral("id")).toString(),
                                   payload.value(QStringLiteral("ok")).toBool(),
                                   payload.value(QStringLiteral("message")).toString());
}

void CameraSession::send_camera_command(const QString &control, const QJsonValue &value)
{
    const auto id = QString::number(++command_sequence_);
    send_signal(QStringLiteral("camera-command"),
                {{QStringLiteral("id"), id},
                 {QStringLiteral("control"), control},
                 {QStringLiteral("value"), value}});
}

void CameraSession::send_signal(const QString &type, const QJsonObject &payload)
{
    if (!websocket_ || !websocket_->isOpen())
        return;
    const QJsonObject message{{QStringLiteral("type"), type},
                              {QStringLiteral("payload"), payload}};
    websocket_->send(QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString());
}

void CameraSession::disconnect_session()
{
    for (auto *reply : network_->findChildren<QNetworkReply *>())
        reply->abort();
    if (websocket_) {
        websocket_->resetCallbacks();
        websocket_->close();
        websocket_.reset();
    }
    if (transport_)
        transport_->close();
    socket_token_.clear();
    pairing_url_.clear();
    ice_servers_.clear();
}

PeerTransport *CameraSession::transport()
{
    return transport_.get();
}

} // namespace tazzio
