#include "DetailView.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

DetailView::DetailView(QWidget *parent) : QWidget(parent) {
  auto *mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(40, 40, 40, 40);
  mainLayout->setSpacing(0);

  this->setStyleSheet("background-color: #121212;");

  QFrame *modalContainer = new QFrame(this);
  modalContainer->setStyleSheet("background-color: #1e1e1e; border: 1px solid "
                                "#2d2d2d; border-radius: 16px;");
  auto *containerLayout = new QHBoxLayout(modalContainer);
  containerLayout->setContentsMargins(0, 0, 0, 0);
  containerLayout->setSpacing(0);

  // --- Left: Media Side (60%) ---
  QFrame *mediaFrame = new QFrame(modalContainer);
  mediaFrame->setStyleSheet("background-color: black; border-top-left-radius: "
                            "16px; border-bottom-left-radius: 16px;");
  auto *mediaLayout = new QVBoxLayout(mediaFrame);

  QLabel *imagePlaceholder =
      new QLabel("INCIDENT CAPTURE\nCAM-04-EAST", mediaFrame);
  imagePlaceholder->setAlignment(Qt::AlignCenter);
  imagePlaceholder->setStyleSheet(
      "color: white; font-weight: bold; font-size: 14px;");
  mediaLayout->addWidget(imagePlaceholder);

  containerLayout->addWidget(mediaFrame, 6);

  // --- Right: Data Side (40%) ---
  QFrame *dataFrame = new QFrame(modalContainer);
  dataFrame->setStyleSheet(
      "background-color: #1c1c1c; border-top-right-radius: 16px; "
      "border-bottom-right-radius: 16px;");
  auto *dataLayout = new QVBoxLayout(dataFrame);
  dataLayout->setContentsMargins(30, 30, 30, 30);
  dataLayout->setSpacing(20);

  // Header
  auto *headerLayout = new QHBoxLayout();
  auto *warningIcon = new QLabel("⚠️", dataFrame);
  QLabel *title = new QLabel("#FE-8921 Fare Evasion", dataFrame);
  title->setStyleSheet("font-size: 24px; font-weight: bold; color: white;");

  QPushButton *closeBtn = new QPushButton("X", dataFrame);
  closeBtn->setFixedSize(30, 30);
  closeBtn->setStyleSheet("background: transparent; color: #6b7280; "
                          "font-weight: bold; font-size: 16px;");
  connect(closeBtn, &QPushButton::clicked, this, &DetailView::closeClicked);

  headerLayout->addWidget(warningIcon);
  headerLayout->addWidget(title);
  headerLayout->addStretch();
  headerLayout->addWidget(closeBtn);
  dataLayout->addLayout(headerLayout);

  // Alert Box
  QFrame *alertBox = new QFrame(dataFrame);
  alertBox->setStyleSheet(
      "background-color: rgba(243, 113, 32, 0.1); border: 1px solid rgba(243, "
      "113, 32, 0.2); border-radius: 8px;");
  auto *alertLayout = new QHBoxLayout(alertBox);
  QLabel *alertText =
      new QLabel("CRITICAL ALERT\nRequires Immediate Action", alertBox);
  alertText->setStyleSheet("color: #f37120; font-weight: bold; text-transform: "
                           "uppercase; font-size: 11px;");
  alertLayout->addWidget(alertText);
  dataLayout->addWidget(alertBox);

  // Metadata List
  auto addRow = [&](QString label, QString val) {
    QFrame *row = new QFrame(dataFrame);
    row->setStyleSheet(
        "background-color: rgba(255,255,255,0.03); border-radius: 8px;");
    auto *rowLayout = new QVBoxLayout(row);
    QLabel *l = new QLabel(label, row);
    l->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: bold; "
                     "text-transform: uppercase;");
    QLabel *v = new QLabel(val, row);
    v->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
    rowLayout->addWidget(l);
    rowLayout->addWidget(v);
    dataLayout->addWidget(row);
  };

  addRow("Card ID", "7721-XXXX-9901");
  addRow("Location", "Central Station - Gate 04");
  addRow("Reason", "Tailgating Detection");

  dataLayout->addSpacing(10);

  // Hardware Status
  QLabel *hwTitle = new QLabel("HARDWARE STATUS", dataFrame);
  hwTitle->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: bold; "
                         "text-transform: uppercase;");
  dataLayout->addWidget(hwTitle);

  QHBoxLayout *hwLayout = new QHBoxLayout();
  auto addHwStatus = [&](QString text, bool ok) {
    QLabel *lbl = new QLabel(QString("%1 %2").arg(ok ? "✓" : "⚠️", text));
    lbl->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 12px;")
                           .arg(ok ? "white" : "#ef4444"));
    hwLayout->addWidget(lbl);
  };
  addHwStatus("Gate Locked", true);
  addHwStatus("Alarm Silent", true);
  hwLayout->addStretch();
  dataLayout->addLayout(hwLayout);

  dataLayout->addSpacing(10);

  // Confidence
  QLabel *confTitle = new QLabel("DETECTION CONFIDENCE", dataFrame);
  confTitle->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: "
                           "bold; text-transform: uppercase;");
  dataLayout->addWidget(confTitle);

  QHBoxLayout *confHeader = new QHBoxLayout();
  QLabel *confVal = new QLabel("98.2%", dataFrame);
  confVal->setStyleSheet("color: #22c55e; font-size: 20px; font-weight: bold;");
  QLabel *aiLbl = new QLabel("AI ACCURACY", dataFrame);
  aiLbl->setStyleSheet("color: #6b7280; font-size: 10px; font-weight: bold;");
  confHeader->addWidget(confVal);
  confHeader->addStretch();
  confHeader->addWidget(aiLbl);
  dataLayout->addLayout(confHeader);

  dataLayout->addStretch();

  // Footer Buttons
  auto *btnLayout = new QHBoxLayout();
  QPushButton *dismissBtn = new QPushButton("Dismiss", dataFrame);
  dismissBtn->setStyleSheet(
      "background-color: rgba(255,255,255,0.05); color: white; border: 1px "
      "solid rgba(255,255,255,0.1); padding: 10px; border-radius: 4px;");

  QPushButton *ackBtn = new QPushButton("Acknowledge", dataFrame);
  ackBtn->setStyleSheet("background-color: #ff6b2c; color: white; padding: "
                        "10px; border-radius: 4px; font-weight: bold;");

  btnLayout->addWidget(dismissBtn);
  btnLayout->addWidget(ackBtn);
  dataLayout->addLayout(btnLayout);

  containerLayout->addWidget(dataFrame, 4);
  mainLayout->addWidget(modalContainer);
}
