#include "fraudmanager.h"
#include <QDebug>

FraudManager::FraudManager(QObject *parent) : QObject(parent)
{
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
    if (socket->state() == QAbstractSocket::ConnectedState) return;
    qDebug() << "[FraudManager] Connecting to" << host << ":" << port;
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

        if (msg.startsWith("FRAUD|")) {
            QStringList parts = msg.split("|", Qt::SkipEmptyParts);
            if (parts.size() >= 5) {
                QString cardId = parts[1];
                QString ageGroup = parts[2];
                QString gateId = parts[3];
                int estAge = parts[4].toInt();
                if (!ageGroup.isEmpty()) ageGroup[0] = ageGroup[0].toUpper();
                emit fraudDetected(cardId, ageGroup, gateId, estAge);
            }
        }
    }

    // 폴백: 개행이 없더라도 버퍼 내용이 완전한 메시지 형식이면 처리
    if (!recvBuffer.isEmpty()) {
        QString s = QString::fromUtf8(recvBuffer).trimmed();
        if (!s.isEmpty() && s.startsWith("FRAUD|")) {
            QStringList parts = s.split("|", Qt::SkipEmptyParts);
            if (parts.size() >= 5) {
                QString cardId = parts[1];
                QString ageGroup = parts[2];
                QString gateId = parts[3];
                int estAge = parts[4].toInt();
                if (!ageGroup.isEmpty()) ageGroup[0] = ageGroup[0].toUpper();
                qDebug() << "[FraudManager] Received (no-nl fallback):" << s;
                emit fraudDetected(cardId, ageGroup, gateId, estAge);
                recvBuffer.clear();
            }
        }

        // 안전장치: 버퍼가 너무 커지면 초기화하여 메모리/무한루프 방지
        if (recvBuffer.size() > 16 * 1024) recvBuffer.clear();
    }
}
