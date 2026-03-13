#ifndef POSITIONMANAGER_H
#define POSITIONMANAGER_H

#include <QByteArray>
#include <QObject>
#include <QProcessEnvironment>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QTcpSocket>

class PositionManager : public QObject
{
    Q_OBJECT
public:
    explicit PositionManager(QObject *parent = nullptr);
    ~PositionManager() override;

    Q_INVOKABLE void connectToServer(const QString &host = "192.168.0.80", int port = 5558);
    Q_INVOKABLE void disconnectFromServer();

signals:
    void objectPositionReceived(const QString &objectId,
                                double left,
                                double top,
                                double right,
                                double bottom,
                                double x,
                                double y,
                                bool isFraud,
                                const QString &tagTime);
    void objectEnded(const QString &objectId, const QString &reason);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void retryConnection();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSslErrors(const QList<QSslError> &errors);

private:
    void attachSocketSignals();
    bool resolvePositionTlsEnabled() const;

    QTcpSocket *socket;
    QTimer *retryTimer;
    QString lastHost;
    int lastPort;
    QByteArray recvBuffer;
    bool m_positionTlsEnabled = false;
};

#endif // POSITIONMANAGER_H
