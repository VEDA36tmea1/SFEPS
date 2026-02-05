#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>


// Forward Declarations
class LoginView;
class MonitoringView;
class DetailView;
class SettingsView;
class AnalyticsView;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow();

public slots:
  void switchToLogin();
  void switchToMonitoring();
  void switchToDetail();
  void switchToSettings();
  void switchToAnalytics();

private:
  void setupUi();
  void updateUiState();

  // UI Components
  QWidget *centralWidget;
  QStackedWidget *centralStack;

  QFrame *sidebar;
  QFrame *header;
  QLabel *headerTitle;

  // Navigation Buttons
  QPushButton *navDashboard;
  QPushButton *navHardware;
  QPushButton *navAnalytics;
  QPushButton *navSettings;

  // Views
  LoginView *loginView;
  MonitoringView *monitoringView;
  DetailView *detailView;
  SettingsView *settingsView;
  AnalyticsView *analyticsView;
};

#endif // MAINWINDOW_H