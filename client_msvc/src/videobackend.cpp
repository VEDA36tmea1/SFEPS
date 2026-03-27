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
#include <limits>
#ifdef SFEPS_HAVE_OPENCV
#include <memory>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include "live_frame_provider.h"
#ifndef CAMERA_RBF_QT_MODE
#include "rbf_pwm_core.h"          // 레거시 모드에서만 필요
#include "native_metadata_tracker.h"
#endif
#include <QImage>
#include <QThread>
#ifdef _WIN32
#include <stdlib.h>
#endif
#endif

namespace {
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
                        qInfo() << "[StreamLatency][tag-time]" << m_streamLatencyMs << "ms";
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
                        qInfo() << "[StreamLatency][tag-time]" << m_streamLatencyMs << "ms";
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
    rbfqt_set_tracked_nativeid(nativeId.toStdString().c_str());
    m_manualTracking = !nativeId.isEmpty();  // 수동 추적 플래그 → fraud 자동 전환 억제
    qDebug() << "[MainWindow] trackByNativeId" << nativeId << "→ auto-tracking enabled, manualTracking=" << m_manualTracking;
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
        qWarning() << "[MainWindow] trackByXmlId: xmlId not found and no fallback bbox:" << xmlId;
        return;
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
    rbfqt_set_tracked_nativeid("");
    rbfqt_clear_target();
    m_manualTracking = false;
#else
    setSelectedDetection(QString{});
#endif
    emit trackingXmlIdChanged(QString{});
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
        // N-ID가 아직 없으면 xmlId를 임시 id로 사용 (매 프레임 rbfqt_process_metadata로 갱신됨)
        const QString displayId = nativeId.empty()
                                  ? QString::fromStdString(obj.id)
                                  : QString::fromStdString(nativeId);

        QVariantMap m;
        m["id"]            = displayId;
        m["xmlId"]         = QString::fromStdString(obj.id);
        m["metaFrameNo"]   = frameNo;
        m["metaTimestamp"] = static_cast<qlonglong>(rtpTs);
        m["fraud"]         = fraudIds.contains(QString::fromStdString(obj.id));

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

#ifdef CAMERA_RBF_QT_MODE
    // ── 수동 추적이 없을 때: fraud 객체 중 화면 중심에서 가장 가까운 것으로 자동 전환 ──
    if (!m_manualTracking) {
        QSet<QString> fraudIds;
        QVariantList dets;
        {
            QMutexLocker lk(&m_mutex);
            fraudIds = m_fraudXmlIds;
            dets     = m_detections;
        }
        if (!fraudIds.isEmpty()) {
            const double cx = W * 0.5, cy = H * 0.5;
            double bestDist = 1e18;
            QString bestNativeId;
            for (const QVariant &v : dets) {
                if (!v.canConvert<QVariantMap>()) continue;
                const QVariantMap dm = v.toMap();
                if (!dm.value("fraud").toBool()) continue;
                // 정규화 좌표 → 픽셀 중심
                const double bx = dm.value("x").toDouble() * W + dm.value("w").toDouble() * W * 0.5;
                const double by = dm.value("y").toDouble() * H + dm.value("h").toDouble() * H * 0.5;
                const double d  = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
                if (d < bestDist) {
                    bestDist = d;
                    bestNativeId = dm.value("id").toString();
                }
            }
            if (!bestNativeId.isEmpty()) {
                rbfqt_set_tracked_nativeid(bestNativeId.toStdString().c_str());
            }
        } else {
            // fraud 없으면 추적 중지
            rbfqt_set_tracked_nativeid("");
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
