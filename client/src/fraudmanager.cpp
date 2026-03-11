#include "fraudmanager.h"
#include <QDebug>
#include <QFile>
#include <QSslCertificate>
#include <QIODevice>
#include <QProcessEnvironment>
#include <QSslConfiguration>
#include <QSslError>

static bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

static QList<QSslCertificate> loadCaCertificates(const QString &path, QString &outError)
{
    QList<QSslCertificate> certs;
    if (path.trimmed().isEmpty()) return certs;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        outError = QString("open failed (%1): %2").arg(path, f.errorString());
        return certs;
    }
    const QByteArray data = f.readAll();
    certs = QSslCertificate::fromData(data, QSsl::Pem);
    if (certs.isEmpty()) outError = QString("no valid PEM certificate found in %1").arg(path);
    return certs;
}

namespace {
bool parseFraudMessage(const QString &msg, QString &cardId, QString &ageGroup, QString &gateId, int &estAge)
{
    if (!msg.startsWith("FRAUD|")) {
        return false;
    }

    const QStringList parts = msg.split('|', Qt::KeepEmptyParts);
    if (parts.size() < 5) {
        qWarning() << "[FraudManager] Ignore malformed message (field missing):" << msg;
        return false;
    }

    cardId = parts[1].trimmed();
    ageGroup = parts[2].trimmed();
    gateId = parts[3].trimmed();

    bool ok = false;
    estAge = parts[4].trimmed().toInt(&ok);
    if (!ok || cardId.isEmpty() || ageGroup.isEmpty() || gateId.isEmpty()) {
        qWarning() << "[FraudManager] Ignore malformed message (invalid value):" << msg;
        return false;
    }

    if (!ageGroup.isEmpty()) {
        ageGroup[0] = ageGroup[0].toUpper();
    }
    return true;
}
}

FraudManager::FraudManager(QObject *parent) : QObject(parent)
{
    // Default to plaintext socket; may switch to QSslSocket when connectToServer is called
    socket = new QTcpSocket(this);
    retryTimer = new QTimer(this);
    retryTimer->setInterval(5000); // 5초 간격 재시도
    retryTimer->setSingleShot(true);

    connect(socket, &QTcpSocket::readyRead, this, &FraudManager::onReadyRead);
    connect(socket, &QTcpSocket::connected, this, &FraudManager::onConnected);
    connect(socket, &QTcpSocket::disconnected, this, &FraudManager::onDisconnected);
    connect(retryTimer, &QTimer::timeout, this, &FraudManager::retryConnection);
}

FraudManager::~FraudManager()
{
    socket->disconnectFromHost();
}

void FraudManager::connectToServer(const QString &host, int port)
{
    lastHost = host;
    lastPort = port;
    // Determine TLS mode from environment (SFEPS_CLIENT_TLS_ENABLE)
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const bool tlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);

    // Recreate socket if type mismatches desired mode
    bool needSsl = tlsEnabled;
    bool currentIsSsl = (qobject_cast<QSslSocket*>(socket) != nullptr);
    if (needSsl != currentIsSsl) {
        socket->disconnectFromHost();
        socket->deleteLater();
        if (needSsl) {
            socket = new QSslSocket(this);
        } else {
            socket = new QTcpSocket(this);
        }
        connect(socket, &QTcpSocket::readyRead, this, &FraudManager::onReadyRead);
        connect(socket, &QTcpSocket::connected, this, &FraudManager::onConnected);
        connect(socket, &QTcpSocket::disconnected, this, &FraudManager::onDisconnected);
    }

    if (socket->state() == QAbstractSocket::ConnectedState) return;
    qDebug() << "[FraudManager] Connecting to" << host << ":" << port << (needSsl ? "(TLS)" : "(Plain)");

    if (needSsl) {
        QSslSocket *ssl = qobject_cast<QSslSocket*>(socket);
        if (ssl) {
            ssl->abort();
            ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
            const QString caPath = env.value("SFEPS_CLIENT_CA_FILE").trimmed();
            QString caErr;
            const QList<QSslCertificate> certs = loadCaCertificates(caPath, caErr);
            if (!certs.isEmpty()) {
                QSslConfiguration cfg = ssl->sslConfiguration();
                cfg.setCaCertificates(certs);
                ssl->setSslConfiguration(cfg);
            } else if (!caPath.isEmpty()) {
                qWarning() << "[FraudManager] CA load failed:" << caErr;
            }

            const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
            if (!serverName.isEmpty()) ssl->setPeerVerifyName(serverName);

            // Capture SSL errors for logging but do not ignore them
            connect(ssl, &QSslSocket::sslErrors, this, [](const QList<QSslError> &errors){
                for (const QSslError &e : errors) qWarning() << "[FraudManager] sslError:" << e.errorString();
            });

            ssl->connectToHostEncrypted(host, static_cast<quint16>(port));
            return;
        }
    }

    socket->connectToHost(host, port);
}

void FraudManager::onConnected()
{
    qDebug() << "[FraudManager] Connected to fraud alert server.";
    retryTimer->stop();
}
void FraudManager::onDisconnected()
{
    qDebug() << "[FraudManager] Disconnected from fraud alert server. Retrying in 5s...";
    retryTimer->start();
}

void FraudManager::retryConnection()
{
    if (socket->state() == QAbstractSocket::UnconnectedState) {
        qDebug() << "[FraudManager] Retrying connection to" << lastHost << ":" << lastPort;
        socket->connectToHost(lastHost, lastPort);
    }
}

void FraudManager::onReadyRead()
{
    // 수신 데이터 누적: '\n' 기준으로 분할하여 처리하고,
    // 개행이 없는 완전한 메시지도 파싱(예: 서버가 개행을 빼먹는 경우)합니다.
    recvBuffer.append(socket->readAll());

    // 완전한 라인(\n)이 있으면 하나씩 처리
    while (true) {
        int nl = recvBuffer.indexOf('\n');
        if (nl == -1) break;
        QByteArray line = recvBuffer.left(nl).trimmed();
        recvBuffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        QString msg = QString::fromUtf8(line);
        qDebug() << "[FraudManager] Received:" << msg;

        QString cardId;
        QString ageGroup;
        QString gateId;
        int estAge = 0;
        if (parseFraudMessage(msg, cardId, ageGroup, gateId, estAge)) {
            emit fraudDetected(cardId, ageGroup, gateId, estAge);
        }
    }

    // 폴백: 개행이 없더라도 버퍼 내용이 완전한 메시지 형식이면 처리
    if (!recvBuffer.isEmpty()) {
        QString s = QString::fromUtf8(recvBuffer).trimmed();
        QString cardId;
        QString ageGroup;
        QString gateId;
        int estAge = 0;
        if (!s.isEmpty() && parseFraudMessage(s, cardId, ageGroup, gateId, estAge)) {
            qDebug() << "[FraudManager] Received (no-nl fallback):" << s;
            emit fraudDetected(cardId, ageGroup, gateId, estAge);
            recvBuffer.clear();
        }

        // 안전장치: 버퍼가 너무 커지면 초기화하여 메모리/무한루프 방지
        if (recvBuffer.size() > 16 * 1024) recvBuffer.clear();
    }
}
