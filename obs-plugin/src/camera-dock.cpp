#include "camera-dock.hpp"
#include "camera-session.hpp"
#include "media-decoder.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "qrcodegen.hpp"

#include <algorithm>

namespace {

QPixmap qr_pixmap(const QString &value, int target_size)
{
    const auto bytes = value.toUtf8();
    const auto qr = qrcodegen::QrCode::encodeText(bytes.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int border = 4;
    const auto modules = qr.getSize() + border * 2;
    const auto scale = std::max(1, target_size / modules);
    QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (!qr.getModule(x, y))
                continue;
            for (int py = 0; py < scale; ++py)
                for (int px = 0; px < scale; ++px)
                    image.setPixelColor((x + border) * scale + px, (y + border) * scale + py, Qt::black);
        }
    }
    return QPixmap::fromImage(image);
}

bool ensure_camera_source()
{
    constexpr const char *source_name = "Tazzio iPhone Camera";
    if (auto *existing = obs_get_source_by_name(source_name)) {
        obs_source_release(existing);
        return true;
    }
    auto *scene_source = obs_frontend_get_current_scene();
    if (!scene_source)
        return false;
    auto *scene = obs_scene_from_source(scene_source);
    auto *source = obs_source_create("tazzio_iphone_camera", source_name, nullptr, nullptr);
    const auto added = scene && source && obs_scene_add(scene, source) != nullptr;
    if (source)
        obs_source_release(source);
    obs_source_release(scene_source);
    return added;
}

QString route_name(const tazzio::TransportStats &stats)
{
    const auto candidates = QString::fromStdString(stats.local_candidate + " " + stats.remote_candidate);
    if (candidates.contains(QStringLiteral(" typ relay")))
        return QStringLiteral("TURN Relay");
    if (!candidates.trimmed().isEmpty())
        return QStringLiteral("Direct P2P");
    return QStringLiteral("ustalanie trasy");
}

} // namespace

void register_iphone_camera_dock()
{
    auto *dock = new QWidget;
    auto *layout = new QVBoxLayout(dock);
    auto *session = new tazzio::CameraSession(dock);
    session->transport()->on_video(tazzio::route_camera_video);
    session->transport()->on_audio(tazzio::route_camera_audio);

    auto *title = new QLabel(QStringLiteral("TAZZIO · IPHONE CAMERA"));
    auto *description = new QLabel(QStringLiteral(
        "Połącz iPhone'a z OBS przez internet albo sieć komórkową. Kod QR jest jednorazowy i wygasa po 2 minutach."));
    description->setWordWrap(true);
    auto *status = new QLabel(QStringLiteral("Utwórz kod QR, aby rozpocząć."));
    status->setWordWrap(true);
    auto *stats = new QLabel(QStringLiteral("Media: nieaktywne"));
    stats->setWordWrap(true);

    auto *qr = new QLabel;
    qr->setAlignment(Qt::AlignCenter);
    qr->setMinimumHeight(290);
    qr->setText(QStringLiteral("Kod QR pojawi się tutaj"));
    auto *url = new QLineEdit;
    url->setReadOnly(true);
    url->setPlaceholderText(QStringLiteral("Jednorazowy adres Safari"));
    auto *copy = new QPushButton(QStringLiteral("Kopiuj adres"));
    copy->setEnabled(false);
    auto *create = new QPushButton(QStringLiteral("Utwórz nowy kod QR"));
    auto *disconnect = new QPushButton(QStringLiteral("Rozłącz kamerę"));
    disconnect->setEnabled(false);

    auto *url_row = new QHBoxLayout;
    url_row->addWidget(url, 1);
    url_row->addWidget(copy);
    layout->addWidget(title);
    layout->addWidget(description);
    layout->addWidget(status);
    layout->addWidget(stats);
    layout->addWidget(qr);
    layout->addLayout(url_row);
    layout->addWidget(create);
    layout->addWidget(disconnect);
    layout->addStretch();

    QObject::connect(create, &QPushButton::clicked, dock, [=] {
        if (!ensure_camera_source()) {
            status->setText(QStringLiteral("Nie udało się dodać źródła do aktualnej sceny OBS."));
            return;
        }
        create->setEnabled(false);
        disconnect->setEnabled(true);
        copy->setEnabled(false);
        qr->setText(QStringLiteral("Generowanie kodu…"));
        qr->setPixmap({});
        url->clear();
        session->create_pairing();
    });
    QObject::connect(copy, &QPushButton::clicked, dock, [=] {
        QGuiApplication::clipboard()->setText(url->text());
        status->setText(QStringLiteral("Adres skopiowany do schowka."));
    });
    QObject::connect(disconnect, &QPushButton::clicked, dock, [=] {
        session->disconnect_session();
        create->setEnabled(true);
        disconnect->setEnabled(false);
        copy->setEnabled(false);
        url->clear();
        qr->setPixmap({});
        qr->setText(QStringLiteral("Kod QR pojawi się tutaj"));
        status->setText(QStringLiteral("Kamera rozłączona."));
        stats->setText(QStringLiteral("Media: nieaktywne"));
    });
    QObject::connect(session, &tazzio::CameraSession::pairingReady, dock,
                     [=](const QString &pairing_url, int) {
                         url->setText(pairing_url);
                         qr->setPixmap(qr_pixmap(pairing_url, 300));
                         copy->setEnabled(true);
                         create->setEnabled(true);
                     });
    QObject::connect(session, &tazzio::CameraSession::statusChanged, status, &QLabel::setText);
    QObject::connect(session, &tazzio::CameraSession::peerReady, dock, [=](bool ready) {
        if (!ready)
            stats->setText(QStringLiteral("Media: oczekiwanie na iPhone'a"));
    });
    QObject::connect(session, &tazzio::CameraSession::errorOccurred, dock, [=](const QString &message) {
        status->setText(QStringLiteral("Błąd: ") + message);
        create->setEnabled(true);
    });

    auto *timer = new QTimer(dock);
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, dock, [=] {
        const auto transport = session->transport()->stats();
        const auto decode = tazzio::camera_decode_stats();
        if (!transport.bytes_received && !decode.video_frames)
            return;
        stats->setText(
            QStringLiteral("Media: %1 · trasa %2 · odebrano %3 MB · klatki %4 · błędy dekodera %5")
                .arg(session->transport()->connected() ? QStringLiteral("połączone") : QStringLiteral("łączenie"))
                .arg(route_name(transport))
                .arg(transport.bytes_received / 1024.0 / 1024.0, 0, 'f', 1)
                .arg(decode.video_frames)
                .arg(decode.video_errors));
    });
    timer->start();

    obs_frontend_add_dock_by_id("tazzio-iphone-camera-dock", "Tazzio iPhone Camera", dock);
}
