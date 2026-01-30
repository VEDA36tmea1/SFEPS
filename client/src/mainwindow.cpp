#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QPushButton>
#include <QDialog>
#include <QApplication>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Subway Fare Evasion Detection System");
    resize(1300, 700); // 메인 창 크기를 1300x700으로 고정

    setupUI();         // 화면 구성
    applyDarkTheme();  // 테마 적용
    addSampleData();   // 데이터 로드

    // RTSP 연결: FFMPEG 백엔드를 명시하여 주소 오인 에러 방지
    QString rtspUrl = "rtsp://192.168.0.89:8554/stream";
    cap.open(rtspUrl.toStdString(), cv::CAP_FFMPEG); 
    
    if(cap.isOpened()) {
        // [끊김 방지] 영상 지연(Lag)을 최소화하기 위해 내부 버퍼를 1로 설정
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    // 약 30FPS(1000ms / 33) 속도로 영상을 갱신하도록 타이머 시작
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateFrame);
    timer->start(33); 
}

MainWindow::~MainWindow() {
    cap.release(); // 프로그램 종료 시 비디오 캡처 자원 해제
}

void MainWindow::setupUI() {
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *rootLayout = new QVBoxLayout(centralWidget);
    
    // 좌우 영역 크기를 사용자가 조절할 수 있도록 스플리터 생성
    QSplitter *horizontalSplitter = new QSplitter(Qt::Horizontal);

    // --- 좌측 영역: 비디오 모니터링 ---
    QWidget *leftWidget = new QWidget();
    QVBoxLayout *leftLayout = new QVBoxLayout(leftWidget);
    
    videoDisplay = new QLabel("WAITING FOR STREAM...");
    videoDisplay->setAlignment(Qt::AlignCenter);
    
    // [화면 커짐 방지 중요 설정]
    // 라벨이 영상 크기에 따라 스스로 커지지 않게 무시(Ignored) 정책 설정
    videoDisplay->setScaledContents(false); 
    videoDisplay->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    videoDisplay->setStyleSheet("background-color: black; border: 1px solid #333;");

    // 제어부: 밝기 슬라이더 및 리셋 버튼
    QHBoxLayout *ctrlLayout = new QHBoxLayout();
    brightnessSlider = new QSlider(Qt::Horizontal);
    brightnessSlider->setRange(-100, 100);
    QPushButton *resetBtn = new QPushButton("RESET ZOOM");
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::resetZoom);

    ctrlLayout->addWidget(new QLabel("BRIGHTNESS"));
    ctrlLayout->addWidget(brightnessSlider);
    ctrlLayout->addWidget(resetBtn);

    leftLayout->addWidget(videoDisplay, 1); // 영상 영역에 가중치 1 부여
    leftLayout->addLayout(ctrlLayout);

    // --- 우측 영역: 탐지 로그 목록 ---
    QGroupBox *logBox = new QGroupBox("INCIDENT LOGS");
    QVBoxLayout *logBoxLayout = new QVBoxLayout(logBox);
    logList = new QListWidget();
    connect(logList, &QListWidget::itemClicked, this, &MainWindow::onLogSelected);
    logBoxLayout->addWidget(logList);

    horizontalSplitter->addWidget(leftWidget);
    horizontalSplitter->addWidget(logBox);
    
    // 초기 화면 비율을 영상 70%, 로그 30%로 설정
    horizontalSplitter->setStretchFactor(0, 7);
    horizontalSplitter->setStretchFactor(1, 3);

    rootLayout->addWidget(horizontalSplitter);
    setCentralWidget(centralWidget);

    // 드래그 영역 표시용 고무줄 위젯 초기화
    rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
}

void MainWindow::updateFrame() {
    if(!cap.isOpened()) return;

    // 스트림으로부터 한 프레임을 읽어옴
    if (!cap.read(currentFrame)) return; 

    // 슬라이더 값에 따라 프레임 밝기 보정
    currentFrame.convertTo(currentFrame, -1, 1, brightnessSlider->value());

    cv::Mat displayMat;
    // 확대 모드일 경우 선택 영역만 잘라냄(Crop)
    if (isZoomed && zoomRect.width > 0) {
        cv::Rect safeRect = zoomRect & cv::Rect(0, 0, currentFrame.cols, currentFrame.rows);
        if(safeRect.area() > 0) displayMat = currentFrame(safeRect);
        else displayMat = currentFrame;
    } else {
        displayMat = currentFrame;
    }

    // [색상 수정] OpenCV는 BGR 형식을 쓰므로 Qt 출력을 위해 RGB로 변환 (파란색 방지)
    cv::Mat rgbMat;
    cv::cvtColor(displayMat, rgbMat, cv::COLOR_BGR2RGB);

    // 이미지 포맷팅 및 화면 표시
    QImage qimg((const unsigned char*)rgbMat.data, rgbMat.cols, rgbMat.rows, rgbMat.step, QImage::Format_RGB888);
    
    // [화면 커짐 방지] 현재 라벨 크기에 딱 맞춰서 이미지를 스케일링하여 출력
    if (!qimg.isNull()) {
        videoDisplay->setPixmap(QPixmap::fromImage(qimg).scaled(
            videoDisplay->size(), 
            Qt::KeepAspectRatio, 
            Qt::SmoothTransformation));
    }
}

void MainWindow::onLogSelected(QListWidgetItem *item) {
    int id = logList->row(item); // 클릭한 행 번호 가져오기
    if (!logDataMap.contains(id)) return;
    FraudLog log = logDataMap[id];

    // --- 로그 클릭 시 상세 정보를 보여줄 새 팝업 창 생성 ---
    QDialog *popup = new QDialog(this);
    popup->setWindowTitle("Incident Clip Detail");
    popup->setFixedSize(450, 500);
    popup->setStyleSheet("background-color: #1E1E1E; color: white;");

    QVBoxLayout *layout = new QVBoxLayout(popup);
    
    // 위반 정보 텍스트 설명
    QLabel *info = new QLabel(QString("<b>TIME:</b> %1<br><b>ID:</b> %2<br><b>REASON:</b> %3")
                              .arg(log.timestamp, log.cardId, log.reason));
    
    // 탐지 당시의 캡처 사진 영역
    QLabel *imgLabel = new QLabel();
    imgLabel->setFixedSize(400, 225);
    imgLabel->setScaledContents(true);

    if (!currentFrame.empty()) {
        cv::Mat temp;
        currentFrame.copyTo(temp);
        // [색상 수정] 캡처 사진도 원본 색상으로 표시되게 변환
        cv::cvtColor(temp, temp, cv::COLOR_BGR2RGB);
        QImage qimg((const unsigned char*)temp.data, temp.cols, temp.rows, temp.step, QImage::Format_RGB888);
        imgLabel->setPixmap(QPixmap::fromImage(qimg));
    }

    QPushButton *closeBtn = new QPushButton("CLOSE");
    connect(closeBtn, &QPushButton::clicked, popup, &QDialog::accept);

    layout->addWidget(info);
    layout->addWidget(new QLabel("SNAPSHOT:"));
    layout->addWidget(imgLabel);
    layout->addWidget(new QLabel("🎥 5s CLIP PLAYING...")); // 클립 재생 안내 문구
    layout->addWidget(closeBtn);

    popup->exec(); // 팝업창 실행 (닫기 전까지 메인 제어 불가)
}

void MainWindow::applyDarkTheme() {
    // 현대적인 다크 테마 스타일시트 적용
    this->setStyleSheet(
        "QMainWindow { background-color: #121212; }"
        "QWidget { background-color: #121212; color: #E0E0E0; }"
        "QGroupBox { border: 2px solid #333; margin-top: 10px; font-weight: bold; color: #BB86FC; }"
        "QListWidget { background-color: #1E1E1E; border: none; color: #CF6679; }"
        "QPushButton { background-color: #333; border: 1px solid #555; padding: 5px; color: white; }"
    );
}

void MainWindow::addSampleData() {
    // 테스트용 데이터 구성
    logDataMap[0] = {"14:02:31", "SN-9921", "Unusual Gate Pass", "img1.jpg"};
    logDataMap[1] = {"14:15:10", "SN-1022", "Invalid Card", "img2.jpg"};
    for(int i=0; i<logDataMap.size(); ++i) {
        logList->addItem(QString("[%1] %2").arg(logDataMap[i].timestamp, logDataMap[i].cardId));
    }
}

// --- 마우스 드래그 확대 기능 구현 부 ---

void MainWindow::mousePressEvent(QMouseEvent *event) {
    // 영상 라벨 내부를 클릭했을 때만 드래그 시작
    if (videoDisplay->geometry().contains(event->pos())) {
        origin = event->pos();
        rubberBand->setGeometry(QRect(origin, QSize()));
        rubberBand->show();
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event) {
    // 드래그 중인 영역을 파란색 사각형으로 표시
    if (rubberBand->isVisible()) {
        rubberBand->setGeometry(QRect(origin, event->pos()).normalized());
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (rubberBand->isVisible()) {
        QRect rect = rubberBand->geometry();
        rubberBand->hide();
        
        // 너무 작은 영역은 무시하고 일정 크기 이상일 때만 확대 적용
        if (rect.width() > 10) {
            // 화면상 좌표와 실제 영상 프레임 해상도 간의 비율 계산(스케일링)
            float scaleX = (float)currentFrame.cols / videoDisplay->width();
            float scaleY = (float)currentFrame.rows / videoDisplay->height();
            QPoint relPos = rect.topLeft() - videoDisplay->pos();
            
            // 확대할 영상 데이터의 좌표 설정
            zoomRect = cv::Rect(relPos.x() * scaleX, relPos.y() * scaleY, 
                                rect.width() * scaleX, rect.height() * scaleY);
            isZoomed = true;
        }
    }
}

void MainWindow::resetZoom() { isZoomed = false; } // 확대 취소 후 전체 화면 복구