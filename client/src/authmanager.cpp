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
    if (socket->state() == QAbstractSocket::ConnectedState) {
        socket->disconnectFromHost();
    }

    // 기존 LoginDialog의 IP와 Port 설정
    socket->connectToHost("192.168.0.89", 5555);

    if (socket->waitForConnected(3000)) {
        QString msg = id + ":" + pw;
        socket->write(msg.toUtf8());
        socket->flush();
        // 응답 처리는 onReadyRead에서 하거나 여기서 블로킹 대기를 할 수 있습니다.
        // QML의 비동기성을 위해서는 시그널 방식이 좋지만, 기존 로직이 트랜잭션 방식이었으므로
        // 편의상 여기서 짧게 대기합니다. (기존 코드: 3초 대기)
        
        if (socket->waitForReadyRead(3000)) {
            QByteArray response = socket->readAll().trimmed();
            if (response == "PASS") {
                emit loginSuccess();
            } else {
                emit loginFailed("ID/PW를 확인하세요");
            }
            socket->disconnectFromHost();
        } else {
             // 타임아웃 또는 데이터 없음
             // 기존 코드의 관리자 우회(bypass) 로직 유지
             if (id == "admin") {
                 emit loginSuccess();
             } else {
                 emit loginFailed("서버 응답 없음 (Timeout)");
             }
        }
    } else {
        // 연결 실패
        if (id == "admin") {
            emit loginSuccess();
        } else {
            emit loginFailed("서버 연결 실패");
        }
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
