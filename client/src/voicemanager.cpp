#include "voicemanager.h"
#include <QDebug>

VoiceManager::VoiceManager(QObject *parent) : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    
    // 오디오 포맷 설정
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    m_audioSource = new QAudioSource(format, this);
    
    connect(m_audioSource, &QAudioSource::stateChanged, this, &VoiceManager::handleStateChanged);
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
    m_audioData.clear();
    m_buffer.setBuffer(&m_audioData);
    m_buffer.open(QIODevice::WriteOnly | QIODevice::Truncate);

    m_audioSource->start(&m_buffer);
    m_active = true;
    emit activeChanged();
    qDebug() << "Recording started...";
}

void VoiceManager::stopAndSendData()
{
    m_audioSource->stop();
    m_buffer.close();
    m_active = false;
    emit activeChanged();
    qDebug() << "Recording stopped. Data size:" << m_audioData.size();

    if (m_audioData.isEmpty()) return;

    // 서버로 전송 (포트 5556)
    m_socket->abort();
    m_socket->connectToHost("192.168.0.89", 5556);

    if (m_socket->waitForConnected(3000)) {
        qDebug() << "Sending audio data to server...";
        m_socket->write(m_audioData);
        m_socket->flush();
        m_socket->disconnectFromHost();
    } else {
        qDebug() << "Failed to connect to audio server";
        emit errorOccurred("서버 연결 실패 (Audio Port 5556)");
    }
}

void VoiceManager::handleStateChanged(QAudio::State newState)
{
    if (newState == QAudio::StoppedState) {
        if (m_audioSource->error() != QAudio::NoError) {
            qDebug() << "Audio Source Error:" << m_audioSource->error();
        }
    }
}
