#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QMutex>

class LiveFrameProvider : public QQuickImageProvider
{
public:
    LiveFrameProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    void setFrame(QImage img);

private:
    QMutex m_mutex;
    QImage m_frame;
};
