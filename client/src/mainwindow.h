#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <opencv2/opencv.hpp>
#include <QVariant>
#include <QVariantList>

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
    void readFailed();

protected:
    void run() override {
        running = true;
        cv::Mat frame;
        int failCount = 0;
        while (running) {
            if (cap && cap->isOpened() && cap->read(frame) && !frame.empty()) {
                emit newFrame(frame);
                failCount = 0;
                QThread::msleep(10);
            } else {
                ++failCount;
                if (failCount >= 20) {
                    emit readFailed();
                    failCount = 0;
                }
                QThread::msleep(100);
            }
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
    Q_PROPERTY(QString streamStatus READ streamStatus NOTIFY streamStatusChanged)
    Q_PROPERTY(bool streamConnected READ streamConnected NOTIFY streamConnectedChanged)
    Q_PROPERTY(QVariantList detections READ detections NOTIFY detectionsChanged)
    Q_PROPERTY(QString selectedDetection READ selectedDetection NOTIFY selectedDetectionChanged)
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageSizeChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageSizeChanged)

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
    QString streamStatus() const { return m_streamStatus; }
    bool streamConnected() const { return m_streamConnected; }

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void setZoomFromItem(const QRectF &itemRect, const QSizeF &itemSize);
    Q_INVOKABLE void setDetections(const QVariantList &list);
    Q_INVOKABLE QString detectionAt(qreal x, qreal y);
    Q_INVOKABLE void clearDetections();
    Q_INVOKABLE void setSelectedDetection(const QString &id);

    QVariantList detections() const { return m_detections; }
    QString selectedDetection() const { return m_selectedDetectionId; }
    int imageWidth() const;
    int imageHeight() const;

signals:
    void runningChanged();
    void brightnessChanged();
    void zoomRectChanged();
    void streamStatusChanged();
    void streamConnectedChanged();

private slots:
    void processFrame(const cv::Mat &frame);
    void onReadFailed();
    void attemptReconnect();

signals:
    void detectionsChanged();
    void selectedDetectionChanged();
    void imageSizeChanged();

private:
    bool openStream();
    void ensureWorkerRunning();
    void updateStreamStatus(const QString &status, bool connected);

    cv::VideoCapture cap;
    VideoCaptureWorker *worker;
    cv::Mat currentFrame;
    QImage m_image;
    mutable QMutex m_mutex;
    QTimer *m_reconnectTimer;

    bool m_running;
    int m_brightness;
    QRectF m_zoomRect;
    QString m_streamStatus;
    bool m_streamConnected;
    QVariantList m_detections;
    QString m_selectedDetectionId;
};

#endif // MAINWINDOW_H
