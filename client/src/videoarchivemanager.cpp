#include "videoarchivemanager.h"
#include <QProcessEnvironment>
#include <QDebug>
#include <QRegularExpression>

namespace {
int parseTotalField(const QString &line)
{
    const QRegularExpression re("TOTAL=(\\d+)");
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) return 0;
    return m.captured(1).toInt();
}

QIODevice *activeDevice(QTcpSocket *tcp, QSslSocket *ssl, bool useTls)
{
    return useTls ? static_cast<QIODevice *>(ssl) : static_cast<QIODevice *>(tcp);
}
}

VideoArchiveManager::VideoArchiveManager(QObject *parent) : QObject(parent)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    host = env.value("VIDEO_CATALOG_HOST", "127.0.0.1");
    const bool tls = env.value("SFEPS_VIDEO_CATALOG_TLS_ENABLE", "0").toLower() == "1" || env.value("SFEPS_VIDEO_CATALOG_TLS_ENABLE", "0").toLower() == "true";
    useTls = tls;
    port = tls ? env.value("SFEPS_VIDEO_CATALOG_TLS_PORT", "6559").toInt() : env.value("SFEPS_VIDEO_CATALOG_PORT", "5559").toInt();
}

void VideoArchiveManager::connectCatalog()
{
    ensureSocket();
}

void VideoArchiveManager::ensureSocket()
{
    if (useTls) {
        if (!ssl) {
            ssl = new QSslSocket(this);
            connect(ssl, &QSslSocket::connected, this, &VideoArchiveManager::onConnected);
            connect(ssl, &QSslSocket::readyRead, this, &VideoArchiveManager::onReadyRead);
            connect(ssl, &QSslSocket::errorOccurred, this, &VideoArchiveManager::onSocketError);
        }
        if (ssl->state() == QAbstractSocket::UnconnectedState) {
            ssl->connectToHostEncrypted(host, port);
        }
    } else {
        if (!tcp) {
            tcp = new QTcpSocket(this);
            connect(tcp, &QTcpSocket::connected, this, &VideoArchiveManager::onConnected);
            connect(tcp, &QTcpSocket::readyRead, this, &VideoArchiveManager::onReadyRead);
            connect(tcp, &QTcpSocket::errorOccurred, this, &VideoArchiveManager::onSocketError);
        }
        if (tcp->state() == QAbstractSocket::UnconnectedState) {
            tcp->connectToHost(host, port);
        }
    }
}

void VideoArchiveManager::sendLine(const QString &line)
{
    ensureSocket();
    QIODevice *dev = activeDevice(tcp, ssl, useTls);
    if (!dev) {
        emit catalogError("DISCONNECTED", "video catalog socket unavailable");
        return;
    }

    const QByteArray data = (line + "\n").toUtf8();
    dev->write(data);
}

void VideoArchiveManager::requestPlayUrl(const QString &id)
{
    const QString trimmed = id.trimmed();
    bool ok = false;
    const qlonglong parsed = trimmed.toLongLong(&ok);
    if (!ok || parsed <= 0) {
        emit playError("INVALID_REQUEST", "id must be positive integer");
        return;
    }
    sendLine(QString("PLAY_REC|%1").arg(trimmed));
}

void VideoArchiveManager::onConnected()
{
    qDebug() << "VideoArchiveManager connected to" << host << port << "tls=" << useTls;
}

void VideoArchiveManager::onReadyRead()
{
    QIODevice *dev = activeDevice(tcp, ssl, useTls);
    if (!dev) return;

    const QByteArray chunk = dev->readAll();
    buffer.append(chunk);

    while (true) {
        int idx = buffer.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = buffer.left(idx).trimmed();
        buffer.remove(0, idx + 1);
        if (line.isEmpty()) continue;
        const QString s = QString::fromUtf8(line);

        if (s.startsWith("REC_SNAPSHOT_BEGIN|")) {
            snapshotInProgress = true;
            emit snapshotBegin(parseTotalField(s));
        } else if (s.startsWith("REC_SNAPSHOT_END|")) {
            snapshotInProgress = false;
            emit snapshotEnd(parseTotalField(s));
        } else if (s.startsWith("REC_ADD|")) {
            const QStringList parts = s.split('|');
            if (parts.size() >= 3) {
                emit recordingAdded(parts[1], parts[2]);
            }
        } else if (s.startsWith("REC_DEL|")) {
            const QStringList parts = s.split('|');
            if (parts.size() >= 2) {
                emit recordingDeleted(parts[1]);
            }
        } else if (s.startsWith("REC|")) {
            QStringList parts = s.split('|');
            if (parts.size() >= 3) {
                const QString id = parts[1];
                const QString createdAt = parts[2];
                if (snapshotInProgress) {
                    emit snapshotRecord(id, createdAt);
                } else {
                    emit recordingAdded(id, createdAt);
                }
            }
        } else if (s.startsWith("PLAY_URL|")) {
            const QStringList parts = s.split('|');
            if (parts.size() >= 4) {
                emit playUrlReady(parts[1], parts[2], parts[3]);
            }
        } else if (s.startsWith("REC_ERR|")) {
            QStringList parts = s.split('|');
            const QString code = parts.size() > 1 ? parts[1] : QString();
            const QString msg = parts.size() > 2 ? parts[2] : QString();
            emit catalogError(code, msg);
        } else if (s.startsWith("PLAY_ERR|")) {
            QStringList parts = s.split('|');
            const QString code = parts.size() > 1 ? parts[1] : QString();
            const QString msg = parts.size() > 2 ? parts[2] : QString();
            emit playError(code, msg);
        } else {
            qDebug() << "VideoArchiveManager: unknown line:" << s;
        }
    }
}

void VideoArchiveManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    qWarning() << "VideoArchiveManager socket error:" << (useTls && ssl ? ssl->errorString() : tcp ? tcp->errorString() : QString());
}
