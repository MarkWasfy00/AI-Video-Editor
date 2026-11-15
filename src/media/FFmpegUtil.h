#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/Time.h"

#include <QString>

#include <functional>
#include <memory>

namespace ve::ff {

inline QString errorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

constexpr AVRational kUsTimeBase{1, 1'000'000};

struct FormatCloser { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecCtxFree { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameFree { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketFree { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct SwsFree { void operator()(SwsContext* p) const { sws_freeContext(p); } };
struct SwrFree { void operator()(SwrContext* p) const { swr_free(&p); } };

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxFree>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFree>;
using SwrPtr = std::unique_ptr<SwrContext, SwrFree>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

// Opens a media file for reading. Returns nullptr on failure.
inline FormatPtr openInput(const QString& path, QString* error = nullptr)
{
    AVFormatContext* raw = nullptr;
    const QByteArray p = path.toUtf8();
    int rc = avformat_open_input(&raw, p.constData(), nullptr, nullptr);
    if (rc < 0) {
        if (error) *error = errorString(rc);
        return {};
    }
    FormatPtr fmt(raw);
    rc = avformat_find_stream_info(fmt.get(), nullptr);
    if (rc < 0) {
        if (error) *error = errorString(rc);
        return {};
    }
    return fmt;
}

// Opens a decoder for the given stream. `configure` (optional) runs right
// before avcodec_open2, e.g. to attach a hardware device.
inline CodecCtxPtr openDecoder(AVStream* st, int threads, QString* error = nullptr,
                               const std::function<void(AVCodecContext*, const AVCodec*)>& configure = {})
{
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (error) *error = QStringLiteral("no decoder for codec");
        return {};
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) return {};
    if (avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0) return {};
    ctx->thread_count = threads;
    ctx->pkt_timebase = st->time_base;
    if (configure) configure(ctx.get(), codec);
    const int rc = avcodec_open2(ctx.get(), codec, nullptr);
    if (rc < 0) {
        if (error) *error = errorString(rc);
        return {};
    }
    return ctx;
}

// Stream timestamp -> microseconds relative to the stream start.
inline TimeUs ptsToUs(std::int64_t pts, const AVStream* st)
{
    if (pts == AV_NOPTS_VALUE) return 0;
    const std::int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    return av_rescale_q(pts - start, st->time_base, kUsTimeBase);
}

inline std::int64_t usToPts(TimeUs t, const AVStream* st)
{
    const std::int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    return start + av_rescale_q(t, kUsTimeBase, st->time_base);
}

// Index of the main video stream (ignores embedded cover art), or -1.
inline int findVideoStream(AVFormatContext* fmt)
{
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) return -1;
    if (fmt->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC) return -1;
    return idx;
}

inline int findAudioStream(AVFormatContext* fmt)
{
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return idx < 0 ? -1 : idx;
}

// True for single still images (png, jpg, ...), which FFmpeg exposes as a
// one-frame video stream.
inline bool isStillImage(const AVFormatContext* fmt, const AVStream* st)
{
    const QString name = QString::fromLatin1(fmt->iformat->name);
    if (name.contains(QLatin1String("image2")) || name.endsWith(QLatin1String("_pipe")))
        return true;
    switch (st->codecpar->codec_id) {
    case AV_CODEC_ID_PNG:
    case AV_CODEC_ID_BMP:
    case AV_CODEC_ID_TIFF:
    case AV_CODEC_ID_WEBP:
    case AV_CODEC_ID_MJPEG:
        return st->nb_frames <= 1 && fmt->duration <= AV_TIME_BASE / 10;
    default:
        return false;
    }
}

inline TimeUs containerDuration(const AVFormatContext* fmt, const AVStream* st)
{
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        return av_rescale_q(fmt->duration, AVRational{1, AV_TIME_BASE}, kUsTimeBase);
    if (st && st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return av_rescale_q(st->duration, st->time_base, kUsTimeBase);
    return 0;
}

} // namespace ve::ff
