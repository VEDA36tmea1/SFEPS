#ifndef POSITIONMANAGER_H
#define POSITIONMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QVariant>

class PositionManager : public QObject {
    Q_OBJECT
public:
    explicit PositionManager(QObject *parent = nullptr);
    ~PositionManager();

    Q_INVOKABLE void connectPositionServer(const QString &host = "192.168.0.101", int port = 5558);
    Q_INVOKABLE void sendPositionCommand(const QString &msg);

signals:
    void positionsUpdated(const QVariantList &list);

private:
    void attachPosSocketSignals();

    QTcpSocket *posSocket = nullptr;
    QByteArray posRecvBuffer;
    QString lastPosHost;
    int lastPosPort = 0;
};

#endif // POSITIONMANAGER_H
