#pragma once

#include "peer-transport.hpp"

#include <QObject>
#include <QJsonObject>
#include <QUrl>

#include <memory>
#include <string>

class QNetworkAccessManager;

namespace rtc {
class WebSocket;
}

namespace tazzio {

class CameraSession final : public QObject {
    Q_OBJECT

public:
    explicit CameraSession(QObject *parent = nullptr);
    ~CameraSession() override;

    void set_server(QUrl server);
    void create_pairing(const QString &mode);
    void disconnect_session();
    [[nodiscard]] PeerTransport *transport();

signals:
    void pairingReady(QString pairingUrl, QString receiverUrl, QString mode, int expiresIn);
    void statusChanged(QString status);
    void peerReady(bool ready);
    void errorOccurred(QString message);

private:
    void open_signaling();
    void handle_message(const std::string &message);
    void send_signal(const QString &type, const QJsonObject &payload);
    QUrl api_url(const QString &path) const;

    QNetworkAccessManager *network_{};
    QUrl server_{QStringLiteral("https://tazzio.pl")};
    QString socket_token_;
    QString pairing_url_;
    std::vector<IceServerConfig> ice_servers_;
    std::shared_ptr<rtc::WebSocket> websocket_;
    std::unique_ptr<PeerTransport> transport_;
};

} // namespace tazzio
