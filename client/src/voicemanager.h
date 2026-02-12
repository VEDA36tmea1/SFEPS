#ifndef VOICEMANAGER_H
#define VOICEMANAGER_H

#include <QObject>
#include <QAudioSource>
#include <QMediaDevices>
#include <QAudioFormat>
#include <QTcpSocket>
#include <QBuffer>

class VoiceManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    explicit VoiceManager(QObject *parent = nullptr);
    ~VoiceManager();

    Q_INVOKABLE void toggleMicrophone();
    bool isActive() const { return m_active; }

signals:
    void activeChanged();
    void errorOccurred(const QString &message);

private slots:
    void handleStateChanged(QAudio::State newState);

private:
    void startRecording();
    void stopAndSendData();

    QAudioSource *m_audioSource = nullptr;
    QTcpSocket *m_socket = nullptr;
    QByteArray m_audioData;
    QBuffer m_buffer;
    bool m_active = false;
};

#endif // VOICEMANAGER_H
