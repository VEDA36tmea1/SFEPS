#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QTcpSocket>

QT_BEGIN_NAMESPACE
namespace Ui {
class LoginDialog;
}
QT_END_NAMESPACE

class LoginDialog : public QDialog {
  Q_OBJECT

public:
  LoginDialog(QWidget *parent = nullptr);

private slots:
  void attemptLogin(); // 로그인 버튼 클릭 시 실행

private:
  Ui::LoginDialog *ui;
  QTcpSocket *socket;
};

#endif