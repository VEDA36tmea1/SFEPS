#include "AnalyticsView.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

// Custom Pie Chart Widget for Entry Status
class PieChartWidget : public QWidget {
public:
  explicit PieChartWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedSize(220, 220);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF rect(10, 10, 200, 200);
    int startAngle = 90 * 16;
    int evasionSpan = -360 * 0.15 * 16; // 15% Evasion

    // Evasion (Orange)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#ff6b2c"));
    p.drawPie(rect, startAngle, evasionSpan);

    // Valid (Blue)
    p.setBrush(QColor("#1e3a8a"));
    p.drawPie(rect, startAngle + evasionSpan,
              360 * 16 - evasionSpan); // Remaining

    // Center hole (Donut style)
    p.setBrush(QColor("#1a1a1a")); // Card bg
    p.drawEllipse(rect.center(), 70, 70);
  }
};

// Custom Bar Chart Widget for Demographics
class BarChartWidget : public QWidget {
public:
  explicit BarChartWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedHeight(220);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Axes
    p.setPen(QPen(QColor("#333333"), 1));
    p.drawLine(40, height() - 30, width() - 20, height() - 30); // X Axis
    p.setPen(Qt::NoPen);

    // Data
    struct Data {
      QString label;
      int val;
    };
    QList<Data> data = {
        {"<18", 12}, {"18-25", 28}, {"26-35", 35}, {"36-45", 18}, {"46+", 7}};

    int barWidth = (width() - 80) / data.size() - 20;
    int maxVal = 40;
    int x = 50;

    for (const auto &d : data) {
      int h = (double)d.val / maxVal * (height() - 60);

      // Bar
      p.setBrush(QColor("#3b82f6"));
      p.drawRoundedRect(x, height() - 30 - h, barWidth, h, 4, 4);

      // Label
      p.setPen(QColor("#9ca3af"));
      p.drawText(QRect(x, height() - 25, barWidth, 20), Qt::AlignCenter,
                 d.label);

      x += barWidth + 20;
    }
  }
};

AnalyticsView::AnalyticsView(QWidget *parent) : QWidget(parent) {
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(40, 40, 40, 40);
  mainLayout->setSpacing(24);

  // 1. Top Metrics Cards
  auto *statsLayout = new QHBoxLayout();
  statsLayout->setSpacing(24);

  struct Stat {
    QString title;
    QString val;
    QString change;
    bool up;
    QString color;
    float progress;
    QString progColor;
  };
  QList<Stat> stats = {
      {"Daily Passenger Count", "42,850", "5.2%", true, "white", 0.65,
       "#ff6b2c"},
      {"Total Evasions Today", "1,240", "12.1%", true, "#ff6b2c", 0.45,
       "#ff6b2c"},
      {"System Health", "98.4%", "0.2%", false, "white", 0.98, "#22c55e"}};

  for (const auto &s : stats) {
    QFrame *card = new QFrame(this);
    card->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; "
                        "border-radius: 8px;");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(24, 24, 24, 24);

    QLabel *l = new QLabel(s.title, card);
    l->setStyleSheet("color: #9ca3af; font-size: 14px;");

    QLabel *v = new QLabel(s.val, card);
    v->setStyleSheet(
        "color: white; font-weight: bold; font-size: 30px; margin-top: 8px;");

    QFrame *progBg = new QFrame(card);
    progBg->setFixedHeight(4);
    progBg->setStyleSheet("background-color: #2a2a2a; border-radius: 2px; "
                          "margin-top: 12px; margin-bottom: 12px;");
    QHBoxLayout *pl = new QHBoxLayout(progBg);
    pl->setContentsMargins(0, 0, 0, 0);
    QWidget *fill = new QWidget();
    fill->setStyleSheet(
        QString("background-color: %1; border-radius: 2px;").arg(s.progColor));
    pl->addWidget(fill, s.progress * 100);
    pl->addStretch((1.0 - s.progress) * 100);

    QLabel *trend =
        new QLabel(QString("%1 %2").arg(s.up ? "▲" : "▼").arg(s.change), card);
    trend->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: bold;")
            .arg(s.up ? (s.title.contains("Evasions") ? "#ef4444" : "#22c55e")
                      : "#9ca3af"));

    cl->addWidget(l);
    cl->addWidget(v);
    cl->addWidget(progBg);
    cl->addWidget(trend);

    statsLayout->addWidget(card);
  }
  mainLayout->addLayout(statsLayout);

  // 2. Charts Row
  auto *chartsLayout = new QHBoxLayout();
  chartsLayout->setSpacing(24);

  // Entry Status
  QFrame *pieCard = new QFrame(this);
  pieCard->setStyleSheet("background-color: #1a1a1a; border: 1px solid "
                         "#333333; border-radius: 8px;");
  auto *pcl = new QHBoxLayout(pieCard);
  pcl->setContentsMargins(24, 24, 24, 24);

  auto *pieWidget = new PieChartWidget(pieCard);

  auto *legendLayout = new QVBoxLayout();
  auto addLegend = [&](QString label, QString color, QString val) {
    QLabel *l =
        new QLabel(QString("<font color='%1'>●</font> %2").arg(color, label));
    l->setStyleSheet("color: #9ca3af; font-size: 12px;");
    QLabel *v = new QLabel(val);
    v->setStyleSheet("color: white; font-size: 20px; font-weight: bold; "
                     "margin-bottom: 10px;");
    legendLayout->addWidget(l);
    legendLayout->addWidget(v);
  };
  addLegend("Evasions", "#ff6b2c", "6,427");
  addLegend("Valid Entries", "#1e3a8a", "36,423");
  legendLayout->addStretch();

  pcl->addWidget(pieWidget);
  pcl->addLayout(legendLayout);
  chartsLayout->addWidget(pieCard, 1);

  // Demographics
  QFrame *barCard = new QFrame(this);
  barCard->setStyleSheet("background-color: #1a1a1a; border: 1px solid "
                         "#333333; border-radius: 8px;");
  auto *bcl = new QVBoxLayout(barCard);
  bcl->setContentsMargins(24, 24, 24, 24);

  QHBoxLayout *bh = new QHBoxLayout();
  QLabel *bt = new QLabel("Demographic Distribution", barCard);
  bt->setStyleSheet("color: white; font-size: 16px; font-weight: bold;");
  bh->addWidget(bt);
  bh->addStretch();
  QLabel *sel = new QLabel("Last 7 Days ▼", barCard);
  sel->setStyleSheet(
      "background-color: #2a2a2a; color: white; padding: 4px 12px; border: 1px "
      "solid #404040; border-radius: 4px; font-size: 12px;");
  bh->addWidget(sel);

  bcl->addLayout(bh);
  bcl->addWidget(new BarChartWidget(barCard));
  chartsLayout->addWidget(barCard, 2);

  mainLayout->addLayout(chartsLayout);

  // 3. Recent Alerts Table
  QFrame *tableCard = new QFrame(this);
  tableCard->setStyleSheet("background-color: #1a1a1a; border: 1px solid "
                           "#333333; border-radius: 8px;");
  auto *tl = new QVBoxLayout(tableCard);
  tl->setContentsMargins(24, 24, 24, 24);

  QLabel *tt = new QLabel("Recent Evasion Alerts", tableCard);
  tt->setStyleSheet(
      "color: white; font-size: 16px; font-weight: bold; margin-bottom: 12px;");
  tl->addWidget(tt);

  QTableWidget *table = new QTableWidget(3, 5, tableCard);
  table->setHorizontalHeaderLabels(
      {"Timestamp", "Gate / Location", "Type", "Confidence", "Action"});
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  table->verticalHeader()->setVisible(false);
  table->setShowGrid(false);
  table->setStyleSheet(
      "QTableWidget { background-color: transparent; border: none; color: "
      "white; selection-background-color: #262626; }"
      "QHeaderView::section { background-color: transparent; color: #9ca3af; "
      "border-bottom: 1px solid #333333; padding: 8px; font-weight: bold; "
      "text-transform: uppercase; font-size: 11px; }"
      "QTableWidget::item { border-bottom: 1px solid #333333; padding: 12px; "
      "}");

  auto setItem = [&](int r, int c, QString txt, QString col = "white") {
    auto *i = new QTableWidgetItem(txt);
    i->setForeground(QColor(col));
    table->setItem(r, c, i);
  };

  setItem(0, 0, "14:23:05");
  setItem(0, 1, "Terminal A - Gate 04");
  setItem(0, 2, "TAILGATING", "#ff6b2c");
  setItem(0, 3, "98.2%");
  QPushButton *actBtn = new QPushButton("▶ Footage");
  actBtn->setStyleSheet(
      "background-color: #2563eb; color: white; border-radius: 4px; border: "
      "none; padding: 4px 8px; font-weight: bold; font-size: 11px;");
  table->setCellWidget(0, 4, actBtn);

  setItem(1, 0, "14:21:58");
  setItem(1, 1, "Terminal B - Gate 12");
  setItem(1, 2, "JUMP OVER", "#ff6b2c");
  setItem(1, 3, "96.5%");

  setItem(2, 0, "14:18:12");
  setItem(2, 1, "Main Hub - North Gate");
  setItem(2, 2, "FORCED ENTRY", "#ff6b2c");
  setItem(2, 3, "99.8%");

  tl->addWidget(table);
  mainLayout->addWidget(tableCard);

  // Back Button
  QPushButton *backBtn = new QPushButton("BACK TO MONITORING", this);
  backBtn->setStyleSheet("background-color: #2a2a2a; color: white; padding: "
                         "10px; border-radius: 4px; margin-top: 10px;");
  connect(backBtn, &QPushButton::clicked, this, &AnalyticsView::backClicked);
  mainLayout->addWidget(backBtn);
}
