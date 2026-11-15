#pragma once

#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ve {

// All timeline positions/durations are integer microseconds.
using TimeUs = std::int64_t;

constexpr TimeUs kUsPerSecond = 1'000'000;

inline TimeUs secondsToUs(double s) { return static_cast<TimeUs>(std::llround(s * kUsPerSecond)); }
inline double usToSeconds(TimeUs t) { return static_cast<double>(t) / kUsPerSecond; }

inline TimeUs frameDurationUs(double fps) { return fps > 0 ? static_cast<TimeUs>(std::llround(kUsPerSecond / fps)) : 33'333; }

// Snap a time to the nearest frame boundary.
inline TimeUs snapToFrame(TimeUs t, double fps)
{
    if (fps <= 0) return t;
    const double frame = std::round(usToSeconds(t) * fps);
    return static_cast<TimeUs>(std::llround(frame / fps * kUsPerSecond));
}

// HH:MM:SS:FF timecode
inline QString formatTimecode(TimeUs t, double fps)
{
    if (t < 0) t = 0;
    const std::int64_t totalFrames = static_cast<std::int64_t>(std::floor(usToSeconds(t) * fps + 1e-6));
    const int ifps = std::max(1, static_cast<int>(std::lround(fps)));
    const std::int64_t ff = totalFrames % ifps;
    const std::int64_t totalSec = totalFrames / ifps;
    return QString::asprintf("%02lld:%02lld:%02lld:%02lld",
                             static_cast<long long>(totalSec / 3600),
                             static_cast<long long>((totalSec / 60) % 60),
                             static_cast<long long>(totalSec % 60),
                             static_cast<long long>(ff));
}

} // namespace ve
