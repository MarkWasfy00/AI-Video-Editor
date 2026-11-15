#pragma once

#include "media/AudioMixer.h"
#include "render/PreviewEngine.h"

#include <QObject>

#include <memory>

class QAudioSink;

namespace ve {

// Plays the mixed timeline audio through Qt Multimedia (pull mode).
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    void start(SnapshotPtr snapshot, TimeUs position);
    void stop();
    void updateSnapshot(SnapshotPtr snapshot);
    bool isAvailable() const { return m_available; }

    // Meter levels of the audio mixed for timeline time <= playhead since the
    // previous call (keeps the meters in sync with the picture). Returns false
    // if no new audio was played.
    bool takeLevels(TimeUs playhead, MixLevels& out);

private:
    class Generator;
    QAudioSink* m_sink = nullptr;
    Generator* m_generator = nullptr;
    bool m_available = false;
};

} // namespace ve
