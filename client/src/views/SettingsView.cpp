#include "SettingsView.h"
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SettingsView::SettingsView(QWidget *parent) : QWidget(parent) {
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setAlignment(Qt::AlignCenter);
  this->setStyleSheet("background-color: rgba(0,0,0,0.8);"); // Overlay dimming

  QFrame *card = new QFrame(this);
  card->setObjectName("card");
  card->setFixedSize(900, 600);
  card->setStyleSheet("background-color: #1e1e1e; border: 1px solid #2d2d2d; "
                      "border-radius: 16px;");

  auto *cardLayout = new QVBoxLayout(card);

  // --- Header ---
  auto *headerLayout = new QHBoxLayout();
  QLabel *icon = new QLabel("⚙️", card);
  QLabel *title = new QLabel("Hardware Device Settings", card);
  title->setStyleSheet("font-size: 20px; font-weight: bold; color: white;");

  QPushButton *closeBtn = new QPushButton("X", card);
  closeBtn->setFixedSize(30, 30);
  closeBtn->setStyleSheet(
      "background: transparent; color: #6b7280; font-weight: bold;");
  connect(closeBtn, &QPushButton::clicked, this, &SettingsView::closeClicked);

  headerLayout->addWidget(icon);
  headerLayout->addWidget(title);
  headerLayout->addStretch();
  headerLayout->addWidget(closeBtn);
  cardLayout->addLayout(headerLayout);

  // --- Content (Grid) ---
  auto *gridLayout = new QGridLayout();
  gridLayout->setSpacing(20);

  // Active Device Card
  QFrame *activeCard = new QFrame(card);
  activeCard->setStyleSheet(
      "background-color: rgba(30,30,30,0.4); border: 2px solid rgba(243, "
      "113, 32, 0.4); border-radius: 12px;");
  auto *activeLayout = new QVBoxLayout(activeCard);

  QLabel *activeTitle = new QLabel("Laser Tracking", activeCard);
  activeTitle->setStyleSheet(
      "font-size: 16px; font-weight: bold; color: white;");
  QLabel *activeSubtitle = new QLabel("System Active • Connected", activeCard);
  activeSubtitle->setStyleSheet("color: #f37120; font-size: 10px; font-weight: "
                                "bold; text-transform: uppercase;");

  QFrame *visual1 = new QFrame(activeCard);
  visual1->setFixedHeight(150);
  visual1->setStyleSheet("background-color: black; border-radius: 8px;");

  activeLayout->addWidget(activeSubtitle);
  activeLayout->addWidget(activeTitle);
  activeLayout->addWidget(visual1);
  activeLayout->addStretch();

  QPushButton *activeToggle = new QPushButton("ON", activeCard);
  activeToggle->setFixedSize(60, 30);
  activeToggle->setStyleSheet("background-color: #f37120; color: white; "
                              "border-radius: 15px; font-weight: bold;");
  activeLayout->addWidget(activeToggle, 0, Qt::AlignRight);

  // Standby Device Card
  QFrame *standbyCard = new QFrame(card);
  standbyCard->setStyleSheet("background-color: rgba(30,30,30,0.2); border: "
                             "1px solid #2d2d2d; border-radius: 12px;");
  auto *standbyLayout = new QVBoxLayout(standbyCard);

  QLabel *standbyTitle = new QLabel("LED Control", standbyCard);
  standbyTitle->setStyleSheet(
      "font-size: 16px; font-weight: bold; color: white;");
  QLabel *standbySubtitle = new QLabel("System Standby", standbyCard);
  standbySubtitle->setStyleSheet(
      "color: #6b7280; font-size: 10px; font-weight: bold; text-transform: "
      "uppercase;");

  QFrame *visual2 = new QFrame(standbyCard);
  visual2->setFixedHeight(150);
  visual2->setStyleSheet(
      "background-color: rgba(0,0,0,0.5); border-radius: 8px;");

  standbyLayout->addWidget(standbySubtitle);
  standbyLayout->addWidget(standbyTitle);
  standbyLayout->addWidget(visual2);
  standbyLayout->addStretch();

  QPushButton *standbyToggle = new QPushButton("OFF", standbyCard);
  standbyToggle->setFixedSize(60, 30);
  standbyToggle->setStyleSheet("background-color: #2d2d2d; color: #6b7280; "
                               "border-radius: 15px; font-weight: bold;");
  standbyLayout->addWidget(standbyToggle, 0, Qt::AlignRight);

  gridLayout->addWidget(activeCard, 0, 0);
  gridLayout->addWidget(standbyCard, 0, 1);

  cardLayout->addLayout(gridLayout);

  // --- Footer ---
  auto *footerLayout = new QHBoxLayout();

  auto *freqCombo = new QComboBox(card);
  freqCombo->addItem("Real-time (Low Latency)");
  freqCombo->setStyleSheet("background-color: #2d2d2d; color: white; padding: "
                           "5px; border-radius: 4px;");

  auto *logCombo = new QComboBox(card);
  logCombo->addItem("Errors Only");
  logCombo->setStyleSheet("background-color: #2d2d2d; color: white; padding: "
                          "5px; border-radius: 4px;");

  footerLayout->addWidget(new QLabel("Frequency:", card));
  footerLayout->addWidget(freqCombo);
  footerLayout->addSpacing(20);
  footerLayout->addWidget(new QLabel("Log Level:", card));
  footerLayout->addWidget(logCombo);
  footerLayout->addStretch();

  QPushButton *applyBtn = new QPushButton("APPLY CONFIGURATION", card);
  applyBtn->setStyleSheet("background-color: #ff6b2c; color: white; padding: "
                          "10px; border-radius: 4px; font-weight: bold;");
  footerLayout->addWidget(applyBtn);

  cardLayout->addLayout(footerLayout);
  mainLayout->addWidget(card);
}
