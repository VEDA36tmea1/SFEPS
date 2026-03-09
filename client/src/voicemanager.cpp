#include "voicemanager.h"
#include <QDebug>
#include <QProcessEnvironment>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QFile>

static bool parseEnvBool(const QProcessEnvironment &env, const QString &key, bool defaultValue)
{
    const QString raw = env.value(key).trimmed().toLower();
    if (raw.isEmpty()) return defaultValue;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return defaultValue;
}

// 서버 주소/포트 (Audio_Speaker_Unit·서버와 동일 포트)
static const char * const AUDIO_SERVER_HOST = "192.168.0.92";
static const quint16 AUDIO_SERVER_PORT = 5556;

VoiceManager::VoiceManager(QObject *parent) : QObject(parent)
{
    // Default socket is plaintext; startRecording will recreate socket for TLS if requested
    m_socket = new QTcpSocket(this);
    m_forwardDevice = new SocketForwardDevice(m_socket, this);

    // RAW PCM: 16kHz, 모노, S16_LE (서버·Audio_Speaker_Unit과 동일)
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    m_audioSource = new QAudioSource(format, this);

    connect(m_audioSource, &QAudioSource::stateChanged, this, &VoiceManager::handleStateChanged);
    connect(m_socket, &QTcpSocket::connected, this, &VoiceManager::onSocketConnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &VoiceManager::onSocketError);
}

VoiceManager::~VoiceManager()
{
    if (m_active) stopAndSendData();
}

void VoiceManager::toggleMicrophone()
{
    if (!m_active) {
        startRecording();
    } else {
        stopAndSendData();
    }
}

void VoiceManager::startRecording()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString host = env.value(
        "AUDIO_SERVER_HOST",
        env.value("FRAUD_SERVER_HOST", QString::fromUtf8(AUDIO_SERVER_HOST))
    );
    const bool tlsEnabled = parseEnvBool(env, "SFEPS_CLIENT_TLS_ENABLE", false);

    // Recreate socket if switching between plaintext and TLS
    const bool currentIsSsl = (qobject_cast<QSslSocket *>(m_socket) != nullptr);
    if (tlsEnabled != currentIsSsl) {
        m_socket->abort();
        m_socket->deleteLater();
        if (tlsEnabled) {
            m_socket = new QSslSocket(this);
        } else {
            m_socket = new QTcpSocket(this);
        }
        m_forwardDevice->deleteLater();
        m_forwardDevice = new SocketForwardDevice(m_socket, this);
        connect(m_socket, &QTcpSocket::connected, this, &VoiceManager::onSocketConnected);
        connect(m_socket, &QTcpSocket::errorOccurred, this, &VoiceManager::onSocketError);
    }

    m_socket->abort();
    if (tlsEnabled) {
        QSslSocket *ssl = qobject_cast<QSslSocket *>(m_socket);
        if (ssl) {
            ssl->setPeerVerifyMode(QSslSocket::VerifyPeer);
            const QString caPath = env.value("SFEPS_CLIENT_CA_FILE").trimmed();
            if (!caPath.isEmpty()) {
                QFile f(caPath);
                if (f.open(QIODevice::ReadOnly)) {
                    const QList<QSslCertificate> certs = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
                    if (!certs.isEmpty()) {
                        QSslConfiguration cfg = ssl->sslConfiguration();
                        cfg.setCaCertificates(certs);
                        ssl->setSslConfiguration(cfg);
                    } else {
                        qWarning() << "[VoiceManager] No valid CA certificates in" << caPath;
                    }
                } else {
                    qWarning() << "[VoiceManager] Failed to open CA file" << caPath << f.errorString();
                }
            }

            const QString serverName = env.value("SFEPS_CLIENT_TLS_SERVER_NAME").trimmed();
            if (!serverName.isEmpty()) ssl->setPeerVerifyName(serverName);

            qDebug() << "Connecting to audio server (TLS)" << host << ":" << AUDIO_SERVER_PORT;
            ssl->connectToHostEncrypted(host, AUDIO_SERVER_PORT);
        }
    } else {
        qDebug() << "Connecting to audio server (Plain)" << host << ":" << AUDIO_SERVER_PORT;
        m_socket->connectToHost(host, AUDIO_SERVER_PORT);
    }
    m_active = true;
    emit activeChanged();
    qDebug() << "Connecting to audio server..." << host << ":" << AUDIO_SERVER_PORT << "(RAW streaming)";
}

void VoiceManager::onSocketConnected()
{
    m_forwardDevice->open(QIODevice::WriteOnly);
    m_audioSource->start(m_forwardDevice);
    qDebug() << "Recording started. Streaming RAW PCM to server...";
}

void VoiceManager::onSocketError(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err);
    if (!m_active) return;
    qDebug() << "Audio socket error:" << m_socket->errorString();
    stopAndSendData();
    emit errorOccurred("서버 연결 실패 (Audio Port " + QString::number(AUDIO_SERVER_PORT) + ")");
}

void VoiceManager::stopAndSendData()
{
    if (m_audioSource->state() != QAudio::StoppedState)
        m_audioSource->stop();
    if (m_forwardDevice->isOpen())
        m_forwardDevice->close();
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->waitForDisconnected(1000);
    m_active = false;
    emit activeChanged();
    qDebug() << "Recording stopped. Stream closed.";
}

void VoiceManager::handleStateChanged(QAudio::State newState)
{
    if (newState == QAudio::StoppedState) {
        if (m_audioSource->error() != QAudio::NoError)
            qDebug() << "Audio Source Error:" << m_audioSource->error();
    }
}
