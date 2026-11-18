#pragma once

#include "core/Project.h"
#include "render/Composite.h"

#include <QHash>
#include <QImage>
#include <QStringList>

namespace ve {

// Draws text clips (titles, lower thirds, ...) as RGBA layers: wrapped,
// aligned text with outline, soft shadow, gradient fill and background box,
// animated in / out / in a loop. QPainter on a QImage, so it runs on the
// preview worker thread and in export. Keeps a small cache of rendered text.
class TextRenderer {
public:
    // What an animation does to the text at one moment.
    struct AnimState {
        double opacity = 1.0;
        double scale = 1.0;
        double dx = 0.0;            // canvas pixels
        double dy = 0.0;
        double rotation = 0.0;      // degrees
        double blur = 0.0;          // shader blur multiplier
        double hue = 0.0;           // degrees
        int visibleChars = -1;      // -1 = all (typewriter / word-by-word)
    };

    // The layer for a text clip at timeline time t (false if nothing shows).
    bool layerFor(const Clip& clip, const ProjectSettings& settings, TimeUs t, Layer& out);

    // Renders text. scale: pixels per canvas pixel (sharper when the clip is scaled up).
    // visibleChars: characters shown (layout stays that of the whole text), -1 = all.
    static QImage render(const TextData& d, int canvasW, int canvasH, double scale = 1.0, int visibleChars = -1);

    // Animation state at `local` seconds into a clip lasting `duration` seconds.
    static AnimState animate(const TextAnimation& a, const QString& text, double local, double duration,
                             int canvasW, int canvasH);

    // The displayed text (ALL CAPS applied).
    static QString displayText(const TextData& d);

private:
    QHash<QString, VideoFramePtr> m_cache;
    QStringList m_order;
};

} // namespace ve
