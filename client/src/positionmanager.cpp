#include "positionmanager.h"
#include <QDebug>
#include <QAbstractSocket>
#include <QDateTime>

// Sensor/target scaling: incoming coordinates are reported in sensor (4K) pixels.
// Scale them to FullHD when numeric.
#define SENSOR_WIDTH  3840.0
#define SENSOR_HEIGHT 2160.0
#define FULLHD_WIDTH  1920.0
#define FULLHD_HEIGHT 1080.0

PositionManager::PositionManager(QObject *parent) : QObject(parent) {}

PositionManager::~PositionManager() {
    if (posSocket) posSocket->disconnectFromHost();
}

void PositionManager::disconnectPositionServer()
{
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
    connect(posSocket, &QTcpSocket::readyRead, this, [this]() {
        posRecvBuffer.append(posSocket->readAll());
        while (true) {
            int nl = posRecvBuffer.indexOf('\n');
            if (nl == -1) break;
            QByteArray line = posRecvBuffer.left(nl).trimmed();
            posRecvBuffer.remove(0, nl + 1);
            if (line.isEmpty()) continue;
            QString s = QString::fromUtf8(line);
            qDebug() << "[PositionManager][POS] Received:" << s;
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
                        // debug: log pending unique count occasionally
                        if ((m_pendingMap.size() % 50) == 0) {
                            qDebug() << "[PositionManager] pending unique count:" << m_pendingMap.size();
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
                    qDebug() << "[PositionManager][POS] End for" << id;
                    // remove from active map/list if present
                    if (m_pendingMap.contains(id)) {
                        m_pendingMap.remove(id);
                        m_lastSeen.remove(id);
                        m_pendingOrder.removeAll(id);
                        qDebug() << "[PositionManager][POS] removed id on OUTLINE_POS_END:" << id;
                    }
                    // clear suspected state when outline ends
                    if (m_suspected.contains(id)) {
                        m_suspected.remove(id);
                        qDebug() << "[PositionManager][POS] cleared suspected state on OUTLINE_POS_END for" << id;
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
                        qDebug() << "[PositionManager][POS] removed id on OBJ_END:" << id;
                    }
                    // clear suspected state when object ends
                    if (m_suspected.contains(id)) {
                        m_suspected.remove(id);
                        qDebug() << "[PositionManager][POS] cleared suspected state on OBJ_END for" << id;
                    }
                }
            } else {
                qDebug() << "[PositionManager][POS] unknown line:" << s;
            }
            
        }
    });
    connect(posSocket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
        if (posSocket) qWarning() << "[PositionManager][POS] socket error:" << posSocket->errorString();
    });
    connect(posSocket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[PositionManager] Position socket connected to" << lastPosHost << ":" << lastPosPort;
    });
    connect(posSocket, &QTcpSocket::disconnected, this, [this]() {
        qDebug() << "[PositionManager] Position socket disconnected.";
        scheduleReconnect();
    });
    connect(posSocket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[PositionManager] Position socket connected to" << lastPosHost << ":" << lastPosPort;
        resetReconnectBackoff();
    });
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
    posSocket = new QTcpSocket(this);
    attachPosSocketSignals();
    // immediate connect attempt
    if (posSocket->state() == QAbstractSocket::UnconnectedState) {
        posSocket->connectToHost(host, static_cast<quint16>(port));
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
                qDebug() << "[PositionManager] Reconnect attempt (delay_ms=" << m_reconnectDelayMs << ") to" << lastPosHost << lastPosPort;
                posSocket->connectToHost(lastPosHost, static_cast<quint16>(lastPosPort));
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

void PositionManager::sendPositionCommand(const QString &msg)
{
    if (!posSocket) {
        qWarning() << "[PositionManager] sendPositionCommand: posSocket is null";
        return;
    }
    if (posSocket->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "[PositionManager] sendPositionCommand: posSocket not connected:" << posSocket->state();
        return;
    }
    QByteArray data = msg.toUtf8();
    if (!data.endsWith('\n')) data.append('\n');
    qint64 n = posSocket->write(data);
    if (n <= 0) {
        qWarning() << "[PositionManager] failed to write pos command:" << msg;
    } else {
        posSocket->flush();
        qDebug() << "[PositionManager] Sent pos command:" << msg;
    }
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
