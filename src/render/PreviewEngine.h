#pragma once

#include "core/Project.h"
#include "render/Composite.h"

#include <QMutex>
#include <QObject>
#include <QThread>

#include <memory>

namespace ve {

class FrameSource;

using SnapshotPtr = std::shared_ptr<const TimelineSnapshot>;

// Decodes and assembles preview frames on a worker thread. Requests are
// coalesced: if the worker is busy only the most recent request is kept,
// so scrubbing never builds up a backlog.
class PreviewEngine : public QObject {
    Q_OBJECT
public:
    explicit PreviewEngine(QObject* parent = nullptr);
    ~PreviewEngine() override;

    void requestFrame(SnapshotPtr snapshot, TimeUs t);

    // Drop all open decoders (e.g. after the hardware decode mode changed);
    // they are reopened with the new settings on the next request.
    void resetDecoders();

signals:
    void frameReady(ve::CompositeFramePtr frame);

private:
    class Worker;
    QThread m_thread;
    Worker* m_worker = nullptr;
};

} // namespace ve
