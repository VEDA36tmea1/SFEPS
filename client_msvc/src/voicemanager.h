#ifndef VOICEMANAGER_H
#define VOICEMANAGER_H

#include <QObject>
#include <QIODevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QAudioFormat>
#include <QTcpSocket>

// 마이크에서 들어온 RAW PCM을 소켓으로 바로 전달하는 디바이스 (스트리밍)
class SocketForwardDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit SocketForwardDevice(QTcpSocket *socket, QObject *parent = nullptr)
        : QIODevice(parent), m_socket(socket) {}

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 maxSize) override
    {
        if (!m_socket || !m_socket->isValid() || !m_socket->isWritable())
            return -1;
        qint64 n = m_socket->write(data, maxSize);
        if (n > 0)
            m_socket->flush();
        return n;
    }

private:
    QTcpSocket *m_socket = nullptr;
};

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
    void onSocketConnected();
    void onSocketError(QAbstractSocket::SocketError err);

private:
    void startRecording();
    void stopAndSendData();

    QAudioSource *m_audioSource = nullptr;
    QTcpSocket *m_socket = nullptr;
    SocketForwardDevice *m_forwardDevice = nullptr;
    bool m_active = false;
};

#endif // VOICEMANAGER_H
