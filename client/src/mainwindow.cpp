#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QPushButton>
#include <QApplication>
#include <QDialog>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("METRO GUARD AI - Security Dashboard");
    resize(1300, 700); // 메인 창 크기 설정

    setupUI();
    applyDarkTheme();
    addSampleData();

    // RTSP 스트림 연결
    QString rtspUrl = "rtsp://192.168.0.89:8554/stream";
    cap.open(rtspUrl.toStdString()); 
    
    // [끊김 방지] 버퍼 크기 최소화 설정
    if(cap.isOpened()) {
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1); 
    }
    
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateFrame);
    timer->start(33); // 약 30 FPS로 프레임 갱신
}

MainWindow::~MainWindow() {
    cap.release();
}

void MainWindow::setupUI() {
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *rootLayout = new QVBoxLayout(centralWidget);
    QSplitter *horizontalSplitter = new QSplitter(Qt::Horizontal);

    // --- 좌측: 영상 모니터링 영역 ---
    QWidget *leftWidget = new QWidget();
    QVBoxLayout *leftLayout = new QVBoxLayout(leftWidget);
    videoDisplay = new QLabel("CONNECTING TO STREAM...");
    videoDisplay->setAlignment(Qt::AlignCenter);
    videoDisplay->setStyleSheet("background-color: black; border: 1px solid #333;");

    QHBoxLayout *ctrlLayout = new QHBoxLayout();
    brightnessSlider = new QSlider(Qt::Horizontal);
    brightnessSlider->setRange(-100, 100);
    QPushButton *resetBtn = new QPushButton("RESET ZOOM");
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::resetZoom);

    ctrlLayout->addWidget(new QLabel("BRIGHTNESS"));
    ctrlLayout->addWidget(brightnessSlider);
    ctrlLayout->addWidget(resetBtn);

    leftLayout->addWidget(videoDisplay, 1);
    leftLayout->addLayout(ctrlLayout);

    // --- 우측: 위반 감지 로그 영역 ---
    QGroupBox *logBox = new QGroupBox("DETECTION LOGS (Click to View)");
    QVBoxLayout *logBoxLayout = new QVBoxLayout(logBox);
    logList = new QListWidget();
    connect(logList, &QListWidget::itemClicked, this, &MainWindow::onLogSelected);
    logBoxLayout->addWidget(logList);

    horizontalSplitter->addWidget(leftWidget);
    horizontalSplitter->addWidget(logBox);
    horizontalSplitter->setStretchFactor(0, 3); // 영상 영역 비율 확대

    rootLayout->addWidget(horizontalSplitter);
    setCentralWidget(centralWidget);

    rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
}

// 로그 선택 시 상세 정보 팝업창 띄우기
void MainWindow::onLogSelected(QListWidgetItem *item) {
    int id = logList->row(item);
    if (!logDataMap.contains(id)) return;
    FraudLog log = logDataMap[id];

    QDialog *popup = new QDialog(this);
    popup->setWindowTitle("Incident Detail");
    popup->setFixedSize(450, 500);
    popup->setStyleSheet("background-color: #1E1E1E; color: white;");

    QVBoxLayout *layout = new QVBoxLayout(popup);
    QLabel *info = new QLabel(QString("<b>[ALERT INFO]</b><br>TIME: %1<br>ID: %2<br>REASON: %3")
                              .arg(log.timestamp, log.cardId, log.reason));
    
    QLabel *imgLabel = new QLabel();
    imgLabel->setFixedSize(400, 225);
    imgLabel->setScaledContents(true);
    imgLabel->setStyleSheet("border: 1px solid #BB86FC;");

    if (!currentFrame.empty()) {
        cv::Mat temp;
        currentFrame.copyTo(temp);
        // [색상 수정] BGR -> RGB 변환 (파란색 현상 해결)
        cv::cvtColor(temp, temp, cv::COLOR_BGR2RGB);
        QImage qimg((const unsigned char*)temp.data, temp.cols, temp.rows, temp.step, QImage::Format_RGB888);
        imgLabel->setPixmap(QPixmap::fromImage(qimg));
    }

    QPushButton *closeBtn = new QPushButton("CLOSE");
    connect(closeBtn, &QPushButton::clicked, popup, &QDialog::accept);

    layout->addWidget(info);
    layout->addWidget(imgLabel);
    layout->addWidget(new QLabel("🎥 STATUS: CLIP READY"));
    layout->addWidget(closeBtn);

    popup->exec();
}

// 실시간 영상 프레임 업데이트
void MainWindow::updateFrame() {
    if(!cap.isOpened() || !cap.read(currentFrame)) return;

    currentFrame.convertTo(currentFrame, -1, 1, brightnessSlider->value());

    cv::Mat displayMat;
    if (isZoomed && zoomRect.width > 0) {
        cv::Rect safeRect = zoomRect & cv::Rect(0, 0, currentFrame.cols, currentFrame.rows);
        displayMat = currentFrame(safeRect);
    } else {
        displayMat = currentFrame;
    }

    cv::Mat rgbMat;
    cv::cvtColor(displayMat, rgbMat, cv::COLOR_BGR2RGB);
    QImage qimg((const unsigned char*)rgbMat.data, rgbMat.cols, rgbMat.rows, rgbMat.step, QImage::Format_RGB888);
    videoDisplay->setPixmap(QPixmap::fromImage(qimg).scaled(videoDisplay->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

// 블랙 테마 스타일 적용
void MainWindow::applyDarkTheme() {
    this->setStyleSheet(
        "QMainWindow { background-color: #121212; }"
        "QWidget { background-color: #121212; color: #E0E0E0; font-family: 'Segoe UI'; }"
        "QGroupBox { border: 2px solid #333; margin-top: 10px; font-weight: bold; color: #BB86FC; }"
        "QListWidget { background-color: #1E1E1E; border: none; color: #CF6679; selection-background-color: #333; }"
        "QPushButton { background-color: #333; border: 1px solid #555; padding: 8px; color: white; }"
    );
}

// 샘플 로그 데이터 생성
void MainWindow::addSampleData() {
    logDataMap[0] = {"14:02:31", "SN-9921", "Unusual Gate Pass", "img1.jpg", "clip1.mp4"};
    logDataMap[1] = {"14:15:10", "SN-1022", "Invalid Senior Card", "img2.jpg", "clip2.mp4"};
    for(int i=0; i<logDataMap.size(); ++i) {
        logList->addItem(QString("[%1] ALERT: %2").arg(logDataMap[i].timestamp, logDataMap[i].cardId));
    }
}

// 마우스 드래그 확대 기능 구현들
void MainWindow::mousePressEvent(QMouseEvent *event) {
    if (videoDisplay->geometry().contains(event->pos())) {
        origin = event->pos();
        rubberBand->setGeometry(QRect(origin, QSize()));
        rubberBand->show();
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event) {
    if (rubberBand->isVisible()) {
        rubberBand->setGeometry(QRect(origin, event->pos()).normalized());
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (rubberBand->isVisible()) {
        QRect rect = rubberBand->geometry();
        rubberBand->hide();
        if (rect.width() > 10) {
            float scaleX = (float)currentFrame.cols / videoDisplay->width();
            float scaleY = (float)currentFrame.rows / videoDisplay->height();
            QPoint relPos = rect.topLeft() - videoDisplay->pos();
            zoomRect = cv::Rect(relPos.x() * scaleX, relPos.y() * scaleY, 
                                rect.width() * scaleX, rect.height() * scaleY);
            isZoomed = true;
        }
    }
}

void MainWindow::resetZoom() { isZoomed = false; }