#ifndef FRAUDMANAGER_H
#define FRAUDMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QStringList>

class FraudManager : public QObject
{
    Q_OBJECT
public:
    explicit FraudManager(QObject *parent = nullptr);
    ~FraudManager();

    Q_INVOKABLE void connectToServer(const QString &host = "192.168.0.89", int port = 5557);

signals:
    void fraudDetected(const QString &cardId, const QString &ageGroup, int gateId, int estAge);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();

private:
    QTcpSocket *socket;
};

#endif // FRAUDMANAGER_H
