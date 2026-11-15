#include "media/AudioMixer.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------
void ChannelLevels::add(const float* x, int n)
{
    float p0 = peak[0], p1 = peak[1];
    double s0 = 0.0, s1 = 0.0;
    for (int i = 0; i < n; ++i) {
        const float l = x[i * 2], r = x[i * 2 + 1];
        p0 = std::max(p0, std::fabs(l));
        p1 = std::max(p1, std::fabs(r));
        s0 += double(l) * l;
        s1 += double(r) * r;
    }
    peak[0] = p0;
    peak[1] = p1;
    sumSq[0] += s0;
    sumSq[1] += s1;
    frames += n;
}

void ChannelLevels::merge(const ChannelLevels& o)
{
    for (int c = 0; c < 2; ++c) {
        peak[c] = std::max(peak[c], o.peak[c]);
        sumSq[c] += o.sumSq[c];
    }
    frames += o.frames;
}

double ChannelLevels::rms(int ch) const
{
    return frames > 0 ? std::sqrt(sumSq[ch] / double(frames)) : 0.0;
}

double ChannelLevels::peakDb(int ch) const
{
    return peak[ch] > 0 ? std::max(kMinGainDb, 20.0 * std::log10(double(peak[ch]))) : kMinGainDb;
}

double ChannelLevels::rmsDb(int ch) const
{
    const double r = rms(ch);
    return r > 0 ? std::max(kMinGainDb, 20.0 * std::log10(r)) : kMinGainDb;
}

void MixLevels::clear()
{
    tracks.clear();
    master = {};
    clippedSamples = 0;
}

void MixLevels::merge(const MixLevels& o)
{
    for (auto it = o.tracks.cbegin(); it != o.tracks.cend(); ++it) tracks[it.key()].merge(it.value());
    master.merge(o.master);
    clippedSamples += o.clippedSamples;
}

bool trackAudible(const TimelineSnapshot& s, const Track& t)
{
    if (!t.isAudio() || t.muted) return false;
    const bool anySolo = std::any_of(s.tracks.begin(), s.tracks.end(), [](const Track& x) { return x.solo; });
    return !anySolo || t.solo;
}

void panGains(double pan, double& left, double& right)
{
    pan = std::clamp(pan, -1.0, 1.0);
    left = pan > 0 ? 1.0 - pan : 1.0;
    right = pan < 0 ? 1.0 + pan : 1.0;
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------
AudioDecoder* AudioMixer::decoderFor(const Clip& clip, const MediaItem& media)
{
    auto it = m_decoders.find(clip.id);
    if (it != m_decoders.end()) return it.value().get();
    auto dec = std::make_shared<AudioDecoder>();
    if (!dec->open(media.path, m_rate)) dec.reset();
    m_decoders.insert(clip.id, dec);
    return dec.get();
}

void AudioMixer::mix(const TimelineSnapshot& s, TimeUs t, int frames, float* out, MixLevels* levels)
{
    if (levels) levels->clear();
    if (frames <= 0) return;
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * 2);

    const auto toSample = [this](TimeUs us) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(us) * m_rate / kUsPerSecond));
    };
    const auto toUs = [this](std::int64_t sample) {
        return static_cast<TimeUs>(std::llround(static_cast<double>(sample) * kUsPerSecond / m_rate));
    };

    const std::int64_t ws = toSample(t);
    const std::int64_t we = ws + frames;
    m_track.resize(static_cast<size_t>(frames) * 2);

    for (const auto& track : s.tracks) {
        if (!track.isAudio()) continue; // video tracks have no sound: it lives on the linked audio clip
        const double fader = dbToGain(track.gainDb);
        if (!trackAudible(s, track) || fader <= 0.0) {
            if (levels) levels->tracks[track.id].frames = frames; // silent block
            continue;
        }
        double panL = 1.0, panR = 1.0;
        panGains(track.pan, panL, panR);
        const float gl = static_cast<float>(fader * panL);
        const float gr = static_cast<float>(fader * panR);

        bool any = false;
        for (const auto& clip : track.clips) {
            if (clip.volume <= 0.0 || clip.freeze) continue; // a frozen frame has no sound
            const std::int64_t cs = toSample(clip.start);
            const std::int64_t ce = toSample(clip.end());
            const std::int64_t a = std::max(ws, cs);
            const std::int64_t b = std::min(we, ce);
            if (a >= b) continue;
            const auto mit = s.media.constFind(clip.mediaId);
            if (mit == s.media.constEnd() || !mit->info.hasAudio) continue;
            AudioDecoder* dec = decoderFor(clip, *mit);
            if (!dec) continue;

            if (!any) {
                std::fill(m_track.begin(), m_track.end(), 0.0f);
                any = true;
            }
            const int n = static_cast<int>(b - a);
            m_tmp.resize(static_cast<size_t>(n) * 2);
            if (clip.plainTiming()) {
                const TimeUs srcT = clip.in + (toUs(a) - clip.start);
                dec->read(srcT, n, m_tmp.data());
            } else {
                // Speed / curve / reverse: read the source span this block covers and
                // resample it (pitch follows the speed, like tape).
                const TimeUs s0 = clip.sourceTimeAt(toUs(a)), s1 = clip.sourceTimeAt(toUs(b));
                const TimeUs lo = std::min(s0, s1), hi = std::max(s0, s1);
                const int m = int(std::min<std::int64_t>(toSample(hi - lo) + 4, std::int64_t(n) * 40 + 4));
                m_src.resize(static_cast<size_t>(m) * 2);
                dec->read(lo, m, m_src.data());
                for (int i = 0; i < n; ++i) {
                    const double pos = double(clip.sourceTimeAt(toUs(a + i)) - lo) * m_rate / kUsPerSecond;
                    const int k = std::clamp(int(pos), 0, m - 2);
                    const float f = float(std::clamp(pos - k, 0.0, 1.0));
                    m_tmp[size_t(i) * 2] = m_src[size_t(k) * 2] * (1 - f) + m_src[size_t(k + 1) * 2] * f;
                    m_tmp[size_t(i) * 2 + 1] = m_src[size_t(k) * 2 + 1] * (1 - f) + m_src[size_t(k + 1) * 2 + 1] * f;
                }
            }

            float* dst = m_track.data() + (a - ws) * 2;
            const bool hasFade = clip.fadeIn > 0 || clip.fadeOut > 0;
            for (int i = 0; i < n; ++i) {
                double g = clip.volume;
                if (hasFade) g *= clip.fadeFactor(toUs(a + i));
                dst[i * 2] += static_cast<float>(m_tmp[i * 2] * g);
                dst[i * 2 + 1] += static_cast<float>(m_tmp[i * 2 + 1] * g);
            }
        }

        if (!any) {
            if (levels) levels->tracks[track.id].frames = frames;
            continue;
        }
        for (int i = 0; i < frames; ++i) {
            m_track[i * 2] *= gl;
            m_track[i * 2 + 1] *= gr;
            out[i * 2] += m_track[i * 2];
            out[i * 2 + 1] += m_track[i * 2 + 1];
        }
        if (levels) levels->tracks[track.id].add(m_track.data(), frames);
    }

    // Master bus
    const float master = s.master.muted ? 0.0f : static_cast<float>(dbToGain(s.master.gainDb));
    if (master != 1.0f)
        for (int i = 0; i < frames * 2; ++i) out[i] *= master;
    if (levels) {
        levels->master.add(out, frames);
        std::int64_t clipped = 0;
        for (int i = 0; i < frames * 2; ++i) clipped += std::fabs(out[i]) > 1.0f;
        levels->clippedSamples = clipped;
    }
    for (int i = 0; i < frames * 2; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);

    // Periodically drop decoders of clips that no longer exist.
    if (++m_calls % 64 == 0) {
        QSet<QString> alive;
        for (const auto& track : s.tracks)
            for (const auto& clip : track.clips) alive.insert(clip.id);
        for (auto it = m_decoders.begin(); it != m_decoders.end();) {
            if (!alive.contains(it.key())) it = m_decoders.erase(it);
            else ++it;
        }
    }
}

MixLevels analyzeMix(const TimelineSnapshot& s, TimeUs start, TimeUs end, int sampleRate,
                     const std::function<bool(double)>& progress)
{
    MixLevels total;
    for (const auto& t : s.tracks)
        if (t.isAudio()) total.tracks.insert(t.id, {});
    if (end <= start) return total;

    AudioMixer mixer(sampleRate);
    constexpr int kBlock = 4096;
    std::vector<float> buf(kBlock * 2);
    MixLevels block;
    const std::int64_t first = std::llround(usToSeconds(start) * sampleRate);
    const std::int64_t last = std::llround(usToSeconds(end) * sampleRate);
    for (std::int64_t pos = first; pos < last; pos += kBlock) {
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, last - pos));
        const TimeUs t = static_cast<TimeUs>(std::llround(double(pos) * kUsPerSecond / sampleRate));
        mixer.mix(s, t, n, buf.data(), &block);
        total.merge(block);
        if (progress && !progress(double(pos - first) / double(last - first))) break;
    }
    return total;
}

} // namespace ve
