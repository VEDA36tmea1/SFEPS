#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QObject>
#include <QMutex>
#include <QTimer>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <atomic>
#include <thread>

class LiveFrameProvider;

#ifdef SFEPS_HAVE_OPENCV
#include "XMLParser.h"
#include "native_metadata_tracker.h"
#include "rbf_pwm_core.h"
#include <vector>
#endif

class MainWindow : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int streamLatency READ streamLatency NOTIFY streamLatencyChanged)
    Q_PROPERTY(int videoMetaDelay READ videoMetaDelay NOTIFY videoMetaDelayChanged)
    Q_PROPERTY(bool running READ isRunning WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(int contrast READ contrast WRITE setContrast NOTIFY contrastChanged)
    Q_PROPERTY(QString streamStatus READ streamStatus NOTIFY streamStatusChanged)
    Q_PROPERTY(bool streamConnected READ streamConnected NOTIFY streamConnectedChanged)
    Q_PROPERTY(QVariantList detections READ detections NOTIFY detectionsChanged)
    Q_PROPERTY(QString selectedDetection READ selectedDetection NOTIFY selectedDetectionChanged)
    Q_PROPERTY(QString externalTrackedId READ externalTrackedId WRITE setExternalTrackedId NOTIFY externalTrackedIdChanged)
#ifdef SFEPS_HAVE_OPENCV
    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY previewRevisionChanged)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY previewRevisionChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY previewRevisionChanged)
    Q_PROPERTY(bool useLowLatencyOpenCv READ useLowLatencyOpenCv CONSTANT)
#endif

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow() override;

    bool isRunning() const { return m_running; }
    void setRunning(bool running);

    int brightness() const { return m_brightness; }
    void setBrightness(int brightness);
    int contrast() const { return m_contrast; }
    void setContrast(int contrast);
    QString streamStatus() const { return m_streamStatus; }
    bool streamConnected() const { return m_streamConnected; }

    Q_INVOKABLE void setDetections(const QVariantList &list);
    Q_INVOKABLE void clearDetections();
    Q_INVOKABLE void setSelectedDetection(const QString &id);
    void setExternalTrackedId(const QString &id);

    QVariantList detections() const { return m_detections; }
    QString selectedDetection() const { return m_selectedDetectionId; }
    QString externalTrackedId() const { return m_externalTrackedId; }
    int streamLatency() const { return m_streamLatencyMs; }
    int videoMetaDelay() const { return m_videoMetaDelayMs; }

#ifdef SFEPS_HAVE_OPENCV
    void setLiveFrameProvider(LiveFrameProvider *p) { m_liveProvider = p; }
    int previewRevision() const { return m_previewRevision; }
    int frameWidth() const
    {
        const int w = m_frameW.load();
        return w > 0 ? w : 1920;
    }
    int frameHeight() const
    {
        const int h = m_frameH.load();
        return h > 0 ? h : 1080;
    }
    bool useLowLatencyOpenCv() const { return true; }
#endif

signals:
    void runningChanged();
    void brightnessChanged();
    void contrastChanged();
    void streamStatusChanged();
    void streamConnectedChanged();
    void streamLatencyChanged();
    void videoMetaDelayChanged();
    void detectionsChanged();
    void selectedDetectionChanged();
    void externalTrackedIdChanged();
#ifdef SFEPS_HAVE_OPENCV
    void previewRevisionChanged();
    void pwmSetRequested(int pan, int tilt);
#endif

private:
    void updateStreamStatus(const QString &status, bool connected);
    void startMetadataWorker();
    void stopMetadataWorker();
    void scheduleBrightnessCgiUpdate();
    void scheduleContrastCgiUpdate();
    void sendBrightnessCgi();
    void sendContrastCgi();
    void fetchCameraImageSettings();

#ifdef SFEPS_HAVE_OPENCV
    void opencvCaptureLoop();
    void applyNativeDetections(std::vector<ParsedMetadataObject> humans, unsigned int rtpTs, qint64 wallMs,
                               qint64 frameNo);
    void onPwmTick();
#endif

    mutable QMutex m_mutex;
    QTimer *m_updateTimer;

    bool m_running;
    int m_brightness;
    int m_contrast;
    QString m_streamStatus;
    bool m_streamConnected;
    QVariantList m_detections;
    QVariantList m_pendingDetections;
    bool m_hasPendingDetections;
    QString m_selectedDetectionId;
    QString m_externalTrackedId;

    int m_streamLatencyMs = 0;
    int m_videoMetaDelayMs = -1;
    qint64 m_lastVideoMetaLogMs = 0;
    bool m_directStreamMode = false;
    bool m_useOnvifMetadata = true;
    std::thread m_metadataThread;
    std::atomic_bool m_metadataRunning{false};
    std::atomic_llong m_metaFrameCounter{0};
    std::atomic_llong m_lastMetaTimestamp{0};

    bool m_useCameraCgiControl;
    bool m_cameraCgiAllowInsecureTls;
    QString m_cameraBrightnessCgiUrlTemplate;
    QString m_cameraContrastCgiUrlTemplate;
    QString m_cameraCgiUser;
    QString m_cameraCgiPassword;
    QNetworkAccessManager *m_cgiNetworkManager;
    QTimer *m_brightnessCgiDebounceTimer;
    QTimer *m_contrastCgiDebounceTimer;

#ifdef SFEPS_HAVE_OPENCV
    LiveFrameProvider *m_liveProvider = nullptr;
    NativeMetadataTracker m_nativeTracker;
    RbfTps2D m_rbfPan;
    RbfTps2D m_rbfTilt;
    bool m_rbfOk = false;
    KalmanBbox2D m_kfPwm;
    int m_prevPan = 1500;
    int m_prevTilt = 1500;
    QString m_prevSelPwm;
    int m_panMin = 500;
    int m_panMax = 2500;
    int m_tiltMin = 500;
    int m_tiltMax = 2500;
    double m_pwmRatio = 0.35;
    double m_pwmAlpha = 0.5;
    double m_predictMs = 300.0;
    int m_previewRevision = 0;
    std::atomic_int m_frameW{0};
    std::atomic_int m_frameH{0};
    std::thread m_opencvThread;
    std::atomic_bool m_opencvRunning{false};
    QTimer *m_pwmTimer = nullptr;
    qint64 m_lastPwmTickMs = 0;
#endif

private slots:
    void onUpdateTimerTimeout();
};

#endif // MAINWINDOW_H
