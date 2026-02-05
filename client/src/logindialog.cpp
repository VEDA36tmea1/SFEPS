#include "logindialog.h"
#include "ui_logindialog.h"
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>


LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::LoginDialog) {
  ui->setupUi(this);

  // [1] 창 테두리 없애고 배경 투명 처리 (둥근 모서리 효과를 위해)
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);

  // [2] 그림자 효과 추가 (Card UI 입체감)
  QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(20);
  shadow->setXOffset(0);
  shadow->setYOffset(0);
  shadow->setColor(QColor(0, 0, 0, 100));
  ui->loginCard->setGraphicsEffect(shadow);

  // [3] 배경화면 스타일 적용 (Mesh Gradient 효과 흉내)
  // 원래는 HTML에서 복잡한 그라디언트였으나, Qt QSS로 유사하게 어두운 그레이톤
  // 처리 (이미 .ui 파일의 styleSheet에 적용됨)

  // [4] 시그널 연결
  connect(ui->loginBtn, &QPushButton::clicked, this,
          &LoginDialog::attemptLogin);

  // Enter 키 누르면 로그인 시도
  connect(ui->pwInput, &QLineEdit::returnPressed, this,
          &LoginDialog::attemptLogin);

  socket = new QTcpSocket(this);
}

void LoginDialog::attemptLogin() {
  // [1] 서버 연결 (IP와 Port를 서버 환경에 맞게 수정하세요)
  socket->connectToHost("192.168.0.89", 5555);

  if (socket->waitForConnected(3000)) {
    // [2] 데이터 전송 (형식: "ID:PW")
    QString msg = ui->idInput->text() + ":" + ui->pwInput->text();
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
    // 실제 서버가 없을 때 테스트용 (임시)
    // QMessageBox::critical(this, "Error", "서버에 연결할 수 없습니다.");

    // [테스트 편의를 위해 서버 실패 시에도 id가 admin이면 통과시키기]
    if (ui->idInput->text() == "admin")
      accept();
    else
      QMessageBox::critical(this, "Error", "서버 연결 실패 및 ID 불일치");
  }
}