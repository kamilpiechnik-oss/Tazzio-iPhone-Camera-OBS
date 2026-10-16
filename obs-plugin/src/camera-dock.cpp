#include "camera-dock.hpp"
#include "camera-session.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSizePolicy>
#include <QUrl>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

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

void update_camera_source_size(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    constexpr const char *source_name = "Tazzio iPhone Camera";
    auto *source = obs_get_source_by_name(source_name);
    if (!source)
        return;
    auto *settings = obs_source_get_settings(source);
    obs_data_set_int(settings, "width", width);
    obs_data_set_int(settings, "height", height);
    obs_source_update(source, settings);
    obs_data_release(settings);
    obs_source_release(source);
}

QString camera_value_name(const QString &value)
{
    if (value == QStringLiteral("weakNetwork")) return QStringLiteral("Oszczędna · 720p");
    if (value == QStringLiteral("lowLatency")) return QStringLiteral("Niskie opóźnienie · 1080p");
    if (value == QStringLiteral("balanced")) return QStringLiteral("Wysoka · 1080p");
    if (value == QStringLiteral("highQuality")) return QStringLiteral("Maksymalna · 1080p");
    if (value == QStringLiteral("continuous")) return QStringLiteral("Ciągły");
    if (value == QStringLiteral("single-shot")) return QStringLiteral("Pojedynczy");
    if (value == QStringLiteral("manual")) return QStringLiteral("Ręczny");
    if (value == QStringLiteral("none")) return QStringLiteral("Wyłączony");
    return value;
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
    const auto installed_version = QStringLiteral(TAZZIO_PLUGIN_VERSION);
    auto *version_status = new QLabel(QStringLiteral("Wersja %1 · sprawdzanie aktualizacji…").arg(installed_version));
    version_status->setWordWrap(true);
    auto *check_updates = new QPushButton(QStringLiteral("Sprawdź aktualizacje"));
    auto *install_update = new QPushButton(QStringLiteral("Pobierz aktualizację"));
    install_update->hide();
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

    auto *controls_group = new QGroupBox(QStringLiteral("Sterowanie kamerą iPhone"));
    auto *controls_form = new QFormLayout(controls_group);
    auto *controls_note = new QLabel(QStringLiteral("Dostępne opcje pojawią się po uruchomieniu kamery na iPhonie."));
    controls_note->setWordWrap(true);
    controls_form->addRow(controls_note);

    QHash<QString, QWidget *> control_rows;
    QHash<QString, QComboBox *> combo_controls;
    QHash<QString, QSlider *> range_controls;
    QHash<QString, QLabel *> range_values;

    auto *camera_select = new QComboBox;
    controls_form->addRow(QStringLiteral("Obiektyw"), camera_select);
    control_rows.insert(QStringLiteral("camera"), camera_select);
    controls_form->setRowVisible(camera_select, false);

    const QHash<QString, QString> enum_labels{
        {QStringLiteral("quality"), QStringLiteral("Jakość transmisji")},
        {QStringLiteral("frameRate"), QStringLiteral("FPS")},
        {QStringLiteral("focusMode"), QStringLiteral("Tryb focusu")},
        {QStringLiteral("exposureMode"), QStringLiteral("Tryb ekspozycji")},
        {QStringLiteral("whiteBalanceMode"), QStringLiteral("Balans bieli")},
    };
    for (auto it = enum_labels.cbegin(); it != enum_labels.cend(); ++it) {
        auto *combo = new QComboBox;
        controls_form->addRow(it.value(), combo);
        control_rows.insert(it.key(), combo);
        combo_controls.insert(it.key(), combo);
        controls_form->setRowVisible(combo, false);
    }

    const QHash<QString, QString> range_labels{
        {QStringLiteral("zoom"), QStringLiteral("Zoom")},
        {QStringLiteral("focusDistance"), QStringLiteral("Dystans focusu")},
        {QStringLiteral("exposureCompensation"), QStringLiteral("Przyciemnienie / ekspozycja")},
        {QStringLiteral("exposureTime"), QStringLiteral("Czas ekspozycji")},
        {QStringLiteral("iso"), QStringLiteral("ISO")},
        {QStringLiteral("brightness"), QStringLiteral("Jasność")},
        {QStringLiteral("colorTemperature"), QStringLiteral("Temperatura barwowa")},
    };
    for (auto it = range_labels.cbegin(); it != range_labels.cend(); ++it) {
        auto *row = new QWidget;
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 1000);
        auto *value = new QLabel(QStringLiteral("—"));
        value->setMinimumWidth(54);
        row_layout->addWidget(slider, 1);
        row_layout->addWidget(value);
        controls_form->addRow(it.value(), row);
        control_rows.insert(it.key(), row);
        range_controls.insert(it.key(), slider);
        range_values.insert(it.key(), value);
        controls_form->setRowVisible(row, false);
    }

    auto *torch = new QCheckBox(QStringLiteral("Włącz latarkę"));
    controls_form->addRow(QStringLiteral("Latarka"), torch);
    control_rows.insert(QStringLiteral("torch"), torch);
    controls_form->setRowVisible(torch, false);
    controls_group->hide();

    auto *url_row = new QHBoxLayout;
    url_row->addWidget(url, 1);
    url_row->addWidget(copy);
    auto *update_row = new QHBoxLayout;
    update_row->addWidget(check_updates);
    update_row->addWidget(install_update);
    update_row->addStretch();
    layout->addWidget(title);
    layout->addWidget(version_status);
    layout->addLayout(update_row);
    layout->addWidget(description);
    layout->addWidget(status);
    layout->addWidget(stats);
    layout->addWidget(mode_label);
    layout->addWidget(mode);
    layout->addWidget(qr);
    layout->addLayout(url_row);
    layout->addWidget(create);
    layout->addWidget(disconnect);
    layout->addWidget(controls_group);
    layout->addStretch();

    QObject::connect(check_updates, &QPushButton::clicked, dock, [=] {
        check_updates->setEnabled(false);
        version_status->setText(QStringLiteral("Wersja %1 · sprawdzanie aktualizacji…").arg(installed_version));
        session->check_for_updates();
    });
    QObject::connect(install_update, &QPushButton::clicked, dock, [=] {
        const auto update_url = install_update->property("updateUrl").toUrl();
        if (update_url.isValid()) QDesktopServices::openUrl(update_url);
    });
    QObject::connect(session, &tazzio::CameraSession::updateCheckFinished, dock,
                     [=](const QString &latest, bool available, const QUrl &download_url) {
                         check_updates->setEnabled(true);
                         if (available) {
                             version_status->setText(QStringLiteral("Dostępna aktualizacja: %1 (zainstalowana: %2)").arg(latest, installed_version));
                             install_update->setText(QStringLiteral("Pobierz aktualizację %1").arg(latest));
                             install_update->setProperty("updateUrl", download_url);
                             install_update->show();
                         } else {
                             version_status->setText(QStringLiteral("Wersja %1 · aktualna").arg(installed_version));
                             install_update->hide();
                         }
                     });
    QObject::connect(session, &tazzio::CameraSession::updateCheckFailed, dock, [=](const QString &) {
        check_updates->setEnabled(true);
        version_status->setText(QStringLiteral("Wersja %1 · nie udało się sprawdzić aktualizacji").arg(installed_version));
        install_update->hide();
    });

    QObject::connect(camera_select, qOverload<int>(&QComboBox::activated), dock, [=](int index) {
        const auto device_id = camera_select->itemData(index).toString();
        if (!device_id.isEmpty()) session->send_camera_command(QStringLiteral("camera"), device_id);
    });
    for (auto it = combo_controls.cbegin(); it != combo_controls.cend(); ++it) {
        const auto name = it.key();
        auto *combo = it.value();
        QObject::connect(combo, qOverload<int>(&QComboBox::activated), dock, [=](int index) {
            session->send_camera_command(name, QJsonValue::fromVariant(combo->itemData(index)));
        });
    }
    for (auto it = range_controls.cbegin(); it != range_controls.cend(); ++it) {
        const auto name = it.key();
        auto *slider = it.value();
        QObject::connect(slider, &QSlider::sliderReleased, dock, [=] {
            const auto minimum = slider->property("cameraMinimum").toDouble();
            const auto maximum = slider->property("cameraMaximum").toDouble();
            const auto value = minimum + (maximum - minimum) * slider->value() / 1000.0;
            session->send_camera_command(name, value);
        });
    }
    QObject::connect(torch, &QCheckBox::toggled, dock, [=](bool checked) {
        if (torch->isVisible()) session->send_camera_command(QStringLiteral("torch"), checked);
    });

    const auto update_controls = [=](const QJsonObject &capabilities) {
        bool any = false;
        for (auto *row : control_rows) controls_form->setRowVisible(row, false);

        const auto devices = capabilities.value(QStringLiteral("devices")).toArray();
        if (devices.size() > 1) {
            const QSignalBlocker blocker(camera_select);
            camera_select->clear();
            const auto active = capabilities.value(QStringLiteral("activeDeviceId")).toString();
            int selected = 0;
            for (const auto &item : devices) {
                const auto device = item.toObject();
                camera_select->addItem(device.value(QStringLiteral("label")).toString(QStringLiteral("Kamera")),
                                       device.value(QStringLiteral("id")).toString());
                if (device.value(QStringLiteral("id")).toString() == active) selected = camera_select->count() - 1;
            }
            camera_select->setCurrentIndex(selected);
            controls_form->setRowVisible(camera_select, true); any = true;
        }

        const auto controls = capabilities.value(QStringLiteral("controls")).toObject();
        for (auto it = combo_controls.cbegin(); it != combo_controls.cend(); ++it) {
            if (!controls.contains(it.key())) continue;
            const auto description = controls.value(it.key()).toObject();
            const auto values = description.value(QStringLiteral("values")).toArray();
            if (values.isEmpty()) continue;
            auto *combo = it.value(); const QSignalBlocker blocker(combo); combo->clear();
            int selected = 0; const auto current = description.value(QStringLiteral("value"));
            for (const auto &item : values) {
                const auto label = it.key() == QStringLiteral("frameRate")
                    ? QStringLiteral("%1 FPS").arg(item.toInt()) : camera_value_name(item.toString());
                combo->addItem(label, item.toVariant());
                if (item == current) selected = combo->count() - 1;
            }
            combo->setCurrentIndex(selected); controls_form->setRowVisible(combo, true); any = true;
        }

        for (auto it = range_controls.cbegin(); it != range_controls.cend(); ++it) {
            if (!controls.contains(it.key())) continue;
            const auto description = controls.value(it.key()).toObject();
            const auto minimum = description.value(QStringLiteral("min")).toDouble();
            const auto maximum = description.value(QStringLiteral("max")).toDouble();
            const auto current = description.value(QStringLiteral("value")).toDouble();
            auto *slider = it.value(); const QSignalBlocker blocker(slider);
            slider->setProperty("cameraMinimum", minimum); slider->setProperty("cameraMaximum", maximum);
            slider->setValue(maximum > minimum ? qRound((current - minimum) * 1000.0 / (maximum - minimum)) : 0);
            range_values.value(it.key())->setText(QString::number(current, 'f', qAbs(current) < 10 ? 2 : 0));
            controls_form->setRowVisible(control_rows.value(it.key()), true); any = true;
        }

        if (controls.contains(QStringLiteral("torch"))) {
            const QSignalBlocker blocker(torch);
            torch->setChecked(controls.value(QStringLiteral("torch")).toObject().value(QStringLiteral("value")).toBool());
            controls_form->setRowVisible(torch, true); any = true;
        }
        controls_note->setText(any ? QStringLiteral("Widoczne są tylko funkcje udostępnione przez ten iPhone i Safari.")
                                   : QStringLiteral("Ten iPhone nie udostępnił dodatkowych ustawień kamery w Safari."));
        controls_group->show();

        const auto video = capabilities.value(QStringLiteral("video")).toObject();
        const auto width = video.value(QStringLiteral("width")).toInt();
        const auto height = video.value(QStringLiteral("height")).toInt();
        const auto fps = qRound(video.value(QStringLiteral("frameRate")).toDouble());
        const auto target_bitrate = video.value(QStringLiteral("targetBitrateMbps")).toDouble();
        const auto quality = camera_value_name(video.value(QStringLiteral("quality")).toString());
        if (width > 0 && height > 0) {
            update_camera_source_size(width, height);
            const auto route = stats->property("cameraRoute").toString();
            stats->setText(QStringLiteral("%1 · obraz %2×%3 · %4 FPS · %5 · limit %6 Mb/s")
                               .arg(route).arg(width).arg(height).arg(fps).arg(quality).arg(target_bitrate, 0, 'f', 0));
        }
    };

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
        stats->setProperty("cameraRoute", QString{});
        controls_group->hide();
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
                         const auto route = selected_mode == QStringLiteral("internet")
                                                ? QStringLiteral("Trasa: Internet przez VPS TURN")
                                                : QStringLiteral("Trasa: LAN bezpośrednio");
                         stats->setProperty("cameraRoute", route);
                         stats->setText(route + QStringLiteral(" · odbiornik OBS Browser Source"));
                     });
    QObject::connect(session, &tazzio::CameraSession::statusChanged, status, &QLabel::setText);
    QObject::connect(session, &tazzio::CameraSession::peerReady, dock, [=](bool ready) {
        if (!ready) {
            stats->setText(QStringLiteral("Media: oczekiwanie na iPhone'a"));
            controls_group->hide();
        }
    });
    QObject::connect(session, &tazzio::CameraSession::cameraCapabilitiesChanged, dock, update_controls);
    QObject::connect(session, &tazzio::CameraSession::cameraStateChanged, dock, update_controls);
    QObject::connect(session, &tazzio::CameraSession::cameraCommandFinished, dock,
                     [=](const QString &, bool ok, const QString &message) {
                         if (!ok) status->setText(QStringLiteral("Nie udało się zmienić ustawienia kamery: ") + message);
                     });
    QObject::connect(session, &tazzio::CameraSession::errorOccurred, dock, [=](const QString &message) {
        status->setText(QStringLiteral("Błąd: ") + message);
        create->setEnabled(true);
        mode->setEnabled(true);
    });

    obs_frontend_add_dock_by_id("tazzio-iphone-camera-dock", "Tazzio iPhone Camera", dock);
    QTimer::singleShot(1000, session, [session] { session->check_for_updates(); });
}
