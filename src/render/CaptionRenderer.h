#pragma once

#include "core/Project.h"
#include "render/Composite.h"

#include <QHash>
#include <QImage>
#include <QStringList>

namespace ve {

// Draws the on-screen caption (CapCut-style: outlined text, the spoken word
// highlighted, optional box, pop / fade animation) as an RGBA layer that sits
// on top of the video. Renders with QPainter on a QImage, so it can run on the
// preview worker thread and in export. Keeps a small cache of rendered captions.
class CaptionRenderer {
public:
    // The caption layer for timeline time t, if a caption is showing.
    bool layerAt(const TimelineSnapshot& s, TimeUs t, Layer& out);

    // What one caption shows at one moment.
    struct Frame {
        QStringList words;
        int highlight = -1;         // the word being said (-1 = none)
        double progress = 1.0;      // how much of it is said (karaoke sweep), 0..1
        int visibleWords = -1;      // reveal: words shown so far (-1 = all)
        int visibleChars = -1;      // typewriter: letters shown so far (-1 = all)
        QString secondary;          // bilingual: the original line, smaller, under the words
    };
    static QImage render(const CaptionStyle& style, const Frame& frame, int canvasW, int canvasH);
    // Renders one caption image (exposed for tests): words with one highlighted (-1 = none).
    static QImage render(const CaptionStyle& style, const QStringList& words, int highlight, int canvasW, int canvasH);

    // Makes a .ttf / .otf usable without installing it. Returns its family name
    // (empty if the file is not a font). Safe to call repeatedly / from any thread.
    static QString loadFontFile(const QString& path);
    // Installed fonts that work well for captions (bold, readable), present on this machine.
    static QStringList suggestedFonts();
    // Installed fonts with Arabic letters (popular caption faces first).
    static QStringList suggestedArabicFonts();
    static bool isArabic(const QString& text);
    // The family used for Arabic words with this style.
    static QString arabicFamily(const CaptionStyle& s);

private:
    QHash<QString, VideoFramePtr> m_cache;
    QStringList m_order;
};

} // namespace ve
