#ifndef PWMTRANSMITTER_H
#define PWMTRANSMITTER_H

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>

/**
 * PwmTransmitter: RBF 계산된 PWM(PAN/TILT)을 하드웨어로 전송
 *
 *  - "raspi" 모드: Raspberry Pi(set_pwm_server.py)에 TCP 클라이언트로 접속
 *                  Raspberry Pi에서 실행: python3 set_pwm_server.py --port 5566 -v
 *
 *  - "stm"   모드: ESP8266에 UDP 패킷 전송
 *
 * 환경변수:
 *   SFEPS_PWM_MODE  = "raspi" | "stm"   (기본: raspi)
 *   SFEPS_PWM_HOST  = Raspberry Pi IP    (기본: 192.168.0.100)
 *   SFEPS_PWM_PORT  = 포트               (기본: 5566)
 */
class PwmTransmitter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)

public:
    explicit PwmTransmitter(QObject *parent = nullptr);
    ~PwmTransmitter() override = default;

    bool    isConnected() const;
    QString mode() const;

    Q_INVOKABLE void setMode(const QString &mode);
    Q_INVOKABLE void connectTarget(const QString &host, int port);
    Q_INVOKABLE void disconnectTarget();

public slots:
    void sendPwm(int pan, int tilt);
    // TRACK_START|<id>\n → STM32 레이저 ON
    void sendTrackStart(const QString &objectId);
    // TRACK_END|<id>\n   → STM32 레이저 OFF
    void sendTrackEnd(const QString &objectId);

signals:
    void connectedChanged();
    void modeChanged();
    void pwmSent(int pan, int tilt);
    void transmitError(const QString &error);

private slots:
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError err);
    void tryReconnect();

private:
    enum class Mode { RaspberryPi, Esp8266 };

    Mode    m_mode    = Mode::RaspberryPi;
    QString m_host    = "192.168.0.100";
    int     m_port    = 5566;
    bool    m_enabled = false;

    QTcpSocket *m_tcpSocket       = nullptr;
    QUdpSocket *m_udpSocket       = nullptr;
    QTimer     *m_reconnectTimer  = nullptr;
    int         m_reconnectDelayMs = 2000;

    void setupTcpSocket();
    void doConnect();
    void sendRaw(const QByteArray &data);
};

#endif // PWMTRANSMITTER_H
