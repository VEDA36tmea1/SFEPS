#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QString>
#include <opencv2/opencv.hpp>
#include <QVariant>
#include <QVariantList>
#include <atomic>

// 프레임 캡처를 위한 워커 스레드
class VideoCaptureWorker : public QThread {
    Q_OBJECT
public:
    VideoCaptureWorker(cv::VideoCapture *cap, QObject *parent = nullptr) 
        : QThread(parent), cap(cap), running(false), framePending(false) {}
    
    void stop() { 
        running = false; 
        wait(); 
    }

    void markFrameConsumed() {
        framePending.store(false, std::memory_order_release);
    }

signals:
    void newFrame(const cv::Mat &frame);
    void readFailed();

protected:
    void run() override {
        running = true;
        cv::Mat frame;
        int failCount = 0;
        QElapsedTimer emitTimer;
        emitTimer.start();
        qint64 lastEmitMs = 0;
        constexpr qint64 kMinEmitIntervalMs = 66; // ~15 FPS
        while (running) {
            if (cap && cap->isOpened()) {
                bool ok = cap->grab();
                if (ok) {
                    // Drop queued frames so UI stays near live edge.
                    for (int i = 0; i < 2; ++i) {
                        if (!cap->grab()) break;
                    }
                    ok = cap->retrieve(frame) && !frame.empty();
                }

                if (ok) {
                    const qint64 now = emitTimer.elapsed();
                    if (now - lastEmitMs >= kMinEmitIntervalMs) {
                        // Keep at most one queued frame; drop extras under UI load.
                        if (!framePending.exchange(true, std::memory_order_acq_rel)) {
                            emit newFrame(frame.clone());
                            lastEmitMs = now;
                        }
                    }
                    failCount = 0;
                    QThread::msleep(1);
                } else {
                    ++failCount;
                    if (failCount >= 20) {
                        emit readFailed();
                        failCount = 0;
                    }
                    QThread::msleep(20);
                }
            } else {
                ++failCount;
                if (failCount >= 20) {
                    emit readFailed();
                    failCount = 0;
                }
                QThread::msleep(20);
            }
        }
    }

private:
    cv::VideoCapture *cap;
    std::atomic_bool running;
    std::atomic_bool framePending;
};

class MainWindow : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(int contrast READ contrast WRITE setContrast NOTIFY contrastChanged)
    Q_PROPERTY(QRectF zoomRect READ zoomRect WRITE setZoomRect NOTIFY zoomRectChanged)
    Q_PROPERTY(QString streamStatus READ streamStatus NOTIFY streamStatusChanged)
    Q_PROPERTY(bool streamConnected READ streamConnected NOTIFY streamConnectedChanged)
    Q_PROPERTY(QVariantList detections READ detections NOTIFY detectionsChanged)
    Q_PROPERTY(QString selectedDetection READ selectedDetection NOTIFY selectedDetectionChanged)
    Q_PROPERTY(QString externalTrackedId READ externalTrackedId WRITE setExternalTrackedId NOTIFY externalTrackedIdChanged)
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
    int contrast() const { return m_contrast; }
    void setContrast(int contrast);
    
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
    void setExternalTrackedId(const QString &id);

    QVariantList detections() const { return m_detections; }
    QString selectedDetection() const { return m_selectedDetectionId; }
    QString externalTrackedId() const { return m_externalTrackedId; }
    int imageWidth() const;
    int imageHeight() const;

signals:
    void runningChanged();
    void brightnessChanged();
    void contrastChanged();
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
    void externalTrackedIdChanged();
    void imageSizeChanged();

private:
    bool openStream();
    void ensureWorkerRunning();
    void updateStreamStatus(const QString &status, bool connected);
    void scheduleBrightnessCgiUpdate();
    void scheduleContrastCgiUpdate();
    void sendBrightnessCgi();
    void sendContrastCgi();
    void fetchCameraImageSettings();

    cv::VideoCapture cap;
    VideoCaptureWorker *worker;
    QImage m_image;
    mutable QMutex m_mutex;
    QTimer *m_reconnectTimer;
    QTimer *m_updateTimer;

    bool m_running;
    int m_brightness;
    int m_contrast;
    QRectF m_zoomRect;
    QString m_streamStatus;
    bool m_streamConnected;
    QSize m_lastImageSize;
    QVariantList m_detections;
    QVariantList m_pendingDetections;
    bool m_hasPendingDetections;
    QString m_selectedDetectionId;
    QString m_externalTrackedId;

    bool m_useCameraCgiControl;
    bool m_cameraCgiAllowInsecureTls;
    QString m_cameraBrightnessCgiUrlTemplate;
    QString m_cameraContrastCgiUrlTemplate;
    QString m_cameraCgiUser;
    QString m_cameraCgiPassword;
    QNetworkAccessManager *m_cgiNetworkManager;
    QTimer *m_brightnessCgiDebounceTimer;
    QTimer *m_contrastCgiDebounceTimer;
    
private slots:
    void onUpdateTimerTimeout();
};

#endif // MAINWINDOW_H
