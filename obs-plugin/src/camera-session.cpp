#include "camera-session.hpp"

#include <rtc/rtc.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

void CameraSession::create_pairing()
{
    disconnect_session();
    emit statusChanged(QStringLiteral("Tworzenie prywatnej sesji kamery…"));
    QNetworkRequest request(api_url(QStringLiteral("/camera/sessions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto *reply = network_->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto object = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(object.value(QStringLiteral("message")).toString(reply->errorString()));
            reply->deleteLater();
            return;
        }
        socket_token_ = object.value(QStringLiteral("socket_token")).toString();
        pairing_url_ = object.value(QStringLiteral("pairing_url")).toString();
        ice_servers_.clear();
        for (const auto server_value : object.value(QStringLiteral("ice_servers")).toArray()) {
            const auto server = server_value.toObject();
            const auto username = server.value(QStringLiteral("username")).toString().toStdString();
            const auto credential = server.value(QStringLiteral("credential")).toString().toStdString();
            for (const auto url : server.value(QStringLiteral("urls")).toArray())
                ice_servers_.push_back({url.toString().toStdString(), username, credential});
        }
        if (socket_token_.isEmpty() || pairing_url_.isEmpty() || ice_servers_.empty()) {
            emit errorOccurred(QStringLiteral("Serwer nie zwrócił kompletnej konfiguracji TURN."));
            reply->deleteLater();
            return;
        }
        transport_->configure(ice_servers_);
        transport_->start(false, true);
        emit pairingReady(pairing_url_, object.value(QStringLiteral("pair_expires_in")).toInt(120));
        emit statusChanged(QStringLiteral("Zeskanuj kod QR i uruchom kamerę w Safari."));
        open_signaling();
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
            emit statusChanged(QStringLiteral("iPhone połączony — uruchamianie WebRTC…"));
        return;
    }
    if (type == QStringLiteral("error")) {
        emit errorOccurred(object.value(QStringLiteral("code")).toString());
        return;
    }
    const auto payload = object.value(QStringLiteral("payload")).toObject();
    try {
        if (type == QStringLiteral("offer")) {
            transport_->set_remote_description(payload.value(QStringLiteral("type")).toString().toStdString(),
                                               payload.value(QStringLiteral("sdp")).toString().toStdString());
        } else if (type == QStringLiteral("ice-candidate")) {
            transport_->add_remote_candidate(payload.value(QStringLiteral("candidate")).toString().toStdString(),
                                             payload.value(QStringLiteral("mid")).toString().toStdString());
        }
    } catch (const std::exception &error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
    }
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
