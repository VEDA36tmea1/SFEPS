#ifndef MONITORINGVIEW_H
#define MONITORINGVIEW_H

#include <QDialog>
#include <QFrame>
#include <QFuture>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QRubberBand>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>
#include <opencv2/opencv.hpp>


struct FraudLog {
  QString timestamp;
  QString cardId;
  QString reason;
  QString imagePath;
};

class MonitoringView : public QWidget {
  Q_OBJECT
public:
  explicit MonitoringView(QWidget *parent = nullptr);
  ~MonitoringView();

signals:
  void viewDetailClicked();
  void openSettingsClicked();
  void goToAnalyticsClicked();
  void logoutClicked();

private slots:
  void updateFrame();
  void resetZoom();
  void onLogSelected(QListWidgetItem *item);

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  // UI Helper Methods
  void createHeader();
  void createSidebar();
  void createFooter();
  void createMainContent();

  // Video Logic Components
  cv::VideoCapture cap;
  cv::Mat currentFrame;
  QTimer *timer;

  // Zoom/Rubberband Components
  QLabel *videoDisplay; // The specific label where video is rendered
  QRubberBand *rubberBand;
  QPoint origin;
  cv::Rect zoomRect;
  bool isZoomed = false;

  // UI Members for logic
  QSlider *brightnessSlider;
  QListWidget *logList;

  // Data
  QMap<int, FraudLog> logDataMap;
};

#endif // MONITORINGVIEW_H
