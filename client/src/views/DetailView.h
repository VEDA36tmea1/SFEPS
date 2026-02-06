#ifndef DETAILVIEW_H
#define DETAILVIEW_H

#include <QWidget>

class DetailView : public QWidget {
  Q_OBJECT
public:
  explicit DetailView(QWidget *parent = nullptr);

signals:
  void closeClicked();
};

#endif // DETAILVIEW_H
