#pragma once

#include "core/Time.h"
#include "media/FFmpegUtil.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace ve {

// Decodes an audio stream to interleaved stereo float at a fixed sample rate.
// Optimised for sequential reads; seeks when the request jumps.
class AudioDecoder {
public:
    static constexpr int kChannels = 2;

    AudioDecoder() = default;
    bool open(const QString& path, int outSampleRate, QString* error = nullptr);
    bool isOpen() const { return m_dec != nullptr; }

    // Fills `frames` stereo frames starting at source time t. Areas without
    // data are silence. `out` must hold frames * 2 floats.
    void read(TimeUs t, int frames, float* out);

private:
    void seekTo(TimeUs t);
    bool decodeMore();                 // appends decoded samples to m_buf

    ff::FormatPtr m_fmt;
    ff::CodecCtxPtr m_dec;
    ff::SwrPtr m_swr;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_frame;
    AVStream* m_stream = nullptr;
    int m_streamIndex = -1;
    int m_rate = 48000;
    bool m_eof = false;
    bool m_flushed = false;
    bool m_finished = false;

    std::vector<float> m_buf;          // interleaved stereo
    std::int64_t m_bufStart = -1;      // sample index of m_buf[0]
    std::vector<float> m_convert;
};

} // namespace ve
