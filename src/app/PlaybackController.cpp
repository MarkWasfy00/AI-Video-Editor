#include "app/PlaybackController.h"

#include "core/Project.h"

#ifdef VE_HAVE_AUDIO
#include "audio/AudioPlayer.h"
#endif

#include <algorithm>

namespace ve {

PlaybackController::PlaybackController(Project* project, PreviewEngine* engine, QObject* parent)
    : QObject(parent)
    , m_project(project)
    , m_engine(engine)
{
#ifdef VE_HAVE_AUDIO
    m_audio = new AudioPlayer(this);
#endif
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PlaybackController::tick);
    connect(m_project, &Project::changed, this, &PlaybackController::refresh);
    connect(m_project, &Project::settingsChanged, this, &PlaybackController::refresh);
    connect(m_project, &Project::mediaChanged, this, &PlaybackController::refresh);
    connect(m_project, &Project::captionsChanged, this, &PlaybackController::refresh);
    connect(m_project, &Project::transcriptsChanged, this, &PlaybackController::refresh); // caption text
    refresh();
}

PlaybackController::~PlaybackController() = default;

bool PlaybackController::hasAudio() const
{
#ifdef VE_HAVE_AUDIO
    return m_audio && m_audio->isAvailable();
#else
    return false;
#endif
}

bool PlaybackController::takeAudioLevels(MixLevels& out)
{
#ifdef VE_HAVE_AUDIO
    if (m_playing && m_audio) return m_audio->takeLevels(m_position, out);
#endif
    Q_UNUSED(out);
    return false;
}

void PlaybackController::refresh()
{
    m_snapshot = std::make_shared<const TimelineSnapshot>(m_project->snapshot());
#ifdef VE_HAVE_AUDIO
    if (m_playing && m_audio) m_audio->updateSnapshot(m_snapshot);
#endif
    if (!m_playing) requestCurrentFrame();
}

void PlaybackController::requestCurrentFrame()
{
    m_engine->requestFrame(m_snapshot, m_position);
}

void PlaybackController::play()
{
    if (m_playing) return;
    const TimeUs dur = m_snapshot->duration();
    if (dur <= 0) return;
    if (m_position >= dur - frameDurationUs(m_snapshot->settings.fps)) m_position = 0;
    m_playing = true;
    m_playStartPos = m_position;
    m_clock.start();
#ifdef VE_HAVE_AUDIO
    if (m_audio) m_audio->start(m_snapshot, m_position);
#endif
    m_timer.start(std::max(4, int(1000.0 / m_snapshot->settings.fps / 2)));
    emit playingChanged(true);
}

void PlaybackController::pause()
{
    if (!m_playing) return;
    m_playing = false;
    m_timer.stop();
#ifdef VE_HAVE_AUDIO
    if (m_audio) m_audio->stop();
#endif
    m_position = snapToFrame(m_position, m_snapshot->settings.fps);
    requestCurrentFrame();
    emit positionChanged(m_position);
    emit playingChanged(false);
}

void PlaybackController::togglePlay()
{
    m_playing ? pause() : play();
}

void PlaybackController::stop()
{
    pause();
    seek(0);
}

void PlaybackController::seek(TimeUs t)
{
    const bool wasPlaying = m_playing;
    if (wasPlaying) pause();
    m_position = std::clamp<TimeUs>(t, 0, std::max<TimeUs>(m_snapshot->duration(), 0));
    requestCurrentFrame();
    emit positionChanged(m_position);
    if (wasPlaying) play();
}

void PlaybackController::stepFrames(int frames)
{
    const double fps = m_snapshot->settings.fps;
    seek(snapToFrame(m_position, fps) + frames * frameDurationUs(fps));
}

void PlaybackController::goToEnd()
{
    seek(m_snapshot->duration());
}

void PlaybackController::tick()
{
    const TimeUs dur = m_snapshot->duration();
    m_position = m_playStartPos + m_clock.nsecsElapsed() / 1000;
    if (m_position >= dur) {
        m_position = dur;
        pause();
        return;
    }
    requestCurrentFrame();
    emit positionChanged(m_position);
}

} // namespace ve
