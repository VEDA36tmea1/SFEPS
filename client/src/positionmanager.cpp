#include "positionmanager.h"

#include <QDebug>
#include <QFile>
#include <QIODevice>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QStringList>

namespace {

bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

QList<QSslCertificate> loadCaCertificates(const QString &path, QString &outError)
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

bool parseDoubleField(const QString &token, const QString &key, double &outValue)
{
    const QString prefix = key + "=";
    if (!token.startsWith(prefix)) return false;
    bool ok = false;
    const double parsed = token.mid(prefix.size()).toDouble(&ok);
    if (!ok) return false;
    outValue = parsed;
    return true;
}

bool parseTextField(const QString &token, const QString &key, QString &outValue)
{
    const QString prefix = key + "=";
    if (!token.startsWith(prefix)) return false;
    outValue = token.mid(prefix.size()).trimmed();
    return !outValue.isEmpty();
}

bool parseObjPosMessage(const QString &msg,
                        QString &objectId,
                        double &left,
                        double &top,
                        double &right,
                        double &bottom,
                        double &x,
                        double &y,
                        bool &isFraud,
                        QString &tagTime)
{
    if (!msg.startsWith("OBJ_POS|")) return false;

    const QStringList parts = msg.split('|', Qt::KeepEmptyParts);
    if (parts.size() < 10) {
        qWarning() << "[PositionManager] Ignore malformed OBJ_POS (field missing):" << msg;
        return false;
    }

    objectId = parts[1].trimmed();
    QString fraudRaw;
    if (objectId.isEmpty() ||
        !parseDoubleField(parts[2], "L", left) ||
        !parseDoubleField(parts[3], "T", top) ||
        !parseDoubleField(parts[4], "R", right) ||
        !parseDoubleField(parts[5], "B", bottom) ||
        !parseDoubleField(parts[6], "X", x) ||
        !parseDoubleField(parts[7], "Y", y) ||
        !parseTextField(parts[8], "FRAUD", fraudRaw) ||
        !parseTextField(parts[9], "TAG", tagTime)) {
        qWarning() << "[PositionManager] Ignore malformed OBJ_POS (value invalid):" << msg;
        return false;
    }

    const QString fraudNorm = fraudRaw.trimmed().toUpper();
    if (fraudNorm == "Y") {
        isFraud = true;
    } else if (fraudNorm == "N") {
        isFraud = false;
    } else {
        qWarning() << "[PositionManager] Ignore malformed OBJ_POS (fraud invalid):" << msg;
        return false;
    }

    return true;
}

bool parseObjEndMessage(const QString &msg, QString &objectId, QString &reason)
{
    if (!msg.startsWith("OBJ_END|")) return false;

    const QStringList parts = msg.split('|', Qt::KeepEmptyParts);
    if (parts.size() < 3) {
        qWarning() << "[PositionManager] Ignore malformed OBJ_END (field missing):" << msg;
        return false;
    }

    objectId = parts[1].trimmed();
    if (objectId.isEmpty() || !parseTextField(parts[2], "REASON", reason)) {
        qWarning() << "[PositionManager] Ignore malformed OBJ_END (value invalid):" << msg;
        return false;
    }
    return true;
}

} // namespace

PositionManager::PositionManager(QObject *parent) : QObject(parent)
{
    socket = new QTcpSocket(this);
    retryTimer = new QTimer(this);
    retryTimer->setInterval(1000);
    retryTimer->setSingleShot(true);

    attachSocketSignals();
    connect(retryTimer, &QTimer::timeout, this, &PositionManager::retryConnection);
}

PositionManager::~PositionManager()
{
    disconnectFromServer();
}

void PositionManager::connectToServer(const QString &host, int port)
{
    lastHost = host;
    lastPort = port;
    m_positionTlsEnabled = resolvePositionTlsEnabled();
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    const bool needSsl = m_positionTlsEnabled;
    const bool currentIsSsl = (qobject_cast<QSslSocket*>(socket) != nullptr);
    if (needSsl != currentIsSsl) {
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->disconnectFromHost();
        }
        socket->deleteLater();
        if (needSsl) {
            socket = new QSslSocket(this);
        } else {
            socket = new QTcpSocket(this);
        }
        attachSocketSignals();
    }

    if (socket->state() == QAbstractSocket::ConnectedState) return;
    qDebug() << "[PositionManager] Connecting to" << host << ":" << port << (needSsl ? "(TLS)" : "(Plain)");

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
                qWarning() << "[PositionManager] CA load failed:" << caErr;
            }

            const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
            if (!serverName.isEmpty()) ssl->setPeerVerifyName(serverName);

            ssl->connectToHostEncrypted(host, static_cast<quint16>(port));
            return;
        }
    }

    socket->connectToHost(host, static_cast<quint16>(port));
}

void PositionManager::disconnectFromServer()
{
    retryTimer->stop();
    recvBuffer.clear();
    if (socket->state() != QAbstractSocket::UnconnectedState) {
        socket->disconnectFromHost();
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->waitForDisconnected(200);
        }
    }
}

void PositionManager::onConnected()
{
    qDebug() << "[PositionManager] Connected to position stream server.";
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    retryTimer->stop();
}

void PositionManager::onDisconnected()
{
    qDebug() << "[PositionManager] Disconnected from position stream server. Retrying in 1s...";
    retryTimer->start();
}

void PositionManager::retryConnection()
{
    if (socket->state() == QAbstractSocket::UnconnectedState) {
        qDebug() << "[PositionManager] Retrying connection to" << lastHost << ":" << lastPort;
        connectToServer(lastHost, lastPort);
    }
}

void PositionManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    qWarning() << "[PositionManager] socket error:" << socket->errorString();
}

void PositionManager::onSslErrors(const QList<QSslError> &errors)
{
    for (const QSslError &err : errors) {
        qWarning() << "[PositionManager] sslError:" << err.errorString();
    }
}

void PositionManager::attachSocketSignals()
{
    connect(socket, &QTcpSocket::readyRead, this, &PositionManager::onReadyRead);
    connect(socket, &QTcpSocket::connected, this, &PositionManager::onConnected);
    connect(socket, &QTcpSocket::disconnected, this, &PositionManager::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this, &PositionManager::onSocketError);
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, &PositionManager::onSocketError);
#endif

    if (QSslSocket *ssl = qobject_cast<QSslSocket*>(socket)) {
        connect(ssl, &QSslSocket::sslErrors, this, &PositionManager::onSslErrors);
    }
}

bool PositionManager::resolvePositionTlsEnabled() const
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const bool provided = !env.value("SFEPS_POSITION_TLS_ENABLE").trimmed().isEmpty();
    if (provided) {
        return parseEnvBool(env, "SFEPS_POSITION_TLS_ENABLE", false);
    }
    return parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
}

void PositionManager::onReadyRead()
{
    recvBuffer.append(socket->readAll());

    while (true) {
        const int nl = recvBuffer.indexOf('\n');
        if (nl == -1) break;
        const QByteArray line = recvBuffer.left(nl).trimmed();
        recvBuffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        const QString msg = QString::fromUtf8(line);
        QString objectId;
        QString reason;
        QString tagTime;
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        double x = 0.0;
        double y = 0.0;
        bool isFraud = false;

        if (parseObjPosMessage(msg, objectId, left, top, right, bottom, x, y, isFraud, tagTime)) {
            emit objectPositionReceived(objectId, left, top, right, bottom, x, y, isFraud, tagTime);
            continue;
        }
        if (parseObjEndMessage(msg, objectId, reason)) {
            emit objectEnded(objectId, reason);
            continue;
        }
    }

    if (recvBuffer.size() > 16 * 1024) {
        recvBuffer.clear();
    }
}
