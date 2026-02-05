#ifndef LOGINVIEW_H
#define LOGINVIEW_H

#include <QLineEdit>
#include <QPushButton>
#include <QTcpSocket>
#include <QWidget>

class LoginView : public QWidget {
  Q_OBJECT
public:
  explicit LoginView(QWidget *parent = nullptr);

signals:
  void loginSuccess();

private slots:
  void handleLogin();

private:
  QLineEdit *usernameInput;
  QLineEdit *passwordInput;
  QPushButton *loginButton;
  QTcpSocket *socket;
};

#endif // LOGINVIEW_H
