#pragma once

#include "core/Project.h"

#include <QImage>
#include <QString>

#include <optional>

namespace ve::MediaProbe {

// Reads stream information for a media file (video, audio or still image).
std::optional<MediaInfo> probe(const QString& path, QString* error = nullptr);

// Small preview image (frame at ~1s or the middle for short clips).
QImage thumbnail(const QString& path, const MediaInfo& info, QSize maxSize = QSize(160, 90));

} // namespace ve::MediaProbe
