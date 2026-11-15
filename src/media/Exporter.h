#pragma once

#include "core/Project.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace ve {

struct ExportSettings {
    QString path;
    int width = 1920;              // output size; the canvas is scaled to it
    int height = 1080;
    double fps = 30.0;
    QString videoEncoder = QStringLiteral("libx264");   // FFmpeg encoder name
    int crf = 20;                  // x264/x265 quality
    qint64 videoBitrate = 0;       // bits/s for encoders without CRF (0 = auto)
    bool includeAudio = true;
    int audioBitrate = 192'000;
    TimeUs start = 0;
    TimeUs end = 0;                // 0 = end of timeline
};

// Renders the timeline with the OpenGL compositor into an offscreen FBO and
// encodes it with FFmpeg (H.264/MPEG-4 + AAC). Runs on the calling (GUI)
// thread; the progress callback returns false to cancel.
class Exporter {
public:
    using Progress = std::function<bool(double fraction)>;

    bool run(const TimelineSnapshot& snapshot, const ExportSettings& settings,
             const Progress& progress, QString* error);

    // Video encoders from our preferred list that this FFmpeg build provides.
    static QStringList availableVideoEncoders();
};

} // namespace ve
