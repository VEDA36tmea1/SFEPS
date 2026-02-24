#include "authmanager.h"
#include <QDebug>

AuthManager::AuthManager(QObject *parent) : QObject(parent)
{
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::readyRead, this, &AuthManager::onReadyRead);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QTcpSocket::errorOccurred, this, &AuthManager::onSocketError);
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, &AuthManager::onSocketError);
#endif
}

void AuthManager::login(const QString &id, const QString &pw)
{
    socket->abort();
    socket->connectToHost("192.168.0.92", 5555);

    bool authenticated = false;
    if (socket->waitForConnected(3000)) {
        socket->write(QString("%1:%2").arg(id, pw).toUtf8());
        socket->flush();
        
        if (socket->waitForReadyRead(3000)) {
            authenticated = (socket->readAll().trimmed() == "PASS");
        }
        socket->disconnectFromHost();
    }

    if (authenticated) {
        m_currentUserId = id;
        emit currentUserIdChanged();
        emit loginSuccess();
    } else {
        emit loginFailed("ID/PW를 확인하세요");
    }
}

void AuthManager::onReadyRead()
{
    // 완전한 비동기 방식이라면 여기서 데이터를 처리합니다.
    // 하지만 login() 함수 내에서 waitForReadyRead를 사용하므로,
    // 거기서 데이터가 읽히면 이 슬롯은 호출되지 않을 수 있습니다.
    // 향후 비동기 리팩토링을 위해 남겨둡니다.
}

void AuthManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    qDebug() << "Socket Error:" << socketError << socket->errorString();
}
