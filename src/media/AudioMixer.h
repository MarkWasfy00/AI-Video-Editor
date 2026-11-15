#pragma once

#include "core/Project.h"
#include "media/AudioDecoder.h"

#include <QHash>

#include <functional>
#include <memory>
#include <vector>

namespace ve {

// Stereo signal statistics for one block of samples (post-fader).
struct ChannelLevels {
    float peak[2] = {0.0f, 0.0f};      // max |sample| per channel (linear)
    double sumSq[2] = {0.0, 0.0};      // sum of squares per channel
    std::int64_t frames = 0;

    void add(const float* interleaved, int frames);
    void merge(const ChannelLevels& o);
    double rms(int ch) const;          // linear
    double peakDb(int ch) const;       // -inf as kMinGainDb (-60)
    double rmsDb(int ch) const;
};

// Levels of a mixed block: one entry per track (by id) + the master bus.
struct MixLevels {
    QHash<QString, ChannelLevels> tracks;
    ChannelLevels master;              // after master gain, before clipping
    std::int64_t clippedSamples = 0;   // master samples with |x| > 1 (0 dBFS)

    bool isEmpty() const { return master.frames == 0; }
    void clear();
    void merge(const MixLevels& o);
};

// Which tracks are heard: muted tracks are silent; if any track is soloed,
// only soloed tracks are heard.
bool trackAudible(const TimelineSnapshot& s, const Track& t);

// Balance-law pan: centre = unity on both sides, hard left = right silent.
void panGains(double pan, double& left, double& right);

// Mixes all audible clips of a timeline into interleaved stereo float.
// Only audio tracks are mixed; video tracks are silent.
// Signal flow per clip: source -> clip volume x fades -> track fader + pan
// (track mute/solo) -> sum -> master fader/mute -> hard clip at 0 dBFS.
// Owns one AudioDecoder per clip so overlapping clips of the same file work.
class AudioMixer {
public:
    explicit AudioMixer(int sampleRate = 48000) : m_rate(sampleRate) {}

    int sampleRate() const { return m_rate; }

    // Mix [t, t + frames / rate) into out (frames * 2 floats).
    // If levels is given it is cleared and filled with this block's levels.
    void mix(const TimelineSnapshot& s, TimeUs t, int frames, float* out, MixLevels* levels = nullptr);
    void reset() { m_decoders.clear(); }

private:
    AudioDecoder* decoderFor(const Clip& clip, const MediaItem& media);

    int m_rate;
    QHash<QString, std::shared_ptr<AudioDecoder>> m_decoders; // by clip id (null = failed)
    std::vector<float> m_tmp;
    std::vector<float> m_src;
    std::vector<float> m_track;
    int m_calls = 0;
};

// Offline analysis of the mix over [start, end): peak / RMS per track and on
// the master bus, plus clipped samples. `progress` may return false to abort.
MixLevels analyzeMix(const TimelineSnapshot& s, TimeUs start, TimeUs end, int sampleRate = 48000,
                     const std::function<bool(double)>& progress = {});

} // namespace ve
