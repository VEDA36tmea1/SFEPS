#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <QObject>
#include <QList>
#include <QSslCertificate>
#include <QSslSocket>

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
    enum class LoginAttemptResult {
        AuthPass,
        AuthFail,
        TransportError
    };

    bool loadTrustedCaCertificates(const QString &caPathOverride,
                                   QList<QSslCertificate> &outCerts,
                                   QString &outSource,
                                   QString &outError) const;

    LoginAttemptResult attemptTlsLogin(const QString &host,
                                       int port,
                                       const QByteArray &payload,
                                       const QString &caPathOverride,
                                       QString &outTransportError);

    LoginAttemptResult attemptPlainLogin(const QString &host,
                                         int port,
                                         const QByteArray &payload,
                                         QString &outTransportError);

    QSslSocket *socket;
    QString m_currentUserId;
};

#endif // AUTHMANAGER_H
