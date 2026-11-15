#include "render/PreviewEngine.h"

#include "media/FrameSource.h"

#include <QMutexLocker>

#include <atomic>

namespace ve {

class PreviewEngine::Worker : public QObject {
    Q_OBJECT
public:
    void post(SnapshotPtr snap, TimeUs t)
    {
        QMutexLocker lock(&m_mutex);
        m_snap = std::move(snap);
        m_time = t;
        m_hasRequest = true;
        if (!m_scheduled) {
            m_scheduled = true;
            QMetaObject::invokeMethod(this, &Worker::process, Qt::QueuedConnection);
        }
    }

    void requestReset() { m_reset = true; }

signals:
    void frameReady(ve::CompositeFramePtr frame);

private:
    void process()
    {
        for (;;) {
            SnapshotPtr snap;
            TimeUs t = 0;
            {
                QMutexLocker lock(&m_mutex);
                if (!m_hasRequest) {
                    m_scheduled = false;
                    return;
                }
                snap = std::move(m_snap);
                t = m_time;
                m_hasRequest = false;
            }
            if (m_reset.exchange(false)) m_source.reset();
            if (snap) emit frameReady(m_source.build(*snap, t));
        }
    }

    QMutex m_mutex;
    SnapshotPtr m_snap;
    TimeUs m_time = 0;
    bool m_hasRequest = false;
    bool m_scheduled = false;
    FrameSource m_source;
    std::atomic<bool> m_reset{false};
};

PreviewEngine::PreviewEngine(QObject* parent)
    : QObject(parent)
{
    m_thread.setObjectName(QStringLiteral("PreviewDecode"));
    m_worker = new Worker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &Worker::frameReady, this, &PreviewEngine::frameReady, Qt::QueuedConnection);
    m_thread.start();
}

PreviewEngine::~PreviewEngine()
{
    m_thread.quit();
    m_thread.wait();
}

void PreviewEngine::resetDecoders()
{
    m_worker->requestReset();
}

void PreviewEngine::requestFrame(SnapshotPtr snapshot, TimeUs t)
{
    m_worker->post(std::move(snapshot), t);
}

} // namespace ve

#include "PreviewEngine.moc"
