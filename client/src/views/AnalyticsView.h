#ifndef ANALYTICSVIEW_H
#define ANALYTICSVIEW_H

#include <QWidget>

class AnalyticsView : public QWidget {
  Q_OBJECT
public:
  explicit AnalyticsView(QWidget *parent = nullptr);

signals:
  void backClicked();
};

#endif // ANALYTICSVIEW_H
