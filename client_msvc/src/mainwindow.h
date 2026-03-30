#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QObject>
#include <QMutex>
#include <QSet>
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
#include <vector>
#include <string>
#ifdef CAMERA_RBF_QT_MODE
// camera_RBF.cpp Qt 인터페이스 전방 선언 (rbf_pwm_core.h / native_metadata_tracker.h 불필요)
struct RbfQtBBox { float l{0}, t{0}, r{0}, b{0}; bool found{false}; };
bool rbfqt_init(double ratio, double alpha, double predict_ms,
                int pan_min, int pan_max, int tilt_min, int tilt_max);
void rbfqt_set_target_bbox(float l, float t, float r, float b, int W, int H);
void rbfqt_clear_target();
void rbfqt_set_tracked_nativeid(const char* nativeId);
void rbfqt_set_pose_aim(float u_px, float v_px, int valid);
bool rbfqt_compute_pwm(long long now_ms, int W, int H, int* pan, int* tilt);
void rbfqt_process_metadata(const std::vector<ParsedMetadataObject>& objects, int W, int H);
std::string rbfqt_find_native_id(const std::string& xmlId);
std::string rbfqt_find_stable_id(const std::string& nativeId);
std::string rbfqt_resolve_to_native_id(const std::string& displayOrXmlOrStable);
RbfQtBBox   rbfqt_get_bbox_by_xmlid(const std::string& xmlId, int W, int H);
#else
#include "native_metadata_tracker.h"
#include "rbf_pwm_core.h"
#endif
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

#ifdef SFEPS_HAVE_OPENCV
    // XML ID(서버 FRAUD ID "1071432")로 객체 추적 시작
    // 1순위: rbfqt_process_metadata()가 갱신한 현재 bbox 사용
    // 2순위(fallback): 서버 Fraud 메시지의 픽셀 좌표 (객체가 화면에 없을 때)
    Q_INVOKABLE void trackByXmlId(const QString &xmlId,
                                  float fallbackL = 0, float fallbackT = 0,
                                  float fallbackR = 0, float fallbackB = 0);
    // QML Track 버튼 → N-ID로 직접 추적 시작 (매 tick 자동 bbox 갱신)
    Q_INVOKABLE void trackByNativeId(const QString &nativeId);
    Q_INVOKABLE void clearRbfTarget();

    // Fraud 알림: xmlId 추가/삭제 (bbox 색상 빨간색 표시 및 자동 전환 트리거)
    Q_INVOKABLE void addFraudXmlId(const QString &xmlId);
    Q_INVOKABLE void removeFraudXmlId(const QString &xmlId);

    // 현재 활성 객체에서 xmlId 에 해당하는 bbox 반환 (QML/외부 용)
    // 반환 맵 키: found(bool), x, y, w, h (정규화 [0,1])
    Q_INVOKABLE QVariantMap getBBoxByXmlId(const QString &xmlId) const;
#endif

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
    // 현재 RBF 추적 중인 XML ID 변화 (빈 문자열 = 추적 없음)
    // FraudManager.setActiveTrackingId 에 연결하여 대기큐 동기화에 사용
    void trackingXmlIdChanged(const QString &xmlId);
#ifdef SFEPS_HAVE_OPENCV
    void previewRevisionChanged();
    void pwmSetRequested(int pan, int tilt);
#endif

#ifdef CAMERA_RBF_QT_MODE
    // 자동 레이저 대상이 화면 밖으로 나가 추적을 중지했을 때
    // (Position server에 TRACK_END 보내기 용도)
    void laserTrackStopped(const QString &xmlId);
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
    // Fraud 알림으로 표시된 XML ID 세트 (빨간 bbox + 자동 추적 후보)
    QSet<QString> m_fraudXmlIds;
    // 수동 추적 활성 여부 (Track 버튼으로 시작된 경우 true → fraud 자동 전환 억제)
    bool m_manualTracking{false};
#ifdef CAMERA_RBF_QT_MODE
    // 부정승차 자동 레이저가 따라갈 ONVIF XML ID (fraudAutoTrackRequest→trackByXmlId 시만 설정, 추가 FRAUD는 색만)
    QString m_fraudLaserStickyXmlId;
    // NativeTrack이 일시적으로 스왑돼도(겹침) 안정적으로 유지하기 위한 IdStabilizer stable_id(S_xxx)
    QString m_fraudLaserStickyStableId;
    int m_fraudLaserStickyStableMissingFrames{0};
#endif

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
    bool m_rbfOk = false;
    int m_prevPan = 1500;
    int m_prevTilt = 1500;
    QString m_prevSelPwm;
    int m_panMin = 500;
    int m_panMax = 2500;
    int m_tiltMin = 500;
    int m_tiltMax = 2500;
    double m_pwmRatio = 0.35;
    // pose(어깨->아래)에서 목표 v를 만드는 비율
    // 0이면 어깨 중심, 1이면 sticky bbox bottom까지(=어깨->bbox bottom 구간 끝)
    double m_poseDownRatio = 0.35;
    double m_pwmAlpha = 0.5;
    double m_predictMs = 300.0;
    int m_previewRevision = 0;
    std::atomic_int m_frameW{0};
    std::atomic_int m_frameH{0};
    std::thread m_opencvThread;
    std::atomic_bool m_opencvRunning{false};
    QTimer *m_pwmTimer = nullptr;
    qint64 m_lastPwmTickMs = 0;
#ifndef CAMERA_RBF_QT_MODE
    // 레거시 모드: native_metadata_tracker + rbf_pwm_core 직접 사용
    NativeMetadataTracker m_nativeTracker;
    RbfTps2D m_rbfPan;
    RbfTps2D m_rbfTilt;
    KalmanBbox2D m_kfPwm;
#endif
#endif

private slots:
    void onUpdateTimerTimeout();
};

#endif // MAINWINDOW_H