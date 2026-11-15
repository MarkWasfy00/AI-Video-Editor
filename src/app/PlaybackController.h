#pragma once

#include "media/AudioMixer.h"
#include "render/PreviewEngine.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace ve {

class AudioPlayer;
class Project;

// Owns the playhead: play/pause/seek, frame requests to the PreviewEngine
// and (optional) audio playback.
class PlaybackController : public QObject {
    Q_OBJECT
public:
    PlaybackController(Project* project, PreviewEngine* engine, QObject* parent = nullptr);
    ~PlaybackController() override;

    TimeUs position() const { return m_position; }
    bool isPlaying() const { return m_playing; }
    bool hasAudio() const;
    SnapshotPtr snapshot() const { return m_snapshot; }

    // Mixer meter levels of the audio played since the last call (false when
    // nothing new was played, e.g. paused or no audio device).
    bool takeAudioLevels(MixLevels& out);

public slots:
    void play();
    void pause();
    void togglePlay();
    void stop();                       // pause + go to start
    void seek(ve::TimeUs t);
    void stepFrames(int frames);
    void goToEnd();
    void refresh();                    // timeline changed: rebuild snapshot + redraw

signals:
    void positionChanged(ve::TimeUs t);
    void playingChanged(bool playing);

private:
    void tick();
    void requestCurrentFrame();

    Project* m_project;
    PreviewEngine* m_engine;
    AudioPlayer* m_audio = nullptr;
    SnapshotPtr m_snapshot;
    QTimer m_timer;
    QElapsedTimer m_clock;
    TimeUs m_playStartPos = 0;
    TimeUs m_position = 0;
    bool m_playing = false;
};

} // namespace ve
