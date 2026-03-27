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
#include <QList>

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

    // 부정승차 감지 시 자동 추적 요청 (positionManager.sendPositionCommand와 연결)
    void fraudAutoTrackRequest(const QString &cmd);
    // 대기 중인 FRAUD 건 수 변화 알림 (QML에서 표시 가능)
    Q_REVISION(1) void fraudQueueChanged(int pendingCount);

public slots:
    // PositionManager의 currentSubscribedIdChanged에 연결하여 추적 상태 동기화
    void setActiveTrackingId(const QString &id);

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

    // 수신된 FRAUD 메시지 처리: 추적 중이면 큐에, 아니면 즉시 emit + 자동 추적 요청
    void processFraud(const QString &objectId,
                      const QString &cardAgeText,
                      const QString &age,
                      bool isFraud,
                      const QString &tag,
                      const QString &imagePath = "");
    void drainFraudQueue();

    QTcpSocket *socket;
    QTimer *retryTimer;
    QString lastHost;
    int lastPort;
    QByteArray recvBuffer;
    bool m_alertTlsEnabled = false;
    QNetworkAccessManager *networkManager;
    QMap<QString, ImgRefData> pendingImages;
    QMap<QString, QString> downloadedImages;

    // 부정승차 대기큐: 현재 추적 중일 때 받은 FRAUD 이벤트를 보관
    struct QueuedFraud {
        QString objectId, cardAgeText, age, tag, imagePath;
        bool isFraud;
    };
    QString m_activeTrackingId;           // 현재 추적 중인 객체 ID (빈 문자열이면 미추적)
    QList<QueuedFraud> m_fraudQueue;      // 대기 중인 이벤트
};

#endif // FRAUDMANAGER_H