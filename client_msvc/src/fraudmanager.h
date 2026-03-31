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

    // 부정승차 감지 시 자동 추적 요청
    // xmlId: 서버 ONVIF XML ID ("1071432")
    // bboxL/T/R/B: 서버가 보낸 픽셀 좌표 (객체가 화면에 없을 때 fallback용, 0이면 미제공)
    void fraudAutoTrackRequest(const QString &xmlId,
                               float bboxL, float bboxT, float bboxR, float bboxB);
    // 대기 중인 FRAUD 건 수 변화 알림 (QML에서 표시 가능)
    Q_REVISION(1) void fraudQueueChanged(int pendingCount);

public slots:
    // PositionManager의 currentSubscribedIdChanged에 연결하여 추적 상태 동기화
    void setActiveTrackingId(const QString &id);

public:
    // 현재 추적 중인 객체 ID 조회 (빈 문자열이면 미추적)
    QString activeTrackingId() const { return m_activeTrackingId; }

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
                      const QString &imagePath = "",
                      float bboxL = 0, float bboxT = 0,
                      float bboxR = 0, float bboxB = 0);
    void drainFraudQueue();

    QTcpSocket *socket;
    QTimer *retryTimer;
    QString lastHost;
    int lastPort;
    QString m_currentHost;
    int m_tlsPort = 6557;
    int m_plainPort = 5557;
    bool m_tlsInProgress = false;
    bool m_tlsFallbackUsed = false;
    bool m_forcePlainAfterTlsFail = false;
    bool m_skipRetryOnDisconnect = false;
    QByteArray recvBuffer;
    bool m_alertTlsEnabled = false;
    QNetworkAccessManager *networkManager;
    QMap<QString, ImgRefData> pendingImages;
    QMap<QString, QString> downloadedImages;

    // 부정승차 대기큐: 현재 추적 중일 때 받은 FRAUD 이벤트를 보관
    struct QueuedFraud {
        QString objectId, cardAgeText, age, tag, imagePath;
        bool isFraud;
        float bboxL{0}, bboxT{0}, bboxR{0}, bboxB{0};  // 서버 제공 픽셀 좌표 (fallback용)
    };
    QString m_activeTrackingId;           // 현재 추적 중인 객체 ID (빈 문자열이면 미추적)
    QList<QueuedFraud> m_fraudQueue;      // 대기 중인 이벤트
};

#endif // FRAUDMANAGER_H