#include "media/VideoDecoder.h"

#include "media/HwAccel.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>

namespace ve {

namespace {
constexpr TimeUs kMaxForwardDecode = 2 * kUsPerSecond; // farther than this -> seek
}

VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open(const QString& path, QString* error)
{
    m_fmt = ff::openInput(path, error);
    if (!m_fmt) return false;
    m_streamIndex = ff::findVideoStream(m_fmt.get());
    if (m_streamIndex < 0) {
        if (error) *error = QStringLiteral("no video stream");
        return false;
    }
    m_stream = m_fmt->streams[m_streamIndex];
    // Only decode the stream we need.
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (static_cast<int>(i) != m_streamIndex) m_fmt->streams[i]->discard = AVDISCARD_ALL;

    m_isImage = ff::isStillImage(m_fmt.get(), m_stream);
    m_dec = ff::openDecoder(m_stream, 0, error, [this](AVCodecContext* ctx, const AVCodec* codec) {
        if (!m_isImage) setupHardware(ctx, codec); // stills are cheaper on the CPU
    });
    if (!m_dec && m_hwPixFmt != AV_PIX_FMT_NONE) {
        // The GPU path failed to open; retry on the CPU.
        m_hwPixFmt = AV_PIX_FMT_NONE;
        m_hwName = nullptr;
        m_dec = ff::openDecoder(m_stream, 0, error);
    }
    if (!m_dec) return false;
    m_swFrame = ff::makeFrame();
    m_duration = ff::containerDuration(m_fmt.get(), m_stream);
    const AVRational fr = av_guess_frame_rate(m_fmt.get(), m_stream, nullptr);
    if (fr.num > 0 && fr.den > 0)
        m_frameDur = std::max<TimeUs>(1000, av_rescale_q(1, av_inv_q(fr), ff::kUsTimeBase));
    m_pkt = ff::makePacket();
    m_pending = ff::makeFrame();
    return true;
}

void VideoDecoder::setupHardware(AVCodecContext* ctx, const AVCodec* codec)
{
    for (AVHWDeviceType type : hw::decodeCandidates()) {
        // Does this decoder support the device type through a device context?
        AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
                pixFmt = cfg->pix_fmt;
                break;
            }
        }
        if (pixFmt == AV_PIX_FMT_NONE) continue;
        AVBufferRef* dev = hw::acquireDevice(type);
        if (!dev) continue;
        ctx->hw_device_ctx = dev; // ownership moves to the codec context
        ctx->opaque = this;
        ctx->get_format = &VideoDecoder::selectFormat;
        m_hwPixFmt = pixFmt;
        m_hwName = av_hwdevice_get_type_name(type);
        m_hwFallback = false;
        return;
    }
}

// Called by FFmpeg once the stream parameters are known: pick the GPU surface
// format if offered, otherwise fall back to a software format.
AVPixelFormat VideoDecoder::selectFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
{
    auto* self = static_cast<VideoDecoder*>(ctx->opaque);
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == self->m_hwPixFmt) {
            self->m_hwFallback = false;
            return *p;
        }
    self->m_hwFallback = true; // e.g. 4:4:4 or 10-bit profile the GPU can't decode
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

void VideoDecoder::setMaxOutputSize(int maxWidth, int maxHeight)
{
    if (maxWidth != m_maxW || maxHeight != m_maxH) m_backCache.clear();
    m_maxW = maxWidth;
    m_maxH = maxHeight;
}

int VideoDecoder::width() const { return m_dec ? m_dec->width : 0; }
int VideoDecoder::height() const { return m_dec ? m_dec->height : 0; }

bool VideoDecoder::seekTo(TimeUs t)
{
    const std::int64_t ts = ff::usToPts(t, m_stream);
    int rc = av_seek_frame(m_fmt.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0)
        rc = avformat_seek_file(m_fmt.get(), m_streamIndex, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(m_dec.get());
    m_eof = false;
    m_flushed = false;
    m_hasPending = false;
    m_last.reset();
    m_lastPts = -1;
    return rc >= 0;
}

bool VideoDecoder::decodeNext(AVFrame* out)
{
    for (;;) {
        int rc = avcodec_receive_frame(m_dec.get(), out);
        if (rc == 0) return true;
        if (rc != AVERROR(EAGAIN)) return false;     // EOF or error
        if (m_eof) {
            if (m_flushed) return false;
            avcodec_send_packet(m_dec.get(), nullptr); // drain
            m_flushed = true;
            continue;
        }
        rc = av_read_frame(m_fmt.get(), m_pkt.get());
        if (rc < 0) {
            m_eof = true;
            continue;
        }
        if (m_pkt->stream_index == m_streamIndex)
            avcodec_send_packet(m_dec.get(), m_pkt.get());
        av_packet_unref(m_pkt.get());
    }
}

static TimeUs framePts(const AVFrame* f, const AVStream* st)
{
    const std::int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    return ff::ptsToUs(pts, st);
}

VideoFramePtr VideoDecoder::frameAt(TimeUs t)
{
    if (!m_dec) return {};
    t = std::max<TimeUs>(0, t);

    if (m_isImage) {
        if (!m_last) {
            auto f = ff::makeFrame();
            if (decodeNext(f.get())) {
                m_last = convert(f.get());
                m_lastPts = 0;
            }
        }
        return m_last;
    }

    // Frames kept for reverse playback
    if (!m_backCache.empty()) {
        auto it = m_backCache.upper_bound(t);
        if (it != m_backCache.begin() && t < std::prev(it)->first + m_frameDur &&
            t >= m_backCache.begin()->first)
            return std::prev(it)->second;
    }
    // Still inside the frame we returned last time?
    if (m_last && t >= m_lastPts && t < m_lastPts + m_frameDur)
        return m_last;
    // A short step backwards (reverse playback): decode that stretch once.
    if (m_last && t < m_lastPts && m_lastPts - t <= 2 * kUsPerSecond) {
        const TimeUs until = m_backCache.empty() ? m_lastPts + m_frameDur : m_backCache.begin()->first;
        return fillBackCache(t, std::max(until, t + m_frameDur));
    }
    m_backCache.clear();
    // The next decoded frame starts after t -> keep showing the last one.
    if (m_last && m_hasPending && t >= m_lastPts && t < framePts(m_pending.get(), m_stream))
        return m_last;

    const bool needSeek = !m_last || t < m_lastPts || t > m_lastPts + kMaxForwardDecode;
    if (needSeek) seekTo(t);

    auto cur = ff::makeFrame();
    auto tmp = ff::makeFrame();
    bool haveCur = false;
    TimeUs curPts = 0;

    for (;;) {
        if (m_hasPending) {
            av_frame_unref(tmp.get());
            av_frame_move_ref(tmp.get(), m_pending.get());
            m_hasPending = false;
        } else if (!decodeNext(tmp.get())) {
            break;
        }
        const TimeUs pts = framePts(tmp.get(), m_stream);
        if (pts > t && (haveCur || m_last)) {
            // Overshot: remember this frame for the next request.
            av_frame_unref(m_pending.get());
            av_frame_move_ref(m_pending.get(), tmp.get());
            m_hasPending = true;
            break;
        }
        av_frame_unref(cur.get());
        av_frame_move_ref(cur.get(), tmp.get());
        haveCur = true;
        curPts = pts;
        if (pts + m_frameDur > t) break;
    }

    if (haveCur) {
        m_last = convert(cur.get());
        m_lastPts = curPts;
    }
    return m_last;
}

VideoFramePtr VideoDecoder::fillBackCache(TimeUs t, TimeUs until)
{
    constexpr size_t kMaxBytes = 256u << 20;
    m_backCache.clear();
    seekTo(t);
    size_t bytes = 0;
    auto f = ff::makeFrame();
    while (decodeNext(f.get())) {
        const TimeUs pts = framePts(f.get(), m_stream);
        if (pts + m_frameDur <= t) continue;                  // before the wanted frame
        if (pts >= until) {                                   // reached what was shown before
            av_frame_unref(m_pending.get());
            av_frame_move_ref(m_pending.get(), f.get());
            m_hasPending = true;
            break;
        }
        VideoFramePtr conv = convert(f.get());
        if (!conv) break;
        bytes += conv->rgba.size();
        m_backCache[pts] = conv;
        m_last = conv;
        m_lastPts = pts;
        if (bytes > kMaxBytes) break;
    }
    auto it = m_backCache.upper_bound(t);
    if (it == m_backCache.begin()) return m_last;
    return std::prev(it)->second;
}

VideoFramePtr VideoDecoder::convert(const AVFrame* frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) return {};
    const AVFrame* f = frame;
    const char* hwDevice = nullptr;
    if (m_hwPixFmt != AV_PIX_FMT_NONE && frame->format == m_hwPixFmt) {
        // Decoded on the GPU: download the surface (usually NV12/P010).
        av_frame_unref(m_swFrame.get());
        if (av_hwframe_transfer_data(m_swFrame.get(), frame, 0) < 0) return {};
        av_frame_copy_props(m_swFrame.get(), frame);
        f = m_swFrame.get();
        hwDevice = m_hwName;
    }
    int dw = f->width;
    int dh = f->height;
    if (m_maxW > 0 && m_maxH > 0 && (dw > m_maxW || dh > m_maxH)) {
        const double s = std::min(static_cast<double>(m_maxW) / dw, static_cast<double>(m_maxH) / dh);
        dw = std::max(2, static_cast<int>(dw * s));
        dh = std::max(2, static_cast<int>(dh * s));
    }

    SwsContext* ctx = sws_getCachedContext(m_sws.release(),
        f->width, f->height, static_cast<AVPixelFormat>(f->format),
        dw, dh, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_sws.reset(ctx);
    if (!ctx) return {};

    const int cs = (f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720))
                       ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    const int srcRange = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    sws_setColorspaceDetails(ctx, sws_getCoefficients(cs), srcRange,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

    auto out = std::make_shared<VideoFrame>();
    out->width = dw;
    out->height = dh;
    out->pts = framePts(frame, m_stream);
    out->hwDevice = hwDevice;
    out->rgba.resize(static_cast<size_t>(dw) * dh * 4);
    std::uint8_t* dst[4] = {out->rgba.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {dw * 4, 0, 0, 0};
    sws_scale(ctx, f->data, f->linesize, 0, f->height, dst, dstStride);
    return out;
}

QImage VideoDecoder::toImage(const VideoFrame& f)
{
    QImage img(f.width, f.height, QImage::Format_RGBA8888);
    for (int y = 0; y < f.height; ++y)
        std::memcpy(img.scanLine(y), f.rgba.data() + static_cast<size_t>(y) * f.width * 4, static_cast<size_t>(f.width) * 4);
    return img;
}

} // namespace ve
