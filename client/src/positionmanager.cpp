#include "positionmanager.h"
#include <QDebug>
#include <QAbstractSocket>
#include <QDateTime>
#include <QFile>
#include <QProcessEnvironment>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QThread>

// Sensor/target scaling: incoming coordinates are reported in sensor (4K) pixels.
// Scale them to FullHD when numeric.
#define SENSOR_WIDTH  3840.0
#define SENSOR_HEIGHT 2160.0
#define FULLHD_WIDTH  1920.0
#define FULLHD_HEIGHT 1080.0

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

    certs = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
    if (certs.isEmpty()) {
        outError = QString("no valid PEM certificate found in %1").arg(path);
    }
    return certs;
}

}  // namespace

PositionManager::PositionManager(QObject *parent) : QObject(parent) {}

PositionManager::~PositionManager() {
    qDebug() << "[PositionManager] ~PositionManager() called. this=" << this << " currentThread=" << QThread::currentThread();
    if (posSocket) posSocket->disconnectFromHost();
}

void PositionManager::disconnectPositionServer()
{
    qDebug() << "[PositionManager] disconnectPositionServer() called. this=" << this << " currentThread=" << QThread::currentThread();
    if (m_reconnectTimer && m_reconnectTimer->isActive()) m_reconnectTimer->stop();
    if (posSocket) {
        if (posSocket->state() != QAbstractSocket::UnconnectedState) posSocket->disconnectFromHost();
        posSocket->deleteLater();
        posSocket = nullptr;
        qDebug() << "[PositionManager] Position socket disconnected by request";
    }
}

void PositionManager::attachPosSocketSignals()
{
    if (!posSocket) return;
    if (!m_batchTimer) {
        m_batchTimer = new QTimer(this);
        m_batchTimer->setInterval(400); // emit batches every 400ms (further reduce UI load)
        connect(m_batchTimer, &QTimer::timeout, this, &PositionManager::flushPending);
        m_batchTimer->start();
    }
    connect(posSocket, &QTcpSocket::readyRead, this, &PositionManager::onPosReadyRead);
    connect(posSocket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
        if (posSocket) qWarning() << "[PositionManager][POS] socket error:" << posSocket->errorString();
    });
    connect(posSocket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[PositionManager] Position socket connected to" << lastPosHost << ":" << lastPosPort;
        resetReconnectBackoff();
        this->flushQueuedCommands();
        emit positionConnected();
    });
    connect(posSocket, &QTcpSocket::disconnected, this, [this]() {
        qDebug() << "[PositionManager] Position socket disconnected.";
        emit positionDisconnected();
        scheduleReconnect();
    });
    if (QSslSocket *ssl = qobject_cast<QSslSocket *>(posSocket)) {
        connect(ssl, &QSslSocket::sslErrors, this, [](const QList<QSslError> &errors) {
            for (const QSslError &err : errors) {
                qWarning() << "[PositionManager][POS] sslError:" << err.errorString();
            }
        });
    }
}

void PositionManager::onPosReadyRead()
{
    if (!posSocket) return;
    posRecvBuffer.append(posSocket->readAll());

    // Backpressure guard: keep only recent data if producer outruns consumer.
    // Prefer dropping oldest complete lines over blocking the UI thread.
    static const int kMaxRecvBufferBytes = 512 * 1024;
    if (posRecvBuffer.size() > kMaxRecvBufferBytes) {
        const int targetSize = kMaxRecvBufferBytes / 2;
        int dropBytes = posRecvBuffer.size() - targetSize;
        int nl = posRecvBuffer.indexOf('\n', dropBytes);
        if (nl != -1) {
            posRecvBuffer.remove(0, nl + 1);
        } else {
            // If no newline boundary is found, hard-trim to recover quickly.
            posRecvBuffer = posRecvBuffer.right(targetSize);
        }
        qWarning() << "[PositionManager][POS] recv buffer overflow; trimmed backlog, new size=" << posRecvBuffer.size();
    }

    // Schedule parsing on the event loop to keep readyRead lightweight.
    if (!m_parseScheduled) {
        m_parseScheduled = true;
        QTimer::singleShot(0, this, [this]() {
            m_parseScheduled = false;
            processPosBuffer();
        });
    }
}

void PositionManager::processPosBuffer()
{
    static const int kMaxLinesPerPass = 120;
    int processed = 0;

    while (processed < kMaxLinesPerPass) {
        int nl = posRecvBuffer.indexOf('\n');
        if (nl == -1) break;

        QByteArray line = posRecvBuffer.left(nl).trimmed();
        posRecvBuffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        ++processed;
        QString s = QString::fromUtf8(line);

        if (s.startsWith("OUTLINE_POS|") || s.startsWith("BCAST_OBJ|") || s.startsWith("OBJ_POS|") || s.startsWith("POS|")) {
            const QStringList parts = s.split('|', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                QVariantMap map;
                map["id"] = parts[1].trimmed();
                for (int i = 2; i < parts.size(); ++i) {
                    const QString p = parts[i].trimmed();
                    int eq = p.indexOf('=');
                    if (eq != -1) {
                        QString k = p.left(eq).trimmed();
                        QString v = p.mid(eq+1).trimmed();
                        bool ok = false;
                        double d = v.toDouble(&ok);
                        if (ok) {
                            // If numeric, scale coordinates reported in sensor pixels to FullHD pixels
                            QString ku = k.toUpper();
                            double scaled = d;
                            if (ku == "L" || ku == "R" || ku == "X") {
                                scaled = d * (FULLHD_WIDTH / SENSOR_WIDTH);
                            } else if (ku == "T" || ku == "B" || ku == "Y") {
                                scaled = d * (FULLHD_HEIGHT / SENSOR_HEIGHT);
                            }
                            map[k] = scaled;
                        } else {
                            map[k] = v;
                        }
                    } else {
                        map[QString("field%1").arg(i)] = p;
                    }
                }
                // enqueue/update parsed map by id: keep latest per id
                QString parsedId = map.value("id").toString();
                if (!parsedId.isEmpty()) {
                    // Track FRAUD flag: if present and 'Y', mark suspected; if 'N' clear
                    if (map.contains("FRAUD")) {
                        QVariant v = map.value("FRAUD");
                        QString sv = v.toString().trimmed().toUpper();
                        if (sv == "Y" || sv == "1" || sv == "TRUE") {
                            m_suspected.insert(parsedId);
                        } else {
                            m_suspected.remove(parsedId);
                        }
                    }
                    bool existed = m_pendingMap.contains(parsedId);
                    m_pendingMap.insert(parsedId, map);
                    qint64 now = QDateTime::currentMSecsSinceEpoch();
                    m_lastSeen.insert(parsedId, now);
                    if (!existed) m_pendingOrder.append(parsedId);
                    // trim oldest unique items if over capacity
                    if (m_pendingOrder.size() > m_maxPending) {
                        int drop = m_pendingOrder.size() - m_maxPending;
                        for (int di = 0; di < drop; ++di) {
                            QString old = m_pendingOrder.takeFirst();
                            m_pendingMap.remove(old);
                        }
                        qDebug() << "[PositionManager] dropped" << drop << "old unique items to enforce maxPending=" << m_maxPending;
                    }
                } else {
                    // fallback: if no id present, append to orderless buffer (rare)
                    QVariantMap tmp = map;
                    QString gen = QString::number(QDateTime::currentMSecsSinceEpoch());
                    tmp["_gen"] = gen;
                    m_pendingMap.insert(gen, tmp);
                    m_lastSeen.insert(gen, QDateTime::currentMSecsSinceEpoch());
                    m_pendingOrder.append(gen);
                }
            }
        } else if (s.startsWith("OUTLINE_POS_END|")) {
            const QStringList parts = s.split('|', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                QString id = parts[1].trimmed();
                // remove from active map/list if present
                if (m_pendingMap.contains(id)) {
                    m_pendingMap.remove(id);
                    m_lastSeen.remove(id);
                    m_pendingOrder.removeAll(id);
                }
                // clear suspected state when outline ends
                if (m_suspected.contains(id)) {
                    m_suspected.remove(id);
                }
            }
        } else if (s.startsWith("OBJ_END|")) {
            const QStringList parts = s.split('|', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                QString id = parts[1].trimmed();
                if (m_pendingMap.contains(id)) {
                    m_pendingMap.remove(id);
                    m_lastSeen.remove(id);
                    m_pendingOrder.removeAll(id);
                }
                // clear suspected state when object ends
                if (m_suspected.contains(id)) {
                    m_suspected.remove(id);
                }
            }
        } else {
            static int unknownLineCount = 0;
            ++unknownLineCount;
            if (unknownLineCount <= 10 || (unknownLineCount % 200) == 0) {
                qDebug() << "[PositionManager][POS] unknown line(" << unknownLineCount << "):" << s;
            }
        }
    }

    // If backlog remains, continue in a short deferred slice.
    if (posRecvBuffer.indexOf('\n') != -1 && !m_parseScheduled) {
        m_parseScheduled = true;
        QTimer::singleShot(5, this, [this]() {
            m_parseScheduled = false;
            processPosBuffer();
        });
    }
}

void PositionManager::connectPositionServer(const QString &host, int port)
{
    lastPosHost = host;
    lastPosPort = port;
    qDebug() << "[PositionManager] Connecting position server" << host << ":" << port;
    if (posSocket) {
        if (posSocket->state() != QAbstractSocket::UnconnectedState) posSocket->disconnectFromHost();
        posSocket->deleteLater();
    }
    if (resolveTlsEnabled()) {
        posSocket = new QSslSocket(this);
    } else {
        posSocket = new QTcpSocket(this);
    }
    attachPosSocketSignals();
    connectCurrentSocket();
}

void PositionManager::scheduleReconnect()
{
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            if (!posSocket) return;
            if (posSocket->state() == QAbstractSocket::UnconnectedState) {
                qDebug() << "[PositionManager] Reconnect attempt (delay_ms=" << m_reconnectDelayMs << ") to" << lastPosHost << lastPosPort;
                connectCurrentSocket();
            }
            // increase delay for next time (exponential backoff)
            m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, m_reconnectMaxMs);
        });
    }

    // Ensure minimum
    if (m_reconnectDelayMs < m_reconnectMinMs) m_reconnectDelayMs = m_reconnectMinMs;
    m_reconnectTimer->start(m_reconnectDelayMs);
}

void PositionManager::resetReconnectBackoff()
{
    m_reconnectDelayMs = m_reconnectMinMs;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) m_reconnectTimer->stop();
}

void PositionManager::flushQueuedCommands()
{
    if (!posSocket) return;
    if (posSocket->state() != QAbstractSocket::ConnectedState) return;
    if (m_pendingCommands.isEmpty()) return;

    const QStringList queued = m_pendingCommands;
    m_pendingCommands.clear();
    for (const QString &cmd : queued) {
        QByteArray data = cmd.toUtf8();
        if (!data.endsWith('\n')) data.append('\n');
        qint64 n = posSocket->write(data);
        if (n <= 0) {
            qWarning() << "[PositionManager] failed to flush queued pos command:" << cmd;
        } else {
            qDebug() << "[PositionManager] Flushed queued pos command:" << cmd;
        }
    }
}

bool PositionManager::resolveTlsEnabled() const
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const bool clientTlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
    return parseEnvBool(env, "SFEPS_POS_TLS_ENABLE", clientTlsEnabled);
}

void PositionManager::connectCurrentSocket()
{
    if (!posSocket) return;
    if (posSocket->state() != QAbstractSocket::UnconnectedState) return;

    if (QSslSocket *ssl = qobject_cast<QSslSocket *>(posSocket)) {
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        ssl->abort();
        ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);

        const QString caPath = env.value("SFEPS_CLIENT_CA_FILE").trimmed();
        QString caErr;
        const QList<QSslCertificate> certs = loadCaCertificates(caPath, caErr);
        if (!certs.isEmpty()) {
            QSslConfiguration cfg = ssl->sslConfiguration();
            cfg.setProtocol(QSsl::TlsV1_2OrLater);
            cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
            cfg.setCaCertificates(certs);
            ssl->setSslConfiguration(cfg);
        } else if (!caPath.isEmpty()) {
            qWarning() << "[PositionManager][POS] CA load failed:" << caErr;
        }

        const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
        if (!serverName.isEmpty()) {
            ssl->setPeerVerifyName(serverName);
        }

        qDebug() << "[PositionManager] Connecting position server (TLS)" << lastPosHost << ":" << lastPosPort;
        ssl->connectToHostEncrypted(lastPosHost, static_cast<quint16>(lastPosPort));
        return;
    }

    qDebug() << "[PositionManager] Connecting position server (Plain)" << lastPosHost << ":" << lastPosPort;
    posSocket->connectToHost(lastPosHost, static_cast<quint16>(lastPosPort));
}

void PositionManager::sendPositionCommand(const QString &msg)
{
    if (!posSocket) {
        qWarning() << "[PositionManager] sendPositionCommand: posSocket is null, queueing:" << msg;
        m_pendingCommands.append(msg);
        return;
    }
    if (posSocket->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "[PositionManager] sendPositionCommand: posSocket not connected, queueing:" << msg << "state=" << posSocket->state();
        m_pendingCommands.append(msg);
        return;
    }

    QByteArray data = msg.toUtf8();
    if (!data.endsWith('\n')) data.append('\n');
    qint64 n = posSocket->write(data);
    if (n <= 0) {
        qWarning() << "[PositionManager] failed to write pos command:" << msg;
    } else {
        // Avoid synchronous flush to prevent blocking the UI thread
        // Update client-side current subscription when SUB_POS/UNSUB_POS used
        if (msg.startsWith("SUB_POS|")) {
            QString id = msg.mid(QString("SUB_POS|").length()).trimmed();
            if (!id.isEmpty()) {
                // Reset cached entries so a new subscription starts with fresh data only.
                m_pendingMap.clear();
                m_pendingOrder.clear();
                m_lastSeen.clear();
                m_suspected.clear();
                setCurrentSubscribedId(id);
            }
        } else if (msg.startsWith("UNSUB_POS|")) {
            QString id = msg.mid(QString("UNSUB_POS|").length()).trimmed();
            if (!id.isEmpty() && id == m_currentSubscribedId) {
                setCurrentSubscribedId(QString());
                m_pendingMap.clear();
                m_pendingOrder.clear();
                m_lastSeen.clear();
                m_suspected.clear();
                emit positionsUpdated(QVariantList());
            }
        }
        qDebug() << "[PositionManager] Sent pos command:" << msg;
    }
}

void PositionManager::unsubscribeCurrent()
{
    if (m_currentSubscribedId.isEmpty()) {
        qDebug() << "[PositionManager] unsubscribeCurrent: no current subscription";
        return;
    }
    QString cmd = QString("UNSUB_POS|%1").arg(m_currentSubscribedId);
    sendPositionCommand(cmd);
}

QString PositionManager::currentSubscribedId() const
{
    return m_currentSubscribedId;
}

void PositionManager::setCurrentSubscribedId(const QString &id)
{
    if (m_currentSubscribedId == id) return;
    m_currentSubscribedId = id;
    if (!id.isEmpty()) qDebug() << "[PositionManager] currentSubscribedId set to" << id;
    else qDebug() << "[PositionManager] currentSubscribedId cleared";
    emit currentSubscribedIdChanged();
}

void PositionManager::flushPending()
{
    if (m_pendingMap.isEmpty()) return;
    QVariantList out;
    out.reserve(m_pendingMap.size());
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Build output from active entries, prune expired ones
    QList<QString> toRemove;
    for (const QString &k : m_pendingOrder) {
        if (!m_pendingMap.contains(k)) continue;
        qint64 last = m_lastSeen.value(k, 0);
        if (now - last > m_ttlMs) {
            toRemove.append(k);
            continue;
        }
        QVariantMap m = m_pendingMap.value(k);
        // annotate with alert if this id is currently suspected
        if (m_suspected.contains(k)) {
            m.insert("alert", true);
        }
        out.append(QVariant::fromValue(m));
    }
    // Remove expired entries
    for (const QString &k : toRemove) {
        m_pendingMap.remove(k);
        m_lastSeen.remove(k);
        m_pendingOrder.removeAll(k);
        if (m_suspected.contains(k)) {
            m_suspected.remove(k);
            qDebug() << "[PositionManager][POS] cleared suspected state due to TTL expiry for" << k;
        }
    }
    // Emit current active set
    emit positionsUpdated(out);
}
