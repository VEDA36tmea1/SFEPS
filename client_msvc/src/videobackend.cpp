#include "mainwindow.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "RTSPClient.h"
#include "XMLParser.h"
#include "Config.h"
#include <QDebug>
#include <QProcessEnvironment>
#include <QDateTime>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QUrl>
#include <QByteArray>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QMetaObject>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <sstream>
#include <thread>
#ifdef SFEPS_HAVE_OPENCV
#include <memory>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include "live_frame_provider.h"
#ifndef CAMERA_RBF_QT_MODE
#include "rbf_pwm_core.h"          // 레거시 모드에서만 필요
#include "native_metadata_tracker.h"
#endif
#include <QImage>
#include <QThread>
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#endif
#endif

namespace {

#ifdef CAMERA_RBF_QT_MODE
#ifdef SFEPS_HAVE_OPENCV
// 최신 원본 프레임( pose crop용 )을 저장해둔다.
static std::mutex g_latestFrameMutex;
static cv::Mat g_latestFrame;

// Qt 모드용 MediaPipe Pose worker (crop JPEG -> shoulder center -> aim u/v)
// - 동기 호출하지 않고 background thread에서 처리
// - 요청은 1-slot만 유지(최신 값만 반영)
struct QtPoseWorker
{
    std::atomic_bool running{false};

#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE child_stdin_write{NULL};
    HANDLE child_stdout_read{NULL};
#endif

    std::thread worker_thread;
    std::mutex reqMutex;
    std::condition_variable reqCv;
    bool reqPending{false};
    bool stopFlag{false};

    // 요청 데이터
    std::vector<uchar> reqJpeg;
    double reqCropX{0}, reqCropY{0}, reqCropW{0}, reqCropH{0};
    double reqBboxBottomY{0};  // padding 제외 bbox bottom (원본 frame 픽셀)
    double reqRatioDown{0.35};
    qint64 reqSubmitWallMs{0}; // submit() 시각 (wall clock)

    std::mutex resMutex;
    bool aimValid{false};
    double aimU{0}, aimV{0};
    qint64 aimWallMs{0};
    qint64 aimPoseRttMs{0}; // submit() -> aimWallMs RTT (wall clock)

    static std::string get_exe_dir()
    {
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n == 0 || n == MAX_PATH) return "";
        return std::filesystem::path(buf).parent_path().string();
#else
        return "";
#endif
    }

    std::string resolve_python_exe(const std::string& exeDir) const
    {
        // camera_RBF standalone과 비슷하게 여러 후보를 탐색
#ifdef _WIN32
        std::vector<std::filesystem::path> bases;
        bases.push_back(exeDir);
        for (int up = 0; up < 6; ++up) {
            std::filesystem::path p = std::filesystem::path(exeDir);
            for (int i = 0; i < up; ++i) p = p.parent_path();
            bases.push_back(p);
        }
        for (const auto& b : bases) {
            auto p1 = b / ".venv" / "Scripts" / "python.exe";
            auto p2 = b / "Camera" / "get_metadata" / ".venv" / "Scripts" / "python.exe";
            if (std::filesystem::exists(p1)) return p1.string();
            if (std::filesystem::exists(p2)) return p2.string();
        }
        return "python";
#else
        return "python3";
#endif
    }

    std::string resolve_script_path(const std::string& exeDir) const
    {
#ifdef _WIN32
        std::vector<std::filesystem::path> bases;
        bases.push_back(exeDir);
        for (int up = 0; up < 6; ++up) {
            std::filesystem::path p = std::filesystem::path(exeDir);
            for (int i = 0; i < up; ++i) p = p.parent_path();
            bases.push_back(p);
        }
        for (const auto& b : bases) {
            auto s1 = b / "Camera" / "get_metadata" / "src" / "mediapipe_pose_worker.py";
            if (std::filesystem::exists(s1)) return s1.string();
        }
        return "";
#else
        return "";
#endif
    }

#ifdef _WIN32
    void start_process()
    {
        const std::string exeDir = get_exe_dir();
        const std::string py = resolve_python_exe(exeDir);
        const std::string script = resolve_script_path(exeDir);
        if (script.empty()) return;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE child_stdout_read_tmp = NULL;
        HANDLE child_stdout_write = NULL;
        HANDLE child_stdin_read = NULL;
        HANDLE child_stdin_write_tmp = NULL;

        if (!CreatePipe(&child_stdout_read_tmp, &child_stdout_write, &sa, 0)) return;
        if (!SetHandleInformation(child_stdout_read_tmp, HANDLE_FLAG_INHERIT, 0)) return;
        if (!CreatePipe(&child_stdin_read, &child_stdin_write_tmp, &sa, 0)) return;
        if (!SetHandleInformation(child_stdin_write_tmp, HANDLE_FLAG_INHERIT, 0)) return;

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_stdin_read;
        si.hStdOutput = child_stdout_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::string cmd = "\"" + py + "\" \"" + script + "\"";
        std::vector<char> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back('\0');

        BOOL ok = CreateProcessA(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        if (!ok) return;

        child_stdin_write = child_stdin_write_tmp;
        child_stdout_read = child_stdout_read_tmp;
        running = true;
    }
#endif

    void worker_loop()
    {
        // 프로세스 시작
#ifdef _WIN32
        start_process();
        if (!running) return;
#else
        running = false;
        return;
#endif

        while (true)
        {
            std::vector<uchar> jpeg;
            double cropX = 0, cropY = 0, cropW = 0, cropH = 0;
            double bboxBottomY = 0;
            double ratioDown = 0.35;
            qint64 submitWallMs = 0;

            {
                std::unique_lock<std::mutex> lk(reqMutex);
                reqCv.wait(lk, [&] { return reqPending || stopFlag; });
                if (stopFlag) break;
                jpeg = std::move(reqJpeg);
                cropX = reqCropX; cropY = reqCropY; cropW = reqCropW; cropH = reqCropH;
                bboxBottomY = reqBboxBottomY;
                ratioDown = reqRatioDown;
                submitWallMs = reqSubmitWallMs;
                reqPending = false;
            }

            if (jpeg.empty() || !running) continue;

            // 1) Python worker에 JPEG 보내기 (4바이트 len + bytes)
            const uint32_t jpeg_len = static_cast<uint32_t>(jpeg.size());
            uint8_t hdr[4];
            hdr[0]=jpeg_len&0xFF; hdr[1]=(jpeg_len>>8)&0xFF; hdr[2]=(jpeg_len>>16)&0xFF; hdr[3]=(jpeg_len>>24)&0xFF;

#ifdef _WIN32
            DWORD wrote = 0;
            if (!WriteFile(child_stdin_write, hdr, 4, &wrote, nullptr) || wrote != 4) { running = false; break; }
            DWORD total = 0;
            while (total < jpeg_len) {
                DWORD remain = jpeg_len - total;
                DWORD chunk = 0;
                if (!WriteFile(child_stdin_write, jpeg.data() + total, remain, &chunk, nullptr) || chunk == 0) {
                    running = false; break;
                }
                total += chunk;
            }
            if (!running) break;

            // 2) stdout 한 줄 읽기 (newline까지)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            std::string line;
            char c = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                DWORD avail = 0;
                if (!PeekNamedPipe(child_stdout_read, nullptr, 0, nullptr, &avail, nullptr)) { running = false; break; }
                if (avail == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                DWORD got = 0;
                if (!ReadFile(child_stdout_read, &c, 1, &got, nullptr) || got != 1) { running = false; break; }
                if (c == '\n') break;
                line.push_back(c);
            }
            if (!running) break;
            if (line.empty()) continue;
            if (line[0] == '0') continue;

            // 3) 파싱: "1 x0 y0 v0 ... x32 y32 v32"
            std::istringstream iss(line);
            int ok = 0;
            if (!(iss >> ok) || ok == 0) continue;

            float x[33]{0}, y[33]{0}, v[33]{0};
            for (int i = 0; i < 33; i++) {
                if (!(iss >> x[i] >> y[i] >> v[i])) { ok = 0; break; }
            }
            if (!ok) continue;

            const int iL = 11, iR = 12;
            const float wsum = v[iL] + v[iR] + 1e-6f;
            const double sx = (x[iL] * v[iL] + x[iR] * v[iR]) / wsum;
            const double sy = (y[iL] * v[iL] + y[iR] * v[iR]) / wsum;

            const double shoulderX = cropX + sx * cropW;
            const double shoulderY = cropY + sy * cropH;
            const double aimU_local = shoulderX;
            const double aimV_local = shoulderY + (bboxBottomY - shoulderY) * ratioDown;

            {
                std::lock_guard<std::mutex> lk(resMutex);
                aimU = aimU_local;
                aimV = aimV_local;
                aimValid = true;
                aimWallMs = QDateTime::currentMSecsSinceEpoch();
                aimPoseRttMs = (submitWallMs > 0) ? std::max<qint64>(0, aimWallMs - submitWallMs) : 0;
            }
#endif
        }
    }

    void ensure_started()
    {
        static std::once_flag once;
        std::call_once(once, [&] {
            stopFlag = false;
            running = false;
            worker_thread = std::thread([this] { worker_loop(); });
        });
    }

    // pose request
    void submit(const std::vector<uchar> &jpeg, double cropX, double cropY, double cropW, double cropH,
                double bboxBottomY, double ratioDown)
    {
        ensure_started();
        if (jpeg.empty()) return;
        if (!running.load()) return;
        {
            std::lock_guard<std::mutex> lk(reqMutex);
            reqJpeg = jpeg;
            reqCropX = cropX; reqCropY = cropY; reqCropW = cropW; reqCropH = cropH;
            reqBboxBottomY = bboxBottomY;
            reqRatioDown = ratioDown;
            reqSubmitWallMs = QDateTime::currentMSecsSinceEpoch();
            reqPending = true;
        }
        reqCv.notify_one();
    }

    // 최신 aim 얻기
    bool getAim(double &outU, double &outV, qint64 &outMs)
    {
        std::lock_guard<std::mutex> lk(resMutex);
        if (!aimValid) return false;
        outU = aimU; outV = aimV; outMs = aimWallMs;
        return true;
    }

    qint64 getPoseRttMs()
    {
        std::lock_guard<std::mutex> lk(resMutex);
        return aimPoseRttMs;
    }

    void clearAim()
    {
        std::lock_guard<std::mutex> lk(resMutex);
        aimValid = false;
    }

    void stop()
    {
#ifdef _WIN32
        {
            std::lock_guard<std::mutex> lk(reqMutex);
            stopFlag = true;
            reqPending = true;
        }
        reqCv.notify_all();
        if (worker_thread.joinable()) worker_thread.join();
        if (child_stdin_write) { CloseHandle(child_stdin_write); child_stdin_write = NULL; }
        if (child_stdout_read) { CloseHandle(child_stdout_read); child_stdout_read = NULL; }
        if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); pi.hProcess = NULL; }
#endif
        running = false;
    }
};

static QtPoseWorker g_qtPoseWorker;
#endif // SFEPS_HAVE_OPENCV
#endif // CAMERA_RBF_QT_MODE

bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

int clampPercent(int value)
{
    if (value < 1) return 1;
    if (value > 100) return 100;
    return value;
}

bool parseOnvifUtcEpochMs(const std::string &tagTime, long long &outEpochMs)
{
    if (tagTime.empty()) return false;

    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0, msec = 0;
    int parsed = std::sscanf(tagTime.c_str(),
                             "%d-%d-%dT%d:%d:%d.%d",
                             &year, &mon, &day, &hour, &min, &sec, &msec);
    if (parsed < 6) {
        parsed = std::sscanf(tagTime.c_str(),
                             "%d-%d-%dT%d:%d:%d",
                             &year, &mon, &day, &hour, &min, &sec);
        if (parsed < 6) return false;
        msec = 0;
    }

    std::tm tmUtc{};
    tmUtc.tm_year = year - 1900;
    tmUtc.tm_mon = mon - 1;
    tmUtc.tm_mday = day;
    tmUtc.tm_hour = hour;
    tmUtc.tm_min = min;
    tmUtc.tm_sec = sec;

#ifdef _WIN32
    const std::time_t utcSec = _mkgmtime(&tmUtc);
#else
    const std::time_t utcSec = timegm(&tmUtc);
#endif
    if (utcSec < 0) return false;

    outEpochMs = static_cast<long long>(utcSec) * 1000LL + static_cast<long long>(msec);
    return true;
}

int computeStreamLatencyMsFromXmlTagTime(const std::string &xml)
{
    const std::size_t utcPos = xml.find("UtcTime=\"");
    if (utcPos == std::string::npos) return -1;

    const std::size_t start = utcPos + 9;
    const std::size_t end = xml.find("\"", start);
    if (end == std::string::npos || end <= start) return -1;

    const std::string tagTime = xml.substr(start, end - start);
    long long tagEpochMs = 0;
    if (!parseOnvifUtcEpochMs(tagTime, tagEpochMs)) return -1;

    const long long nowEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    long long latencyMs = nowEpochMs - tagEpochMs;
    if (latencyMs < 0) latencyMs = 0;
    if (latencyMs > static_cast<long long>(std::numeric_limits<int>::max())) {
        latencyMs = static_cast<long long>(std::numeric_limits<int>::max());
    }
    return static_cast<int>(latencyMs);
}
}

MainWindow::MainWindow(QObject *parent)
    : QObject(parent),
      m_updateTimer(new QTimer(this)),
      m_running(false),
      m_brightness(53),
      m_contrast(52),
      m_streamStatus("STOPPED"),
      m_streamConnected(false),
      m_hasPendingDetections(false),
      m_useCameraCgiControl(false),
      m_cameraCgiAllowInsecureTls(true),
      m_cgiNetworkManager(nullptr),
      m_brightnessCgiDebounceTimer(nullptr),
      m_contrastCgiDebounceTimer(nullptr)
{
    m_updateTimer->setSingleShot(true);
    connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::onUpdateTimerTimeout);

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    m_directStreamMode = parseEnvBool(env, "SFEPS_DIRECT_STREAM_MODE", false);
    m_useOnvifMetadata = parseEnvBool(env, "SFEPS_USE_ONVIF_METADATA", true);
    m_cameraBrightnessCgiUrlTemplate = env.value(
        "CAMERA_BRIGHTNESS_CGI_URL",
        "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Brightness={value}").trimmed();
    m_cameraContrastCgiUrlTemplate = env.value(
        "CAMERA_CONTRAST_CGI_URL",
        "https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=set&Contrast={value}").trimmed();
    m_cameraCgiUser = env.value("CAMERA_CGI_USER", "admin").trimmed();
    m_cameraCgiPassword = env.value("CAMERA_CGI_PASSWORD", "admin");
    m_cameraCgiAllowInsecureTls = parseEnvBool(env, "CAMERA_CGI_ALLOW_INSECURE_TLS", true);
    m_useCameraCgiControl = !m_cameraBrightnessCgiUrlTemplate.isEmpty() || !m_cameraContrastCgiUrlTemplate.isEmpty();

    if (m_useCameraCgiControl) {
        m_cgiNetworkManager = new QNetworkAccessManager(this);
        connect(m_cgiNetworkManager, &QNetworkAccessManager::authenticationRequired,
                this, [this](QNetworkReply *, QAuthenticator *auth) {
            auth->setUser(m_cameraCgiUser);
            auth->setPassword(m_cameraCgiPassword);
        });

        m_brightnessCgiDebounceTimer = new QTimer(this);
        m_brightnessCgiDebounceTimer->setSingleShot(true);
        m_brightnessCgiDebounceTimer->setInterval(150);
        connect(m_brightnessCgiDebounceTimer, &QTimer::timeout, this, &MainWindow::sendBrightnessCgi);

        m_contrastCgiDebounceTimer = new QTimer(this);
        m_contrastCgiDebounceTimer->setSingleShot(true);
        m_contrastCgiDebounceTimer->setInterval(150);
        connect(m_contrastCgiDebounceTimer, &QTimer::timeout, this, &MainWindow::sendContrastCgi);

        QTimer::singleShot(0, this, &MainWindow::fetchCameraImageSettings);
    }

#ifdef SFEPS_HAVE_OPENCV
    m_pwmTimer = new QTimer(this);
    m_pwmTimer->setInterval(33);
    connect(m_pwmTimer, &QTimer::timeout, this, &MainWindow::onPwmTick);
    m_pwmTimer->start();

    bool okParse = true;
    m_pwmRatio = env.value(QStringLiteral("SFEPS_RBF_RATIO"), QStringLiteral("0.35")).toDouble(&okParse);
    if (!okParse)
        m_pwmRatio = 0.35;
    m_pwmAlpha = env.value(QStringLiteral("SFEPS_RBF_ALPHA"), QStringLiteral("0.5")).toDouble(&okParse);
    if (!okParse)
        m_pwmAlpha = 0.5;
    m_predictMs = env.value(QStringLiteral("SFEPS_RBF_PREDICT_MS"), QStringLiteral("300")).toDouble(&okParse);
    if (!okParse)
        m_predictMs = 300.0;

    // pose(어깨 중심 아래) 목표 v 비율
    // shoulder_y + (bboxBottom - shoulder_y) * m_poseDownRatio
    // 여기서 bboxBottom은 sticky bbox의 bottom(패딩 제외)입니다.
    m_poseDownRatio = env.value(QStringLiteral("SFEPS_POSE_DOWN_RATIO"), QStringLiteral("0.35")).toDouble(&okParse);
    if (!okParse)
        m_poseDownRatio = 0.35;
    m_panMin = env.value(QStringLiteral("SFEPS_PWM_PAN_MIN"), QStringLiteral("500")).toInt();
    m_panMax = env.value(QStringLiteral("SFEPS_PWM_PAN_MAX"), QStringLiteral("2500")).toInt();
    m_tiltMin = env.value(QStringLiteral("SFEPS_PWM_TILT_MIN"), QStringLiteral("500")).toInt();
    m_tiltMax = env.value(QStringLiteral("SFEPS_PWM_TILT_MAX"), QStringLiteral("2500")).toInt();

#ifdef CAMERA_RBF_QT_MODE
    // camera_RBF Qt 모드: rbfqt_init 으로 초기화 (loadCalibPointsBuiltin 내부 사용)
    m_rbfOk = rbfqt_init(m_pwmRatio, m_pwmAlpha, m_predictMs,
                         m_panMin, m_panMax, m_tiltMin, m_tiltMax);
    if (!m_rbfOk)
        qWarning() << "[rbfqt] init failed — PWM/RBF disabled";
    else
        qDebug() << "[SFEPS] camera_RBF Qt mode active (rbfqt_init OK)";
#else
    const auto pts = loadCalibPointsBuiltin();
    std::vector<cv::Point2d> px;
    std::vector<double> pan_y, tilt_y;
    for (const auto &p : pts) {
        px.emplace_back(p.u, p.v);
        pan_y.push_back(p.pan);
        tilt_y.push_back(p.tilt);
    }
    m_rbfOk = m_rbfPan.fit(px, pan_y) && m_rbfTilt.fit(px, tilt_y);
    if (!m_rbfOk)
        qWarning() << "[RBF] fit failed — PWM/RBF disabled";
    else
        qDebug() << "[SFEPS] OpenCV preview + native metadata track + RBF/PWM path active";
#endif
#endif
}

MainWindow::~MainWindow()
{
    setRunning(false);
#ifdef SFEPS_HAVE_OPENCV
    m_opencvRunning.store(false, std::memory_order_release);
    if (m_opencvThread.joinable())
        m_opencvThread.join();
#endif
    stopMetadataWorker();
#if defined(CAMERA_RBF_QT_MODE) && defined(SFEPS_HAVE_OPENCV)
    // pose worker 중복 실행/백그라운드 프로세스 남김 방지
    g_qtPoseWorker.stop();
#endif
}

void MainWindow::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    emit runningChanged();

    if (m_running) {
        updateStreamStatus("CONNECTING", false);
        if (m_useOnvifMetadata)
            startMetadataWorker();
#ifdef SFEPS_HAVE_OPENCV
        m_opencvRunning.store(true, std::memory_order_release);
        if (m_opencvThread.joinable())
            m_opencvThread.join();
        m_opencvThread = std::thread(&MainWindow::opencvCaptureLoop, this);
#endif
    } else {
#ifdef SFEPS_HAVE_OPENCV
        m_opencvRunning.store(false, std::memory_order_release);
        if (m_opencvThread.joinable())
            m_opencvThread.join();
#endif
        stopMetadataWorker();
        updateStreamStatus("STOPPED", false);
    }
}

void MainWindow::setBrightness(int brightness)
{
    brightness = clampPercent(brightness);
    if (m_brightness == brightness) return;
    m_brightness = brightness;
    emit brightnessChanged();
    scheduleBrightnessCgiUpdate();
}

void MainWindow::setContrast(int contrast)
{
    contrast = clampPercent(contrast);
    if (m_contrast == contrast) return;
    m_contrast = contrast;
    emit contrastChanged();
    scheduleContrastCgiUpdate();
}

void MainWindow::setDetections(const QVariantList &list)
{
    QMutexLocker locker(&m_mutex);
    m_pendingDetections = list;
    m_hasPendingDetections = true;
    if (m_updateTimer && !m_updateTimer->isActive()) {
        m_updateTimer->start(16);
    }
}

void MainWindow::onUpdateTimerTimeout()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_hasPendingDetections) {
            m_hasPendingDetections = false;
            if (m_detections != m_pendingDetections) {
                m_detections = m_pendingDetections;
                emit detectionsChanged();
            }
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 latestMetaMs = -1;
    {
        QMutexLocker locker(&m_mutex);
        for (const QVariant &v : m_detections) {
            if (!v.canConvert<QVariantMap>()) continue;
            const QVariantMap m = v.toMap();
            const qint64 metaMs = m.value("metaTsMs").toLongLong();
            if (metaMs > latestMetaMs) latestMetaMs = metaMs;
        }
    }
    const int delayMs = (latestMetaMs > 0) ? int(nowMs - latestMetaMs) : -1;
    if (delayMs != m_videoMetaDelayMs) {
        m_videoMetaDelayMs = delayMs;
        emit videoMetaDelayChanged();
    }
    if (delayMs >= 0 && (nowMs - m_lastVideoMetaLogMs) >= 1000) {
        m_lastVideoMetaLogMs = nowMs;
        qDebug() << "[VIDEO-META]" << delayMs << "ms";
    }
}

void MainWindow::clearDetections()
{
    QMutexLocker locker(&m_mutex);
    m_detections.clear();
    m_selectedDetectionId.clear();
    emit detectionsChanged();
    emit selectedDetectionChanged();
}

void MainWindow::setSelectedDetection(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (m_selectedDetectionId == id) return;
    m_selectedDetectionId = id;
    emit selectedDetectionChanged();
}

void MainWindow::setExternalTrackedId(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (m_externalTrackedId == id) return;
    m_externalTrackedId = id;
    emit externalTrackedIdChanged();
}

void MainWindow::updateStreamStatus(const QString &status, bool connected)
{
    if (m_streamStatus != status) {
        m_streamStatus = status;
        emit streamStatusChanged();
    }
    if (m_streamConnected != connected) {
        m_streamConnected = connected;
        emit streamConnectedChanged();
    }
}

void MainWindow::startMetadataWorker()
{
    if (m_metadataRunning.load(std::memory_order_acquire)) return;
    m_metadataRunning.store(true, std::memory_order_release);
    m_metadataThread = std::thread([this]() {
        RTSPClient client;
        XMLParser parser;
        if (!client.connectToCamera()) {
            QMetaObject::invokeMethod(this, [this]() { updateStreamStatus("DISCONNECTED", false); }, Qt::QueuedConnection);
            m_metadataRunning.store(false, std::memory_order_release);
            return;
        }
        client.sendHandshake();
        QMetaObject::invokeMethod(this, [this]() { updateStreamStatus("ONLINE", true); }, Qt::QueuedConnection);

        unsigned char header[4];
        std::string accumulatedXml;
        unsigned int lastTimestamp = 0;
        std::vector<char> bigBuffer(65536);
        socket_t sock = client.getSocket();
#ifdef _WIN32
        DWORD tv = 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        while (m_metadataRunning.load(std::memory_order_acquire))
        {
            client.sendHeartbeat();
            const int readLen = recv(sock, reinterpret_cast<char*>(header), 4, MSG_WAITALL);
            if (readLen <= 0) break;
            if (header[0] != '$') continue;

            const int channel = static_cast<int>(header[1]);
            const int payloadLen = (static_cast<int>(header[2]) << 8) | static_cast<int>(header[3]);
            int totalRead = 0;
            while (totalRead < payloadLen) {
                int toRead = payloadLen - totalRead;
                if (toRead > static_cast<int>(bigBuffer.size())) toRead = static_cast<int>(bigBuffer.size());
                int r = recv(sock, bigBuffer.data() + totalRead, toRead, 0);
                if (r <= 0) { totalRead = 0; break; }
                totalRead += r;
            }
            if (totalRead <= 12) continue;
            if (channel != 2) continue;

            auto *rtp = reinterpret_cast<unsigned char*>(bigBuffer.data());
            unsigned int currentTimestamp =
                (rtp[4] << 24) | (rtp[5] << 16) | (rtp[6] << 8) | rtp[7];
            char *xmlData = bigBuffer.data() + 12;
            int xmlLen = totalRead - 12;

            if (currentTimestamp != lastTimestamp && lastTimestamp != 0)
            {
                const int metaLatencyMs = computeStreamLatencyMsFromXmlTagTime(accumulatedXml);
                auto humans = parser.parseHumanObjectsForAnalytics(accumulatedXml, false);
                accumulatedXml.clear();

                const qint64 frameNo = m_metaFrameCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
                m_lastMetaTimestamp.store(static_cast<long long>(lastTimestamp), std::memory_order_release);
#ifdef SFEPS_HAVE_OPENCV
                const unsigned int rtpSnap = lastTimestamp;
                auto payload = std::make_shared<std::vector<ParsedMetadataObject>>(std::move(humans));
                QMetaObject::invokeMethod(this, [this, payload, rtpSnap, frameNo, metaLatencyMs]() {
                    if (metaLatencyMs >= 0 && m_streamLatencyMs != metaLatencyMs) {
                        m_streamLatencyMs = metaLatencyMs;
                        emit streamLatencyChanged();
                        // qInfo() << "[StreamLatency][tag-time]" << m_streamLatencyMs << "ms";
                    }
                    applyNativeDetections(std::move(*payload), rtpSnap, QDateTime::currentMSecsSinceEpoch(), frameNo);
                }, Qt::QueuedConnection);
#else
                QVariantList dets;
                for (const auto &obj : humans)
                {
                    float l = obj.left;
                    float t = obj.top;
                    float r = obj.right;
                    float b = obj.bottom;
                    const float maxAbs = std::max(std::max(std::fabs(l), std::fabs(t)),
                                                  std::max(std::fabs(r), std::fabs(b)));
                    if (maxAbs > 2.0f) {
                        l /= SENSOR_WIDTH; r /= SENSOR_WIDTH;
                        t /= SENSOR_HEIGHT; b /= SENSOR_HEIGHT;
                    }
                    l = std::max(0.0f, std::min(1.0f, l));
                    r = std::max(0.0f, std::min(1.0f, r));
                    t = std::max(0.0f, std::min(1.0f, t));
                    b = std::max(0.0f, std::min(1.0f, b));

                    QVariantMap m;
                    m["id"] = QString::fromStdString(obj.id);
                    m["type"] = QString::fromStdString(obj.type);
                    m["x"] = l;
                    m["y"] = t;
                    m["w"] = std::max(0.0f, r - l);
                    m["h"] = std::max(0.0f, b - t);
                    m["metaFrameNo"] = frameNo;
                    m["metaTimestamp"] = static_cast<qlonglong>(lastTimestamp);
                    m["metaTsMs"] = QDateTime::currentMSecsSinceEpoch();
                    dets.push_back(m);
                }

                QMetaObject::invokeMethod(this, [this, dets, metaLatencyMs]() {
                    if (metaLatencyMs >= 0 && m_streamLatencyMs != metaLatencyMs) {
                        m_streamLatencyMs = metaLatencyMs;
                        emit streamLatencyChanged();
                        // qInfo() << "[StreamLatency][tag-time]" << m_streamLatencyMs << "ms";
                    }
                    this->setDetections(dets);
                }, Qt::QueuedConnection);
#endif
            }

            accumulatedXml.append(xmlData, xmlLen);
            lastTimestamp = currentTimestamp;
        }

        QMetaObject::invokeMethod(this, [this]() { updateStreamStatus("DISCONNECTED", false); }, Qt::QueuedConnection);
        m_metadataRunning.store(false, std::memory_order_release);
    });
}

void MainWindow::stopMetadataWorker()
{
    if (!m_metadataRunning.load(std::memory_order_acquire)) return;
    m_metadataRunning.store(false, std::memory_order_release);
    if (m_metadataThread.joinable()) {
        m_metadataThread.join();
    }
}

void MainWindow::fetchCameraImageSettings()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager) return;

    const QString base = QStringLiteral("https://192.168.0.84/stw-cgi/image.cgi?msubmenu=imageenhancements2&action=view");
    auto parseBody = [this](const QString &body) {
        for (const QString &line : body.split('\n')) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith("Brightness=", Qt::CaseInsensitive)) {
                bool ok = false;
                const int val = trimmed.mid(11).toInt(&ok);
                if (ok) {
                    m_brightness = clampPercent(val);
                    emit brightnessChanged();
                }
            } else if (trimmed.startsWith("Contrast=", Qt::CaseInsensitive)) {
                bool ok = false;
                const int val = trimmed.mid(9).toInt(&ok);
                if (ok) {
                    m_contrast = clampPercent(val);
                    emit contrastChanged();
                }
            }
        }
    };

    auto sendRequest = [this, parseBody](const QUrl &url, bool retriedFromHttps) {
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
        if (url.scheme().compare("https", Qt::CaseInsensitive) == 0 && m_cameraCgiAllowInsecureTls) {
            QSslConfiguration conf = request.sslConfiguration();
            conf.setPeerVerifyMode(QSslSocket::VerifyNone);
            request.setSslConfiguration(conf);
        }

        QNetworkReply *reply = m_cgiNetworkManager->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, url, retriedFromHttps, parseBody]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = (reply->error() == QNetworkReply::NoError) && (status >= 200 && status < 300);
            const QString body = ok ? QString::fromUtf8(reply->readAll()) : QString();
            reply->deleteLater();

            if (ok) {
                parseBody(body);
                return;
            }

            if (!retriedFromHttps && url.scheme().compare("https", Qt::CaseInsensitive) == 0) {
                QUrl httpUrl(url);
                httpUrl.setScheme("http");
                QNetworkRequest httpReq(httpUrl);
                httpReq.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
                QNetworkReply *httpReply = m_cgiNetworkManager->get(httpReq);
                connect(httpReply, &QNetworkReply::finished, this, [httpReply, parseBody]() {
                    const int httpStatus = httpReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const bool httpOk = (httpReply->error() == QNetworkReply::NoError) && (httpStatus >= 200 && httpStatus < 300);
                    const QString httpBody = httpOk ? QString::fromUtf8(httpReply->readAll()) : QString();
                    httpReply->deleteLater();
                    if (httpOk) parseBody(httpBody);
                });
                return;
            }
        });
    };

    sendRequest(QUrl(base), false);
}

void MainWindow::scheduleBrightnessCgiUpdate()
{
    if (!m_useCameraCgiControl || !m_brightnessCgiDebounceTimer || m_cameraBrightnessCgiUrlTemplate.isEmpty()) return;
    m_brightnessCgiDebounceTimer->start();
}

void MainWindow::scheduleContrastCgiUpdate()
{
    if (!m_useCameraCgiControl || !m_contrastCgiDebounceTimer || m_cameraContrastCgiUrlTemplate.isEmpty()) return;
    m_contrastCgiDebounceTimer->start();
}

void MainWindow::sendBrightnessCgi()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager || m_cameraBrightnessCgiUrlTemplate.isEmpty()) return;

    QString urlText = m_cameraBrightnessCgiUrlTemplate;
    urlText.replace("{value}", QString::number(m_brightness));
    const QUrl url(urlText);
    if (!url.isValid()) return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
    if (url.scheme().compare("https", Qt::CaseInsensitive) == 0 && m_cameraCgiAllowInsecureTls) {
        QSslConfiguration conf = request.sslConfiguration();
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(conf);
    }
    QNetworkReply *reply = m_cgiNetworkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

void MainWindow::sendContrastCgi()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager || m_cameraContrastCgiUrlTemplate.isEmpty()) return;

    QString urlText = m_cameraContrastCgiUrlTemplate;
    urlText.replace("{value}", QString::number(m_contrast));
    const QUrl url(urlText);
    if (!url.isValid()) return;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
    if (url.scheme().compare("https", Qt::CaseInsensitive) == 0 && m_cameraCgiAllowInsecureTls) {
        QSslConfiguration conf = request.sslConfiguration();
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(conf);
    }
    QNetworkReply *reply = m_cgiNetworkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

#ifdef SFEPS_HAVE_OPENCV

// ── camera_RBF Qt 인터페이스 연동 ─────────────────────────────────────────

void MainWindow::trackByNativeId(const QString &nativeId)
{
#ifdef CAMERA_RBF_QT_MODE
    // 수동 추적 모드에선 pose aim 오버라이드를 기본값으로 복귀
    g_qtPoseWorker.clearAim();
    rbfqt_set_pose_aim(0.0f, 0.0f, 0);
    rbfqt_set_tracked_nativeid(nativeId.toStdString().c_str());
    m_manualTracking = !nativeId.isEmpty();
    // pose worker bbox 탐색에 사용할 display ID를 저장 (빈 문자열이면 pose worker 중지)
    m_manualTrackDisplayId = nativeId;
    qDebug() << "[MainWindow] trackByNativeId" << nativeId
             << "→ manualTracking=" << m_manualTracking
             << " manualTrackDisplayId=" << m_manualTrackDisplayId;
#endif
}

void MainWindow::addFraudXmlId(const QString &xmlId)
{
    QMutexLocker lk(&m_mutex);
    m_fraudXmlIds.insert(xmlId);
}

void MainWindow::removeFraudXmlId(const QString &xmlId)
{
    QMutexLocker lk(&m_mutex);
    m_fraudXmlIds.remove(xmlId);
}

void MainWindow::trackByXmlId(const QString &xmlId,
                              float fallbackL, float fallbackT,
                              float fallbackR, float fallbackB)
{
    const int W = m_frameW.load() > 0 ? m_frameW.load() : 1920;
    const int H = m_frameH.load() > 0 ? m_frameH.load() : 1080;
#ifdef CAMERA_RBF_QT_MODE
    // 수동 추적 중에는 서버에서 오는 자동 trackByXmlId 요청이 들어와도
    // 현재 수동 target을 절대 덮어쓰지 않는다(수동 값이 최우선).
    if (m_manualTracking) {
        qDebug() << "[MainWindow] trackByXmlId ignored (manualTracking active):" << xmlId;
        return;
    }
    const std::string nativeId = rbfqt_find_native_id(xmlId.toStdString());
    auto nb = rbfqt_get_bbox_by_xmlid(xmlId.toStdString(), W, H);
    if (nb.found) {
        // 1순위: tracker에 있으면 N-ID로 auto-tracking 설정 (매 tick 자동 갱신)
        if (!nativeId.empty()) {
            rbfqt_set_tracked_nativeid(nativeId.c_str());
        } else {
            rbfqt_set_target_bbox(nb.l, nb.t, nb.r, nb.b, W, H);
        }
        qDebug() << "[MainWindow] trackByXmlId" << xmlId
                 << "nativeId=" << QString::fromStdString(nativeId)
                 << "bbox=(" << nb.l << nb.t << nb.r << nb.b << ")";
    } else if (fallbackR > fallbackL && fallbackB > fallbackT) {
        // 2순위: 서버 Fraud 메시지의 픽셀 좌표 (객체가 화면에 없을 때 fallback)
        // rbfqt_set_target_bbox는 픽셀 좌표도 자동 판별 (max > 1.5 이면 픽셀로 처리)
        rbfqt_set_tracked_nativeid("");  // auto-tracking 없음 (화면에 없으므로)
        rbfqt_set_target_bbox(fallbackL, fallbackT, fallbackR, fallbackB, W, H);
        qDebug() << "[MainWindow] trackByXmlId" << xmlId
                 << "server fallback bbox=(" << fallbackL << fallbackT
                 << fallbackR << fallbackB << ")";
    } else {
        // tracker에도 없고 fallback bbox도 없지만 sticky는 등록해 둔다.
        // onPwmTick이 다음 RTSP 메타데이터 tick에서 이 xmlId를 detections에서 찾으면 자동으로 추적 시작.
        qWarning() << "[MainWindow] trackByXmlId: xmlId not in tracker yet, registering sticky only:" << xmlId;
    }
    {
        QMutexLocker lk(&m_mutex);
        m_fraudLaserStickyXmlId = xmlId;
        // trackByXmlId 호출 시점에서 stable 매핑이 있으면 바로 고정
        std::string sid;
        if (!nativeId.empty())
            sid = rbfqt_find_stable_id(nativeId);
        m_fraudLaserStickyStableId = sid.empty() ? QString{} : QString::fromStdString(sid);
        m_fraudLaserStickyStableMissingFrames = 0;
    }
    emit trackingXmlIdChanged(xmlId);
#else
    const auto nb2 = m_nativeTracker.getBBoxByXmlId(xmlId);
    if (!nb2.found) {
        qWarning() << "[MainWindow] trackByXmlId: xmlId not found:" << xmlId;
        return;
    }
    const QString nid = m_nativeTracker.findNativeIdByXmlId(xmlId);
    if (!nid.isEmpty()) setSelectedDetection(nid);
#endif
}

void MainWindow::clearRbfTarget()
{
#ifdef CAMERA_RBF_QT_MODE
    // 수동 Untrack 버튼으로 호출되는 경우엔,
    // 수동 종료 이후에도 "이미 잡혀있는 fraud sticky target"을 다시 이어서 추적할 수 있게
    // m_fraudLaserSticky*는 유지한다.
    const bool wasManualTracking = m_manualTracking;
    rbfqt_set_tracked_nativeid("");
    rbfqt_clear_target();
    g_qtPoseWorker.clearAim();
    rbfqt_set_pose_aim(0.0f, 0.0f, 0);
    m_manualTracking = false;
    m_manualTrackDisplayId.clear();
    {
        QMutexLocker lk(&m_mutex);
        if (!wasManualTracking) {
            // 자동 추적 중지(clearRbfTarget이 간접 호출되는 경우)엔 sticky도 정리한다.
            m_fraudLaserStickyXmlId.clear();
            m_fraudLaserStickyStableId.clear();
        }
        // 공통: stable missing frame 카운터는 초기화
        m_fraudLaserStickyStableMissingFrames = 0;
    }
#else
    setSelectedDetection(QString{});
#endif
    emit trackingXmlIdChanged(QString{});
}

void MainWindow::setLaserTrackingEnabled(bool enabled)
{
#ifdef SFEPS_HAVE_OPENCV
    if (m_laserTrackingEnabled == enabled) return;
    m_laserTrackingEnabled = enabled;
#ifdef CAMERA_RBF_QT_MODE
    if (!enabled) {
        // 레이저/pose 추정을 멈추되, rbfqt_set_tracked_nativeid 자체는 유지해
        // 토글을 다시 ON했을 때 즉시 SET_PWM 계산이 재개되게 한다.
        g_qtPoseWorker.clearAim();
        rbfqt_set_pose_aim(0.0f, 0.0f, 0);

        const bool poseChanged = m_poseAimValid;
        m_poseAimU = 0.0;
        m_poseAimV = 0.0;
        m_poseAimValid = false;
        if (poseChanged) emit poseAimChanged();

        const bool rbfChanged = m_rbfTargetValid;
        m_rbfTargetU = 0.0;
        m_rbfTargetV = 0.0;
        m_rbfTargetValid = false;
        if (rbfChanged) emit rbfTargetChanged();
    }
#else
    // 레거시 모드에서는 pose 오버레이가 없으므로 플래그만 갱신.
#endif
#endif
}

QVariantMap MainWindow::getBBoxByXmlId(const QString &xmlId) const
{
    QVariantMap res;
    const int W = m_frameW.load() > 0 ? m_frameW.load() : 1920;
    const int H = m_frameH.load() > 0 ? m_frameH.load() : 1080;
#ifdef CAMERA_RBF_QT_MODE
    const auto nb = rbfqt_get_bbox_by_xmlid(xmlId.toStdString(), W, H);
    res["found"] = nb.found;
    if (nb.found) {
        res["x"] = nb.l;
        res["y"] = nb.t;
        res["w"] = static_cast<double>(nb.r - nb.l);
        res["h"] = static_cast<double>(nb.b - nb.t);
        res["xmlId"] = xmlId;
        res["nativeId"] = QString::fromStdString(rbfqt_find_native_id(xmlId.toStdString()));
    }
#else
    const auto nb = m_nativeTracker.getBBoxByXmlId(xmlId);
    res["found"] = nb.found;
    if (nb.found) {
        res["x"] = nb.l;
        res["y"] = nb.t;
        res["w"] = static_cast<double>(nb.r - nb.l);
        res["h"] = static_cast<double>(nb.b - nb.t);
        res["xmlId"] = xmlId;
        res["nativeId"] = m_nativeTracker.findNativeIdByXmlId(xmlId);
    }
#endif
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::applyNativeDetections(std::vector<ParsedMetadataObject> humans, unsigned int rtpTs, qint64 wallMs,
                                       qint64 frameNo)
{
    int W = m_frameW.load();
    int H = m_frameH.load();
    if (W <= 0) W = 1920;
    if (H <= 0) H = 1080;

#ifdef CAMERA_RBF_QT_MODE
    // camera_RBF.cpp 자체 NativeTrack tracker 사용 (native_metadata_tracker 불필요)
    rbfqt_process_metadata(humans, W, H);

    // UI에 표시할 QVariantList 직접 구성
    // QML VideoDisplay.qml 은 x/y/w/h 를 0~1 정규화 좌표로 기대함
    // ParsedMetadataObject.left/top/right/bottom 은 값에 따라:
    //   <= 1.5  → 이미 정규화 (0~1)
    //   >  1.5  → 4K 센서 픽셀 좌표 → 정규화로 변환 (Config.h: SENSOR_WIDTH=3840, SENSOR_HEIGHT=2160)
    static constexpr float kSensorW = 3840.0f;   // Config.h SENSOR_WIDTH
    static constexpr float kSensorH = 2160.0f;   // Config.h SENSOR_HEIGHT

    QSet<QString> fraudIds;
    { QMutexLocker lk(&m_mutex); fraudIds = m_fraudXmlIds; }

    QVariantList dets;
    dets.reserve(static_cast<int>(humans.size()));
    for (const auto& obj : humans) {
        const std::string nativeId = rbfqt_find_native_id(obj.id);
        const std::string stableId = rbfqt_find_stable_id(nativeId);
        // IdStabilizer S_xxx > N-ID > ONVIF XML (표시)
        const QString displayId = !stableId.empty() ? QString::fromStdString(stableId)
                                 : (!nativeId.empty() ? QString::fromStdString(nativeId)
                                                      : QString::fromStdString(obj.id));

        QVariantMap m;
        m["id"]            = displayId;
        m["nativeId"]      = QString::fromStdString(nativeId);
        m["xmlId"]         = QString::fromStdString(obj.id);
        m["metaFrameNo"]   = frameNo;
        m["metaTimestamp"] = static_cast<qlonglong>(rtpTs);
        m["fraud"]         = fraudIds.contains(QString::fromStdString(obj.id));

        // Qt 모드에선 트래커가 계산/스무딩한 bbox를 UI에 직접 사용한다.
        // 그래야 standalone처럼 위/아래 떨림이 줄어든다.
        const auto qbbox = rbfqt_get_bbox_by_xmlid(obj.id, W, H);
        if (qbbox.found && qbbox.r > qbbox.l && qbbox.b > qbbox.t) {
            m["x"] = static_cast<double>(qbbox.l);
            m["y"] = static_cast<double>(qbbox.t);
            m["w"] = static_cast<double>(qbbox.r - qbbox.l);
            m["h"] = static_cast<double>(qbbox.b - qbbox.t);
        } else {
            // fallback: 서버 원시 bbox 사용
            float nl = obj.left, nt = obj.top, nr = obj.right, nb = obj.bottom;
            if (std::max({nl, nt, nr, nb}) > 1.5f) {
                nl /= kSensorW; nr /= kSensorW;
                nt /= kSensorH; nb /= kSensorH;
            }
            nl = std::max(0.0f, std::min(1.0f, nl));
            nt = std::max(0.0f, std::min(1.0f, nt));
            nr = std::max(0.0f, std::min(1.0f, nr));
            nb = std::max(0.0f, std::min(1.0f, nb));

            if (nr > nl && nb > nt) {
                m["x"] = static_cast<double>(nl);
                m["y"] = static_cast<double>(nt);
                m["w"] = static_cast<double>(nr - nl);
                m["h"] = static_cast<double>(nb - nt);
            }
        }
        dets.append(m);
    }
    setDetections(dets);
#else
    QVariantList dets = m_nativeTracker.process(humans, W, H, wallMs);
    for (int i = 0; i < dets.size(); ++i) {
        QVariantMap mm = dets[i].toMap();
        mm["metaFrameNo"]   = frameNo;
        mm["metaTimestamp"] = static_cast<qlonglong>(rtpTs);
        dets[i] = mm;
    }
    setDetections(dets);
#endif
}

void MainWindow::opencvCaptureLoop()
{
#ifdef _WIN32
    _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS",
              "rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0|reorder_queue_size;0|analyzeduration;"
              "0|probesize;32768");
#endif
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString url = env.value(QStringLiteral("RTSP_STREAM_URL"), QStringLiteral("rtsp://192.168.0.101:8554/cam1"));
    const QString gstPipe = env.value(QStringLiteral("SFEPS_GSTREAMER_PIPELINE")).trimmed();

    cv::VideoCapture cap;
    bool opened = false;
    if (!gstPipe.isEmpty())
        opened = cap.open(gstPipe.toStdString(), cv::CAP_GSTREAMER);
    if (!opened)
        opened = cap.open(url.toStdString(), cv::CAP_FFMPEG);
    if (!opened) {
        qWarning() << "[OpenCV] cannot open RTSP (set SFEPS_GSTREAMER_PIPELINE or RTSP_STREAM_URL)";
        QMetaObject::invokeMethod(this, [this]() { updateStreamStatus("DISCONNECTED", false); }, Qt::QueuedConnection);
        return;
    }

    QMetaObject::invokeMethod(this, [this]() { updateStreamStatus("ONLINE", true); }, Qt::QueuedConnection);

    while (m_opencvRunning.load(std::memory_order_acquire)) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            QThread::msleep(5);
            continue;
        }
        if (frame.channels() == 1)
            cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
        else if (frame.channels() == 4)
            cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);

#if defined(CAMERA_RBF_QT_MODE) && defined(SFEPS_HAVE_OPENCV)
        // pose crop용 최신 프레임 저장
        {
            std::lock_guard<std::mutex> lk(g_latestFrameMutex);
            g_latestFrame = frame.clone();
        }
#endif

        QImage img(frame.data, frame.cols, frame.rows, static_cast<int>(frame.step), QImage::Format_BGR888,
                   [](void *) {}, nullptr);
        img = img.copy();
        m_frameW.store(frame.cols);
        m_frameH.store(frame.rows);

        QMetaObject::invokeMethod(
            this,
            [this, img]() mutable {
                if (m_liveProvider)
                    m_liveProvider->setFrame(img);
                ++m_previewRevision;
                emit previewRevisionChanged();
            },
            Qt::QueuedConnection);
    }
}

void MainWindow::onPwmTick()
{
    const int W = m_frameW.load() > 0 ? m_frameW.load() : 1920;
    const int H = m_frameH.load() > 0 ? m_frameH.load() : 1080;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto setPoseOverlay = [this](double u, double v, bool valid) {
        const bool changed = (m_poseAimValid != valid)
                             || (std::fabs(m_poseAimU - u) > 1e-3)
                             || (std::fabs(m_poseAimV - v) > 1e-3);
        if (changed) {
            m_poseAimU = u;
            m_poseAimV = v;
            m_poseAimValid = valid;
            emit poseAimChanged();
        }
    };
    // RBF가 실제로 겨냥하는 위치(bbox 중심 또는 pose aim) – 주황색 십자 마커로 표시
    auto setRbfTarget = [this](double u, double v, bool valid) {
        const bool changed = (m_rbfTargetValid != valid)
                             || (std::fabs(m_rbfTargetU - u) > 1e-3)
                             || (std::fabs(m_rbfTargetV - v) > 1e-3);
        if (changed) {
            m_rbfTargetU = u;
            m_rbfTargetV = v;
            m_rbfTargetValid = valid;
            emit rbfTargetChanged();
        }
    };

    // Laser Tracking 토글 OFF면: pose 추정/target 갱신/SET_PWM 계산 자체를 스킵한다.
    // (빨간 bbox 표시를 유지하더라도 하드웨어 PWM은 더 이상 보내지 않음)
    if (!m_laserTrackingEnabled) {
        setPoseOverlay(0.0, 0.0, false);
        setRbfTarget(0.0, 0.0, false);
        return;
    }

#ifdef CAMERA_RBF_QT_MODE
    // ── 수동 추적이 없을 때: 레이저는 fraudAutoTrackRequest로 고정된 XML(sticky)만 추적
    //    (추가 FRAUD는 fraudDetected→addFraudXmlId로 빨간색만, 레이저 대상은 안 바뀜)
    if (!m_manualTracking) {
        QSet<QString> fraudIds;
        QVariantList dets;
        QString stickyXml;
        QString stickyStable;
        int stableMissingFrames = 0;
        {
            QMutexLocker lk(&m_mutex);
            fraudIds   = m_fraudXmlIds;
            dets       = m_detections;
            stickyXml  = m_fraudLaserStickyXmlId;
            stickyStable = m_fraudLaserStickyStableId;
            stableMissingFrames = m_fraudLaserStickyStableMissingFrames;
        }

        // sticky target이 없으면 레이저는 완전히 멈춘다.
        if (stickyXml.isEmpty()) {
            rbfqt_set_tracked_nativeid("");
            g_qtPoseWorker.clearAim();
            rbfqt_set_pose_aim(0.0f, 0.0f, 0);
        } else {
        // fraud 없음: 타겟 해제
        if (fraudIds.isEmpty()) {
            rbfqt_set_tracked_nativeid("");
            QMutexLocker lk(&m_mutex);
            m_fraudLaserStickyStableId.clear();
            m_fraudLaserStickyStableMissingFrames = 0;
            g_qtPoseWorker.clearAim();
            rbfqt_set_pose_aim(0.0f, 0.0f, 0);
        } else {
            const double kSwitchRatio = 0.85;  // 새 후보가 충분히 작을 때만 전환
            const int kMaxMissingFrames = 10; // ~330ms 허용: RTSP 지연/일시적 누락 무시

            // 현재 타겟이 화면에 존재하는지 여부는 stable 매핑이 아니라
            // ONVIF XML ID(stickyXml) 자체로 판단한다.
            // (겹침/occlusion으로 NativeTrack N-ID가 스왑돼도 false stop을 방지)

            // 1) fraud 후보 중 면적 최소(단, ID는 stable_id 우선)
            double bestArea = 1e18;
            std::string bestKey;
            QString bestXmlId;
            double currentArea = 1e18;
            bool currentFound = false;
            // pose crop용 sticky bbox (정규화 [0,1])
            double stickyX = 0.0, stickyY = 0.0, stickyW = 0.0, stickyH = 0.0;
            bool stickyBoxFound = false;
            // best 후보 rect (정규화 [0,1])
            double bestX = 0.0, bestY = 0.0, bestW = 0.0, bestH = 0.0;
            // 정책:
            //  - stickyStableId(S_xxx)가 있으면 유지/전환 판정은 S_xxx 기준만 사용
            //  - 없으면 기존처럼 xmlId(ONVIF XML ID) 기준으로 fallback
            const bool useStablePolicy = !stickyStable.isEmpty();
            const std::string stickyStableStr = useStablePolicy ? stickyStable.toStdString()
                                                                 : std::string{};

            for (const QVariant &v : dets) {
                if (!v.canConvert<QVariantMap>()) continue;
                const QVariantMap dm = v.toMap();
                const bool isFraud = dm.value("fraud").toBool();

                const double w = dm.value("w").toDouble();
                const double h = dm.value("h").toDouble();
                const double area = w * h;
                // UI에선 w/h가 0에 가까워도 Math.max(2, ...) 때문에 보일 수 있지만,
                // 레이저 후보 선택은 area 기준이라 0이면 전부 스킵되어 bestKey가 비게 됨.
                // 따라서 area가 0이면 아주 작은 epsilon으로 대체한다.
                const double safeArea = (area > 0.0) ? area : 1e-6;

                const QString xmlId = dm.value("xmlId").toString();
                if (xmlId.isEmpty()) continue;

                // 1순위: 현재 프레임 매핑
                std::string nid = rbfqt_find_native_id(xmlId.toStdString());
                if (nid.empty())
                    nid = rbfqt_resolve_to_native_id(xmlId.toStdString());
                // 2순위: applyNativeDetections 시점에 저장된 nativeId (타이밍 갭 커버)
                if (nid.empty())
                    nid = dm.value("nativeId").toString().toStdString();
                // 3순위: xmlId 자체를 키로 사용 (rbfqt_set_tracked_nativeid가 내부 resolve)
                const std::string key_str = !nid.empty() ? nid : xmlId.toStdString();
                const std::string sid = !nid.empty() ? rbfqt_find_stable_id(nid) : std::string{};
                const std::string key = !sid.empty() ? sid : key_str;

                // sticky target이 화면에 보이는지 여부 판정
                // 기존: stable 우선 -> stable-mapping이 잠깐이라도 어긋나면 "안 보임"으로 처리되어
                //         kMaxMissingFrames를 넘기면 TRACK_END/추적 종료가 날 수 있었음.
                // 변경: xmlId 매칭도 같이 허용해서 "보이는데 END" 오동작을 줄인다.
                const bool isStickyPresent =
                    (!stickyXml.isEmpty() && xmlId == stickyXml) ||
                    (useStablePolicy && key == stickyStableStr);
                if (isStickyPresent) {
                    currentArea = safeArea;
                    currentFound = true;
                    stickyX = dm.value("x").toDouble();
                    stickyY = dm.value("y").toDouble();
                    stickyW = dm.value("w").toDouble();
                    stickyH = dm.value("h").toDouble();
                    stickyBoxFound = true;
                }

                // best candidate는 fraud=true인 객체들로만 구성
                if (!isFraud) continue;

                // stickyStableId가 존재하면, 전환 판단은 S_xxx stable-mapped 후보로만 허용
                // (stable 매핑이 안 되는 후보로는 전환하지 않음)
                if (useStablePolicy && sid.empty()) continue;

                if (safeArea < bestArea) {
                    bestArea = safeArea;
                    bestKey = key;
                    bestXmlId = xmlId;
                    bestX = dm.value("x").toDouble();
                    bestY = dm.value("y").toDouble();
                    bestW = dm.value("w").toDouble();
                    bestH = dm.value("h").toDouble();
                }
            }

            // ── pose aim 오버라이드 주입 (Qt 모드) ─────────────────────────────
            // pose는 최신 프레임에서 sticky target bbox를 padding crop 하여 어깨 랜드마크를 잡는다.
            // 결과가 stale 하지 않으면 rbfqt_compute_pwm에서 bbox_cx/bbox_cy 대신 이 값을 사용한다.
            {
                const int kPoseEveryTicks = 5;          // 33ms 타이머 기준 약 165ms 간격
                const qint64 kPoseStaleMs = 800;       // pose 결과가 너무 오래되면 무시
                const double kPosePadRatio = 0.15;     // sel bbox 대비 crop padding
                const int kPoseJpegQuality = 80;
                static int sPoseTick = 0;
                sPoseTick++;

                if (stickyXml.isEmpty() || !stickyBoxFound) {
                    g_qtPoseWorker.clearAim();
                    rbfqt_set_pose_aim(0.0f, 0.0f, 0);
                    setPoseOverlay(0.0, 0.0, false);
                    setRbfTarget(0.0, 0.0, false);
                } else {
                    if (sPoseTick % kPoseEveryTicks == 0) {
                        cv::Mat frameCopy;
                        {
                            std::lock_guard<std::mutex> lk(g_latestFrameMutex);
                            if (!g_latestFrame.empty())
                                frameCopy = g_latestFrame.clone();
                        }

                        if (!frameCopy.empty()) {
                            const int l = std::max(0, std::min(W - 1, (int)std::lround(stickyX * W)));
                            const int t = std::max(0, std::min(H - 1, (int)std::lround(stickyY * H)));
                            const int r = std::max(l + 1, std::min(W, (int)std::lround((stickyX + stickyW) * W)));
                            const int b = std::max(t + 1, std::min(H, (int)std::lround((stickyY + stickyH) * H)));
                            const int objW = std::max(1, r - l);
                            const int objH = std::max(1, b - t);

                            const int padX = std::max(0, (int)std::lround(objW * kPosePadRatio));
                            const int padY = std::max(0, (int)std::lround(objH * kPosePadRatio));
                            const int cl = std::max(0, l - padX);
                            const int ct = std::max(0, t - padY);
                            const int cr = std::min(W, r + padX);
                            const int cb = std::min(H, b + padY);

                            const int cw = std::max(1, cr - cl);
                            const int ch = std::max(1, cb - ct);
                            if (cw > 8 && ch > 8) {
                                cv::Mat crop = frameCopy(cv::Rect(cl, ct, cw, ch)).clone();
                                std::vector<uchar> buf;
                                std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, kPoseJpegQuality};
                                if (cv::imencode(".jpg", crop, buf, params)) {
                                    const double bboxBottomY = (double)(t + objH);
                                    g_qtPoseWorker.submit(buf,
                                                           (double)cl, (double)ct,
                                                           (double)cw, (double)ch,
                                                           bboxBottomY,
                                                           m_poseDownRatio);
                                }
                            }
                        }
                    }

                    static bool sAutoPoseHistValid = false;
                    static double sAutoPrevAimU = 0.0, sAutoPrevAimV = 0.0;
                    static double sAutoPrevBoxU = 0.0, sAutoPrevBoxV = 0.0;
                    const double bboxCx = (stickyX + stickyW * 0.5) * W;
                    const double bboxCy = (stickyY + stickyH * m_pwmRatio) * H;
                    double aimU = 0.0, aimV = 0.0;
                    qint64 aimMs = 0;
                    const bool gotAim = g_qtPoseWorker.getAim(aimU, aimV, aimMs);
                    const bool staleOk = gotAim && (now - aimMs) <= kPoseStaleMs;
                    if (staleOk) {
                        // bbox 이동량 대비 pose 이동량이 과도하면 스파이크로 보고 차단
                        const double kPoseJumpBasePx = 16.0;
                        const double kPoseJumpScale = 2.5;
                        const double kPoseJumpFloorPx = 45.0;
                        const double kPoseEmaAlpha = 0.45;

                        double filteredAimU = aimU;
                        double filteredAimV = aimV;
                        bool jumpRejected = false;
                        if (sAutoPoseHistValid) {
                            const double poseMove = std::hypot(aimU - sAutoPrevAimU, aimV - sAutoPrevAimV);
                            const double boxMove = std::hypot(bboxCx - sAutoPrevBoxU, bboxCy - sAutoPrevBoxV);
                            const double allowedJump = std::max(kPoseJumpFloorPx, kPoseJumpBasePx + kPoseJumpScale * boxMove);
                            if (poseMove > allowedJump) {
                                jumpRejected = true;
                                filteredAimU = sAutoPrevAimU;
                                filteredAimV = sAutoPrevAimV;
                            } else {
                                filteredAimU = sAutoPrevAimU + kPoseEmaAlpha * (aimU - sAutoPrevAimU);
                                filteredAimV = sAutoPrevAimV + kPoseEmaAlpha * (aimV - sAutoPrevAimV);
                            }
                        }
                        rbfqt_set_pose_aim((float)filteredAimU, (float)filteredAimV, 1);
                        setPoseOverlay(filteredAimU, filteredAimV, true);
                        setRbfTarget(filteredAimU, filteredAimV, true);   // pose 있으면 pose aim이 RBF 타겟
                        sAutoPrevAimU = filteredAimU;
                        sAutoPrevAimV = filteredAimV;
                        sAutoPrevBoxU = bboxCx;
                        sAutoPrevBoxV = bboxCy;
                        sAutoPoseHistValid = true;
                        if (jumpRejected) {
                            qDebug() << "[PoseAim] auto jump rejected raw=(" << aimU << "," << aimV
                                     << ") filtered=(" << filteredAimU << "," << filteredAimV << ")";
                        }
                    } else {
                        rbfqt_set_pose_aim(0.0f, 0.0f, 0);
                        setPoseOverlay(0.0, 0.0, false);
                        // pose 없으면 bbox 중심이 RBF 타겟
                        setRbfTarget(bboxCx, bboxCy, stickyBoxFound);
                        sAutoPoseHistValid = false;
                    }
                    // 디버그: stale 통과 여부를 1초에 1번 출력
                    static qint64 lastPoseLogMs = 0;
                    if (now - lastPoseLogMs > 1000) {
                        if (gotAim) {
                            const qint64 poseRttMs = g_qtPoseWorker.getPoseRttMs();
                            const qint64 ageMs = (now - aimMs);
                            const qint64 streamMs = (m_streamLatencyMs > 0) ? m_streamLatencyMs : 0;
                            qDebug() << "[PoseAim] got aim=(" << aimU << "," << aimV << ") ageMs=" << ageMs
                                     << " poseRttMs=" << poseRttMs << " streamMs=" << streamMs
                                     << " totalMs=" << (streamMs + poseRttMs + ageMs)
                                     << " staleOk=" << staleOk << " xml=" << stickyXml;
                        } else {
                            qDebug() << "[PoseAim] no aim yet staleOk=false xml=" << stickyXml;
                        }
                        lastPoseLogMs = now;
                    }
                }
            }

            // fraud 후보(best)가 없어도 stickyXml이 현재 프레임에 보이면 즉시 멈추면 안 됨.
            // (fraud 플래그는 다음 메타데이터 tick에서 반영될 수 있음)
            if (bestKey.empty()) {
                bool shouldStop = false;
                if (!currentFound) {
                    stableMissingFrames++;
                    if (stableMissingFrames > kMaxMissingFrames) {
                        shouldStop = true;
                    }
                } else {
                    stableMissingFrames = 0;
                }

                if (shouldStop) {
                    emit laserTrackStopped(stickyXml);
                    // activeTrackingId 정리 → Fraud 대기큐 drain이 다음 tick 이후에 안전하게 진행되도록 queued 실행
                    QMetaObject::invokeMethod(this, [this]() { clearRbfTarget(); }, Qt::QueuedConnection);
                    rbfqt_set_tracked_nativeid("");
                    QMutexLocker lk(&m_mutex);
                    m_fraudLaserStickyXmlId.clear();
                    m_fraudLaserStickyStableId.clear();
                    m_fraudLaserStickyStableMissingFrames = 0;
                    stableMissingFrames = 0;
                } else {
                    QMutexLocker lk(&m_mutex);
                    m_fraudLaserStickyStableMissingFrames = stableMissingFrames;
                }
            } else {
                bool shouldSwitch = false;
                QString stoppedXmlToEmit;

                if (!currentFound) {
                    // sticky target이 화면 밖이라면 전환하지 않고 유지하다가 종료.
                    stableMissingFrames++;
                    if (stableMissingFrames > kMaxMissingFrames) {
                        // 완전히 멈추기 + 서버에 TRACK_END 보내기
                        stoppedXmlToEmit = stickyXml;
                        rbfqt_set_tracked_nativeid("");

                        QMutexLocker lk(&m_mutex);
                        m_fraudLaserStickyXmlId.clear();
                        m_fraudLaserStickyStableId.clear();
                        m_fraudLaserStickyStableMissingFrames = 0;
                        stableMissingFrames = 0;
                    }
                } else {
                    // 현재 타겟이 보이는 상태: 새 후보가 "충분히" 더 작을 때만 전환
                    stableMissingFrames = 0;
                    const bool diffOk = useStablePolicy ? (bestKey != stickyStableStr)
                                                          : (bestXmlId != stickyXml);
                    if (diffOk && bestArea < currentArea * kSwitchRatio) {
                        // 겹침(occlusion) 상황에서 다른 사람에게 뺏기는 걸 막기 위해,
                        // sticky bbox와 best 후보 bbox의 IoU가 큰 동안은 전환을 억제한다.
                        auto iouNorm = [](double ax, double ay, double aw, double ah,
                                           double bx, double by, double bw, double bh) -> double {
                            const double a1x = ax;
                            const double a1y = ay;
                            const double a2x = ax + aw;
                            const double a2y = ay + ah;
                            const double b1x = bx;
                            const double b1y = by;
                            const double b2x = bx + bw;
                            const double b2y = by + bh;
                            const double ix1 = std::max(a1x, b1x);
                            const double iy1 = std::max(a1y, b1y);
                            const double ix2 = std::min(a2x, b2x);
                            const double iy2 = std::min(a2y, b2y);
                            const double iw = std::max(0.0, ix2 - ix1);
                            const double ih = std::max(0.0, iy2 - iy1);
                            const double inter = iw * ih;
                            const double areaA = std::max(0.0, aw) * std::max(0.0, ah);
                            const double areaB = std::max(0.0, bw) * std::max(0.0, bh);
                            const double uni = areaA + areaB - inter;
                            return (uni > 1e-9) ? (inter / uni) : 0.0;
                        };

                        const double iou = (stickyBoxFound && bestArea < 1e17)
                                                ? iouNorm(stickyX, stickyY, stickyW, stickyH,
                                                          bestX, bestY, bestW, bestH)
                                                : 0.0;
                        const double kMaxIouBlock = 0.25; // 겹치면 전환 억제
                        if (!(iou > kMaxIouBlock)) {
                            shouldSwitch = true;
                        }
                    }
                }

                if (!stoppedXmlToEmit.isEmpty()) {
                    emit laserTrackStopped(stoppedXmlToEmit);
                    // stop 처리 후 activeTrackingId 정리 + 큐 drain(다음 fraud로 자연스럽게 넘어가기)
                    QMetaObject::invokeMethod(this, [this]() { clearRbfTarget(); }, Qt::QueuedConnection);
                } else if (shouldSwitch) {
                    rbfqt_set_tracked_nativeid(bestKey.c_str());
                    // 전환 시 stickyXml도 해당 후보로 갱신
                    bool isStable = bestKey.size() >= 2 && bestKey[0] == 'S' && bestKey[1] == '_';
                    QMutexLocker lk(&m_mutex);
                    m_fraudLaserStickyXmlId = bestXmlId;
                    m_fraudLaserStickyStableId = isStable ? QString::fromStdString(bestKey) : QString{};
                    m_fraudLaserStickyStableMissingFrames = stableMissingFrames;
                } else {
                    // 타겟 유지: 매 tick rbfqt_set_tracked_nativeid를 호출해 tracker를 동기화.
                    // trackByXmlId 호출 시 N-ID 매핑이 아직 없었거나 N-ID가 프레임 간 바뀌어도 복구됨.
                    if (currentFound)
                        rbfqt_set_tracked_nativeid(bestKey.c_str());
                    QMutexLocker lk(&m_mutex);
                    m_fraudLaserStickyStableMissingFrames = stableMissingFrames;
                }
            }
        }
        } // end stickyXml isEmpty guard
    } else {
        // 수동 tracking에서도 pose aim을 사용할 수 있게 현재 선택/추적 대상 bbox를 기반으로 추론한다.
        // m_manualTrackDisplayId: trackByNativeId(S_xxx) 시 저장된 display ID
        // m_selectedDetectionId: 클릭으로만 설정되며 Track 버튼과 무관 → pose 탐색에는 사용 안 함
        QVariantList dets;
        QString selectedId;
        {
            QMutexLocker lk(&m_mutex);
            dets = m_detections;
            selectedId = m_manualTrackDisplayId;  // Track 버튼으로 선택한 ID
        }

        double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;
        bool bboxFound = false;
        for (const QVariant &v : dets) {
            if (!v.canConvert<QVariantMap>()) continue;
            const QVariantMap dm = v.toMap();
            const QString id = dm.value("id").toString();
            const QString nativeId = dm.value("nativeId").toString();
            const QString xmlId = dm.value("xmlId").toString();
            const bool match = (!selectedId.isEmpty()) &&
                               (selectedId == id || selectedId == nativeId || selectedId == xmlId);
            if (!match) continue;
            bx = dm.value("x").toDouble();
            by = dm.value("y").toDouble();
            bw = dm.value("w").toDouble();
            bh = dm.value("h").toDouble();
            bboxFound = (bw > 0.0 && bh > 0.0);
            break;
        }

        const int kPoseEveryTicks = 5;
        const qint64 kPoseStaleMs = 800;
        const double kPosePadRatio = 0.15;
        const int kPoseJpegQuality = 80;
        static int sManualPoseTick = 0;
        sManualPoseTick++;

        static bool sManualPoseHistValid = false;
        static double sManualPrevAimU = 0.0, sManualPrevAimV = 0.0;
        static double sManualPrevBoxU = 0.0, sManualPrevBoxV = 0.0;
        if (!bboxFound) {
            g_qtPoseWorker.clearAim();
            rbfqt_set_pose_aim(0.0f, 0.0f, 0);
            setPoseOverlay(0.0, 0.0, false);
            setRbfTarget(0.0, 0.0, false);
            sManualPoseHistValid = false;
        } else {
            // bbox 중심 (m_pwmRatio 적용) → RBF가 pose 없을 때 겨냥하는 픽셀
            const double bboxCx = (bx + bw * 0.5) * W;
            const double bboxCy = (by + bh * m_pwmRatio) * H;
            setRbfTarget(bboxCx, bboxCy, true);

            if (sManualPoseTick % kPoseEveryTicks == 0) {
                cv::Mat frameCopy;
                {
                    std::lock_guard<std::mutex> lk(g_latestFrameMutex);
                    if (!g_latestFrame.empty()) frameCopy = g_latestFrame.clone();
                }
                if (!frameCopy.empty()) {
                    const int l = std::max(0, std::min(W - 1, (int)std::lround(bx * W)));
                    const int t = std::max(0, std::min(H - 1, (int)std::lround(by * H)));
                    const int r = std::max(l + 1, std::min(W, (int)std::lround((bx + bw) * W)));
                    const int b = std::max(t + 1, std::min(H, (int)std::lround((by + bh) * H)));
                    const int objW = std::max(1, r - l);
                    const int objH = std::max(1, b - t);
                    const int padX = std::max(0, (int)std::lround(objW * kPosePadRatio));
                    const int padY = std::max(0, (int)std::lround(objH * kPosePadRatio));
                    const int cl = std::max(0, l - padX);
                    const int ct = std::max(0, t - padY);
                    const int cr = std::min(W, r + padX);
                    const int cb = std::min(H, b + padY);
                    const int cw = std::max(1, cr - cl);
                    const int ch = std::max(1, cb - ct);
                    if (cw > 8 && ch > 8) {
                        cv::Mat crop = frameCopy(cv::Rect(cl, ct, cw, ch)).clone();
                        std::vector<uchar> buf;
                        std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, kPoseJpegQuality};
                        if (cv::imencode(".jpg", crop, buf, params)) {
                            const double bboxBottomY = (double)(t + objH);
                            g_qtPoseWorker.submit(buf, (double)cl, (double)ct, (double)cw, (double)ch,
                                                  bboxBottomY, m_poseDownRatio);
                        }
                    }
                }
            }

            double aimU = 0.0, aimV = 0.0;
            qint64 aimMs = 0;
            const bool gotAim = g_qtPoseWorker.getAim(aimU, aimV, aimMs);
            const bool staleOk = gotAim && (now - aimMs) <= kPoseStaleMs;
            if (staleOk) {
                // bbox 대비 급점프 pose aim 차단 + EMA 스무딩
                const double kPoseJumpBasePx = 16.0;
                const double kPoseJumpScale = 2.5;
                const double kPoseJumpFloorPx = 45.0;
                const double kPoseEmaAlpha = 0.45;

                double filteredAimU = aimU;
                double filteredAimV = aimV;
                bool jumpRejected = false;
                if (sManualPoseHistValid) {
                    const double poseMove = std::hypot(aimU - sManualPrevAimU, aimV - sManualPrevAimV);
                    const double boxMove = std::hypot(bboxCx - sManualPrevBoxU, bboxCy - sManualPrevBoxV);
                    const double allowedJump = std::max(kPoseJumpFloorPx, kPoseJumpBasePx + kPoseJumpScale * boxMove);
                    if (poseMove > allowedJump) {
                        jumpRejected = true;
                        filteredAimU = sManualPrevAimU;
                        filteredAimV = sManualPrevAimV;
                    } else {
                        filteredAimU = sManualPrevAimU + kPoseEmaAlpha * (aimU - sManualPrevAimU);
                        filteredAimV = sManualPrevAimV + kPoseEmaAlpha * (aimV - sManualPrevAimV);
                    }
                }

                rbfqt_set_pose_aim((float)filteredAimU, (float)filteredAimV, 1);
                setPoseOverlay(filteredAimU, filteredAimV, true);
                setRbfTarget(filteredAimU, filteredAimV, true);      // pose 유효 → pose aim이 RBF 타겟
                sManualPrevAimU = filteredAimU;
                sManualPrevAimV = filteredAimV;
                sManualPrevBoxU = bboxCx;
                sManualPrevBoxV = bboxCy;
                sManualPoseHistValid = true;
                if (jumpRejected) {
                    qDebug() << "[PoseAim][manual] jump rejected raw=(" << aimU << "," << aimV
                             << ") filtered=(" << filteredAimU << "," << filteredAimV << ")";
                }
            } else {
                rbfqt_set_pose_aim(0.0f, 0.0f, 0);
                setPoseOverlay(0.0, 0.0, false);
                setRbfTarget(bboxCx, bboxCy, true);  // pose 없음 → bbox 중심이 RBF 타겟
                sManualPoseHistValid = false;
            }
            static qint64 lastManualPoseLogMs = 0;
            if (now - lastManualPoseLogMs > 1000) {
                if (gotAim) {
                    const qint64 poseRttMs = g_qtPoseWorker.getPoseRttMs();
                    qDebug() << "[PoseAim][manual] aim=(" << aimU << "," << aimV
                             << ") ageMs=" << (now - aimMs) << " poseRttMs=" << poseRttMs
                             << " staleOk=" << staleOk << " id=" << selectedId;
                } else {
                    qDebug() << "[PoseAim][manual] no aim yet staleOk=false id=" << selectedId;
                }
                lastManualPoseLogMs = now;
            }
        }
    }

    int pan = 1500, tilt = 1500;
    if (rbfqt_compute_pwm(static_cast<long long>(now), W, H, &pan, &tilt))
        emit pwmSetRequested(pan, tilt);
#else
    // ── 레거시 모드: rbf_pwm_core.h 의 RbfTps2D 직접 사용 ─────────────────
    if (!m_rbfOk) return;

    QString sel;
    QVariantList dets;
    {
        QMutexLocker locker(&m_mutex);
        sel = m_selectedDetectionId;
        dets = m_detections;
    }

    double dt = (m_lastPwmTickMs > 0) ? (now - m_lastPwmTickMs) / 1000.0 : (1.0 / 30.0);
    m_lastPwmTickMs = now;
    dt = std::max(1.0 / 120.0, std::min(1.0 / 15.0, dt));

    bool selOk = false;
    cv::Rect sel_rect;
    QString selIdResolved;

    if (!sel.isEmpty()) {
        for (const QVariant &v : dets) {
            if (!v.canConvert<QVariantMap>()) continue;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() != sel) continue;
            ParsedMetadataObject po;
            po.id = sel.toStdString();
            po.type = m.value(QStringLiteral("type")).toString().toStdString();
            const float x = m.value(QStringLiteral("x")).toFloat();
            const float y = m.value(QStringLiteral("y")).toFloat();
            const float w = m.value(QStringLiteral("w")).toFloat();
            const float h = m.value(QStringLiteral("h")).toFloat();
            po.left = x; po.top = y; po.right = x + w; po.bottom = y + h;
            if (computeRectFromObj(po, W, H, sel_rect)) {
                selOk = true; selIdResolved = sel;
            }
            break;
        }
    }

    if (selOk && selIdResolved != m_prevSelPwm) {
        m_kfPwm.reset(); m_prevPan = 1500; m_prevTilt = 1500;
        m_prevSelPwm = selIdResolved;
    }
    if (!selOk && !m_prevSelPwm.isEmpty()) {
        m_kfPwm.reset(); m_prevSelPwm.clear();
    }

    int pred_u = W / 2, pred_v = H / 2;
    if (selOk) {
        const double bbox_cx = sel_rect.x + sel_rect.width * 0.5;
        double bbox_cy = std::max(0.0, std::min(sel_rect.y + sel_rect.height * m_pwmRatio, double(H - 1)));
        m_kfPwm.update(bbox_cx, bbox_cy, double(sel_rect.width), double(sel_rect.height), dt);
        double pcx = 0, pcy = 0;
        m_kfPwm.predict(m_predictMs / 1000.0, pcx, pcy);
        pred_u = static_cast<int>(std::lround(std::max(0.0, std::min(double(W - 1), pcx))));
        pred_v = static_cast<int>(std::lround(std::max(0.0, std::min(double(H - 1), pcy))));
    }

    const double pan_d  = m_rbfPan.eval(double(pred_u), double(pred_v));
    const double tilt_d = m_rbfTilt.eval(double(pred_u), double(pred_v));
    int pan  = static_cast<int>(std::lround(std::max(double(m_panMin),  std::min(double(m_panMax),  pan_d))));
    int tilt = static_cast<int>(std::lround(std::max(double(m_tiltMin), std::min(double(m_tiltMax), tilt_d))));
    pan  = static_cast<int>(std::lround(m_pwmAlpha * pan  + (1.0 - m_pwmAlpha) * m_prevPan));
    tilt = static_cast<int>(std::lround(m_pwmAlpha * tilt + (1.0 - m_pwmAlpha) * m_prevTilt));
    m_prevPan = pan; m_prevTilt = tilt;

    if (selOk) emit pwmSetRequested(pan, tilt);
#endif
}

#endif // SFEPS_HAVE_OPENCV
