#pragma once

#include "core/Project.h"
#include "media/VideoDecoder.h"
#include "render/CaptionRenderer.h"
#include "render/Composite.h"
#include "render/TextRenderer.h"

#include <QHash>

#include <memory>

namespace ve {

// Turns a timeline snapshot + time into a CompositeFrame (decoded layers).
// Owns one VideoDecoder per clip. Not thread-safe: one instance per thread.
class FrameSource {
public:
    CompositeFramePtr build(const TimelineSnapshot& s, TimeUs t);
    void reset() { m_decoders.clear(); }

private:
    VideoDecoder* decoderFor(const Clip& clip, const MediaItem& media);

    QHash<QString, std::shared_ptr<VideoDecoder>> m_decoders; // by clip id (null = failed)
    int m_calls = 0;
    CaptionRenderer m_captions;
    TextRenderer m_text;
};

} // namespace ve
