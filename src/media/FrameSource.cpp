#include "media/FrameSource.h"

#include <QSet>

namespace ve {

VideoDecoder* FrameSource::decoderFor(const Clip& clip, const MediaItem& media)
{
    auto it = m_decoders.find(clip.id);
    if (it != m_decoders.end()) return it.value().get();
    auto dec = std::make_shared<VideoDecoder>();
    if (!dec->open(media.path)) dec.reset();
    m_decoders.insert(clip.id, dec);
    return dec.get();
}

CompositeFramePtr FrameSource::build(const TimelineSnapshot& s, TimeUs t)
{
    auto out = std::make_shared<CompositeFrame>();
    out->canvasWidth = s.settings.width;
    out->canvasHeight = s.settings.height;
    out->background = s.settings.background;
    out->time = t;

    // Track 0 is the top of the stack: walk bottom -> top.
    for (auto ti = s.tracks.rbegin(); ti != s.tracks.rend(); ++ti) {
        if (ti->hidden || ti->isAudio()) continue; // audio tracks have no picture
        for (const auto& clip : ti->clips) {
            if (clip.start > t) break; // clips are sorted
            if (!clip.contains(t)) continue;
            const auto mit = s.media.constFind(clip.mediaId);
            if (mit == s.media.constEnd() || !mit->info.hasVideo) continue;
            VideoDecoder* dec = decoderFor(clip, *mit);
            if (!dec) continue;
            // No point decoding beyond the canvas resolution.
            dec->setMaxOutputSize(s.settings.width * std::max(1.0, clip.transform.scale),
                                  s.settings.height * std::max(1.0, clip.transform.scale));
            Layer layer;
            layer.frame = dec->frameAt(clip.sourceTimeAt(t)); // speed / curve / reverse / freeze
            if (!layer.frame) continue;
            layer.transform = clip.transform;
            layer.effects = clip.effects;
            layer.opacity = clip.transform.opacity * clip.fadeFactor(t);
            out->layers.push_back(std::move(layer));
        }
    }

    // On-screen captions from the transcript, above everything else.
    if (s.captions.enabled) {
        Layer caption;
        if (m_captions.layerAt(s, t, caption)) out->layers.push_back(std::move(caption));
    }

    if (++m_calls % 120 == 0) {
        QSet<QString> alive;
        for (const auto& track : s.tracks)
            for (const auto& clip : track.clips) alive.insert(clip.id);
        for (auto it = m_decoders.begin(); it != m_decoders.end();) {
            if (!alive.contains(it.key())) it = m_decoders.erase(it);
            else ++it;
        }
    }
    return out;
}

} // namespace ve
