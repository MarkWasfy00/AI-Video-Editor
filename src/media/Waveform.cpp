#include "media/Waveform.h"

#include "core/Project.h"
#include "media/AudioDecoder.h"

#include <algorithm>
#include <cmath>

namespace ve {

std::shared_ptr<const std::vector<float>> computeWaveform(const QString& path, TimeUs duration,
                                                          const std::atomic<bool>* cancel)
{
    // A low decode rate is plenty for peaks and keeps this fast.
    constexpr int kRate = 8000;
    constexpr int kPerBucket = kRate / kWaveformBucketsPerSecond;
    AudioDecoder dec;
    if (duration <= 0 || !dec.open(path, kRate)) return nullptr;

    const std::int64_t buckets = std::int64_t(std::ceil(usToSeconds(duration) * kWaveformBucketsPerSecond));
    auto peaks = std::make_shared<std::vector<float>>();
    peaks->reserve(size_t(buckets));
    constexpr int kChunkBuckets = kWaveformBucketsPerSecond; // decode one second at a time
    std::vector<float> buf(size_t(kChunkBuckets * kPerBucket) * 2);
    for (std::int64_t b = 0; b < buckets; b += kChunkBuckets) {
        if (cancel && cancel->load()) return nullptr;
        const int n = int(std::min<std::int64_t>(kChunkBuckets, buckets - b));
        const TimeUs t = TimeUs(b) * kUsPerSecond / kWaveformBucketsPerSecond;
        dec.read(t, n * kPerBucket, buf.data());
        for (int i = 0; i < n; ++i) {
            float peak = 0.0f;
            const float* s = buf.data() + size_t(i) * kPerBucket * 2;
            for (int k = 0; k < kPerBucket * 2; ++k) peak = std::max(peak, std::fabs(s[k]));
            peaks->push_back(std::min(peak, 1.0f));
        }
    }
    return peaks;
}

} // namespace ve
