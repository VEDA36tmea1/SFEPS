#include "authmanager.h"

#include <QDebug>
#include <QFile>
#include <QProcessEnvironment>
#include <QSslError>
#include <QStringList>

namespace {

constexpr int kAuthConnectTimeoutMs = 3000;
constexpr int kDefaultAuthTlsPort = 6555;
constexpr int kDefaultAuthPlainPort = 5555;
constexpr bool kDefaultAuthTlsEnable = true;
constexpr bool kDefaultPlainFallbackEnable = false;
constexpr const char* kDefaultAuthHost = "192.168.0.92";
constexpr const char* kResourceCaPath = ":/certs/auth_ca.pem";

QString maskUserId(const QString& userId)
{
    if (userId.isEmpty()) return QStringLiteral("<empty>");
    if (userId.size() <= 2) return QStringLiteral("**");
    return userId.left(2) + QStringLiteral("***");
}

bool parseEnvBool(const QProcessEnvironment& env, const QString& key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;

    qWarning() << "[AuthManager] Invalid boolean env" << key << "=" << raw
               << ", using default" << defaultValue;
    return defaultValue;
}

int parseEnvPort(const QProcessEnvironment& env, const QString& key, int defaultValue)
{
    const QString raw = env.value(key).trimmed();
    if (raw.isEmpty()) return defaultValue;

    bool ok = false;
    const int parsed = raw.toInt(&ok);
    if (!ok || parsed < 1 || parsed > 65535) {
        qWarning() << "[AuthManager] Invalid port env" << key << "=" << raw
                   << ", using default" << defaultValue;
        return defaultValue;
    }
    return parsed;
}

} // namespace

AuthManager::AuthManager(QObject *parent) : QObject(parent)
{
    socket = new QSslSocket(this);
    connect(socket, &QSslSocket::readyRead, this, &AuthManager::onReadyRead);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QSslSocket::errorOccurred, this, &AuthManager::onSocketError);
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QSslSocket::error), this, &AuthManager::onSocketError);
#endif
}

void AuthManager::login(const QString &id, const QString &pw)
{
    const QString userId = id.trimmed();
    const QString trimmedPw = pw.trimmed();
    if (userId.isEmpty() || pw.trimmed().isEmpty()) {
        emit loginFailed("ID와 PW를 모두 입력하세요");
        return;
    }

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString authHost = env.value("AUTH_SERVER_HOST", QString::fromUtf8(kDefaultAuthHost));
    const int authTlsPort = parseEnvPort(env, "AUTH_TLS_PORT", kDefaultAuthTlsPort);
    const int authPlainPort = parseEnvPort(env, "AUTH_PLAINTEXT_PORT", kDefaultAuthPlainPort);
    const bool authTlsEnable = parseEnvBool(env, "AUTH_TLS_ENABLE", kDefaultAuthTlsEnable);
    const bool allowPlainFallback = parseEnvBool(env,
                                                 "AUTH_ALLOW_PLAINTEXT_FALLBACK",
                                                 kDefaultPlainFallbackEnable);
    const QString authTlsCaPath = env.value("AUTH_TLS_CA_FILE").trimmed();
    const QByteArray payload = QString("%1:%2").arg(userId, pw).toUtf8();

    qInfo().noquote()
        << QString("[AuthFlow][1] login payload prepared (user=%1, id_len=%2, pw_len=%3, bytes=%4)")
               .arg(maskUserId(userId))
               .arg(userId.size())
               .arg(trimmedPw.size())
               .arg(payload.size());
    qInfo().noquote()
        << QString("[AuthFlow][2] transport policy host=%1 tls=%2 tls_port=%3 plain_port=%4 plain_fallback=%5")
               .arg(authHost)
               .arg(authTlsEnable ? "on" : "off")
               .arg(authTlsPort)
               .arg(authPlainPort)
               .arg(allowPlainFallback ? "on" : "off");

    LoginAttemptResult result = LoginAttemptResult::TransportError;
    QString transportError;
    bool attemptedTls = false;
    bool attemptedFallback = false;

    if (authTlsEnable) {
        attemptedTls = true;
        qInfo().noquote() << QString("[AuthFlow][3] attempting TLS auth connect %1:%2")
                                 .arg(authHost)
                                 .arg(authTlsPort);
        result = attemptTlsLogin(authHost, authTlsPort, payload, authTlsCaPath, transportError);

        if (result == LoginAttemptResult::TransportError) {
            qWarning() << "[AuthManager] TLS auth transport failed:" << transportError;
            if (allowPlainFallback) {
                attemptedFallback = true;
                qWarning() << "[SECURITY] plaintext fallback used for auth";
                qInfo().noquote() << QString("[AuthFlow][3] fallback plaintext auth connect %1:%2")
                                         .arg(authHost)
                                         .arg(authPlainPort);

                QString plainTransportError;
                result = attemptPlainLogin(authHost, authPlainPort, payload, plainTransportError);
                if (result == LoginAttemptResult::TransportError) {
                    transportError = plainTransportError;
                    qWarning() << "[AuthManager] plaintext fallback failed:" << transportError;
                }
            }
        }
    } else {
        qWarning() << "[SECURITY] AUTH_TLS_ENABLE=0, using plaintext auth transport.";
        qInfo().noquote() << QString("[AuthFlow][3] attempting plaintext auth connect %1:%2")
                                 .arg(authHost)
                                 .arg(authPlainPort);
        result = attemptPlainLogin(authHost, authPlainPort, payload, transportError);
        if (result == LoginAttemptResult::TransportError) {
            qWarning() << "[AuthManager] plaintext auth transport failed:" << transportError;
        }
    }

    if (result == LoginAttemptResult::AuthPass) {
        m_currentUserId = userId;
        emit currentUserIdChanged();
        emit loginSuccess();
        return;
    }

    if (result == LoginAttemptResult::AuthFail) {
        emit loginFailed("존재하지 않는 계정이거나 비밀번호가 올바르지 않습니다");
        return;
    }

    if (attemptedTls && !attemptedFallback) {
        emit loginFailed("보안 연결(TLS)로 인증 서버에 연결할 수 없습니다");
    } else {
        emit loginFailed("인증 서버에 연결할 수 없습니다");
    }
}

void AuthManager::onReadyRead()
{
    // 완전한 비동기 방식이라면 여기서 데이터를 처리합니다.
    // 하지만 login() 함수 내에서 waitForReadyRead를 사용하므로,
    // 거기서 데이터가 읽히면 이 슬롯은 호출되지 않을 수 있습니다.
    // 향후 비동기 리팩토링을 위해 남겨둡니다.
}

void AuthManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    qWarning() << "[AuthManager] Socket Error:" << socketError << socket->errorString();
}

bool AuthManager::loadTrustedCaCertificates(const QString &caPathOverride,
                                            QList<QSslCertificate> &outCerts,
                                            QString &outSource,
                                            QString &outError) const
{
    auto loadPemCertificates = [](const QString &path,
                                  QList<QSslCertificate> &out,
                                  QString &err) -> bool {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            err = QString("open failed (%1): %2").arg(path, file.errorString());
            return false;
        }

        const QByteArray pemData = file.readAll();
        const QList<QSslCertificate> certs = QSslCertificate::fromData(pemData, QSsl::Pem);
        if (certs.isEmpty()) {
            err = QString("no valid PEM certificate found in %1").arg(path);
            return false;
        }

        out = certs;
        return true;
    };

    const QString trimmedOverride = caPathOverride.trimmed();
    QString overrideError;
    if (!trimmedOverride.isEmpty()) {
        if (loadPemCertificates(trimmedOverride, outCerts, overrideError)) {
            outSource = trimmedOverride;
            return true;
        }
        qWarning() << "[AuthManager] AUTH_TLS_CA_FILE load failed:" << overrideError;
    }

    const QString resourcePath = QString::fromUtf8(kResourceCaPath);
    QString resourceError;
    if (loadPemCertificates(resourcePath, outCerts, resourceError)) {
        outSource = resourcePath;
        return true;
    }

    if (!overrideError.isEmpty()) {
        outError = QString("custom CA failed (%1), resource CA failed (%2)")
                       .arg(overrideError, resourceError);
    } else {
        outError = QString("resource CA failed (%1)").arg(resourceError);
    }
    return false;
}

AuthManager::LoginAttemptResult AuthManager::attemptTlsLogin(const QString &host,
                                                             int port,
                                                             const QByteArray &payload,
                                                             const QString &caPathOverride,
                                                             QString &outTransportError)
{
    QList<QSslCertificate> trustedCerts;
    QString caSource;
    QString caError;
    if (!loadTrustedCaCertificates(caPathOverride, trustedCerts, caSource, caError)) {
        outTransportError = QString("TLS CA load failed: %1").arg(caError);
        return LoginAttemptResult::TransportError;
    }
    qInfo().noquote() << QString("[AuthFlow][3] TLS CA loaded from %1 (count=%2)")
                             .arg(caSource)
                             .arg(trustedCerts.size());

    socket->abort();
    socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket->setPeerVerifyName(host);

    QSslConfiguration sslConfig = socket->sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    sslConfig.setCaCertificates(trustedCerts);
    socket->setSslConfiguration(sslConfig);

    QStringList sslErrorMessages;
    auto sslConn = connect(socket,
                           &QSslSocket::sslErrors,
                           this,
                           [&sslErrorMessages](const QList<QSslError> &errors) {
                               for (const QSslError &err : errors) {
                                   sslErrorMessages.push_back(err.errorString());
                               }
                           });

    socket->connectToHostEncrypted(host, static_cast<quint16>(port));
    if (!socket->waitForEncrypted(kAuthConnectTimeoutMs)) {
        disconnect(sslConn);
        QString detail = socket->errorString();
        if (!sslErrorMessages.isEmpty()) {
            detail += QString(" | sslErrors=%1").arg(sslErrorMessages.join("; "));
        }
        outTransportError = QString("TLS handshake failed (%1:%2, ca=%3): %4")
                                .arg(host)
                                .arg(port)
                                .arg(caSource, detail);
        socket->abort();
        return LoginAttemptResult::TransportError;
    }
    disconnect(sslConn);

    const qint64 written = socket->write(payload);
    socket->flush();
    if (written < 0 || !socket->waitForBytesWritten(kAuthConnectTimeoutMs)) {
        outTransportError = QString("TLS write failed (%1:%2): %3")
                                .arg(host)
                                .arg(port)
                                .arg(socket->errorString());
        socket->abort();
        return LoginAttemptResult::TransportError;
    }

    if (!socket->waitForReadyRead(kAuthConnectTimeoutMs)) {
        outTransportError = QString("TLS auth response timeout (%1:%2): %3")
                                .arg(host)
                                .arg(port)
                                .arg(socket->errorString());
        socket->abort();
        return LoginAttemptResult::TransportError;
    }

    const QByteArray response = socket->readAll().trimmed();
    socket->disconnectFromHost();

    if (response == "PASS") return LoginAttemptResult::AuthPass;
    if (response == "FAIL") return LoginAttemptResult::AuthFail;

    outTransportError = QString("unexpected TLS auth response: %1")
                            .arg(QString::fromUtf8(response));
    return LoginAttemptResult::TransportError;
}

AuthManager::LoginAttemptResult AuthManager::attemptPlainLogin(const QString &host,
                                                               int port,
                                                               const QByteArray &payload,
                                                               QString &outTransportError)
{
    socket->abort();
    socket->connectToHost(host, static_cast<quint16>(port));

    if (!socket->waitForConnected(kAuthConnectTimeoutMs)) {
        outTransportError = QString("plaintext connect failed (%1:%2): %3")
                                .arg(host)
                                .arg(port)
                                .arg(socket->errorString());
        socket->abort();
        return LoginAttemptResult::TransportError;
    }

    const qint64 written = socket->write(payload);
    socket->flush();
    if (written < 0 || !socket->waitForBytesWritten(kAuthConnectTimeoutMs)) {
        outTransportError = QString("plaintext write failed (%1:%2): %3")
                                .arg(host)
                                .arg(port)
                                .arg(socket->errorString());
        socket->abort();
        return LoginAttemptResult::TransportError;
    }

    if (!socket->waitForReadyRead(kAuthConnectTimeoutMs)) {
        outTransportError = QString("plaintext auth response timeout (%1:%2): %3")
                                .arg(host)
                                .arg(port)
                                .arg(socket->errorString());
        socket->abort();
        return LoginAttemptResult::TransportError;
    }

    const QByteArray response = socket->readAll().trimmed();
    socket->disconnectFromHost();

    if (response == "PASS") return LoginAttemptResult::AuthPass;
    if (response == "FAIL") return LoginAttemptResult::AuthFail;

    outTransportError = QString("unexpected plaintext auth response: %1")
                            .arg(QString::fromUtf8(response));
    return LoginAttemptResult::TransportError;
}
