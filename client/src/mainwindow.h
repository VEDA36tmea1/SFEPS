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

// 탐지된 위반 사례의 정보를 저장하는 구조체
struct FraudLog {
    QString timestamp; // 발생 시각
    QString cardId;    // 카드 번호 또는 ID
    QString reason;    // 위반 사유
    QString imagePath; // 증거 사진 경로
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    // 마우스 이벤트 오버라이딩: 영상 내 특정 영역 드래그 확대 기능용
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void updateFrame();                     // 타이머에 의해 초당 약 30회 호출되는 영상 갱신 함수
    void onLogSelected(QListWidgetItem *item); // 리스트의 로그를 클릭했을 때 실행되는 팝업 함수
    void resetZoom();                       // 확대를 취소하고 원본 화면으로 복구

private:
    void setupUI();         // 화면 레이아웃 및 위젯 초기 설정
    void applyDarkTheme();  // UI 스타일시트(검은색 테마) 적용
    void addSampleData();   // 테스트용 더미 데이터 삽입

    cv::VideoCapture cap;   // OpenCV 영상 캡처 객체 (RTSP 스트림용)
    cv::Mat currentFrame;   // 현재 읽어온 원본 영상 프레임 데이터
    QTimer *timer;          // 영상 갱신 주기를 관리하는 타이머
    
    QLabel *videoDisplay;   // 영상을 실제로 화면에 뿌려주는 라벨 위젯
    QListWidget *logList;   // 우측 위반 사례 목록 위젯
    QSlider *brightnessSlider; // 실시간 영상 밝기 조절 슬라이더
    QRubberBand *rubberBand;   // 드래그 시 나타나는 파란색 선택 영역 가이드
    QPoint origin;             // 드래그 시작 좌표 저장

    cv::Rect zoomRect;         // 사용자가 선택한 확대 대상 영역 좌표
    bool isZoomed = false;     // 현재 화면이 확대 상태인지 여부 플래그
    QMap<int, FraudLog> logDataMap; // 행(row) 번호와 로그 데이터를 매핑하여 관리
};

#endif