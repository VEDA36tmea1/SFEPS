#include "live_frame_provider.h"

LiveFrameProvider::LiveFrameProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage LiveFrameProvider::requestImage(const QString &, QSize *size, const QSize &)
{
    QMutexLocker locker(&m_mutex);
    if (size)
        *size = m_frame.size();
    return m_frame;
}

void LiveFrameProvider::setFrame(QImage img)
{
    QMutexLocker locker(&m_mutex);
    m_frame = std::move(img);
}
