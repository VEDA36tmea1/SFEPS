#include "pwmtransmitter.h"
#include <QDebug>
#include <QHostAddress>

PwmTransmitter::PwmTransmitter(QObject *parent) : QObject(parent) {}

bool PwmTransmitter::isConnected() const
{
    if (m_mode == Mode::RaspberryPi) {
        return m_tcpSocket
               && m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    }
    // ESP8266 UDP: 소켓이 열려있으면 항상 전송 가능
    return m_udpSocket != nullptr;
}

QString PwmTransmitter::mode() const
{
    return (m_mode == Mode::RaspberryPi) ? QStringLiteral("raspi")
                                         : QStringLiteral("stm");
}

void PwmTransmitter::setMode(const QString &modeStr)
{
    const Mode newMode = (modeStr.toLower() == "stm" || modeStr.toLower() == "esp8266")
                             ? Mode::Esp8266
                             : Mode::RaspberryPi;
    if (newMode == m_mode) return;
    m_mode = newMode;
    qDebug() << "[PwmTransmitter] Mode changed to" << mode();
    emit modeChanged();
    emit connectedChanged();
}

void PwmTransmitter::setupTcpSocket()
{
    if (m_tcpSocket) return;
    m_tcpSocket = new QTcpSocket(this);
    connect(m_tcpSocket, &QTcpSocket::connected,
            this, &PwmTransmitter::onTcpConnected);
    connect(m_tcpSocket, &QTcpSocket::disconnected,
            this, &PwmTransmitter::onTcpDisconnected);
    connect(m_tcpSocket, &QAbstractSocket::errorOccurred,
            this, &PwmTransmitter::onTcpError);
}

void PwmTransmitter::connectTarget(const QString &host, int port)
{
    m_host    = host;
    m_port    = port;
    m_enabled = true;

    if (m_mode == Mode::RaspberryPi) {
        setupTcpSocket();
        if (m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
            qDebug() << "[PwmTransmitter] Connecting (TCP/RaspberryPi) to"
                     << host << ":" << port;
            m_tcpSocket->connectToHost(host, static_cast<quint16>(port));
        }
    } else {
        // ESP8266 UDP: 소켓만 생성 (연결 불필요)
        if (!m_udpSocket) {
            m_udpSocket = new QUdpSocket(this);
        }
        qDebug() << "[PwmTransmitter] ESP8266 UDP target set to"
                 << host << ":" << port;
        emit connectedChanged();
    }
}

void PwmTransmitter::disconnectTarget()
{
    m_enabled = false;
    if (m_reconnectTimer && m_reconnectTimer->isActive())
        m_reconnectTimer->stop();
    if (m_tcpSocket)
        m_tcpSocket->disconnectFromHost();
}

void PwmTransmitter::sendPwm(int pan, int tilt)
{
    if (!m_enabled) return;

    // 형식: "SET_PWM,PAN=<pan>,TILT=<tilt>\n"
    const QString cmd = QStringLiteral("SET_PWM,PAN=%1,TILT=%2\n").arg(pan).arg(tilt);
    const QByteArray data = cmd.toUtf8();

    if (m_mode == Mode::RaspberryPi) {
        if (!m_tcpSocket
            || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
            qWarning() << "[PwmTransmitter] RaspberryPi TCP not connected, drop PWM:"
                       << pan << tilt;
            return;
        }
        m_tcpSocket->write(data);
        emit pwmSent(pan, tilt);
        qDebug() << "[PwmTransmitter][RASPI] Sent PWM PAN=" << pan << "TILT=" << tilt;
    } else {
        // ESP8266 UDP
        if (!m_udpSocket || m_host.isEmpty()) {
            qWarning() << "[PwmTransmitter] ESP8266 UDP not configured, drop PWM";
            return;
        }
        m_udpSocket->writeDatagram(data, QHostAddress(m_host),
                                   static_cast<quint16>(m_port));
        emit pwmSent(pan, tilt);
        qDebug() << "[PwmTransmitter][ESP8266] Sent UDP PWM PAN=" << pan << "TILT=" << tilt;
    }
}

void PwmTransmitter::onTcpConnected()
{
    qDebug() << "[PwmTransmitter] TCP connected to" << m_host << ":" << m_port;
    m_reconnectDelayMs = 1000;
    if (m_reconnectTimer && m_reconnectTimer->isActive())
        m_reconnectTimer->stop();
    emit connectedChanged();
}

void PwmTransmitter::onTcpDisconnected()
{
    qDebug() << "[PwmTransmitter] TCP disconnected";
    emit connectedChanged();
    if (m_enabled) scheduleReconnect();
}

void PwmTransmitter::onTcpError(QAbstractSocket::SocketError /*err*/)
{
    const QString errStr = m_tcpSocket ? m_tcpSocket->errorString() : "unknown";
    qWarning() << "[PwmTransmitter] TCP error:" << errStr;
    emit transmitError(errStr);
    if (m_enabled) scheduleReconnect();
}

void PwmTransmitter::scheduleReconnect()
{
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
            if (m_enabled && m_tcpSocket
                && m_tcpSocket->state() == QAbstractSocket::UnconnectedState) {
                qDebug() << "[PwmTransmitter] Reconnect attempt to"
                         << m_host << ":" << m_port;
                m_tcpSocket->connectToHost(m_host, static_cast<quint16>(m_port));
            }
            m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 30000);
        });
    }
    m_reconnectTimer->start(m_reconnectDelayMs);
}
