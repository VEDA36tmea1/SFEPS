#ifndef VIDEOARCHIVEMANAGER_H
#define VIDEOARCHIVEMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QSslSocket>

class VideoArchiveManager : public QObject {
    Q_OBJECT
public:
    explicit VideoArchiveManager(QObject *parent = nullptr);

    Q_INVOKABLE void connectCatalog();
    Q_INVOKABLE void requestPlayUrl(const QString &id);

signals:
    void snapshotBegin(int total);
    void snapshotRecord(const QString &id, const QString &createdAt);
    void snapshotEnd(int total);
    void recordingAdded(const QString &id, const QString &createdAt);
    void recordingDeleted(const QString &id);
    void playUrlReady(const QString &id, const QString &createdAt, const QString &url);
    void catalogError(const QString &code, const QString &message);
    void playError(const QString &code, const QString &message);
    void storageUpdated(qulonglong usedBytes, qulonglong capBytes);
    void sysStatusUpdated(float cpuTempC, float cpuUsagePct);

private slots:
    void onConnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);

private:
    void ensureSocket();
    void sendLine(const QString &line);

    QTcpSocket *tcp = nullptr;
    QSslSocket *ssl = nullptr;
    QByteArray buffer;
    bool useTls = false;
    QString host;
    int port = 5559; // current port (depends on useTls)
    int tlsPort = 6559;
    int plainPort = 5559;
    bool tlsAttemptInProgress = false;
    bool snapshotInProgress = false;
};

#endif // VIDEOARCHIVEMANAGER_H
