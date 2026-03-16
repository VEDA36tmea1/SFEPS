#include "positionmanager.h"
#include <QDebug>
#include <QAbstractSocket>

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

void PositionManager::attachPosSocketSignals()
{
    if (!posSocket) return;
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
            if (s.startsWith("OUTLINE_POS|")) {
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
                    QVariantList list;
                    // Debug: print parsed/scaled coordinate fields for visibility
                    QString parsedId = map.value("id").toString();
                    QVariant Lv = map.contains("L") ? map.value("L") : map.value("l");
                    QVariant Tv = map.contains("T") ? map.value("T") : map.value("t");
                    QVariant Rv = map.contains("R") ? map.value("R") : map.value("r");
                    QVariant Bv = map.contains("B") ? map.value("B") : map.value("b");
                    QVariant Xv = map.contains("X") ? map.value("X") : map.value("x");
                    QVariant Yv = map.contains("Y") ? map.value("Y") : map.value("y");
                    QVariant TAGv = map.contains("TAG") ? map.value("TAG") : map.value("tag");
                    qDebug() << "[PositionManager] Parsed POS -> id:" << parsedId
                             << "L=" << Lv << "T=" << Tv << "R=" << Rv << "B=" << Bv
                             << "X=" << Xv << "Y=" << Yv << "TAG=" << TAGv;
                    list.append(map);
                    emit positionsUpdated(list);
                }
            } else if (s.startsWith("OUTLINE_POS_END|")) {
                const QStringList parts = s.split('|', Qt::SkipEmptyParts);
                if (parts.size() >= 2) {
                    QString id = parts[1].trimmed();
                    qDebug() << "[PositionManager][POS] End for" << id;
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
        qDebug() << "[PositionManager] Position socket disconnected. Retrying in 1s...";
        QTimer::singleShot(1000, this, [this]() {
            if (posSocket && posSocket->state() == QAbstractSocket::UnconnectedState) {
                posSocket->connectToHost(lastPosHost, static_cast<quint16>(lastPosPort));
            }
        });
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
    posSocket->connectToHost(host, static_cast<quint16>(port));
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