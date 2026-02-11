#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <opencv2/opencv.hpp>

// 프레임 캡처를 위한 워커 스레드
class VideoCaptureWorker : public QThread {
    Q_OBJECT
public:
    VideoCaptureWorker(cv::VideoCapture *cap, QObject *parent = nullptr) 
        : QThread(parent), cap(cap), running(false) {}
    
    void stop() { 
        running = false; 
        wait(); 
    }

signals:
    void newFrame(const cv::Mat &frame);

protected:
    void run() override {
        running = true;
        cv::Mat frame;
        while (running) {
            if (cap->read(frame)) {
                emit newFrame(frame);
            }
            QThread::msleep(10);
        }
    }

private:
    cv::VideoCapture *cap;
    bool running;
};

class MainWindow : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(QRectF zoomRect READ zoomRect WRITE setZoomRect NOTIFY zoomRectChanged)

public:
    explicit MainWindow(QQuickItem *parent = nullptr);
    ~MainWindow() override;

    void paint(QPainter *painter) override;

    bool isRunning() const { return m_running; }
    void setRunning(bool running);

    int brightness() const { return m_brightness; }
    void setBrightness(int brightness);
    
    QRectF zoomRect() const { return m_zoomRect; }
    void setZoomRect(const QRectF &rect);

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void setZoomFromItem(const QRectF &itemRect, const QSizeF &itemSize);

signals:
    void runningChanged();
    void brightnessChanged();
    void zoomRectChanged();

private slots:
    void processFrame(const cv::Mat &frame);

private:
    cv::VideoCapture cap;
    VideoCaptureWorker *worker;
    cv::Mat currentFrame;
    QImage m_image;
    QMutex m_mutex;

    bool m_running;
    int m_brightness;
    QRectF m_zoomRect;
};

#endif // MAINWINDOW_H
