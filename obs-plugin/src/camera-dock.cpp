#include "camera-dock.hpp"
#include "camera-session.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSizePolicy>
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

bool ensure_camera_source(const QString &receiver_url)
{
    constexpr const char *source_name = "Tazzio iPhone Camera";
    if (auto *existing = obs_get_source_by_name(source_name)) {
        const auto *source_id = obs_source_get_id(existing);
        if (source_id && std::string(source_id) == "browser_source") {
            auto *settings = obs_source_get_settings(existing);
            obs_data_set_string(settings, "url", receiver_url.toUtf8().constData());
            obs_data_set_int(settings, "width", 1920);
            obs_data_set_int(settings, "height", 1080);
            obs_data_set_bool(settings, "shutdown", false);
            obs_data_set_bool(settings, "restart_when_active", false);
            obs_data_set_bool(settings, "reroute_audio", true);
            obs_source_update(existing, settings);
            obs_data_release(settings);
            obs_source_release(existing);
            return true;
        }
        obs_source_remove(existing);
        obs_source_release(existing);
    }
    auto *scene_source = obs_frontend_get_current_scene();
    if (!scene_source)
        return false;
    auto *scene = obs_scene_from_source(scene_source);
    auto *settings = obs_data_create();
    obs_data_set_string(settings, "url", receiver_url.toUtf8().constData());
    obs_data_set_int(settings, "width", 1920);
    obs_data_set_int(settings, "height", 1080);
    obs_data_set_bool(settings, "shutdown", false);
    obs_data_set_bool(settings, "restart_when_active", false);
    obs_data_set_bool(settings, "reroute_audio", true);
    auto *source = obs_source_create("browser_source", source_name, settings, nullptr);
    obs_data_release(settings);
    const auto added = scene && source && obs_scene_add(scene, source) != nullptr;
    if (source)
        obs_source_release(source);
    obs_source_release(scene_source);
    return added;
}

} // namespace

void register_iphone_camera_dock()
{
    auto *dock = new QScrollArea;
    dock->setWidgetResizable(true);
    dock->setFrameShape(QFrame::NoFrame);
    dock->setMinimumSize(0, 0);
    dock->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    auto *content = new QWidget;
    content->setMinimumSize(0, 0);
    auto *layout = new QVBoxLayout(content);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    dock->setWidget(content);
    auto *session = new tazzio::CameraSession(dock);

    auto *title = new QLabel(QStringLiteral("TAZZIO · IPHONE CAMERA"));
    auto *description = new QLabel(QStringLiteral(
        "Wybierz trasę w Docku. LAN przesyła obraz bezpośrednio do źródła Przeglądarka w OBS; Internet używa przekaźnika VPS."));
    description->setWordWrap(true);
    auto *status = new QLabel(QStringLiteral("Utwórz kod QR, aby rozpocząć."));
    status->setWordWrap(true);
    auto *stats = new QLabel(QStringLiteral("Media: nieaktywne"));
    stats->setWordWrap(true);

    auto *mode_label = new QLabel(QStringLiteral("Tryb połączenia"));
    auto *mode = new QComboBox;
    mode->addItem(QStringLiteral("Wi‑Fi / LAN — bezpośrednio"), QStringLiteral("lan"));
    mode->addItem(QStringLiteral("Sieć komórkowa / Internet — przez VPS"), QStringLiteral("internet"));

    auto *qr = new QLabel;
    qr->setAlignment(Qt::AlignCenter);
    qr->setMinimumSize(0, 0);
    qr->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    layout->addWidget(mode_label);
    layout->addWidget(mode);
    layout->addWidget(qr);
    layout->addLayout(url_row);
    layout->addWidget(create);
    layout->addWidget(disconnect);
    layout->addStretch();

    QObject::connect(create, &QPushButton::clicked, dock, [=] {
        create->setEnabled(false);
        disconnect->setEnabled(true);
        copy->setEnabled(false);
        qr->setText(QStringLiteral("Generowanie kodu…"));
        qr->setPixmap({});
        url->clear();
        mode->setEnabled(false);
        session->create_pairing(mode->currentData().toString());
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
        mode->setEnabled(true);
    });
    QObject::connect(session, &tazzio::CameraSession::pairingReady, dock,
                     [=](const QString &pairing_url, const QString &receiver_url, const QString &selected_mode, int) {
                         if (!ensure_camera_source(receiver_url)) {
                             status->setText(QStringLiteral("Nie udało się dodać źródła Przeglądarka do aktualnej sceny OBS."));
                             create->setEnabled(true);
                             mode->setEnabled(true);
                             return;
                         }
                         url->setText(pairing_url);
                         qr->setPixmap(qr_pixmap(pairing_url, 230));
                         copy->setEnabled(true);
                         create->setEnabled(true);
                         stats->setText(selected_mode == QStringLiteral("internet")
                                            ? QStringLiteral("Trasa: Internet przez VPS TURN · odbiornik OBS Browser Source")
                                            : QStringLiteral("Trasa: LAN bezpośrednio · odbiornik OBS Browser Source"));
                     });
    QObject::connect(session, &tazzio::CameraSession::statusChanged, status, &QLabel::setText);
    QObject::connect(session, &tazzio::CameraSession::peerReady, dock, [=](bool ready) {
        if (!ready)
            stats->setText(QStringLiteral("Media: oczekiwanie na iPhone'a"));
    });
    QObject::connect(session, &tazzio::CameraSession::errorOccurred, dock, [=](const QString &message) {
        status->setText(QStringLiteral("Błąd: ") + message);
        create->setEnabled(true);
        mode->setEnabled(true);
    });

    obs_frontend_add_dock_by_id("tazzio-iphone-camera-dock", "Tazzio iPhone Camera", dock);
}
