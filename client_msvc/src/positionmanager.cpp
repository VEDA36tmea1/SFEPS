#include "positionmanager.h"
#include <QDebug>
#include <QAbstractSocket>
#include <QDateTime>
#include <QThread>
#include <QProcessEnvironment>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QFile>

static bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

static quint16 parseEnvPort(const QProcessEnvironment &env, const QString &key, quint16 defaultValue)
{
    const QString raw = env.value(key).trimmed();
    if (raw.isEmpty()) return defaultValue;
    bool ok = false;
    const int parsed = raw.toInt(&ok);
    if (!ok || parsed < 1 || parsed > 65535) return defaultValue;
    return static_cast<quint16>(parsed);
}

// Sensor/target scaling: incoming coordinates are reported in sensor (4K) pixels.
// Scale them to FullHD when numeric.
#define SENSOR_WIDTH  3840.0
#define SENSOR_HEIGHT 2160.0
#define FULLHD_WIDTH  1920.0
#define FULLHD_HEIGHT 1080.0

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
        if (!posSocket) return;
        qWarning() << "[PositionManager][POS] socket error:" << posSocket->errorString();

        // TLS->Plain fallback: if TLS socket fails before encryption is established, switch to plaintext.
        if (m_posTlsPrefer && !m_tlsFallbackUsed && !m_fallbackInProgress) {
            if (QSslSocket *ssl = qobject_cast<QSslSocket *>(posSocket)) {
                if (!ssl->isEncrypted()) {
                    qWarning() << "[PositionManager][POS] TLS failed -> fallback to plain" << lastPosHost << ":" << m_posPlainPort;
                    m_tlsFallbackUsed = true;
                    m_fallbackInProgress = true;
                    if (m_reconnectTimer && m_reconnectTimer->isActive()) m_reconnectTimer->stop();

                    // Recreate plaintext socket.
                    if (posSocket) {
                        posSocket->abort();
                        posSocket->deleteLater();
                    }
                    posSocket = new QTcpSocket(this);
                    attachPosSocketSignals();
                    posSocket->connectToHost(lastPosHost, m_posPlainPort);
                }
            }
        }
    });

    // When using TLS, wait for QSslSocket::encrypted() before notifying the app.
    if (QSslSocket *ssl = qobject_cast<QSslSocket *>(posSocket)) {
        connect(ssl, &QSslSocket::encrypted, this, [this]() {
            qDebug() << "[PositionManager] Position TLS encrypted to" << lastPosHost << ":" << lastPosPort;
            resetReconnectBackoff();
            this->flushQueuedCommands();
            if (m_fallbackInProgress) m_fallbackInProgress = false;
            emit positionConnected();
        });
    }

    connect(posSocket, &QTcpSocket::connected, this, [this]() {
        // QSslSocket의 connected()는 TLS handshake 전에 날 수 있으므로, TLS에서는 encrypted() 쪽에서만 positionConnected를 방출합니다.
        if (qobject_cast<QSslSocket *>(posSocket)) return;
        qDebug() << "[PositionManager] Position socket connected to" << lastPosHost << ":" << lastPosPort;
        resetReconnectBackoff();
        this->flushQueuedCommands();
        if (m_fallbackInProgress) m_fallbackInProgress = false;
        emit positionConnected();
    });

    connect(posSocket, &QTcpSocket::disconnected, this, [this]() {
        qDebug() << "[PositionManager] Position socket disconnected.";
        if (m_fallbackInProgress) {
            m_fallbackInProgress = false;
            return; // suppress server-down trigger while we are switching transports
        }
        emit positionDisconnected();
        scheduleReconnect();
    });
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
        } else if (s.startsWith("PWM_OUT,")) {
            // camera_RBF.cpp --qt-mode 에서 역방향으로 전송하는 PWM 값
            // 형식: PWM_OUT,PAN=1500,TILT=1600
            int pan = -1, tilt = -1;
            const QStringList parts = s.mid(8).split(',', Qt::SkipEmptyParts);
            for (const QString &p : parts) {
                if (p.startsWith("PAN=")) pan = p.mid(4).toInt();
                else if (p.startsWith("TILT=")) tilt = p.mid(5).toInt();
            }
            if (pan >= 0 && tilt >= 0) {
                emit pwmReceived(pan, tilt);
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
    Q_UNUSED(port);

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    m_posTlsPrefer = parseEnvBool(env, "SFEPS_POS_TLS_ENABLE", false);
    m_posTlsPort = parseEnvPort(env, "SFEPS_POS_TLS_PORT", 6558);
    m_posPlainPort = parseEnvPort(env, "POS_SERVER_PORT", 5558);

    m_tlsFallbackUsed = false;
    m_fallbackInProgress = false;

    lastPosPort = m_posTlsPrefer ? static_cast<int>(m_posTlsPort) : static_cast<int>(m_posPlainPort);

    qDebug() << "[PositionManager] Connecting position server" << host << ":" << lastPosPort
             << (m_posTlsPrefer ? "(prefer TLS)" : "(plain)");

    if (posSocket) {
        if (posSocket->state() != QAbstractSocket::UnconnectedState) posSocket->disconnectFromHost();
        posSocket->deleteLater();
    }

    // immediate connect attempt
    if (m_posTlsPrefer) {
        QSslSocket *ssl = new QSslSocket(this);
        posSocket = ssl;
        ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
        const QString caPath = env.value("SFEPS_CLIENT_CA_FILE").trimmed();
        if (!caPath.isEmpty()) {
            QFile f(caPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QList<QSslCertificate> certs = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
                if (!certs.isEmpty()) {
                    QSslConfiguration cfg = ssl->sslConfiguration();
                    cfg.setCaCertificates(certs);
                    ssl->setSslConfiguration(cfg);
                }
            }
        }
        const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
        if (!serverName.isEmpty()) ssl->setPeerVerifyName(serverName);

        attachPosSocketSignals();
        ssl->connectToHostEncrypted(host, m_posTlsPort);
    } else {
        posSocket = new QTcpSocket(this);
        attachPosSocketSignals();
        posSocket->connectToHost(host, m_posPlainPort);
    }
}

void PositionManager::scheduleReconnect()
{
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            if (!posSocket) return;
            if (posSocket->state() == QAbstractSocket::UnconnectedState) {
                qDebug() << "[PositionManager] Reconnect attempt (delay_ms=" << m_reconnectDelayMs << ") to"
                         << lastPosHost << lastPosPort;
                // Recreate socket with current TLS preference.
                connectPositionServer(lastPosHost, lastPosPort);
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

    // TRACK_START|id 시 m_pendingMap에서 bbox를 자동으로 포함하여 전송
    // → camera_RBF.cpp에서 ID 매칭 실패 시 IoU 매칭 보완
    QString actualMsg = msg;
    if (msg.startsWith("TRACK_START|") && !msg.contains("|L=")) {
        const QString id = msg.mid(QString("TRACK_START|").length()).trimmed();
        if (!id.isEmpty() && m_pendingMap.contains(id)) {
            const QVariantMap &m = m_pendingMap.value(id);
            bool hasL = m.contains("L"), hasT = m.contains("T"),
                 hasR = m.contains("R"), hasB = m.contains("B");
            if (hasL && hasT && hasR && hasB) {
                int l = qRound(m.value("L").toDouble());
                int t = qRound(m.value("T").toDouble());
                int r = qRound(m.value("R").toDouble());
                int b = qRound(m.value("B").toDouble());
                actualMsg = QString("TRACK_START|%1|L=%2|T=%3|R=%4|B=%5")
                                .arg(id).arg(l).arg(t).arg(r).arg(b);
                qDebug() << "[PositionManager] Enriched TRACK_START with bbox:" << actualMsg;
            }
        }
    }

    QByteArray data = actualMsg.toUtf8();
    if (!data.endsWith('\n')) data.append('\n');
    qint64 n = posSocket->write(data);
    if (n <= 0) {
        qWarning() << "[PositionManager] failed to write pos command:" << actualMsg;
    } else {
        // Update client-side current subscription state
        if (msg.startsWith("TRACK_START|") || msg.startsWith("SUB_POS|")) {
            const QString prefix = msg.startsWith("TRACK_START|") ? "TRACK_START|" : "SUB_POS|";
            QString id = msg.mid(prefix.length()).split('|').first().trimmed();
            if (!id.isEmpty()) {
                m_pendingMap.clear();
                m_pendingOrder.clear();
                m_lastSeen.clear();
                m_suspected.clear();
                setCurrentSubscribedId(id);
            }
        } else if (msg.startsWith("TRACK_END|") || msg.startsWith("UNSUB_POS|")) {
            const QString prefix = msg.startsWith("TRACK_END|") ? "TRACK_END|" : "UNSUB_POS|";
            const QString id = msg.mid(prefix.length()).trimmed();
            if (!id.isEmpty() && id == m_currentSubscribedId) {
                setCurrentSubscribedId(QString());
                m_pendingMap.clear();
                m_pendingOrder.clear();
                m_lastSeen.clear();
                m_suspected.clear();
                emit positionsUpdated(QVariantList());
            }
        }
        qDebug() << "[PositionManager] Sent pos command:" << actualMsg;
    }
}

void PositionManager::unsubscribeCurrent()
{
    if (m_currentSubscribedId.isEmpty()) {
        qDebug() << "[PositionManager] unsubscribeCurrent: no current subscription";
        return;
    }
    QString cmd = QString("TRACK_END|%1").arg(m_currentSubscribedId);
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
