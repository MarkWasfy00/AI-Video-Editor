#include "audio/AudioPlayer.h"

#include "media/AudioMixer.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

namespace ve {

namespace {
constexpr int kRate = 48000;
constexpr int kMeterChunk = 1024;
}

// QIODevice that produces mixed timeline audio on demand.
class AudioPlayer::Generator : public QIODevice {
public:
    explicit Generator(QAudioFormat::SampleFormat fmt) : m_format(fmt) {}

    void reset(SnapshotPtr snap, TimeUs pos)
    {
        QMutexLocker lock(&m_mutex);
        m_snap = std::move(snap);
        m_sample = static_cast<std::int64_t>(std::llround(usToSeconds(pos) * kRate));
        m_mixer.reset();
        m_levels.clear();
    }

    bool takeLevels(TimeUs playhead, MixLevels& out)
    {
        QMutexLocker lock(&m_mutex);
        const std::int64_t pos = static_cast<std::int64_t>(std::llround(usToSeconds(playhead) * kRate));
        out.clear();
        bool any = false;
        while (!m_levels.empty() && m_levels.front().first <= pos) {
            out.merge(m_levels.front().second);
            m_levels.pop_front();
            any = true;
        }
        return any;
    }

    void setSnapshot(SnapshotPtr snap)
    {
        QMutexLocker lock(&m_mutex);
        m_snap = std::move(snap);
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        QMutexLocker lock(&m_mutex);
        const int bytesPerFrame = m_format == QAudioFormat::Float ? 8 : 4;
        const int frames = static_cast<int>(std::min<qint64>(maxlen / bytesPerFrame, 8192));
        if (frames <= 0 || !m_snap) return 0;

        // Mix in small chunks so the meters get ~20 ms resolution.
        m_buf.resize(size_t(frames) * 2);
        for (int done = 0; done < frames;) {
            const int n = std::min(frames - done, kMeterChunk);
            const TimeUs t = static_cast<TimeUs>(std::llround(double(m_sample) * kUsPerSecond / kRate));
            MixLevels lv;
            m_mixer.mix(*m_snap, t, n, m_buf.data() + size_t(done) * 2, &lv);
            m_levels.emplace_back(m_sample, std::move(lv));
            if (m_levels.size() > 512) m_levels.pop_front();
            m_sample += n;
            done += n;
        }

        if (m_format == QAudioFormat::Float) {
            std::memcpy(data, m_buf.data(), size_t(frames) * 8);
        } else {
            auto* out = reinterpret_cast<qint16*>(data);
            for (size_t i = 0; i < m_buf.size(); ++i)
                out[i] = static_cast<qint16>(std::clamp(m_buf[i], -1.0f, 1.0f) * 32767.0f);
        }
        return qint64(frames) * bytesPerFrame;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QMutex m_mutex;
    QAudioFormat::SampleFormat m_format;
    SnapshotPtr m_snap;
    std::int64_t m_sample = 0;
    AudioMixer m_mixer{kRate};
    std::vector<float> m_buf;
    std::deque<std::pair<std::int64_t, MixLevels>> m_levels; // (start sample, levels)
};

AudioPlayer::AudioPlayer(QObject* parent)
    : QObject(parent)
{
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;

    QAudioFormat fmt;
    fmt.setSampleRate(kRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    if (!dev.isFormatSupported(fmt)) fmt.setSampleFormat(QAudioFormat::Int16);
    if (!dev.isFormatSupported(fmt)) return;

    m_generator = new Generator(fmt.sampleFormat());
    m_generator->open(QIODevice::ReadOnly);
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(kRate * (fmt.sampleFormat() == QAudioFormat::Float ? 8 : 4) / 10); // ~100 ms
    m_available = true;
}

AudioPlayer::~AudioPlayer()
{
    if (m_sink) m_sink->stop();
    delete m_generator;
}

void AudioPlayer::start(SnapshotPtr snapshot, TimeUs position)
{
    if (!m_available) return;
    m_sink->stop();
    m_generator->reset(std::move(snapshot), position);
    m_sink->start(m_generator);
}

void AudioPlayer::stop()
{
    if (m_sink) m_sink->stop();
}

bool AudioPlayer::takeLevels(TimeUs playhead, MixLevels& out)
{
    return m_generator && m_generator->takeLevels(playhead, out);
}

void AudioPlayer::updateSnapshot(SnapshotPtr snapshot)
{
    if (m_generator) m_generator->setSnapshot(std::move(snapshot));
}

} // namespace ve
