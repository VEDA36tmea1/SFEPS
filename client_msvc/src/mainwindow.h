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

private:
    void updateStreamStatus(const QString &status, bool connected);
    void startMetadataWorker();
    void stopMetadataWorker();
    void scheduleBrightnessCgiUpdate();
    void scheduleContrastCgiUpdate();
    void sendBrightnessCgi();
    void sendContrastCgi();
    void fetchCameraImageSettings();

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

private slots:
    void onUpdateTimerTimeout();
};

#endif // MAINWINDOW_H
