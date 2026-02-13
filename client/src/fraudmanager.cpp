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
    // 서버가 newline을 보내지 않을 경우를 대비해 canReadLine() 외의 처리도 고려할 수 있으나,
    // 현재 서버 dummy generator가 "\n"을 보내므로 readLine()을 기본으로 함.
    while (socket->canReadLine()) {
        QByteArray data = socket->readLine().trimmed();
        if (data.isEmpty()) continue;

        QString msg = QString::fromUtf8(data);
        qDebug() << "[FraudManager] Received:" << msg;

        // 형식: "FRAUD|CardID|AgeGroup|Gate|EstAge"
        // 예: "FRAUD|A1B2C3D4|senior|Gate1|65"
        if (msg.startsWith("FRAUD|")) {
            QStringList parts = msg.split("|", Qt::SkipEmptyParts);
            if (parts.size() >= 5) {
                QString cardId = parts[1];
                QString ageGroup = parts[2];
                QString gateId = parts[3];
                int estAge = parts[4].toInt();
                
                // 나이 그룹 첫 글자 대문자화 (UI 미관을 위해)
                if (!ageGroup.isEmpty()) {
                    ageGroup[0] = ageGroup[0].toUpper();
                }

                emit fraudDetected(cardId, ageGroup, gateId, estAge);
            }
        }
    }
}
