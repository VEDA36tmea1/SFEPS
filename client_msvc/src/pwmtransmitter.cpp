#include "pwmtransmitter.h"
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>

PwmTransmitter::PwmTransmitter(QObject *parent) : QObject(parent)
{
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &PwmTransmitter::tryReconnect);
}

bool PwmTransmitter::isConnected() const
{
    if (m_mode == Mode::RaspberryPi)
        return m_tcpSocket && m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    return m_udpSocket != nullptr;   // UDP: 설정되면 전송 가능
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
    qDebug() << "[PwmTransmitter] Mode ->" << mode();
    emit modeChanged();
    emit connectedChanged();
}

void PwmTransmitter::connectTarget(const QString &host, int port)
{
    m_host    = host;
    m_port    = port;
    m_enabled = true;

    if (m_mode == Mode::RaspberryPi) {
        setupTcpSocket();
        doConnect();
    } else {
        if (!m_udpSocket)
            m_udpSocket = new QUdpSocket(this);
        qDebug() << "[PwmTransmitter] ESP8266 UDP target:" << host << ":" << port;
        emit connectedChanged();
    }
}

void PwmTransmitter::disconnectTarget()
{
    m_enabled = false;
    m_reconnectTimer->stop();
    if (m_tcpSocket) {
        m_tcpSocket->disconnectFromHost();
    }
    if (m_udpSocket) {
        m_udpSocket->close();
        m_udpSocket->deleteLater();
        m_udpSocket = nullptr;
    }
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

void PwmTransmitter::doConnect()
{
    if (!m_tcpSocket || !m_enabled) return;
    if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) return;

    qDebug() << "[PwmTransmitter] Connecting to Raspberry Pi" << m_host << ":" << m_port;
    m_tcpSocket->connectToHost(m_host, static_cast<quint16>(m_port));
}

void PwmTransmitter::onTcpConnected()
{
    qDebug() << "[PwmTransmitter] Connected to Raspberry Pi" << m_host << ":" << m_port;
    m_reconnectDelayMs = 2000;
    emit connectedChanged();
}

void PwmTransmitter::onTcpDisconnected()
{
    qDebug() << "[PwmTransmitter] Disconnected from Raspberry Pi. Retry in"
             << m_reconnectDelayMs << "ms";
    emit connectedChanged();
    if (m_enabled)
        m_reconnectTimer->start(m_reconnectDelayMs);
}

void PwmTransmitter::onTcpError(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err)
    const QString msg = m_tcpSocket ? m_tcpSocket->errorString() : "unknown";
    qWarning() << "[PwmTransmitter] TCP error:" << msg;
    emit transmitError(msg);

    // 지수 백오프: 2s → 4s → 8s → 최대 30s
    if (m_enabled) {
        m_reconnectTimer->start(m_reconnectDelayMs);
        m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 30000);
    }
}

void PwmTransmitter::tryReconnect()
{
    if (!m_enabled || !m_tcpSocket) return;
    if (m_tcpSocket->state() != QAbstractSocket::UnconnectedState) return;
    qDebug() << "[PwmTransmitter] Retrying connection...";
    doConnect();
}

// ── 공통 raw 전송 헬퍼 ──────────────────────────────────────────────────────
void PwmTransmitter::sendRaw(const QByteArray &data)
{
    if (!m_enabled) return;

    if (m_mode == Mode::RaspberryPi) {
        if (!m_tcpSocket ||
            m_tcpSocket->state() != QAbstractSocket::ConnectedState)
            return;
        m_tcpSocket->write(data);
    } else {
        if (!m_udpSocket || m_host.isEmpty()) return;
        m_udpSocket->writeDatagram(data, QHostAddress(m_host),
                                   static_cast<quint16>(m_port));
    }
}

void PwmTransmitter::sendTrackStart(const QString &objectId)
{
    const QByteArray data =
        QStringLiteral("TRACK_START|%1\n").arg(objectId).toUtf8();
    qDebug() << "[PwmTransmitter] →" << data.trimmed();
    sendRaw(data);
}

void PwmTransmitter::sendTrackEnd(const QString &objectId)
{
    const QByteArray data =
        QStringLiteral("TRACK_END|%1\n").arg(objectId).toUtf8();
    qDebug() << "[PwmTransmitter] →" << data.trimmed();
    sendRaw(data);
}

void PwmTransmitter::sendPwm(int pan, int tilt)
{
    if (!m_enabled) return;

    if (m_mode == Mode::RaspberryPi &&
        (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState)) {
        static qint64 lastWarnMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastWarnMs > 5000) {
            qWarning() << "[PwmTransmitter] Not connected, dropping PWM."
                       << "(서버 실행: python3 set_pwm_server.py --port" << m_port << ")";
            lastWarnMs = now;
        }
        return;
    }
    if (m_mode == Mode::Esp8266 && (!m_udpSocket || m_host.isEmpty())) {
        qWarning() << "[PwmTransmitter] ESP8266 UDP not configured";
        return;
    }

    const QByteArray data =
        QStringLiteral("SET_PWM,PAN=%1,TILT=%2\n").arg(pan).arg(tilt).toUtf8();
    sendRaw(data);
    emit pwmSent(pan, tilt);
}
