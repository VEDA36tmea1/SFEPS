#include "fraudmanager.h"
#include <QDebug>
#include <QFile>
#include <QSslCertificate>
#include <QIODevice>
#include <QProcessEnvironment>
#include <QSslConfiguration>
#include <QSslError>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>

static bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

static QList<QSslCertificate> loadCaCertificates(const QString &path, QString &outError)
{
    QList<QSslCertificate> certs;
    if (path.trimmed().isEmpty()) return certs;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        outError = QString("open failed (%1): %2").arg(path, f.errorString());
        return certs;
    }
    const QByteArray data = f.readAll();
    certs = QSslCertificate::fromData(data, QSsl::Pem);
    if (certs.isEmpty()) outError = QString("no valid PEM certificate found in %1").arg(path);
    return certs;
}

namespace {
bool parseFraudMessage(const QString &msg,
                       QString &objectId,
                       QString &cardAgeText,
                       QString &age,
                       bool &isFraud,
                       QString &tag,
                       float &bboxL, float &bboxT, float &bboxR, float &bboxB)
{
    if (!msg.startsWith("FRAUD|")) {
        return false;
    }

    const QStringList parts = msg.split('|', Qt::KeepEmptyParts);
    // 최소 필수 필드: FRAUD|objectId|cardAgeText|age|fraudFlag
    // 뒤쪽(TAG/L/T/R/B/X/Y...)은 가변 확장 필드로 처리한다.
    if (parts.size() < 5) {
        qWarning() << "[FraudManager] Ignore malformed message (field missing):" << msg;
        return false;
    }

    objectId = parts[1].trimmed();
    cardAgeText = parts[2].trimmed();
    age = parts[3].trimmed();

    const QString fraudRaw = parts[4].trimmed().toLower();
    if (fraudRaw == "1" || fraudRaw == "true" || fraudRaw == "y" || fraudRaw == "yes") {
        isFraud = true;
    } else if (fraudRaw == "0" || fraudRaw == "false" || fraudRaw == "n" || fraudRaw == "no") {
        isFraud = false;
    } else {
        qWarning() << "[FraudManager] Ignore malformed message (invalid fraud flag):" << msg;
        return false;
    }

    if (objectId.isEmpty() || cardAgeText.isEmpty() || age.isEmpty()) {
        qWarning() << "[FraudManager] Ignore malformed message (invalid value):" << msg;
        return false;
    }

    if (!age.isEmpty()) {
        age[0] = age[0].toUpper();
    }

    // Extract TAG / L / T / R / B from remaining fields
    // 형식 예: FRAUD|1071432|0|20|Y|L=1510.0|T=71.0|R=1880.0|B=830.0|X=1695.0|Y=45|TAG=...
    tag.clear();
    bboxL = bboxT = bboxR = bboxB = 0.0f;
    for (int i = 5; i < parts.size(); ++i) {
        const QString p = parts[i].trimmed();
        // 서버 필드는 포맷/대소문자가 섞일 수 있으므로 '=' 기준으로 값만 뽑는다.
        const QString pu = p.toUpper();
        if (pu.startsWith("TAG=")) {
            tag = p.mid(p.indexOf('=') + 1).trimmed();
        } else {
            const int eq = p.indexOf('=');
            if (eq > 0) {
                const QString key = pu.left(eq).trimmed();
                const QString val = p.mid(eq + 1).trimmed();
                bool ok = false;
                const float f = val.toFloat(&ok);
                if (ok) {
                    if (key == "L") bboxL = f;
                    else if (key == "T") bboxT = f;
                    else if (key == "R") bboxR = f;
                    else if (key == "B") bboxB = f;
                }
            }
        }
    }

    return true;
}

bool parseImgRefMessage(const QString &msg, ImgRefData &imgRef)
{
    if (!msg.startsWith("IMG_REF|")) {
        return false;
    }

    // Parse key=value pattern
    // Format: IMG_REF|OBJECT_ID=123|URL=http://...|TAG=2026-03-19T10:11:12.123Z|NAME=...
    const QStringList parts = msg.split('|', Qt::KeepEmptyParts);
    
    imgRef.objectId.clear();
    imgRef.url.clear();
    imgRef.tag.clear();
    imgRef.name.clear();

    for (int i = 1; i < parts.size(); ++i) {
        const QString part = parts[i].trimmed();
        if (part.startsWith("OBJECT_ID=")) {
            imgRef.objectId = part.mid(10).trimmed();
        } else if (part.startsWith("URL=")) {
            imgRef.url = part.mid(4).trimmed();
        } else if (part.startsWith("TAG=")) {
            imgRef.tag = part.mid(4).trimmed();
        } else if (part.startsWith("NAME=")) {
            imgRef.name = part.mid(5).trimmed();
        }
    }

    if (imgRef.objectId.isEmpty() || imgRef.url.isEmpty() || imgRef.tag.isEmpty()) {
        qWarning() << "[FraudManager] Malformed IMG_REF message (missing required field):" << msg;
        return false;
    }

    return true;
}
}

FraudManager::FraudManager(QObject *parent) : QObject(parent)
{
    // Default to plaintext socket; may switch to QSslSocket when connectToServer is called
    socket = new QTcpSocket(this);
    retryTimer = new QTimer(this);
    retryTimer->setInterval(1000); // 1초 간격 재시도
    retryTimer->setSingleShot(true);

    // Network manager for image downloads
    networkManager = new QNetworkAccessManager(this);
    connect(networkManager, &QNetworkAccessManager::finished, this, &FraudManager::onImageDownloadFinished);

    attachSocketSignals();
    connect(retryTimer, &QTimer::timeout, this, &FraudManager::retryConnection);
}

FraudManager::~FraudManager()
{
    socket->disconnectFromHost();
}

void FraudManager::connectToServer(const QString &host, int port)
{
    lastHost = host;
    lastPort = port;
    m_alertTlsEnabled = resolveAlertTlsEnabled();
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // Recreate socket if type mismatches desired mode
    const bool needSsl = m_alertTlsEnabled;
    bool currentIsSsl = (qobject_cast<QSslSocket*>(socket) != nullptr);
    if (needSsl != currentIsSsl) {
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->disconnectFromHost();
        }
        socket->deleteLater();
        if (needSsl) {
            socket = new QSslSocket(this);
        } else {
            socket = new QTcpSocket(this);
        }
        attachSocketSignals();
    }

    if (socket->state() == QAbstractSocket::ConnectedState) return;
    qDebug() << "[FraudManager] Connecting to" << host << ":" << port << (needSsl ? "(TLS)" : "(Plain)");

    if (needSsl) {
        QSslSocket *ssl = qobject_cast<QSslSocket*>(socket);
        if (ssl) {
            ssl->abort();
            ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
            const QString caPath = env.value("SFEPS_CLIENT_CA_FILE").trimmed();
            QString caErr;
            const QList<QSslCertificate> certs = loadCaCertificates(caPath, caErr);
            if (!certs.isEmpty()) {
                QSslConfiguration cfg = ssl->sslConfiguration();
                cfg.setCaCertificates(certs);
                ssl->setSslConfiguration(cfg);
            } else if (!caPath.isEmpty()) {
                qWarning() << "[FraudManager] CA load failed:" << caErr;
            }

            const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
            if (!serverName.isEmpty()) ssl->setPeerVerifyName(serverName);

            ssl->connectToHostEncrypted(host, static_cast<quint16>(port));
            return;
        }
    }

    socket->connectToHost(host, port);
}

void FraudManager::onConnected()
{
    qDebug() << "[FraudManager] Connected to fraud alert server.";
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    retryTimer->stop();
    emit serverConnected();
}
void FraudManager::onDisconnected()
{
    qDebug() << "[FraudManager] Disconnected from fraud alert server. Retrying in 1s...";
    retryTimer->start();
    emit serverDisconnected();
}

void FraudManager::retryConnection()
{
    if (socket->state() == QAbstractSocket::UnconnectedState) {
        qDebug() << "[FraudManager] Retrying connection to" << lastHost << ":" << lastPort;
        connectToServer(lastHost, lastPort);
    }
}

void FraudManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    qWarning() << "[FraudManager] socket error:" << socket->errorString();
}

void FraudManager::onSslErrors(const QList<QSslError> &errors)
{
    for (const QSslError &err : errors) {
        qWarning() << "[FraudManager] sslError:" << err.errorString();
    }
}

void FraudManager::sendCommand(const QString &msg)
{
    if (!socket) {
        qWarning() << "[FraudManager] sendCommand: socket is null";
        return;
    }
    if (socket->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "[FraudManager] sendCommand: socket not connected:" << socket->state();
        return;
    }
    QByteArray data = msg.toUtf8();
    if (!data.endsWith('\n')) data.append('\n');
    qint64 n = socket->write(data);
    if (n <= 0) {
        qWarning() << "[FraudManager] failed to write command:" << msg;
    } else {
        socket->flush();
        qDebug() << "[FraudManager] Sent command:" << msg;
    }
}

void FraudManager::attachSocketSignals()
{
    connect(socket, &QTcpSocket::readyRead, this, &FraudManager::onReadyRead);
    connect(socket, &QTcpSocket::connected, this, &FraudManager::onConnected);
    connect(socket, &QTcpSocket::disconnected, this, &FraudManager::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QAbstractSocket::errorOccurred, this, &FraudManager::onSocketError);
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, &FraudManager::onSocketError);
#endif

    if (QSslSocket *ssl = qobject_cast<QSslSocket*>(socket)) {
        connect(ssl, &QSslSocket::sslErrors, this, &FraudManager::onSslErrors);
    }

    // Position socket moved to PositionManager
}

// Position handling moved to PositionManager

bool FraudManager::resolveAlertTlsEnabled() const
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const bool alertTlsProvided = !env.value("SFEPS_ALERT_TLS_ENABLE").trimmed().isEmpty();
    if (alertTlsProvided) {
        return parseEnvBool(env, "SFEPS_ALERT_TLS_ENABLE", false);
    }
    return parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);
}

void FraudManager::onReadyRead()
{
    // 수신 데이터 누적: '\n' 기준으로 분할하여 처리하고,
    // 개행이 없는 완전한 메시지도 파싱(예: 서버가 개행을 빼먹는 경우)합니다.
    recvBuffer.append(socket->readAll());

    // 완전한 라인(\n)이 있으면 하나씩 처리
    while (true) {
        int nl = recvBuffer.indexOf('\n');
        if (nl == -1) break;
        QByteArray line = recvBuffer.left(nl).trimmed();
        recvBuffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        QString msg = QString::fromUtf8(line);
        qDebug() << "[FraudManager] Received:" << msg;

        if (msg.startsWith("TEST|LOGIN_OK|")) {
            const QString userId = msg.section('|', 2, 2).trimmed();
            if (!userId.isEmpty()) {
                qInfo() << "[FraudManager] login ack received for user:" << userId;
                emit loginAckReceived(userId);
            }
            continue;
        }

        // Detect server-enforced force logout alerts
        if (msg.startsWith("AUTH|FORCE_LOGOUT|")) {
            qInfo() << "[FraudManager] force logout event received:" << msg;
            emit forceLogoutEvent(msg);
            continue;
        }

        // Handle IMG_REF message
        if (msg.startsWith("IMG_REF|")) {
            ImgRefData imgRef;
            if (parseImgRefMessage(msg, imgRef)) {
                qDebug() << "[FraudManager] Parsed IMG_REF: objectId=" << imgRef.objectId << " url=" << imgRef.url;
                downloadImage(imgRef);
            }
            continue;
        }

        // Handle FRAUD message
        QString objectId;
        QString cardAgeText;
        QString age;
        QString tag;
        bool isFraud = false;
        float bboxL = 0, bboxT = 0, bboxR = 0, bboxB = 0;
        if (parseFraudMessage(msg, objectId, cardAgeText, age, isFraud, tag,
                              bboxL, bboxT, bboxR, bboxB)) {
            QString eventKey = objectId + "|" + tag;
            QString imagePath;
            if (downloadedImages.contains(eventKey)) {
                imagePath = downloadedImages.value(eventKey);
            }
            processFraud(objectId, cardAgeText, age, isFraud, tag, imagePath,
                         bboxL, bboxT, bboxR, bboxB);
        }
    }

    // 폴백: 개행이 없더라도 버퍼 내용이 완전한 메시지 형식이면 처리
    if (!recvBuffer.isEmpty()) {
        QString s = QString::fromUtf8(recvBuffer).trimmed();
        QString objectId;
        QString cardAgeText;
        QString age;
        QString tag;
        bool isFraud = false;
        float bboxL = 0, bboxT = 0, bboxR = 0, bboxB = 0;
        if (!s.isEmpty()) {
            qDebug() << "[FraudManager] Received (no-nl fallback):" << s;
            if (s.startsWith("TEST|LOGIN_OK|")) {
                const QString userId = s.section('|', 2, 2).trimmed();
                if (!userId.isEmpty()) {
                    qInfo() << "[FraudManager] login ack received (fallback) for user:" << userId;
                    emit loginAckReceived(userId);
                }
                recvBuffer.clear();
                return;
            }
            if (s.startsWith("IMG_REF|")) {
                ImgRefData imgRef;
                if (parseImgRefMessage(s, imgRef)) {
                    qDebug() << "[FraudManager] Parsed IMG_REF (fallback): objectId=" << imgRef.objectId;
                    downloadImage(imgRef);
                }
                recvBuffer.clear();
                return;
            }
            if (parseFraudMessage(s, objectId, cardAgeText, age, isFraud, tag,
                                  bboxL, bboxT, bboxR, bboxB)) {
                QString eventKey = objectId + "|" + tag;
                QString imagePath;
                if (downloadedImages.contains(eventKey)) {
                    imagePath = downloadedImages.value(eventKey);
                }
                processFraud(objectId, cardAgeText, age, isFraud, tag, imagePath,
                             bboxL, bboxT, bboxR, bboxB);
                recvBuffer.clear();
            }
        }

        // 안전장치: 버퍼가 너무 커지면 초기화하여 메모리/무한루프 방지
        if (recvBuffer.size() > 16 * 1024) recvBuffer.clear();
    }
}

QString FraudManager::getImageStoragePath() const
{
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString fraudImageDir = appDataPath + "/fraud_images";
    
    // Create directory if it doesn't exist
    QDir dir(fraudImageDir);
    if (!dir.exists()) {
        dir.mkpath(fraudImageDir);
    }
    
    return fraudImageDir;
}

void FraudManager::downloadImage(const ImgRefData &imgRef)
{
    QString eventKey = imgRef.objectId + "|" + imgRef.tag;
    
    // Check if already downloaded
    if (downloadedImages.contains(eventKey)) {
        qDebug() << "[FraudManager] Image already downloaded for event:" << eventKey;
        emit imageReceived(imgRef.objectId, imgRef.tag, downloadedImages.value(eventKey));
        return;
    }
    
    // Store pending image for matching with FRAUD message
    pendingImages[eventKey] = imgRef;
    
    // Start download
    QUrl url(imgRef.url);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SFEPS-Client/1.0");
    
    QNetworkReply *reply = networkManager->get(request);
    // Store metadata in reply object for later use
    reply->setProperty("eventKey", eventKey);
    reply->setProperty("objectId", imgRef.objectId);
    reply->setProperty("tag", imgRef.tag);
    reply->setProperty("name", imgRef.name);
    
    qDebug() << "[FraudManager] Starting image download from URL:" << imgRef.url;
}

// ─── 추적 상태 동기화 ───────────────────────────────────────────────────────
void FraudManager::setActiveTrackingId(const QString &id)
{
    const bool wasTracking = !m_activeTrackingId.isEmpty();
    m_activeTrackingId = id;
    qDebug() << "[FraudManager] activeTrackingId set to" << (id.isEmpty() ? "(none)" : id);

    if (wasTracking && id.isEmpty()) {
        // 추적이 끝났으면 대기큐에서 꺼내서 처리
        drainFraudQueue();
    }
}

// ─── 대기큐 처리 ────────────────────────────────────────────────────────────
void FraudManager::drainFraudQueue()
{
    if (m_fraudQueue.isEmpty()) return;
    QueuedFraud next = m_fraudQueue.takeFirst();
    qDebug() << "[FraudManager] drainFraudQueue: tracking queued fraud objectId="
             << next.objectId;
    emit fraudQueueChanged(m_fraudQueue.size());

    // 큐에 넣을 때 이벤트 로그(fraudDetected)는 이미 emit 되었으므로,
    // drain 시점에는 자동 추적 요청만 수행한다.
    if (next.isFraud) {
        m_activeTrackingId = next.objectId;
        qDebug() << "[FraudManager] fraudAutoTrackRequest (drain) objectId=" << next.objectId
                 << "fallbackBbox=(" << next.bboxL << next.bboxT << next.bboxR << next.bboxB << ")";
        emit fraudAutoTrackRequest(next.objectId, next.bboxL, next.bboxT, next.bboxR, next.bboxB);
    }
}

// ─── FRAUD 처리 핵심 ────────────────────────────────────────────────────────
void FraudManager::processFraud(const QString &objectId,
                                 const QString &cardAgeText,
                                 const QString &age,
                                 bool isFraud,
                                 const QString &tag,
                                 const QString &imagePath,
                                 float bboxL, float bboxT, float bboxR, float bboxB)
{
    // 이벤트 로그/통계는 추적 상태와 무관하게 즉시 반영한다.
    emit fraudDetected(objectId, cardAgeText, age, isFraud, tag, imagePath);

    // 부정승차이고 다른 객체를 이미 추적 중이면 대기큐에 저장
    if (isFraud && !m_activeTrackingId.isEmpty()
        && m_activeTrackingId != objectId)
    {
        QueuedFraud qf{ objectId, cardAgeText, age, tag, imagePath, isFraud,
                        bboxL, bboxT, bboxR, bboxB };
        m_fraudQueue.append(qf);
        qDebug() << "[FraudManager] Queued fraud objectId=" << objectId
                 << "(currently tracking:" << m_activeTrackingId << ")"
                 << "bbox=(" << bboxL << bboxT << bboxR << bboxB << ")";
        emit fraudQueueChanged(m_fraudQueue.size());
        return;
    }

    // 부정승차면 자동 추적 요청 (main.cpp에서 videoBackend.trackByXmlId와 연결)
    if (isFraud) {
        // 추적 시작 전에 activeTrackingId 즉시 설정 (동기적으로) → 연속 FRAUD 중복 처리 방지
        m_activeTrackingId = objectId;
        qDebug() << "[FraudManager] fraudAutoTrackRequest objectId=" << objectId
                 << "fallbackBbox=(" << bboxL << bboxT << bboxR << bboxB << ")";
        emit fraudAutoTrackRequest(objectId, bboxL, bboxT, bboxR, bboxB);
    }
}

// ─── 이미지 다운로드 완료 ────────────────────────────────────────────────────
void FraudManager::onImageDownloadFinished(QNetworkReply *reply)
{
    if (!reply) return;

    QString eventKey = reply->property("eventKey").toString();
    QString objectId = reply->property("objectId").toString();
    QString tag = reply->property("tag").toString();
    QString fileName = reply->property("name").toString();

    qDebug() << "[FraudManager] Image download finished for event:" << eventKey;

    // Check response status
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        qWarning() << "[FraudManager] Image download failed with status code:" << statusCode << "for URL:" << reply->url();
        reply->deleteLater();
        return;
    }

    // Get response data
    QByteArray imageData = reply->readAll();
    if (imageData.isEmpty()) {
        qWarning() << "[FraudManager] Image download returned empty data for event:" << eventKey;
        reply->deleteLater();
        return;
    }

    // Validate JPG file
    if (!imageData.startsWith(QByteArray("\xFF\xD8"))) {
        qWarning() << "[FraudManager] Downloaded file is not a valid JPG (missing JPEG magic bytes) for event:" << eventKey;
        reply->deleteLater();
        return;
    }

    // Determine save filename
    QString saveFileName = fileName;
    if (saveFileName.isEmpty()) {
        // Fallback: fraud_<objectId>_<epoch_ms>.jpg
        qint64 epochMs = QDateTime::currentMSecsSinceEpoch();
        saveFileName = QString("fraud_%1_%2.jpg").arg(objectId).arg(epochMs);
    }

    // Save to local storage
    QString storagePath = getImageStoragePath();
    QString localFilePath = storagePath + "/" + saveFileName;

    QFile file(localFilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[FraudManager] Failed to open file for writing:" << localFilePath << file.errorString();
        reply->deleteLater();
        return;
    }

    if (file.write(imageData) < 0) {
        qWarning() << "[FraudManager] Failed to write image data to file:" << localFilePath;
        file.close();
        file.remove();
        reply->deleteLater();
        return;
    }

    file.close();

    // Convert to file:// URL format
    QString fileUrl = "file:///" + localFilePath.replace("\\", "/");

    // Cache the downloaded image path
    downloadedImages[eventKey] = fileUrl;

    qDebug() << "[FraudManager] Image successfully saved to:" << localFilePath << "URL:" << fileUrl;

    // Emit signal
    emit imageReceived(objectId, tag, fileUrl);

    reply->deleteLater();
}