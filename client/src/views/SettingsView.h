#ifndef SETTINGSVIEW_H
#define SETTINGSVIEW_H

#include <QWidget>

class SettingsView : public QWidget {
  Q_OBJECT
public:
  explicit SettingsView(QWidget *parent = nullptr);

signals:
  void closeClicked();
};

#endif // SETTINGSVIEW_H
