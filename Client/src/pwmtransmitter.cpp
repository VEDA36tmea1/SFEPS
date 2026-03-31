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
    if (m_mode == Mode::RaspberryPi) {
        return m_tcpSocket && m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    }
    if (m_mode == Mode::Esp8266) {
        return m_udpSocket != nullptr;   // UDP: 설정되면 전송 가능
    }
    const bool tcpOk = m_tcpSocket && m_tcpSocket->state() == QAbstractSocket::ConnectedState;
    const bool udpOk = m_udpSocket != nullptr;
    return tcpOk || udpOk;
}

QString PwmTransmitter::mode() const
{
    if (m_mode == Mode::RaspberryPi) return QStringLiteral("raspi");
    if (m_mode == Mode::Esp8266) return QStringLiteral("stm");
    return QStringLiteral("both");
}

void PwmTransmitter::setMode(const QString &modeStr)
{
    const QString m = modeStr.toLower().trimmed();
    Mode newMode = Mode::RaspberryPi;
    if (m == "stm" || m == "esp8266") newMode = Mode::Esp8266;
    else if (m == "both") newMode = Mode::Both;
    if (newMode == m_mode) return;
    m_mode = newMode;
    qDebug() << "[PwmTransmitter] Mode ->" << mode();
    emit modeChanged();
    emit connectedChanged();
}

void PwmTransmitter::setStmTransport(const QString &transport)
{
    const QString t = transport.toLower().trimmed();
    m_stmTransport = (t == "tcp") ? StmTransport::Tcp : StmTransport::Udp;
    qDebug() << "[PwmTransmitter] STM transport ->" << ((m_stmTransport == StmTransport::Tcp) ? "tcp" : "udp");
}

void PwmTransmitter::connectTarget(const QString &host, int port)
{
    m_host    = host;
    m_port    = port;
    m_enabled = true;

    if (m_mode == Mode::RaspberryPi || m_mode == Mode::Both) {
        setupTcpSocket();
        doConnect();
    }
    if (m_mode == Mode::Esp8266 || m_mode == Mode::Both) {
        if (m_stmTransport == StmTransport::Tcp) {
            setupStmTcpSocket();
            doConnectStmTcp();
        } else {
            if (!m_udpSocket)
                m_udpSocket = new QUdpSocket(this);
            const QString udpHost = (m_mode == Mode::Both) ? m_secondaryHost : m_host;
            const int udpPort = (m_mode == Mode::Both) ? m_secondaryPort : m_port;
            qDebug() << "[PwmTransmitter] ESP8266 UDP target:" << udpHost << ":" << udpPort;
            emit connectedChanged();
        }
    }
}

void PwmTransmitter::connectSecondaryTarget(const QString &host, int port)
{
    m_secondaryHost = host;
    m_secondaryPort = port;
    if (m_mode == Mode::Both) {
        if (!m_udpSocket) m_udpSocket = new QUdpSocket(this);
        qDebug() << "[PwmTransmitter] Secondary target (ESP8266):" << m_secondaryHost << ":" << m_secondaryPort;
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
    if (m_stmTcpSocket) {
        m_stmTcpSocket->disconnectFromHost();
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

void PwmTransmitter::setupStmTcpSocket()
{
    if (m_stmTcpSocket) return;
    m_stmTcpSocket = new QTcpSocket(this);
    connect(m_stmTcpSocket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[PwmTransmitter] Connected to ESP8266(TCP)" << m_secondaryHost << ":" << m_secondaryPort;
        emit connectedChanged();
    });
    connect(m_stmTcpSocket, &QTcpSocket::disconnected, this, [this]() {
        qDebug() << "[PwmTransmitter] Disconnected from ESP8266(TCP)";
        emit connectedChanged();
    });
    connect(m_stmTcpSocket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        qWarning() << "[PwmTransmitter] ESP8266 TCP error:"
                   << (m_stmTcpSocket ? m_stmTcpSocket->errorString() : QStringLiteral("unknown"));
    });
}

void PwmTransmitter::doConnectStmTcp()
{
    if (!m_enabled || !m_stmTcpSocket) return;
    const QString host = (m_mode == Mode::Both) ? m_secondaryHost : m_host;
    const int port = (m_mode == Mode::Both) ? m_secondaryPort : m_port;
    if (host.isEmpty() || port <= 0) return;
    if (m_stmTcpSocket->state() != QAbstractSocket::UnconnectedState) return;
    qDebug() << "[PwmTransmitter] Connecting to ESP8266(TCP)" << host << ":" << port;
    m_stmTcpSocket->connectToHost(host, static_cast<quint16>(port));
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
    } else if (m_mode == Mode::Esp8266) {
        if (m_stmTransport == StmTransport::Tcp) {
            if (!m_stmTcpSocket || m_stmTcpSocket->state() != QAbstractSocket::ConnectedState) return;
            m_stmTcpSocket->write(data);
        } else {
            if (!m_udpSocket || m_host.isEmpty()) return;
            m_udpSocket->writeDatagram(data, QHostAddress(m_host), static_cast<quint16>(m_port));
        }
    } else {
        if (m_tcpSocket && m_tcpSocket->state() == QAbstractSocket::ConnectedState) {
            m_tcpSocket->write(data);
        }
        if (m_stmTransport == StmTransport::Tcp) {
            if (m_stmTcpSocket && m_stmTcpSocket->state() == QAbstractSocket::ConnectedState) {
                m_stmTcpSocket->write(data);
            }
        } else if (m_udpSocket && !m_secondaryHost.isEmpty()) {
            m_udpSocket->writeDatagram(data, QHostAddress(m_secondaryHost), static_cast<quint16>(m_secondaryPort));
        }
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
    // 요청사항: TRACK_END는 RaspberryPi(1차 TCP)로만 보낸다.
    // - Mode::Both에서도 보조(ESP8266)로는 END를 보내지 않는다.
    if (m_mode == Mode::Esp8266) {
        // raspberry TCP가 없으면(ESP only) 기존 동작을 유지
        sendRaw(data);
        return;
    }

    if (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        // 연결이 없으면 END를 전송할 수 없음
        return;
    }
    m_tcpSocket->write(data);
}

void PwmTransmitter::sendPwm(int pan, int tilt)
{
    if (!m_enabled) return;

    if ((m_mode == Mode::RaspberryPi || m_mode == Mode::Both) &&
        (!m_tcpSocket || m_tcpSocket->state() != QAbstractSocket::ConnectedState)) {
        static qint64 lastWarnMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastWarnMs > 5000) {
            qWarning() << "[PwmTransmitter] Not connected, dropping PWM."
                       << "(서버 실행: python3 set_pwm_server.py --port" << m_port << ")";
            lastWarnMs = now;
        }
        if (m_mode == Mode::RaspberryPi) return;
    }
    if (m_mode == Mode::Esp8266 && (!m_udpSocket || m_host.isEmpty())) {
        if (m_stmTransport == StmTransport::Udp) {
            qWarning() << "[PwmTransmitter] ESP8266 UDP not configured";
            return;
        }
    }
    if (m_mode == Mode::Esp8266 && m_stmTransport == StmTransport::Tcp &&
        (!m_stmTcpSocket || m_stmTcpSocket->state() != QAbstractSocket::ConnectedState)) {
        static qint64 lastStmWarnMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastStmWarnMs > 5000) {
            qWarning() << "[PwmTransmitter] ESP8266(TCP) not connected, dropping PWM.";
            lastStmWarnMs = now;
        }
        return;
    }
    if (m_mode == Mode::Both && m_stmTransport == StmTransport::Udp &&
        (!m_udpSocket || m_secondaryHost.isEmpty())) {
        qWarning() << "[PwmTransmitter] ESP8266 UDP secondary target not configured";
    }
    if (m_mode == Mode::Both && m_stmTransport == StmTransport::Tcp &&
        (!m_stmTcpSocket || m_stmTcpSocket->state() != QAbstractSocket::ConnectedState)) {
        static qint64 lastBothStmWarnMs = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastBothStmWarnMs > 5000) {
            qWarning() << "[PwmTransmitter] ESP8266(TCP) secondary not connected (Raspi only will receive).";
            lastBothStmWarnMs = now;
        }
    }

    const QByteArray data =
        QStringLiteral("SET_PWM,PAN=%1,TILT=%2\n").arg(pan).arg(tilt).toUtf8();
    sendRaw(data);
    emit pwmSent(pan, tilt);
}
