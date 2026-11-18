#include "media/MediaProbe.h"

#include "media/FFmpegUtil.h"
#include "media/VideoDecoder.h"

#include <QPainter>

namespace ve::MediaProbe {

std::optional<MediaInfo> probe(const QString& path, QString* error)
{
    auto fmt = ff::openInput(path, error);
    if (!fmt) return std::nullopt;

    MediaInfo info;
    info.container = QString::fromLatin1(fmt->iformat->name);

    const int vi = ff::findVideoStream(fmt.get());
    if (vi >= 0) {
        const AVStream* st = fmt->streams[vi];
        info.hasVideo = true;
        info.width = st->codecpar->width;
        info.height = st->codecpar->height;
        info.isImage = ff::isStillImage(fmt.get(), st);
        const AVRational fr = av_guess_frame_rate(fmt.get(), const_cast<AVStream*>(st), nullptr);
        info.fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        if (const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id))
            info.videoCodec = QString::fromLatin1(d->name);
    }

    const int ai = ff::findAudioStream(fmt.get());
    if (ai >= 0) {
        const AVStream* st = fmt->streams[ai];
        info.hasAudio = true;
        info.sampleRate = st->codecpar->sample_rate;
        info.channels = st->codecpar->ch_layout.nb_channels;
        if (const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id))
            info.audioCodec = QString::fromLatin1(d->name);
    }

    if (!info.hasVideo && !info.hasAudio) {
        if (error) *error = QStringLiteral("no audio or video streams");
        return std::nullopt;
    }

    info.duration = info.isImage ? 0 : ff::containerDuration(fmt.get(), vi >= 0 ? fmt->streams[vi] : fmt->streams[ai]);
    return info;
}

QImage thumbnail(const QString& path, const MediaInfo& info, QSize maxSize)
{
    QImage out(maxSize, QImage::Format_RGB32);
    out.fill(QColor(21, 21, 24));

    if (info.hasVideo) {
        VideoDecoder dec;
        if (dec.open(path)) {
            dec.setMaxOutputSize(maxSize.width() * 2, maxSize.height() * 2);
            const TimeUs t = info.isImage ? 0 : std::min<TimeUs>(kUsPerSecond, info.duration / 2);
            if (auto f = dec.frameAt(t)) {
                const QImage img = VideoDecoder::toImage(*f).scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QPainter p(&out);
                p.drawImage((maxSize.width() - img.width()) / 2, (maxSize.height() - img.height()) / 2, img);
                return out;
            }
        }
    }

    // Audio-only placeholder: a simple waveform glyph.
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(90, 200, 140), 2));
    const int mid = maxSize.height() / 2;
    for (int x = 8; x < maxSize.width() - 8; x += 5) {
        const int h = 6 + ((x * 37) % 23);
        p.drawLine(x, mid - h, x, mid + h);
    }
    return out;
}

} // namespace ve::MediaProbe
