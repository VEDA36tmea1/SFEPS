#include "mainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QTextStream>


void loadStyleSheet(QApplication &app) {
  qDebug() << "Loading stylesheet from resources...";
  QFile file(":/styles.qss");
  if (file.open(QFile::ReadOnly | QFile::Text)) {
    QTextStream stream(&file);
    app.setStyleSheet(stream.readAll());
    file.close();
    qDebug() << "Stylesheet applied.";
  } else {
    qWarning() << "Resource :/styles.qss not found!";
  }
}

int main(int argc, char *argv[]) {
  qDebug() << "Application starting...";
  QApplication a(argc, argv);

  a.setApplicationName("Hanwha Vision SFEPS");
  loadStyleSheet(a);

  qDebug() << "Initializing MainWindow...";
  MainWindow w;
  qDebug() << "Displaying MainWindow...";
  w.show();
  return a.exec();
}