#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <QObject>
#include <QTcpSocket>

class AuthManager : public QObject
{
    Q_OBJECT
public:
    explicit AuthManager(QObject *parent = nullptr);

    Q_INVOKABLE void login(const QString &id, const QString &pw);

signals:
    void loginSuccess();
    void loginFailed(const QString &message);

private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);

private:
    QTcpSocket *socket;
};

#endif // AUTHMANAGER_H
