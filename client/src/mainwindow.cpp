#include "mainwindow.h"
#include <QPainter>
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

int parseEnvInt(const QProcessEnvironment &env, const QString &key, int defaultValue)
{
    const QString raw = env.value(key).trimmed();
    if (raw.isEmpty()) return defaultValue;
    bool ok = false;
    const int parsed = raw.toInt(&ok);
    if (!ok) return defaultValue;
    return parsed;
}
}

MainWindow::MainWindow(QQuickItem *parent)
    : QQuickPaintedItem(parent),
      worker(nullptr),
    m_reconnectTimer(new QTimer(this)),
    m_updateTimer(nullptr),
      m_running(false),
            m_brightness(53),
            m_contrast(52),
      m_streamStatus("STOPPED"),
    m_streamConnected(false),
    m_lastImageSize(0, 0)
        ,m_hasPendingDetections(false),
        m_useCameraCgiControl(false),
        m_cameraCgiAllowInsecureTls(true),
        m_cgiNetworkManager(nullptr),
        m_brightnessCgiDebounceTimer(nullptr),
        m_contrastCgiDebounceTimer(nullptr)
{
    // 성능 최적화: QQuickPaintedItem은 기본적으로 FBO(FramebufferObject)에 렌더링하도록 설정
    setRenderTarget(QQuickPaintedItem::FramebufferObject);

    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

    // Coalesce frequent detection updates to avoid flooding the UI thread
    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    connect(m_updateTimer, &QTimer::timeout, this, &MainWindow::onUpdateTimerTimeout);

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
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
        // Handle both Basic and Digest auth challenges from camera automatically.
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

        qDebug() << "[MainWindow] Camera CGI control enabled. brightnessUrl=" << m_cameraBrightnessCgiUrlTemplate
                 << "contrastUrl=" << m_cameraContrastCgiUrlTemplate;
        // Fetch current values from camera on startup so sliders reflect real state.
        QTimer::singleShot(0, this, &MainWindow::fetchCameraImageSettings);
    }
}

MainWindow::~MainWindow()
{
    setRunning(false);
    if (cap.isOpened()) {
        cap.release();
    }
}

void MainWindow::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    emit runningChanged();

    if (m_running) {
        if (openStream()) {
            ensureWorkerRunning();
            m_reconnectTimer->stop();
            updateStreamStatus("CONNECTING", false);
        } else {
            updateStreamStatus("DISCONNECTED", false);
            if (!m_reconnectTimer->isActive()) {
                m_reconnectTimer->start();
            }
        }
    } else {
        m_reconnectTimer->stop();
        if (worker) {
            worker->stop();
            delete worker;
            worker = nullptr;
        }
        if (cap.isOpened()) {
            cap.release();
        }
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

void MainWindow::setZoomRect(const QRectF &rect)
{
    if (m_zoomRect == rect) return;
    m_zoomRect = rect;
    emit zoomRectChanged();
    update(); // 다시 그리기 요청
}

void MainWindow::resetZoom()
{
    setZoomRect(QRectF());
}

void MainWindow::setZoomFromItem(const QRectF &itemRect, const QSizeF &itemSize)
{
    QMutexLocker locker(&m_mutex);
    if (m_image.isNull() || itemSize.width() <= 0 || itemSize.height() <= 0) return;

    // Item 내의 상대적 비율 계산 (0.0 ~ 1.0)
    double rx = itemRect.x() / itemSize.width();
    double ry = itemRect.y() / itemSize.height();
    double rw = itemRect.width() / itemSize.width();
    double rh = itemRect.height() / itemSize.height();

    // 실제 이미지 좌표로 변환
    double imgX = rx * m_image.width();
    double imgY = ry * m_image.height();
    double imgW = rw * m_image.width();
    double imgH = rh * m_image.height();

    setZoomRect(QRectF(imgX, imgY, imgW, imgH));
}

void MainWindow::processFrame(const cv::Mat &frame, qint64 ts)
{
    if (!m_running) {
        if (worker) worker->markFrameConsumed();
        return;
    }
    updateStreamStatus("ONLINE", true);

    if (m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }

    // Build display mat with minimal copying to reduce memory pressure.
    cv::Mat displayMat;
    const bool remoteBrightness = m_useCameraCgiControl && !m_cameraBrightnessCgiUrlTemplate.isEmpty();
    const bool remoteContrast = m_useCameraCgiControl && !m_cameraContrastCgiUrlTemplate.isEmpty();
    const double alpha = remoteContrast ? 1.0 : (0.5 + (static_cast<double>(m_contrast) / 100.0)); // 0.5 .. 1.5
    const double beta = remoteBrightness ? 0.0 : ((static_cast<double>(m_brightness) - 50.0) * 2.55); // -127.5 .. 127.5

    if (qFuzzyCompare(alpha, 1.0) && qFuzzyIsNull(beta)) {
        displayMat = frame;
    } else {
        frame.convertTo(displayMat, -1, alpha, beta);
    }

    // 2. 줌(Zoom) 처리
    // 여기서는 단순히 원본 비율 유지를 위해 전체를 처리하고 paint()에서 자를 수도 있지만,
    // 데이터 처리 단계에서 미리 자르는 것이 효율적일 수 있습니다.
    // 하지만 QML과의 좌표 매핑 편의성을 위해 paint() 단계에서 처리하는 것이 간단할 수 있습니다.
    
    if (!m_zoomRect.isEmpty() && m_zoomRect.width() > 0 && m_zoomRect.height() > 0) {
        // m_zoomRect는 QML Item 좌표계이므로, 실제 이미지 좌표계로 변환해야 정확합니다.
        // 이번 구현에서는 paint()에서 drawImage의 소스 영역(source rect)을 지정하는 방식을 사용합니다.
    }

    // Qt 표시를 위해 RGB로 변환
    cv::Mat rgbMat;
    cv::cvtColor(displayMat, rgbMat, cv::COLOR_BGR2RGB);
    
    // QImage 생성
    QImage nextImage = QImage((const unsigned char*)rgbMat.data,
                              rgbMat.cols, rgbMat.rows,
                              rgbMat.step,
                              QImage::Format_RGB888).copy();
                     // Mat 데이터가 소멸될 수 있으므로 깊은 복사(Deep Copy) 필요

    bool sizeChanged = false;
    {
        QMutexLocker locker(&m_mutex);
        m_image = std::move(nextImage);
        const QSize newSize(m_image.width(), m_image.height());
        if (m_lastImageSize != newSize) {
            m_lastImageSize = newSize;
            sizeChanged = true;
        }
    }

    if (sizeChanged) emit imageSizeChanged();
                     
    // 메인 스레드에 화면 갱신 요청
    update();

    if (worker) {
        worker->markFrameConsumed();
    }

    // Update measured stream latency (ms) using the timestamp provided by the capture worker.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int measured = int(nowMs - ts);
    if (measured != m_streamLatencyMs) {
        m_streamLatencyMs = measured;
        emit streamLatencyChanged();
    }
}

void MainWindow::paint(QPainter *painter)
{
    QImage image;
    QVariantList detections;
    QString selectedId;
    QString trackedId;
    QRectF zoomRect;
    QString streamStatus;

    {
        QMutexLocker locker(&m_mutex);
        image = m_image;
        detections = m_detections;
        selectedId = m_selectedDetectionId;
        trackedId = m_externalTrackedId;
        zoomRect = m_zoomRect;
        streamStatus = m_streamStatus;
    }

    if (image.isNull()) {
        painter->fillRect(boundingRect(), Qt::black);
        painter->setPen(Qt::white);
        painter->drawText(boundingRect(), Qt::AlignCenter, streamStatus == "DISCONNECTED" ? "STREAM DISCONNECTED" : "WAITING FOR STREAM...");
        return;
    }

    // 줌/크롭 처리
    QRectF sourceRect(0, 0, image.width(), image.height());
    
    if (!zoomRect.isEmpty()) {
        // 만약 m_zoomRect가 설정되어 있다면 해당 영역만 그립니다.
        // (단, QML에서 전달받은 좌표계와 이미지 좌표계의 매핑이 필요할 수 있음)
        // 여기서는 단순화를 위해 넘겨받은 rect를 그대로 사용합니다.
        sourceRect = zoomRect;
    }

    painter->drawImage(boundingRect(), image, sourceRect);

    // Draw detections (expected as QVariantList of maps: {id: string, x: double, y: double, w: double, h: double}
    // Coordinates are normalized to image size (0..1). Map image coords -> item coords using sourceRect -> boundingRect mapping.
    QRectF itemRect = boundingRect();
    for (const QVariant &v : detections) {
        if (!v.canConvert<QVariantMap>()) continue;
        const QVariantMap m = v.toMap();
        const QString id = m.value("id").toString();
        const double nx = m.value("x").toDouble();
        const double ny = m.value("y").toDouble();
        const double nw = m.value("w").toDouble();
        const double nh = m.value("h").toDouble();

        // Image coordinates (within full image)
        const double imgW = image.width();
        const double imgH = image.height();
        const double imgX = nx * imgW;
        const double imgY = ny * imgH;
        const double imgBoxW = nw * imgW;
        const double imgBoxH = nh * imgH;

        // Map to item coords considering sourceRect crop
        const double srcX = sourceRect.x();
        const double srcY = sourceRect.y();
        const double srcW = sourceRect.width();
        const double srcH = sourceRect.height();

        // Skip boxes that don't intersect visible sourceRect
        QRectF imgBox(imgX, imgY, imgBoxW, imgBoxH);
        QRectF srcRect(srcX, srcY, srcW, srcH);
        if (!imgBox.intersects(srcRect)) continue;

        // Clip box to sourceRect for proper mapping
        QRectF visible = imgBox.intersected(srcRect);

        const double fx = (visible.x() - srcX) / srcW;
        const double fy = (visible.y() - srcY) / srcH;
        const double fw = visible.width() / srcW;
        const double fh = visible.height() / srcH;

        QRectF drawRect(
            itemRect.x() + fx * itemRect.width(),
            itemRect.y() + fy * itemRect.height(),
            fw * itemRect.width(),
            fh * itemRect.height()
        );

        QPen pen(Qt::green);
        pen.setWidth(2);

        // If detection carries a type/alert indicating suspected fare evasion,
        // draw the box in red (give suspicion priority over selection).
        bool suspected = false;
        if (m.contains("type")) {
            const QString t = m.value("type").toString().toLower();
            if (t.contains("fraud") || t.contains("fare") || t.contains("susp")) suspected = true;
        }
        if (!suspected && m.contains("alert")) {
            // some sources may provide an explicit alert boolean
            if (m.value("alert").toBool()) suspected = true;
        }

        const bool tracked = !trackedId.isEmpty() && trackedId == id;

        if (tracked) {
            pen.setColor(QColor("#1e90ff"));
            pen.setWidth(3);
        } else if (suspected) {
            pen.setColor(Qt::red);
            pen.setWidth(3);
        } else if (!selectedId.isEmpty() && selectedId == id) {
            pen.setColor(Qt::yellow);
            pen.setWidth(3);
        }
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(drawRect);

        // Draw ID label
        painter->setPen(Qt::white);
        QFont f = painter->font();
        f.setPointSize(10);
        painter->setFont(f);
        if (tracked) {
            painter->setPen(QColor("#60a5fa"));
            QFont tf = painter->font();
            tf.setPointSize(10);
            tf.setBold(true);
            painter->setFont(tf);
            painter->drawText(drawRect.topLeft() + QPointF(4, -4), "Tracking");
        }

        painter->setPen(Qt::white);
        QFont idf = painter->font();
        idf.setPointSize(10);
        idf.setBold(false);
        painter->setFont(idf);
        painter->drawText(drawRect.topLeft() + QPointF(4, tracked ? 16 : 14), id);
    }
}

void MainWindow::setDetections(const QVariantList &list)
{
    QMutexLocker locker(&m_mutex);
    m_pendingDetections = list;
    m_hasPendingDetections = true;
    // Coalesce multiple rapid detection updates.
    if (m_updateTimer && !m_updateTimer->isActive()) {
        m_updateTimer->start(120);
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
    update();
}

QString MainWindow::detectionAt(qreal x, qreal y)
{
    QMutexLocker locker(&m_mutex);
    if (m_image.isNull()) return QString();

    QRectF srcRect(0, 0, m_image.width(), m_image.height());
    if (!m_zoomRect.isEmpty()) srcRect = m_zoomRect;

    QRectF itemRect = boundingRect();
    if (itemRect.width() <= 0 || itemRect.height() <= 0) return QString();

    // Map item coords -> image coords
    const double fx = (x - itemRect.x()) / itemRect.width();
    const double fy = (y - itemRect.y()) / itemRect.height();
    const double imgX = srcRect.x() + fx * srcRect.width();
    const double imgY = srcRect.y() + fy * srcRect.height();

    for (const QVariant &v : m_detections) {
        if (!v.canConvert<QVariantMap>()) continue;
        const QVariantMap m = v.toMap();
        const QString id = m.value("id").toString();
        const double nx = m.value("x").toDouble();
        const double ny = m.value("y").toDouble();
        const double nw = m.value("w").toDouble();
        const double nh = m.value("h").toDouble();

        const double imgW = m_image.width();
        const double imgH = m_image.height();
        QRectF imgBox(nx * imgW, ny * imgH, nw * imgW, nh * imgH);
        if (imgBox.contains(QPointF(imgX, imgY))) return id;
    }
    return QString();
}

void MainWindow::clearDetections()
{
    QMutexLocker locker(&m_mutex);
    m_detections.clear();
    m_selectedDetectionId.clear();
    emit detectionsChanged();
    emit selectedDetectionChanged();
    update();
}

void MainWindow::setSelectedDetection(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (m_selectedDetectionId == id) return;
    m_selectedDetectionId = id;
    emit selectedDetectionChanged();
    update();
}

void MainWindow::setExternalTrackedId(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (m_externalTrackedId == id) return;
    m_externalTrackedId = id;
    emit externalTrackedIdChanged();
    update();
}

int MainWindow::imageWidth() const
{
    QMutexLocker locker(&m_mutex);
    return m_image.isNull() ? 0 : m_image.width();
}

int MainWindow::imageHeight() const
{
    QMutexLocker locker(&m_mutex);
    return m_image.isNull() ? 0 : m_image.height();
}

void MainWindow::onReadFailed()
{
    if (!m_running) return;

    if (worker) {
        worker->stop();
        delete worker;
        worker = nullptr;
    }

    if (cap.isOpened()) {
        cap.release();
    }

    updateStreamStatus("RECONNECTING", false);

    if (!m_reconnectTimer->isActive()) {
        m_reconnectTimer->start();
    }
}

void MainWindow::attemptReconnect()
{
    if (!m_running) return;

    if (openStream()) {
        ensureWorkerRunning();
        updateStreamStatus("CONNECTING", false);
    } else {
        updateStreamStatus("DISCONNECTED", false);
    }
}

bool MainWindow::openStream()
{
    if (cap.isOpened()) {
        return true;
    }

        qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS",
            QByteArray("rtsp_transport;tcp|fflags;nobuffer|flags;low_delay|max_delay;0"));

    const QString rtspUrl = QProcessEnvironment::systemEnvironment().value("RTSP_STREAM_URL", "rtsp://192.168.0.101:8554/cam1");
    cap.open(rtspUrl.toStdString(), cv::CAP_FFMPEG);
    if (!cap.isOpened()) {
        qWarning() << "[MainWindow] Failed to open stream:" << rtspUrl;
        return false;
    }

    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    // Conservative decode size to avoid FFmpeg/OpenCV allocation spikes.
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_FPS, 15);
    // Log stream and source frame size for diagnosing image-size/resolution
    double srcW = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double srcH = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    qDebug() << "[MainWindow] Stream open success:" << rtspUrl << "source size:" << srcW << "x" << srcH;
    emit imageSizeChanged();
    return true;
}

void MainWindow::ensureWorkerRunning()
{
    if (!worker) {
        worker = new VideoCaptureWorker(&cap, this);
        connect(worker, &VideoCaptureWorker::newFrame, this, &MainWindow::processFrame, Qt::QueuedConnection);
        connect(worker, &VideoCaptureWorker::readFailed, this, &MainWindow::onReadFailed);
    }

    if (!worker->isRunning()) {
        worker->start();
    }
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

void MainWindow::fetchCameraImageSettings()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager) return;

    // Hanwha CGI: replace action=set&<Param>=<val> with action=view to read current values
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
        qDebug() << "[MainWindow] Camera image settings fetched: brightness=" << m_brightness
                 << "contrast=" << m_contrast;
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
                qWarning() << "[MainWindow] fetchCameraImageSettings https failed, retrying with http:" << status;

                QNetworkRequest httpReq(httpUrl);
                httpReq.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
                QNetworkReply *httpReply = m_cgiNetworkManager->get(httpReq);
                connect(httpReply, &QNetworkReply::finished, this, [this, httpReply, parseBody]() {
                    const int httpStatus = httpReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const bool httpOk = (httpReply->error() == QNetworkReply::NoError) && (httpStatus >= 200 && httpStatus < 300);
                    const QString httpBody = httpOk ? QString::fromUtf8(httpReply->readAll()) : QString();
                    if (!httpOk) {
                        qWarning() << "[MainWindow] fetchCameraImageSettings failed. status=" << httpStatus
                                   << httpReply->errorString();
                        httpReply->deleteLater();
                        return;
                    }
                    httpReply->deleteLater();
                    parseBody(httpBody);
                });
                return;
            }

            qWarning() << "[MainWindow] fetchCameraImageSettings failed. status=" << status
                       << reply->errorString();
        });
    };

    sendRequest(QUrl(base), false);
}

void MainWindow::scheduleBrightnessCgiUpdate()
{
    if (!m_useCameraCgiControl || !m_brightnessCgiDebounceTimer || m_cameraBrightnessCgiUrlTemplate.isEmpty()) {
        update();
        return;
    }
    m_brightnessCgiDebounceTimer->start();
}

void MainWindow::scheduleContrastCgiUpdate()
{
    if (!m_useCameraCgiControl || !m_contrastCgiDebounceTimer || m_cameraContrastCgiUrlTemplate.isEmpty()) {
        update();
        return;
    }
    m_contrastCgiDebounceTimer->start();
}

void MainWindow::sendBrightnessCgi()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager || m_cameraBrightnessCgiUrlTemplate.isEmpty()) return;

    QString urlText = m_cameraBrightnessCgiUrlTemplate;
    urlText.replace("{value}", QString::number(m_brightness));

    const QUrl url(urlText);
    if (!url.isValid()) {
        qWarning() << "[MainWindow] Invalid CAMERA_BRIGHTNESS_CGI_URL:" << urlText;
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");

    if (url.scheme().compare("https", Qt::CaseInsensitive) == 0 && m_cameraCgiAllowInsecureTls) {
        QSslConfiguration conf = request.sslConfiguration();
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(conf);
    }

    QNetworkReply *reply = m_cgiNetworkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = (reply->error() == QNetworkReply::NoError) && (status >= 200 && status < 300);
        if (ok) {
            reply->deleteLater();
            return;
        }

        if (url.scheme().compare("https", Qt::CaseInsensitive) == 0) {
            QUrl httpUrl(url);
            httpUrl.setScheme("http");
            qWarning() << "[MainWindow] Brightness CGI https failed, retrying with http:" << status;

            QNetworkRequest httpReq(httpUrl);
            httpReq.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
            QNetworkReply *httpReply = m_cgiNetworkManager->get(httpReq);
            connect(httpReply, &QNetworkReply::finished, this, [httpReply]() {
                const int httpStatus = httpReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (httpReply->error() != QNetworkReply::NoError || (httpStatus < 200 || httpStatus >= 300)) {
                    qWarning() << "[MainWindow] Brightness CGI update failed. status=" << httpStatus
                               << "error=" << httpReply->errorString();
                }
                httpReply->deleteLater();
            });
            reply->deleteLater();
            return;
        }

        qWarning() << "[MainWindow] Brightness CGI update failed. status=" << status
                   << "error=" << reply->errorString();
        reply->deleteLater();
    });
}

void MainWindow::sendContrastCgi()
{
    if (!m_useCameraCgiControl || !m_cgiNetworkManager || m_cameraContrastCgiUrlTemplate.isEmpty()) return;

    QString urlText = m_cameraContrastCgiUrlTemplate;
    urlText.replace("{value}", QString::number(m_contrast));

    const QUrl url(urlText);
    if (!url.isValid()) {
        qWarning() << "[MainWindow] Invalid CAMERA_CONTRAST_CGI_URL:" << urlText;
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");

    if (url.scheme().compare("https", Qt::CaseInsensitive) == 0 && m_cameraCgiAllowInsecureTls) {
        QSslConfiguration conf = request.sslConfiguration();
        conf.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(conf);
    }

    QNetworkReply *reply = m_cgiNetworkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = (reply->error() == QNetworkReply::NoError) && (status >= 200 && status < 300);
        if (ok) {
            reply->deleteLater();
            return;
        }

        if (url.scheme().compare("https", Qt::CaseInsensitive) == 0) {
            QUrl httpUrl(url);
            httpUrl.setScheme("http");
            qWarning() << "[MainWindow] Contrast CGI https failed, retrying with http:" << status;

            QNetworkRequest httpReq(httpUrl);
            httpReq.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
            QNetworkReply *httpReply = m_cgiNetworkManager->get(httpReq);
            connect(httpReply, &QNetworkReply::finished, this, [httpReply]() {
                const int httpStatus = httpReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (httpReply->error() != QNetworkReply::NoError || (httpStatus < 200 || httpStatus >= 300)) {
                    qWarning() << "[MainWindow] Contrast CGI update failed. status=" << httpStatus
                               << "error=" << httpReply->errorString();
                }
                httpReply->deleteLater();
            });
            reply->deleteLater();
            return;
        }

        qWarning() << "[MainWindow] Contrast CGI update failed. status=" << status
                   << "error=" << reply->errorString();
        reply->deleteLater();
    });
}