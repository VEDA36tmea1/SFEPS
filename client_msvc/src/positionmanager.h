#ifndef POSITIONMANAGER_H
#define POSITIONMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QVariant>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

class PositionManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentSubscribedId READ currentSubscribedId NOTIFY currentSubscribedIdChanged)
public:
    explicit PositionManager(QObject *parent = nullptr);
    ~PositionManager();

    Q_INVOKABLE void connectPositionServer(const QString &host = "192.168.0.82", int port = 5558);
    Q_INVOKABLE void sendPositionCommand(const QString &msg);
    Q_INVOKABLE void unsubscribeCurrent();
    Q_INVOKABLE QString currentSubscribedId() const;
    Q_INVOKABLE void disconnectPositionServer();

signals:
    void positionsUpdated(const QVariantList &list);
    void positionDisconnected();
    void positionConnected();
    void currentSubscribedIdChanged();

private slots:
    void flushPending();
    void onPosReadyRead();
    void processPosBuffer();

private:
    void attachPosSocketSignals();
    void flushQueuedCommands();

    QTcpSocket *posSocket = nullptr;
    QByteArray posRecvBuffer;
    QStringList m_pendingCommands;
    bool m_parseScheduled = false;
    QString lastPosHost;
    int lastPosPort = 0;
    QTimer *m_batchTimer = nullptr;
    // Use a map keyed by id to keep only the latest update per object (reduces duplicates)
    QHash<QString, QVariantMap> m_pendingMap;
    QList<QString> m_pendingOrder; // insertion order for trimming oldest
    int m_maxPending = 100; // cap unique pending items to avoid UI overload
    // Track last-seen timestamps per id and TTL for active items
    QHash<QString, qint64> m_lastSeen;
    int m_ttlMs = 2000; // milliseconds to keep an object without updates before dropping
    QSet<QString> m_suspected; // IDs currently marked as suspected/fraud
    // reconnect/backoff
    QTimer *m_reconnectTimer = nullptr;
    int m_reconnectDelayMs = 1000;
    const int m_reconnectMinMs = 1000;
    const int m_reconnectMaxMs = 30000;
    void scheduleReconnect();
    void resetReconnectBackoff();
    // currently subscribed/tracked id (client-side state)
    QString m_currentSubscribedId;
    void setCurrentSubscribedId(const QString &id);
};

#endif // POSITIONMANAGER_H
