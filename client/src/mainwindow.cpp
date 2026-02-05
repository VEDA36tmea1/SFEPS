#include "mainwindow.h"
#include "views/AnalyticsView.h"
#include "views/DetailView.h"
#include "views/LoginView.h"
#include "views/MonitoringView.h"
#include "views/SettingsView.h"


#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>


MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("Hanwha Vision SFEPS");
  resize(1280, 800);

  setupUi();

  // --- Initialize Views ---
  loginView = new LoginView(this);
  monitoringView = new MonitoringView(this);
  detailView = new DetailView(this);
  settingsView = new SettingsView(this);
  analyticsView = new AnalyticsView(this);

  // Add views to stack
  centralStack->addWidget(loginView);      // 0
  centralStack->addWidget(monitoringView); // 1
  centralStack->addWidget(detailView);     // 2
  centralStack->addWidget(settingsView);   // 3
  centralStack->addWidget(analyticsView);  // 4

  // --- Connect Signals ---

  // Login -> Monitoring
  connect(loginView, &LoginView::loginSuccess, this,
          &MainWindow::switchToMonitoring);

  // Monitoring Interactions
  connect(monitoringView, &MonitoringView::viewDetailClicked, this,
          &MainWindow::switchToDetail);
  connect(monitoringView, &MonitoringView::openSettingsClicked, this,
          &MainWindow::switchToSettings);
  connect(monitoringView, &MonitoringView::goToAnalyticsClicked, this,
          &MainWindow::switchToAnalytics);
  connect(monitoringView, &MonitoringView::logoutClicked, this,
          &MainWindow::switchToLogin);

  // Sidebar Navigation
  connect(navDashboard, &QPushButton::clicked, this,
          &MainWindow::switchToMonitoring);
  connect(navAnalytics, &QPushButton::clicked, this,
          &MainWindow::switchToAnalytics);
  connect(navHardware, &QPushButton::clicked, this,
          &MainWindow::switchToDetail);
  connect(navSettings, &QPushButton::clicked, this,
          &MainWindow::switchToSettings);

  // Start at Login
  switchToLogin();
}

MainWindow::~MainWindow() {}

void MainWindow::setupUi() {
  centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);

  QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);

  // --- Sidebar ---
  sidebar = new QFrame(centralWidget);
  sidebar->setObjectName("sidebar");
  sidebar->setFixedWidth(240);

  QVBoxLayout *sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(16, 24, 16, 24);
  sidebarLayout->setSpacing(8);

  // Sidebar Header (Logo)
  QHBoxLayout *logoLayout = new QHBoxLayout();
  QLabel *logoIcon = new QLabel(sidebar);
  logoIcon->setFixedSize(32, 32);
  logoIcon->setStyleSheet("background-color: #ff6b2c; border-radius: 6px;");
  QLabel *logoText = new QLabel("Hanwha Vision", sidebar);
  logoText->setObjectName("sidebar_title");

  logoLayout->addWidget(logoIcon);
  logoLayout->addWidget(logoText);
  logoLayout->addStretch();
  sidebarLayout->addLayout(logoLayout);

  QLabel *subTitle = new QLabel("SFEPS CONTROL", sidebar);
  subTitle->setObjectName("sidebar_subtitle");
  subTitle->setContentsMargins(40, 0, 0, 20);
  sidebarLayout->addWidget(subTitle);

  // Navigation Items
  navDashboard = new QPushButton("Dashboard", sidebar);
  navDashboard->setObjectName("nav_item");
  navDashboard->setCheckable(true);

  navHardware = new QPushButton("Hardware Status", sidebar);
  navHardware->setObjectName("nav_item");
  navHardware->setCheckable(true);

  navAnalytics = new QPushButton("Evasion Analytics", sidebar);
  navAnalytics->setObjectName("nav_item");
  navAnalytics->setCheckable(true);

  navSettings = new QPushButton("System Settings", sidebar);
  navSettings->setObjectName("nav_item");
  navSettings->setCheckable(true);

  sidebarLayout->addWidget(navDashboard);
  sidebarLayout->addWidget(navHardware);
  sidebarLayout->addWidget(navAnalytics);
  sidebarLayout->addWidget(navSettings);
  sidebarLayout->addStretch();

  // --- Right Content Area ---
  QWidget *contentWidget = new QWidget(centralWidget);
  QVBoxLayout *contentLayout = new QVBoxLayout(contentWidget);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);

  // Header
  header = new QFrame(contentWidget);
  header->setObjectName("header");
  header->setFixedHeight(64);
  QHBoxLayout *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(24, 0, 24, 0);

  headerTitle = new QLabel("Hanwha Vision SFEPS", header);
  headerTitle->setObjectName("header_title");
  headerLayout->addWidget(headerTitle);
  headerLayout->addStretch();

  contentLayout->addWidget(header);

  // Central Stack
  centralStack = new QStackedWidget(contentWidget);
  contentLayout->addWidget(centralStack);

  // Add to Main Layout
  mainLayout->addWidget(sidebar);
  mainLayout->addWidget(contentWidget);
}

void MainWindow::updateUiState() {
  QWidget *current = centralStack->currentWidget();
  bool isLogin = (current == loginView);

  if (sidebar)
    sidebar->setVisible(!isLogin);
  if (header)
    header->setVisible(!isLogin);

  // Update active nav button
  if (navDashboard)
    navDashboard->setChecked(current == monitoringView);
  if (navAnalytics)
    navAnalytics->setChecked(current == analyticsView);
  if (navHardware)
    navHardware->setChecked(current == detailView);
  if (navSettings)
    navSettings->setChecked(current == settingsView);

  // Update Header Title depending on view
  if (current == monitoringView)
    headerTitle->setText("Live View Control");
  else if (current == analyticsView)
    headerTitle->setText("Evasion Analytics");
  else if (current == settingsView)
    headerTitle->setText("System Settings");
}

void MainWindow::switchToLogin() {
  centralStack->setCurrentWidget(loginView);
  updateUiState();
}

void MainWindow::switchToMonitoring() {
  centralStack->setCurrentWidget(monitoringView);
  updateUiState();
}

void MainWindow::switchToDetail() {
  centralStack->setCurrentWidget(detailView);
  updateUiState();
}

void MainWindow::switchToSettings() {
  centralStack->setCurrentWidget(settingsView);
  updateUiState();
}

void MainWindow::switchToAnalytics() {
  centralStack->setCurrentWidget(analyticsView);
  updateUiState();
}