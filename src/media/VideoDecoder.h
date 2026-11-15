#pragma once

#include "core/Time.h"
#include "media/FFmpegUtil.h"

#include <QImage>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace ve {

// Tightly packed RGBA8 frame, ready for GPU upload.
struct VideoFrame {
    int width = 0;
    int height = 0;
    TimeUs pts = 0;
    std::vector<std::uint8_t> rgba;     // width * height * 4 bytes
    const char* hwDevice = nullptr;     // e.g. "d3d11va" when decoded on the GPU
};
using VideoFramePtr = std::shared_ptr<const VideoFrame>;

// Decodes video (or a still image) to RGBA. Optimised for mostly-forward
// access: sequential requests keep decoding, big jumps trigger a seek.
// Not thread-safe; each consumer owns its own decoder.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const QString& path, QString* error = nullptr);
    bool isOpen() const { return m_dec != nullptr; }

    // Frame that is visible at source time t (microseconds from media start).
    VideoFramePtr frameAt(TimeUs t);

    // Limit the output size (keeps aspect). 0 = native size.
    void setMaxOutputSize(int maxWidth, int maxHeight);

    int width() const;
    int height() const;
    TimeUs duration() const { return m_duration; }

    // GPU decoding (see media/HwAccel.h). Falls back to the CPU per stream if
    // the GPU cannot handle the codec/profile.
    bool usesHardware() const { return m_hwPixFmt != AV_PIX_FMT_NONE && !m_hwFallback; }
    const char* hardwareDevice() const { return usesHardware() ? m_hwName : nullptr; }

    static QImage toImage(const VideoFrame& f);

private:
    bool seekTo(TimeUs t);
    // Decodes the next frame into `out`. Returns false at EOF / error.
    bool decodeNext(AVFrame* out);
    VideoFramePtr convert(const AVFrame* f);
    void setupHardware(AVCodecContext* ctx, const AVCodec* codec);
    static AVPixelFormat selectFormat(AVCodecContext* ctx, const AVPixelFormat* fmts);

    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    const char* m_hwName = nullptr;
    bool m_hwFallback = false;
    ff::FramePtr m_swFrame;           // GPU -> system memory download target

    ff::FormatPtr m_fmt;
    ff::CodecCtxPtr m_dec;
    ff::SwsPtr m_sws;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_pending;           // decoded frame past the last request
    bool m_hasPending = false;
    AVStream* m_stream = nullptr;
    int m_streamIndex = -1;
    bool m_eof = false;
    bool m_flushed = false;
    bool m_isImage = false;
    TimeUs m_duration = 0;
    TimeUs m_frameDur = 33'333;
    int m_maxW = 0;
    int m_maxH = 0;

    VideoFramePtr m_last;             // last frame returned
    TimeUs m_lastPts = -1;
    // Reverse playback: stepping back one frame would mean seeking to a key
    // frame and decoding forward every time. Instead, one backward step decodes
    // the frames up to the previous position once and keeps them here.
    std::map<TimeUs, VideoFramePtr> m_backCache;
    VideoFramePtr fillBackCache(TimeUs t, TimeUs until);
};

} // namespace ve
