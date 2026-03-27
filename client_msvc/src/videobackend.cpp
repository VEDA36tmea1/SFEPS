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
#include "rbf_pwm_core.h"
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

void MainWindow::applyNativeDetections(std::vector<ParsedMetadataObject> humans, unsigned int rtpTs, qint64 wallMs,
                                       qint64 frameNo)
{
    int W = m_frameW.load();
    int H = m_frameH.load();
    if (W <= 0)
        W = 1920;
    if (H <= 0)
        H = 1080;
    QVariantList dets = m_nativeTracker.process(humans, W, H, wallMs);
    for (int i = 0; i < dets.size(); ++i) {
        QVariantMap mm = dets[i].toMap();
        mm["metaFrameNo"] = frameNo;
        mm["metaTimestamp"] = static_cast<qlonglong>(rtpTs);
        dets[i] = mm;
    }
    setDetections(dets);
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
    if (!m_rbfOk)
        return;

    QString sel;
    QVariantList dets;
    {
        QMutexLocker locker(&m_mutex);
        sel = m_selectedDetectionId;
        dets = m_detections;
    }

    const int W = m_frameW.load() > 0 ? m_frameW.load() : 1920;
    const int H = m_frameH.load() > 0 ? m_frameH.load() : 1080;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double dt = (m_lastPwmTickMs > 0) ? (now - m_lastPwmTickMs) / 1000.0 : (1.0 / 30.0);
    m_lastPwmTickMs = now;
    dt = std::max(1.0 / 120.0, std::min(1.0 / 15.0, dt));

    bool selOk = false;
    cv::Rect sel_rect;
    QString selIdResolved;

    if (!sel.isEmpty()) {
        for (const QVariant &v : dets) {
            if (!v.canConvert<QVariantMap>())
                continue;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() != sel)
                continue;
            ParsedMetadataObject po;
            po.id = sel.toStdString();
            po.type = m.value(QStringLiteral("type")).toString().toStdString();
            const float x = m.value(QStringLiteral("x")).toFloat();
            const float y = m.value(QStringLiteral("y")).toFloat();
            const float w = m.value(QStringLiteral("w")).toFloat();
            const float h = m.value(QStringLiteral("h")).toFloat();
            po.left = x;
            po.top = y;
            po.right = x + w;
            po.bottom = y + h;
            if (computeRectFromObj(po, W, H, sel_rect)) {
                selOk = true;
                selIdResolved = sel;
            }
            break;
        }
    }

    if (selOk && selIdResolved != m_prevSelPwm) {
        m_kfPwm.reset();
        m_prevPan = 1500;
        m_prevTilt = 1500;
        m_prevSelPwm = selIdResolved;
    }
    if (!selOk && !m_prevSelPwm.isEmpty()) {
        m_kfPwm.reset();
        m_prevSelPwm.clear();
    }

    int pred_u = W / 2;
    int pred_v = H / 2;
    if (selOk) {
        const double bbox_cx = sel_rect.x + sel_rect.width * 0.5;
        double bbox_cy = sel_rect.y + sel_rect.height * m_pwmRatio;
        bbox_cy = std::max(0.0, std::min(double(H - 1), bbox_cy));
        m_kfPwm.update(bbox_cx, bbox_cy, double(sel_rect.width), double(sel_rect.height), dt);
        double pcx = 0, pcy = 0;
        m_kfPwm.predict(m_predictMs / 1000.0, pcx, pcy);
        pcx = std::max(0.0, std::min(double(W - 1), pcx));
        pcy = std::max(0.0, std::min(double(H - 1), pcy));
        pred_u = static_cast<int>(std::lround(pcx));
        pred_v = static_cast<int>(std::lround(pcy));
    }

    const double pan_d = m_rbfPan.eval(double(pred_u), double(pred_v));
    const double tilt_d = m_rbfTilt.eval(double(pred_u), double(pred_v));
    int pan = static_cast<int>(std::lround(std::max(double(m_panMin), std::min(double(m_panMax), pan_d))));
    int tilt = static_cast<int>(std::lround(std::max(double(m_tiltMin), std::min(double(m_tiltMax), tilt_d))));
    pan = static_cast<int>(std::lround(m_pwmAlpha * pan + (1.0 - m_pwmAlpha) * m_prevPan));
    tilt = static_cast<int>(std::lround(m_pwmAlpha * tilt + (1.0 - m_pwmAlpha) * m_prevTilt));
    m_prevPan = pan;
    m_prevTilt = tilt;

    if (selOk)
        emit pwmSetRequested(pan, tilt);
}

#endif // SFEPS_HAVE_OPENCV
