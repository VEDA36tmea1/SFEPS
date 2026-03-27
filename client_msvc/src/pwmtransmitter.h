#ifndef PWMTRANSMITTER_H
#define PWMTRANSMITTER_H

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>

/**
 * PwmTransmitter: camera_RBF.cpp에서 수신한 PWM(PAN/TILT)을
 * 두 가지 하드웨어 모드로 전송하는 클래스.
 *
 *  - "raspi"  모드: Raspberry Pi에 TCP 이더넷 (기본 포트 5566)
 *  - "stm"    모드: ESP8266을 거쳐 STM에 UDP 무선 전송 (기본 포트 5566)
 *
 * 환경변수:
 *   SFEPS_PWM_MODE  = "raspi" | "stm"    (기본: raspi)
 *   SFEPS_PWM_HOST  = IP 주소             (기본: 192.168.0.100)
 *   SFEPS_PWM_PORT  = 포트                (기본: 5566)
 *
 * QML에서 pwmTransmitter.mode, pwmTransmitter.connected 조회 가능.
 */
class PwmTransmitter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)

public:
    explicit PwmTransmitter(QObject *parent = nullptr);
    ~PwmTransmitter() override = default;

    bool isConnected() const;
    QString mode() const;

    Q_INVOKABLE void setMode(const QString &mode);   // "raspi" or "stm"
    Q_INVOKABLE void connectTarget(const QString &host, int port);
    Q_INVOKABLE void disconnectTarget();

public slots:
    void sendPwm(int pan, int tilt);

signals:
    void connectedChanged();
    void modeChanged();
    void pwmSent(int pan, int tilt);
    void transmitError(const QString &error);

private slots:
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError err);
    void scheduleReconnect();

private:
    enum class Mode { RaspberryPi, Esp8266 };

    Mode    m_mode   = Mode::RaspberryPi;
    QString m_host   = "192.168.0.100";
    int     m_port   = 5566;
    bool    m_enabled = false;

    // RaspberryPi: TCP
    QTcpSocket *m_tcpSocket  = nullptr;
    // ESP8266: UDP (connectionless)
    QUdpSocket *m_udpSocket  = nullptr;

    QTimer *m_reconnectTimer = nullptr;
    int     m_reconnectDelayMs = 1000;

    void setupTcpSocket();
};

#endif // PWMTRANSMITTER_H
