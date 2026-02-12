#include "fraudmanager.h"
#include <QDebug>

FraudManager::FraudManager(QObject *parent) : QObject(parent)
{
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::readyRead, this, &FraudManager::onReadyRead);
    connect(socket, &QTcpSocket::connected, this, &FraudManager::onConnected);
    connect(socket, &QTcpSocket::disconnected, this, &FraudManager::onDisconnected);
}

FraudManager::~FraudManager()
{
    socket->disconnectFromHost();
}

void FraudManager::connectToServer(const QString &host, int port)
{
    if (socket->state() == QAbstractSocket::ConnectedState) return;
    socket->connectToHost(host, port);
}

void FraudManager::onConnected()
{
    qDebug() << "[FraudManager] Connected to fraud alert server.";
}

void FraudManager::onDisconnected()
{
    qDebug() << "[FraudManager] Disconnected from fraud alert server. Retrying in 5s...";
}

void FraudManager::onReadyRead()
{
    while (socket->canReadLine()) {
        QByteArray data = socket->readLine().trimmed();
        QString msg = QString::fromUtf8(data);
        qDebug() << "[FraudManager] Received:" << msg;

        // 형식: "FRAUD|CardID|AgeGroup|Gate|EstAge"
        if (msg.startsWith("FRAUD|")) {
            QStringList parts = msg.split("|");
            if (parts.size() >= 5) {
                QString cardId = parts[1];
                QString ageGroup = parts[2];
                int gateId = parts[3].toInt();
                int estAge = parts[4].toInt();
                emit fraudDetected(cardId, ageGroup, gateId, estAge);
            }
        }
    }
}
