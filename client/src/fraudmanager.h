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

class FraudManager : public QObject
{
    Q_OBJECT
public:
    explicit FraudManager(QObject *parent = nullptr);
    ~FraudManager();

    Q_INVOKABLE void connectToServer(const QString &host = "192.168.0.101", int port = 5557);
    Q_INVOKABLE void sendCommand(const QString &msg);
    // Position channel moved to PositionManager

signals:
    void fraudDetected(const QString &objectId,
                       const QString &cardAgeText,
                       const QString &age,
                       bool isFraud);
    

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void retryConnection();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSslErrors(const QList<QSslError> &errors);

private:
    void attachSocketSignals();
    bool resolveAlertTlsEnabled() const;

    QTcpSocket *socket;
    QTimer *retryTimer;
    QString lastHost;
    int lastPort;
    QByteArray recvBuffer; // 누적 수신 버퍼 (부분 수신 처리용)
    bool m_alertTlsEnabled = false;
    
};

#endif // FRAUDMANAGER_H