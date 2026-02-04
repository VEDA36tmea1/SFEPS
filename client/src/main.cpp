#include "mainwindow.h"
#include "logindialog.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    LoginDialog login;
    // 로그인 창을 먼저 띄우고, 결과가 'Accepted'인 경우에만 메인 창 실행
    if (login.exec() == QDialog::Accepted) {
        MainWindow w;
        w.show();
        return a.exec();
    }

    return 0; // 로그인 취소 시 종료
}