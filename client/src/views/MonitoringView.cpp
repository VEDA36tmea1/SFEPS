#include "MonitoringView.h"
#include <QDateTime>
#include <QDebug>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

MonitoringView::MonitoringView(QWidget *parent) : QWidget(parent) {
  // --- Layout Setup from qt_client ---
  auto *globalLayout = new QHBoxLayout(this);
  globalLayout->setContentsMargins(0, 0, 0, 0);
  globalLayout->setSpacing(0);

  // Left: Main Content
  auto *leftWidget = new QWidget(this);
  auto *leftLayout = new QVBoxLayout(leftWidget);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  leftLayout->setSpacing(0);

  // 1. Content Header
  QFrame *contentHeader = new QFrame(leftWidget);
  contentHeader->setStyleSheet(
      "border-bottom: 1px solid #333333; background-color: #0a0a0a;");
  contentHeader->setFixedHeight(64);
  auto *headerLayout = new QHBoxLayout(contentHeader);
  headerLayout->setContentsMargins(24, 0, 24, 0);

  QLabel *icon = new QLabel("▦", contentHeader);
  icon->setFixedSize(32, 32);
  icon->setStyleSheet(
      "background-color: #ff6b2c; border-radius: 4px; color: white; "
      "qproperty-alignment: AlignCenter; font-size: 16px;");
  QLabel *title = new QLabel("Active Surveillance Streams", contentHeader);
  title->setStyleSheet(
      "color: white; font-size: 18px; font-weight: bold; margin-left: 8px;");

  headerLayout->addWidget(icon);
  headerLayout->addWidget(title);
  headerLayout->addStretch();

  // Grid Toggle (Mock)
  QPushButton *gridBtn = new QPushButton("Grid", contentHeader);
  gridBtn->setFlat(true);
  gridBtn->setStyleSheet("color: #9ca3af; font-weight: bold;"); // Fixed color
  headerLayout->addWidget(gridBtn);

  leftLayout->addWidget(contentHeader);

  // 2. Camera Grid
  QFrame *gridContainer = new QFrame(leftWidget);
  gridContainer->setStyleSheet("background-color: #0a0a0a;");
  auto *gridLayout = new QGridLayout(gridContainer);
  gridLayout->setContentsMargins(24, 24, 24, 24);
  gridLayout->setSpacing(16);

  // We will create the MAIN Video Display here.
  // For this migration, we will use a large single player for CAM-01 and
  // placeholders for others, Or stick to the grid. MAPPING: Top-Left (CAM-01)
  // is the Real Stream.

  auto createCameraCard = [&](int row, int col, QString id, QString name,
                              bool isReal) -> QFrame * {
    QFrame *card = new QFrame();
    card->setObjectName("cameraCard");
    card->setStyleSheet(
        "QFrame#cameraCard { background-color: #1a1a1a; border: 1px solid "
        "#333333; border-radius: 8px; } QFrame#cameraCard:hover { border: 1px "
        "solid #ff6b2c; }");

    QVBoxLayout *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);

    QLabel *img = new QLabel(card);
    img->setStyleSheet("background-color: black; border-top-left-radius: 8px; "
                       "border-top-right-radius: 8px;");
    img->setAlignment(Qt::AlignCenter);

    if (isReal) {
      this->videoDisplay = img; // Assign to class member for OpenCV update
      img->setText("Initializing Stream...");
      img->setScaledContents(true);
      // VERY IMPORTANT: Enable Mouse Tracking for RubberBand
      img->setMouseTracking(true);
    } else {
      img->setText("OFFLINE / PLACEHOLDER\n" + id);
    }

    // Overlays
    QLabel *status = new QLabel(card);
    status->setText(" ● " + id);
    status->setStyleSheet(
        "background-color: rgba(0,0,0,0.7); color: #22c55e; border-radius: "
        "4px; padding: 4px; font-weight: bold; font-size: 10px;");
    status->move(12, 12);

    cardLayout->addWidget(img);
    return card;
  };

  // Add Cards
  gridLayout->addWidget(createCameraCard(0, 0, "CAM-01", "MAIN GATE", true), 0,
                        0);
  gridLayout->addWidget(createCameraCard(0, 1, "CAM-02", "TICKET HALL", false),
                        0, 1);
  gridLayout->addWidget(createCameraCard(1, 0, "CAM-03", "PLATFORM A", false),
                        1, 0);
  gridLayout->addWidget(createCameraCard(1, 1, "CAM-04", "PLATFORM B", false),
                        1, 1);

  leftLayout->addWidget(gridContainer, 1);

  // 3. Control Bar
  QFrame *controlBar = new QFrame(leftWidget);
  controlBar->setStyleSheet(
      "background-color: #1a1a1a; border-top: 1px solid #333333;");
  controlBar->setFixedHeight(80);
  auto *ctrlLayout = new QHBoxLayout(controlBar);
  ctrlLayout->setContentsMargins(24, 0, 24, 0);
  ctrlLayout->setSpacing(24);

  // Brightness Slider
  QLabel *brLabel = new QLabel("Brightness", controlBar);
  brLabel->setStyleSheet("color: #9ca3af; font-size: 10px; font-weight: bold; "
                         "text-transform: uppercase;");
  brightnessSlider = new QSlider(Qt::Horizontal, controlBar);
  brightnessSlider->setRange(-100, 100);
  brightnessSlider->setValue(0);
  ctrlLayout->addWidget(brLabel);
  ctrlLayout->addWidget(brightnessSlider);
  connect(brightnessSlider, &QSlider::valueChanged, this,
          &MonitoringView::updateFrame); // Connect brightness slider

  // Reset Zoom Button
  QPushButton *zoomBtn = new QPushButton("Reset Zoom", controlBar);
  zoomBtn->setStyleSheet("color: white; border: 1px solid #404040; "
                         "border-radius: 4px; padding: 6px 12px;");
  connect(zoomBtn, &QPushButton::clicked, this, &MonitoringView::resetZoom);
  ctrlLayout->addWidget(zoomBtn);
  ctrlLayout->addStretch();

  leftLayout->addWidget(controlBar);

  // Right Sidebar (Event Log)
  QFrame *rightSidebar = new QFrame(this);
  rightSidebar->setFixedWidth(380);
  rightSidebar->setStyleSheet(
      "background-color: #1a1a1a; border-left: 1px solid #333333;");
  auto *rightLayout = new QVBoxLayout(rightSidebar);
  rightLayout->setContentsMargins(0, 0, 0, 0);

  // Header
  QFrame *rsHeader = new QFrame(rightSidebar);
  rsHeader->setFixedHeight(64);
  rsHeader->setStyleSheet("border-bottom: 1px solid #333333;");
  auto *rshLayout = new QHBoxLayout(rsHeader);
  rshLayout->setContentsMargins(20, 0, 20, 0);

  QLabel *rsTitle = new QLabel("Event Log", rsHeader);
  rsTitle->setStyleSheet("color: white; font-weight: bold; font-size: 14px;");

  QLabel *liveBadge = new QLabel("LIVE FEED", rsHeader);
  liveBadge->setStyleSheet(
      "background-color: #ff6b2c; color: white; padding: 4px 8px; "
      "border-radius: 4px; font-size: 10px; font-weight: bold;");

  rshLayout->addWidget(rsTitle);
  rshLayout->addStretch();
  rshLayout->addWidget(liveBadge);
  rightLayout->addWidget(rsHeader);

  // Search
  QFrame *searchBox = new QFrame(rightSidebar);
  searchBox->setStyleSheet("padding: 16px; border-bottom: 1px solid #333333;");
  auto *sbLayout = new QVBoxLayout(searchBox);
  sbLayout->setContentsMargins(16, 16, 16, 16);
  QLineEdit *search = new QLineEdit(searchBox);
  search->setPlaceholderText("Filter events...");
  search->setStyleSheet("background-color: #2a2a2a; border: 1px solid #404040; "
                        "border-radius: 4px; padding: 8px; color: white;");
  sbLayout->addWidget(search);
  rightLayout->addWidget(searchBox);

  // List
  logList = new QListWidget(rightSidebar);
  logList->setStyleSheet(
      "QListWidget { background-color: transparent; border: none; outline: "
      "none; } "
      "QListWidget::item { border-bottom: 1px solid #333333; padding: 10px; "
      "color: white; } "
      "QListWidget::item:selected { background-color: #2a2a2a; }");
  connect(logList, &QListWidget::itemClicked, this,
          &MonitoringView::onLogSelected);
  rightLayout->addWidget(logList);

  // Add some sample logs with better formatting
  logDataMap[0] = {"14:02:31", "SN-9921", "Unusual Gate Pass", "img1.jpg"};
  logDataMap[1] = {"14:15:10", "SN-1022", "Invalid Card", "img2.jpg"};
  logDataMap[2] = {"14:51:58", "SN-5621", "Tailgating Detected", "img3.jpg"};

  for (int i = 0; i < logDataMap.size(); ++i) {
    QListWidgetItem *item = new QListWidgetItem(logList);
    item->setText(QString(" ● %1 | %2\n   %3")
                      .arg(logDataMap[i].timestamp, logDataMap[i].cardId,
                           logDataMap[i].reason));
    logList->addItem(item);
  }

  globalLayout->addWidget(leftWidget, 1);
  globalLayout->addWidget(rightSidebar, 0);

  // --- SFEPS Video Logic ---
  rubberBand = new QRubberBand(QRubberBand::Rectangle, this);

  timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &MonitoringView::updateFrame);

  // Start stream asynchronously in a separate thread to avoid UI hang
  QTimer::singleShot(500, this, [this]() {
    qDebug() << "Launching async stream opening...";
    QtConcurrent::run([this]() {
      qDebug() << "Thread: Opening RTSP stream...";
      QString rtspUrl = "rtsp://192.168.0.89:8554/live";

      // Use CAP_FFMPEG explicitly
      bool success = cap.open(rtspUrl.toStdString(), cv::CAP_FFMPEG);

      if (success) {
        qDebug() << "Thread: Stream opened successfully.";
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        // Start timer in main thread
        QMetaObject::invokeMethod(timer, "start", Qt::QueuedConnection,
                                  Q_ARG(int, 33));
      } else {
        qWarning() << "Thread: Failed to open RTSP stream!";
        // Update UI in main thread
        QMetaObject::invokeMethod(
            videoDisplay, "setText", Qt::QueuedConnection,
            Q_ARG(QString, "Stream Unavailable (RTSP Offline)"));
      }
    });
  });
}

MonitoringView::~MonitoringView() { cap.release(); }

void MonitoringView::updateFrame() {
  if (!cap.isOpened())
    return;

  // Read frame
  if (!cap.read(currentFrame))
    return;

  // [Restored] Brightness Control
  currentFrame.convertTo(currentFrame, -1, 1, brightnessSlider->value());

  cv::Mat displayMat;
  if (isZoomed && zoomRect.width > 0) {
    cv::Rect safeRect =
        zoomRect & cv::Rect(0, 0, currentFrame.cols, currentFrame.rows);
    if (safeRect.area() > 0)
      displayMat = currentFrame(safeRect);
    else
      displayMat = currentFrame;
  } else {
    displayMat = currentFrame;
  }

  // Convert to RGB
  cv::Mat rgbMat;
  cv::cvtColor(displayMat, rgbMat, cv::COLOR_BGR2RGB);

  QImage qimg((const unsigned char *)rgbMat.data, rgbMat.cols, rgbMat.rows,
              rgbMat.step, QImage::Format_RGB888);

  if (!qimg.isNull() && videoDisplay) {
    // Scale to label size
    videoDisplay->setPixmap(QPixmap::fromImage(qimg).scaled(
        videoDisplay->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
}

void MonitoringView::onLogSelected(QListWidgetItem *item) {
  int id = logList->row(item);
  if (!logDataMap.contains(id))
    return;
  FraudLog log = logDataMap[id];

  // [Restored] Incident Clip Detail Popup
  QDialog *popup = new QDialog(this);
  popup->setWindowTitle("Incident Clip Detail");
  popup->setFixedSize(450, 500);
  popup->setStyleSheet("background-color: #1E1E1E; color: white;");

  QVBoxLayout *layout = new QVBoxLayout(popup);

  QLabel *info = new QLabel(
      QString("<b>TIME:</b> %1<br><b>ID:</b> %2<br><b>REASON:</b> %3")
          .arg(log.timestamp, log.cardId, log.reason));

  QLabel *imgLabel = new QLabel();
  imgLabel->setFixedSize(400, 225);
  imgLabel->setScaledContents(true);

  if (!currentFrame.empty()) {
    cv::Mat temp;
    currentFrame.copyTo(temp);
    cv::cvtColor(temp, temp, cv::COLOR_BGR2RGB);
    QImage qimg((const unsigned char *)temp.data, temp.cols, temp.rows,
                temp.step, QImage::Format_RGB888);
    imgLabel->setPixmap(QPixmap::fromImage(qimg));
  }

  QPushButton *closeBtn = new QPushButton("CLOSE");
  closeBtn->setStyleSheet("background-color: #ff6b2c; color: white; padding: "
                          "10px; border-radius: 4px;");
  connect(closeBtn, &QPushButton::clicked, popup, &QDialog::accept);

  layout->addWidget(info);
  layout->addWidget(new QLabel("SNAPSHOT:"));
  layout->addWidget(imgLabel);
  layout->addWidget(new QLabel("🎥 Incident evidence captured."));
  layout->addWidget(closeBtn);

  popup->exec();
}

// Mouse Events for Zoom
void MonitoringView::mousePressEvent(QMouseEvent *event) {
  // We need to map global/widget coordinates to the videoDisplay label
  // Check if click is inside videoDisplay
  if (videoDisplay && videoDisplay->geometry().contains(
                          videoDisplay->mapFrom(this, event->pos()))) {
    // Ideally we should sub-class QLabel for better mouse handling,
    // but for now we check if the event position (in this widget's coords)
    // maps correctly.
    // Actually layout makes this tricky.
    // Simpler approach: Check if child at pos is videoDisplay.

    QPoint localPos = videoDisplay->mapFrom(this, event->pos());
    if (videoDisplay->rect().contains(localPos)) {
      origin = event->pos(); // Store global-ish pos for rubberband
      rubberBand->setGeometry(QRect(origin, QSize()));
      rubberBand->show();
    }
  }
  // Note: Since videoDisplay is deep in the layout, 'event->pos()' is relative
  // to MonitoringView. 'videoDisplay->geometry()' is relative to ITS PARENT
  // (the card frame). So direct comparison fails. We need to map positions.

  QWidget *child = childAt(event->pos());
  if (child == videoDisplay) {
    origin = event->pos();
    rubberBand->setGeometry(QRect(origin, QSize()));
    rubberBand->show();
  }
}

void MonitoringView::mouseMoveEvent(QMouseEvent *event) {
  if (rubberBand->isVisible()) {
    rubberBand->setGeometry(QRect(origin, event->pos()).normalized());
  }
}

void MonitoringView::mouseReleaseEvent(QMouseEvent *event) {
  if (rubberBand->isVisible()) {
    QRect rect = rubberBand->geometry();
    rubberBand->hide();

    if (rect.width() > 10 && videoDisplay) {
      // Calculate scale based on the selection relative to the videoDisplay
      // We need to map coordinates to videoDisplay's local 0,0

      QPoint topLeft = videoDisplay->mapFrom(this, rect.topLeft());
      QPoint bottomRight = videoDisplay->mapFrom(this, rect.bottomRight());
      QRect localSelection = QRect(topLeft, bottomRight);

      // Limit to videoDisplay bounds
      localSelection = localSelection.intersected(videoDisplay->rect());

      if (localSelection.width() > 10) {
        float scaleX = (float)currentFrame.cols / videoDisplay->width();
        float scaleY = (float)currentFrame.rows / videoDisplay->height();

        zoomRect = cv::Rect(
            localSelection.x() * scaleX, localSelection.y() * scaleY,
            localSelection.width() * scaleX, localSelection.height() * scaleY);
        isZoomed = true;
      }
    }
  }
}

void MonitoringView::resetZoom() { isZoomed = false; }

// Deprecated Headers
void MonitoringView::createHeader() {}
void MonitoringView::createSidebar() {}
void MonitoringView::createFooter() {}
void MonitoringView::createMainContent() {}
