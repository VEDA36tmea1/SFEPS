#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QListWidget>
#include <QSlider>
#include <QTimer>
#include <QRubberBand>
#include <QMouseEvent>
#include <QMap>
#include <opencv2/opencv.hpp>

// 위반 사례 데이터를 담는 구조체
struct FraudLog {
    QString timestamp;
    QString cardId;
    QString reason;
    QString imagePath;
    QString videoPath;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    // 마우스 이벤트: 영상 확대 영역 지정을 위한 핸들러
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void updateFrame();                     // 매 프레임 영상 갱신
    void onLogSelected(QListWidgetItem *item); // 로그 선택 시 팝업창 호출
    void resetZoom();                       // 확대 초기화

private:
    void setupUI();         // UI 레이아웃 설정
    void applyDarkTheme();  // 블랙 테마 스타일시트 적용
    void addSampleData();   // 테스트용 로그 데이터 생성

    cv::VideoCapture cap;   // RTSP 스트림 캡처 객체
    cv::Mat currentFrame;   // 현재 원본 프레임
    QTimer *timer;          // 갱신 주기 제어용 타이머
    
    QLabel *videoDisplay;   // 메인 영상 출력 라벨
    QListWidget *logList;   // 우측 로그 목록
    QSlider *brightnessSlider; // 밝기 제어
    QRubberBand *rubberBand;   // 드래그 가이드 사각형
    QPoint origin;             // 드래그 시작점

    cv::Rect zoomRect;         // 현재 확대 중인 영역
    bool isZoomed = false;     // 확대 상태 플래그
    QMap<int, FraudLog> logDataMap; // 로그 ID 매핑 데이터
};

#endif