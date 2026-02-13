#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <QObject>
#include <QTcpSocket>

class AuthManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY currentUserIdChanged)
public:
    explicit AuthManager(QObject *parent = nullptr);

    Q_INVOKABLE void login(const QString &id, const QString &pw);
    QString currentUserId() const { return m_currentUserId; }

signals:
    void loginSuccess();
    void loginFailed(const QString &message);
    void currentUserIdChanged();

private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);

private:
    QTcpSocket *socket;
    QString m_currentUserId;
};

#endif // AUTHMANAGER_H
