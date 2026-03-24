#ifndef FRAUDMANAGER_H
#define FRAUDMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QSslSocket>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>
#include <QByteArray>
#include <QVariant>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>

struct ImgRefData {
    QString objectId;
    QString url;
    QString tag;
    QString name;
};

class FraudManager : public QObject
{
    Q_OBJECT
public:
    explicit FraudManager(QObject *parent = nullptr);
    ~FraudManager();

    Q_INVOKABLE void connectToServer(const QString &host = "192.168.0.101", int port = 5557);

signals:
    void fraudDetected(const QString &objectId,
                       const QString &cardAgeText,
                       const QString &age,
                       bool isFraud,
                       const QString &tag = "",
                       const QString &imagePath = "");
    void loginAckReceived(const QString &userId);
    void serverDisconnected();
    void serverConnected();
    void forceLogoutEvent(const QString &rawMsg);
    void imageReceived(const QString &objectId, const QString &tag, const QString &localFilePath);
    
private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void retryConnection();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSslErrors(const QList<QSslError> &errors);
    void onImageDownloadFinished(QNetworkReply *reply);

private:
    void sendCommand(const QString &msg);
    void attachSocketSignals();
    bool resolveAlertTlsEnabled() const;
    void downloadImage(const ImgRefData &imgRef);
    QString getImageStoragePath() const;

    QTcpSocket *socket;
    QTimer *retryTimer;
    QString lastHost;
    int lastPort;
    QByteArray recvBuffer; // 누적 수신 버퍼 (부분 수신 처리용)
    bool m_alertTlsEnabled = false;
    QNetworkAccessManager *networkManager;
    // Cache pending images by event_key = objectId|tag
    QMap<QString, ImgRefData> pendingImages;
    // Track downloaded images to avoid re-downloading
    QMap<QString, QString> downloadedImages; // key: objectId|tag, value: localFilePath
    
};

#endif // FRAUDMANAGER_H