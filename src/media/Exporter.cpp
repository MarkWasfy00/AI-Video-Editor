#include "media/Exporter.h"

#include "media/AudioMixer.h"
#include "media/FFmpegUtil.h"
#include "media/FrameSource.h"
#include "media/HwAccel.h"
#include "render/OffscreenRenderer.h"

#include <QFile>

#include <cmath>
#include <cstring>
#include <vector>

namespace ve {

namespace {

constexpr int kAudioRate = 48000;

struct OutputGuard {
    AVFormatContext* oc = nullptr;
    bool ioOpened = false;
    ~OutputGuard()
    {
        if (!oc) return;
        if (ioOpened) avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
};

struct Stream {
    AVStream* st = nullptr;
    ff::CodecCtxPtr ctx;
};

bool fail(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return false;
}

// Sends one frame (or nullptr to flush) and writes all resulting packets.
bool encode(AVFormatContext* oc, Stream& s, AVFrame* frame, AVPacket* pkt, QString* error)
{
    int rc = avcodec_send_frame(s.ctx.get(), frame);
    if (rc < 0 && rc != AVERROR_EOF) return fail(error, QStringLiteral("encode: ") + ff::errorString(rc));
    for (;;) {
        rc = avcodec_receive_packet(s.ctx.get(), pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) return fail(error, QStringLiteral("encode: ") + ff::errorString(rc));
        av_packet_rescale_ts(pkt, s.ctx->time_base, s.st->time_base);
        pkt->stream_index = s.st->index;
        rc = av_interleaved_write_frame(oc, pkt); // takes ownership of the packet data
        if (rc < 0) return fail(error, QStringLiteral("write: ") + ff::errorString(rc));
    }
}

AVPixelFormat pixelFormatFor(const QString& encoder)
{
    if (encoder == QLatin1String("prores_ks")) return AV_PIX_FMT_YUV422P10LE;
    // Quick Sync, AMF and VideoToolbox want NV12 system-memory frames.
    if (encoder.endsWith(QLatin1String("_qsv")) || encoder.endsWith(QLatin1String("_amf")) ||
        encoder.endsWith(QLatin1String("_videotoolbox")))
        return AV_PIX_FMT_NV12;
    return AV_PIX_FMT_YUV420P;
}

// Constant-quality settings mapped onto each encoder family's options.
void applyQuality(AVCodecContext* vc, const ExportSettings& cfg)
{
    const QString& e = cfg.videoEncoder;
    const QByteArray q = QByteArray::number(cfg.crf);
    if (e == QLatin1String("libx264") || e == QLatin1String("libx265")) {
        av_opt_set(vc->priv_data, "preset", "medium", 0);
        av_opt_set(vc->priv_data, "crf", q.constData(), 0);
    } else if (e == QLatin1String("libvpx-vp9")) {
        av_opt_set(vc->priv_data, "crf", q.constData(), 0);
        av_opt_set(vc->priv_data, "row-mt", "1", 0);
        vc->bit_rate = 0;
    } else if (e.endsWith(QLatin1String("_nvenc"))) {
        av_opt_set(vc->priv_data, "preset", "p5", 0);       // slower = better, p1..p7
        av_opt_set(vc->priv_data, "tune", "hq", 0);
        av_opt_set(vc->priv_data, "rc", "vbr", 0);
        av_opt_set(vc->priv_data, "cq", q.constData(), 0);   // constant quality
        av_opt_set(vc->priv_data, "spatial-aq", "1", 0);
        vc->bit_rate = 0;
        vc->max_b_frames = 2;
    } else if (e.endsWith(QLatin1String("_amf"))) {
        av_opt_set(vc->priv_data, "quality", "quality", 0);
        av_opt_set(vc->priv_data, "rc", "cqp", 0);
        for (const char* k : {"qp_i", "qp_p", "qp_b"}) av_opt_set(vc->priv_data, k, q.constData(), 0);
    } else if (e.endsWith(QLatin1String("_qsv"))) {
        vc->global_quality = cfg.crf;                        // ICQ mode
        av_opt_set(vc->priv_data, "preset", "slower", 0);
    } else if (e != QLatin1String("prores_ks")) {
        vc->bit_rate = cfg.videoBitrate > 0 ? cfg.videoBitrate
                                            : static_cast<std::int64_t>(cfg.width) * cfg.height * cfg.fps * 0.2;
        if (e == QLatin1String("mpeg4")) vc->max_b_frames = 2;
    }
}

bool timelineHasAudio(const TimelineSnapshot& s)
{
    for (const auto& t : s.tracks) {
        if (t.muted) continue;
        for (const auto& c : t.clips) {
            const auto it = s.media.constFind(c.mediaId);
            if (it != s.media.constEnd() && it->info.hasAudio) return true;
        }
    }
    return false;
}

} // namespace

QStringList Exporter::availableVideoEncoders()
{
    QStringList out;
    for (const auto& e : hw::availableEncoders()) out << e.name;
    return out;
}

bool Exporter::run(const TimelineSnapshot& snapshot, const ExportSettings& settingsIn,
                   const Progress& progress, QString* error)
{
    ExportSettings cfg = settingsIn;
    cfg.width = std::max(16, cfg.width & ~1);   // 4:2:0 needs even sizes
    cfg.height = std::max(16, cfg.height & ~1);
    const TimeUs end = cfg.end > 0 ? cfg.end : snapshot.duration();
    if (end <= cfg.start) return fail(error, QObject::tr("The timeline is empty."));
    const AVRational fpsQ = av_d2q(cfg.fps, 1001000);
    const std::int64_t frameCount = std::max<std::int64_t>(1, std::llround(std::ceil(usToSeconds(end - cfg.start) * cfg.fps)));
    const QByteArray pathUtf8 = cfg.path.toUtf8();

    // ---- container
    OutputGuard out;
    int rc = avformat_alloc_output_context2(&out.oc, nullptr, nullptr, pathUtf8.constData());
    if (rc < 0 || !out.oc) return fail(error, QObject::tr("Unsupported output format: %1").arg(ff::errorString(rc)));
    AVFormatContext* oc = out.oc;
    const bool webm = cfg.path.endsWith(QLatin1String(".webm"), Qt::CaseInsensitive);

    // ---- video encoder
    const AVCodec* vcodec = avcodec_find_encoder_by_name(cfg.videoEncoder.toUtf8().constData());
    if (!vcodec) {
        vcodec = avcodec_find_encoder(webm ? AV_CODEC_ID_VP9 : AV_CODEC_ID_MPEG4);
        if (!vcodec) return fail(error, QObject::tr("No usable video encoder found."));
        cfg.videoEncoder = QString::fromLatin1(vcodec->name);
    }
    Stream video;
    video.ctx.reset(avcodec_alloc_context3(vcodec));
    AVCodecContext* vc = video.ctx.get();
    vc->width = cfg.width;
    vc->height = cfg.height;
    vc->time_base = av_inv_q(fpsQ);
    vc->framerate = fpsQ;
    vc->pix_fmt = pixelFormatFor(cfg.videoEncoder);
    vc->gop_size = std::max(1, int(std::lround(cfg.fps * 2)));
    vc->color_range = AVCOL_RANGE_MPEG;
    vc->colorspace = AVCOL_SPC_BT709;
    vc->color_primaries = AVCOL_PRI_BT709;
    vc->color_trc = AVCOL_TRC_BT709;
    vc->thread_count = 0;
    applyQuality(vc, cfg);
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    rc = avcodec_open2(vc, vcodec, nullptr);
    if (rc < 0) return fail(error, QObject::tr("Cannot open video encoder %1: %2").arg(cfg.videoEncoder, ff::errorString(rc)));
    video.st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(video.st->codecpar, vc);
    video.st->time_base = vc->time_base;
    video.st->avg_frame_rate = fpsQ;

    // ---- audio encoder
    Stream audio;
    const bool withAudio = cfg.includeAudio && timelineHasAudio(snapshot);
    int audioFrameSize = 1024;
    if (withAudio) {
        const AVCodec* acodec = webm ? avcodec_find_encoder_by_name("libopus") : avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!acodec && webm) acodec = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
        if (!acodec) return fail(error, QObject::tr("No usable audio encoder found."));
        audio.ctx.reset(avcodec_alloc_context3(acodec));
        AVCodecContext* ac = audio.ctx.get();
        ac->sample_rate = kAudioRate;
        ac->sample_fmt = (acodec->id == AV_CODEC_ID_OPUS) ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&ac->ch_layout, 2);
        ac->bit_rate = cfg.audioBitrate;
        ac->time_base = AVRational{1, kAudioRate};
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        rc = avcodec_open2(ac, acodec, nullptr);
        if (rc < 0) return fail(error, QObject::tr("Cannot open audio encoder: %1").arg(ff::errorString(rc)));
        if (ac->frame_size > 0) audioFrameSize = ac->frame_size;
        audio.st = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(audio.st->codecpar, ac);
        audio.st->time_base = ac->time_base;
    }

    // ---- open file + header
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&oc->pb, pathUtf8.constData(), AVIO_FLAG_WRITE);
        if (rc < 0) return fail(error, QObject::tr("Cannot write %1: %2").arg(cfg.path, ff::errorString(rc)));
        out.ioOpened = true;
    }
    rc = avformat_write_header(oc, nullptr);
    if (rc < 0) return fail(error, QObject::tr("Cannot write header: %1").arg(ff::errorString(rc)));

    // ---- offscreen OpenGL compositor (GPU)
    OffscreenRenderer renderer;
    QString glError;
    if (!renderer.initialize(cfg.width, cfg.height, &glError)) return fail(error, glError);

    ff::SwsPtr sws(sws_getContext(cfg.width, cfg.height, AV_PIX_FMT_RGBA,
                                  cfg.width, cfg.height, vc->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

    ff::FramePtr vframe = ff::makeFrame();
    vframe->format = vc->pix_fmt;
    vframe->width = cfg.width;
    vframe->height = cfg.height;
    av_frame_get_buffer(vframe.get(), 0);

    ff::FramePtr aframe;
    if (withAudio) {
        aframe = ff::makeFrame();
        aframe->format = audio.ctx->sample_fmt;
        aframe->sample_rate = kAudioRate;
        aframe->nb_samples = audioFrameSize;
        av_channel_layout_copy(&aframe->ch_layout, &audio.ctx->ch_layout);
        av_frame_get_buffer(aframe.get(), 0);
    }

    ff::PacketPtr pkt = ff::makePacket();
    FrameSource source;
    AudioMixer mixer(kAudioRate);
    std::vector<float> mix(size_t(audioFrameSize) * 2);
    std::int64_t samplesDone = 0;
    bool ok = true;
    bool cancelled = false;

    for (std::int64_t i = 0; i < frameCount && ok; ++i) {
        const TimeUs t = cfg.start + static_cast<TimeUs>(std::llround(double(i) * kUsPerSecond / cfg.fps));
        const CompositeFramePtr frame = source.build(snapshot, t);

        const std::uint8_t* rgba = renderer.renderRaw(*frame);

        // GL rows are bottom-up: feed them flipped via a negative stride.
        av_frame_make_writable(vframe.get());
        const std::uint8_t* src[4] = {rgba + size_t(cfg.height - 1) * cfg.width * 4, nullptr, nullptr, nullptr};
        const int srcStride[4] = {-cfg.width * 4, 0, 0, 0};
        sws_scale(sws.get(), src, srcStride, 0, cfg.height, vframe->data, vframe->linesize);
        vframe->pts = i;
        ok = encode(oc, video, vframe.get(), pkt.get(), error);

        if (ok && withAudio) {
            const std::int64_t target = std::llround(double(i + 1) / cfg.fps * kAudioRate);
            while (ok && samplesDone < target) {
                const TimeUs at = cfg.start + static_cast<TimeUs>(std::llround(double(samplesDone) * kUsPerSecond / kAudioRate));
                mixer.mix(snapshot, at, audioFrameSize, mix.data());
                av_frame_make_writable(aframe.get());
                if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(aframe->format))) {
                    auto* l = reinterpret_cast<float*>(aframe->data[0]);
                    auto* r = reinterpret_cast<float*>(aframe->data[1]);
                    for (int k = 0; k < audioFrameSize; ++k) {
                        l[k] = mix[size_t(k) * 2];
                        r[k] = mix[size_t(k) * 2 + 1];
                    }
                } else {
                    std::memcpy(aframe->data[0], mix.data(), mix.size() * sizeof(float));
                }
                aframe->pts = samplesDone;
                ok = encode(oc, audio, aframe.get(), pkt.get(), error);
                samplesDone += audioFrameSize;
            }
        }

        if (ok && progress && !progress(double(i + 1) / double(frameCount))) {
            cancelled = true;
            break;
        }
    }

    if (ok && !cancelled) {
        ok = encode(oc, video, nullptr, pkt.get(), error);
        if (ok && withAudio) ok = encode(oc, audio, nullptr, pkt.get(), error);
        if (ok) {
            rc = av_write_trailer(oc);
            if (rc < 0) ok = fail(error, QObject::tr("Cannot finalize file: %1").arg(ff::errorString(rc)));
        }
    }


    if (cancelled) {
        if (out.ioOpened) {
            avio_closep(&oc->pb);
            out.ioOpened = false;
        }
        QFile::remove(cfg.path);
        return fail(error, QObject::tr("Export cancelled."));
    }
    return ok;
}

} // namespace ve
