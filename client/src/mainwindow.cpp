#include "mainwindow.h"
#include <QPainter>
#include <QDebug>
#include <QProcessEnvironment>

MainWindow::MainWindow(QQuickItem *parent)
    : QQuickPaintedItem(parent),
      worker(nullptr),
      m_reconnectTimer(new QTimer(this)),
      m_running(false),
      m_brightness(0),
      m_streamStatus("STOPPED"),
      m_streamConnected(false)
{
    // 성능 최적화: QQuickPaintedItem은 기본적으로 FBO(FramebufferObject)에 렌더링하도록 설정
    setRenderTarget(QQuickPaintedItem::FramebufferObject);

    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);
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
    if (m_brightness == brightness) return;
    m_brightness = brightness;
    emit brightnessChanged();
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

void MainWindow::processFrame(const cv::Mat &frame)
{
    if (!m_running) return;
    updateStreamStatus("ONLINE", true);

    if (m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }

    QMutexLocker locker(&m_mutex);
    
    // 원본 프레임 저장 (워커와의 분리를 위해 복사)
    currentFrame = frame.clone();
    
    // 화면 표시를 위한 가공
    cv::Mat displayMat = currentFrame.clone();

    // 1. 밝기 조절
    if (m_brightness != 0) {
        displayMat.convertTo(displayMat, -1, 1, m_brightness);
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
    cv::cvtColor(displayMat, displayMat, cv::COLOR_BGR2RGB);
    
    // QImage 생성
    m_image = QImage((const unsigned char*)displayMat.data, 
                     displayMat.cols, displayMat.rows, 
                     displayMat.step, 
                     QImage::Format_RGB888).copy(); 
                     // Mat 데이터가 소멸될 수 있으므로 깊은 복사(Deep Copy) 필요
                     
    // 메인 스레드에 화면 갱신 요청
    update();
}

void MainWindow::paint(QPainter *painter)
{
    QMutexLocker locker(&m_mutex);
    if (m_image.isNull()) {
        painter->fillRect(boundingRect(), Qt::black);
        painter->setPen(Qt::white);
        painter->drawText(boundingRect(), Qt::AlignCenter, m_streamStatus == "DISCONNECTED" ? "STREAM DISCONNECTED" : "WAITING FOR STREAM...");
        return;
    }

    // 줌/크롭 처리
    QRectF sourceRect(0, 0, m_image.width(), m_image.height());
    
    if (!m_zoomRect.isEmpty()) {
        // 만약 m_zoomRect가 설정되어 있다면 해당 영역만 그립니다.
        // (단, QML에서 전달받은 좌표계와 이미지 좌표계의 매핑이 필요할 수 있음)
        // 여기서는 단순화를 위해 넘겨받은 rect를 그대로 사용합니다.
        sourceRect = m_zoomRect;
    }

    painter->drawImage(boundingRect(), m_image, sourceRect);
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

    const QString rtspUrl = QProcessEnvironment::systemEnvironment().value("RTSP_STREAM_URL", "rtsp://192.168.0.89:8554/cam1");
    cap.open(rtspUrl.toStdString(), cv::CAP_FFMPEG);
    if (!cap.isOpened()) {
        qWarning() << "[MainWindow] Failed to open stream:" << rtspUrl;
        return false;
    }

    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    qDebug() << "[MainWindow] Stream open success:" << rtspUrl;
    return true;
}

void MainWindow::ensureWorkerRunning()
{
    if (!worker) {
        worker = new VideoCaptureWorker(&cap, this);
        connect(worker, &VideoCaptureWorker::newFrame, this, &MainWindow::processFrame);
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
