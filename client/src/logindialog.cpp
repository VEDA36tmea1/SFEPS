#include "logindialog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>

LoginDialog::LoginDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("METRO GUARD - Login");
    setFixedSize(300, 200);

    QVBoxLayout *layout = new QVBoxLayout(this);
    
    idInput = new QLineEdit();
    idInput->setPlaceholderText("ID");
    
    pwInput = new QLineEdit();
    pwInput->setPlaceholderText("Password");
    pwInput->setEchoMode(QLineEdit::Password); // 비밀번호 숨김 처리

    QPushButton *loginBtn = new QPushButton("Login");
    connect(loginBtn, &QPushButton::clicked, this, &LoginDialog::attemptLogin);

    layout->addWidget(new QLabel("Authorized Access Only"));
    layout->addWidget(idInput);
    layout->addWidget(pwInput);
    layout->addWidget(loginBtn);

    socket = new QTcpSocket(this);
}

void LoginDialog::attemptLogin() {
    // [1] 서버 연결 (IP와 Port를 서버 환경에 맞게 수정하세요)
    socket->connectToHost("192.168.0.89", 5555); 

    if (socket->waitForConnected(3000)) {
        // [2] 데이터 전송 (형식: "ID:PW")
        QString msg = idInput->text() + ":" + pwInput->text();
        socket->write(msg.toUtf8());
        socket->flush();

        // [3] 서버 응답 대기
        if (socket->waitForReadyRead(3000)) {
            QByteArray response = socket->readAll().trimmed();

            if (response == "PASS") {
                accept(); // 로그인 성공 (창 닫고 Accepted 반환)
            } else {
                QMessageBox::warning(this, "Login Failed", "ID/PW를 확인하세요");
            }
        }
    } else {
        QMessageBox::critical(this, "Error", "서버에 연결할 수 없습니다.");
    }
}