#include "render/CaptionRenderer.h"

#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

namespace {

struct Placed {
    QString text;
    double x = 0;   // left edge within its line
    double w = 0;
    int line = 0;
};

QFont captionFont(const CaptionStyle& s, double px)
{
    QString family = s.font;
    if (!s.fontFile.isEmpty()) {
        const QString loaded = CaptionRenderer::loadFontFile(s.fontFile);
        if (!loaded.isEmpty() && (family.isEmpty() || !QFontDatabase::families().contains(family))) family = loaded;
    }
    QFont f(family);
    f.setPixelSize(std::max(6, int(std::lround(px))));
    f.setBold(s.bold);
    f.setItalic(s.italic);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

} // namespace

QString CaptionRenderer::loadFontFile(const QString& path)
{
    static QMutex mutex;
    static QHash<QString, QString> loaded;   // absolute path -> family
    const QString abs = QFileInfo(path).absoluteFilePath();
    QMutexLocker lock(&mutex);
    if (const auto it = loaded.constFind(abs); it != loaded.constEnd()) return it.value();
    QString family;
    if (QFileInfo::exists(abs)) {
        const int id = QFontDatabase::addApplicationFont(abs);
        const QStringList fams = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
        if (!fams.isEmpty()) family = fams.first();
    }
    loaded.insert(abs, family);
    return family;
}

QStringList CaptionRenderer::suggestedFonts()
{
    static const QStringList wanted = {
        "Arial Black", "Impact", "Segoe UI Black", "Segoe UI", "Segoe UI Semibold", "Bahnschrift", "Montserrat",
        "Poppins", "Bebas Neue", "Anton", "Oswald", "Roboto", "Verdana", "Tahoma", "Trebuchet MS", "Georgia",
        "Comic Sans MS", "Franklin Gothic Medium", "Gill Sans MT", "Rockwell", "Cooper Black", "Arial Rounded MT Bold",
        "Calibri", "Cambria", "Consolas", "Courier New", "Times New Roman", "Arial"};
    const QStringList installed = QFontDatabase::families();
    QStringList out;
    for (const QString& f : wanted)
        if (installed.contains(f, Qt::CaseInsensitive)) out << f;
    return out;
}

bool CaptionRenderer::isArabic(const QString& text)
{
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if ((u >= 0x0600 && u <= 0x06FF) || (u >= 0x0750 && u <= 0x077F) || (u >= 0x08A0 && u <= 0x08FF) ||
            (u >= 0xFB50 && u <= 0xFDFF) || (u >= 0xFE70 && u <= 0xFEFF))
            return true;
    }
    return false;
}

QString CaptionRenderer::arabicFamily(const CaptionStyle& s)
{
    // 1. an explicit Arabic font (installed or from a file), 2. the main font if it has Arabic,
    // 3. a good installed Arabic face.
    const QStringList installed = QFontDatabase::families();
    if (!s.arabicFontFile.isEmpty()) {
        const QString f = loadFontFile(s.arabicFontFile);
        if (!f.isEmpty()) return f;
    }
    if (!s.arabicFont.isEmpty() && installed.contains(s.arabicFont, Qt::CaseInsensitive)) return s.arabicFont;
    QString main = s.font;
    if (!s.fontFile.isEmpty()) main = loadFontFile(s.fontFile);
    if (!main.isEmpty() && QFontDatabase::writingSystems(main).contains(QFontDatabase::Arabic)) return main;
    for (const char* f : {"Segoe UI", "Tahoma", "Arial", "Sakkal Majalla", "Simplified Arabic"})
        if (installed.contains(QLatin1String(f))) return QLatin1String(f);
    return main;
}

QStringList CaptionRenderer::suggestedArabicFonts()
{
    static const QStringList wanted = {
        "Cairo", "Tajawal", "Almarai", "Changa", "El Messiri", "Lalezar", "Reem Kufi", "IBM Plex Sans Arabic",
        "Noto Sans Arabic", "Noto Kufi Arabic", "Dubai", "Segoe UI", "Tahoma", "Arial", "Sakkal Majalla",
        "Simplified Arabic", "Traditional Arabic", "Arabic Typesetting", "Aldhabi", "Andalus", "Microsoft Uighur",
        "Times New Roman"};
    QStringList out;
    for (const QString& f : wanted)
        if (QFontDatabase::families().contains(f, Qt::CaseInsensitive) &&
            QFontDatabase::writingSystems(f).contains(QFontDatabase::Arabic))
            out << f;
    return out;
}

QImage CaptionRenderer::render(const CaptionStyle& s, const QStringList& words, int highlight, int canvasW, int canvasH)
{
    Frame f;
    f.words = words;
    f.highlight = highlight;
    return render(s, f, canvasW, canvasH);
}

QImage CaptionRenderer::render(const CaptionStyle& s, const Frame& fr, int canvasW, int canvasH)
{
    QStringList words = fr.words;
    if (s.uppercase)
        for (QString& w : words) w = w.toUpper();   // no effect on Arabic
    if (words.isEmpty()) return {};
    const int highlight = fr.highlight;
    const QString& mode = s.highlightMode;

    // Arabic captions run right to left; Arabic words get an Arabic-capable font.
    int arabicWords = 0;
    for (const QString& w : words) arabicWords += isArabic(w) ? 1 : 0;
    const bool rtl = arabicWords * 2 >= words.size() && arabicWords > 0;
    const QString secondary = fr.secondary.trimmed();
    const bool secondaryArabic = isArabic(secondary);
    const QString arFamily = (arabicWords > 0 || secondaryArabic) ? arabicFamily(s) : QString();

    const double maxWidth = canvasW * 0.86;
    double px = s.size * canvasH;
    QFont latin, arabic;
    std::vector<Placed> placed;
    std::vector<double> lineWidth;
    auto fontFor = [&](const QString& w) -> const QFont& { return isArabic(w) ? arabic : latin; };
    for (int attempt = 0; attempt < 6; ++attempt) {
        latin = captionFont(s, px);
        arabic = latin;
        if (!arFamily.isEmpty()) {
            arabic.setFamily(arFamily);
            arabic.setItalic(false);                 // Arabic faces rarely have a real italic
        }
        const double space = QFontMetricsF(latin).horizontalAdvance(QLatin1Char(' '));
        placed.clear();
        lineWidth.assign(1, 0.0);
        for (const QString& w : words) {
            const double ww = QFontMetricsF(fontFor(w)).horizontalAdvance(w);
            if (lineWidth.back() > 0 && lineWidth.back() + space + ww > maxWidth) lineWidth.push_back(0.0);
            double& line = lineWidth.back();
            const double x = line > 0 ? line + space : 0.0;
            placed.push_back({w, x, ww, int(lineWidth.size()) - 1});
            line = x + ww;
        }
        if (int(lineWidth.size()) <= s.maxLines) break;
        px *= 0.85;
    }
    if (rtl)   // mirror each line: the first word sits on the right
        for (Placed& p : placed) p.x = lineWidth[size_t(p.line)] - (p.x + p.w);

    const QFontMetricsF lfm(latin), afm(arabic);
    const bool mixed = arabicWords > 0;
    const double ascent = mixed ? std::max(lfm.ascent(), afm.ascent()) : lfm.ascent();
    const double fontH = mixed ? std::max(lfm.height(), afm.height()) : lfm.height();
    const double lineH = fontH * 1.08;
    const double stroke = s.outlineWidth * px;

    // Bilingual: the original line, smaller, wrapped under the caption.
    QFont secFont = secondaryArabic && !arFamily.isEmpty() ? arabic : latin;
    secFont.setPixelSize(std::max(6, int(std::lround(px * 0.62))));
    secFont.setBold(false);
    const QFontMetricsF sfm(secFont);
    QStringList secLines;
    if (!secondary.isEmpty()) {
        QString cur;
        for (const QString& w : secondary.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QString cand = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (!cur.isEmpty() && sfm.horizontalAdvance(cand) > maxWidth) {
                secLines << cur;
                cur = w;
            } else {
                cur = cand;
            }
        }
        if (!cur.isEmpty()) secLines << cur;
    }
    const double secLineH = sfm.height() * 1.05;
    double secW = 0;
    for (const QString& l : secLines) secW = std::max(secW, sfm.horizontalAdvance(l));

    const double grow = mode == QLatin1String("grow") ? 0.18 : 0.0;
    const double pad = std::ceil(stroke + px * (s.box ? 0.35 : 0.12) + (s.shadow ? px * 0.08 : 0.0) + px * grow * 0.6 + 2);
    const double mainW = *std::max_element(lineWidth.begin(), lineWidth.end());
    const double contentW = std::max(mainW, secW);
    const double mainH = lineH * lineWidth.size();
    const double secGap = secLines.isEmpty() ? 0.0 : px * 0.15;
    const int w = int(std::ceil(contentW + 2 * pad)) & ~1;
    const int h = int(std::ceil(mainH + secGap + secLineH * secLines.size() + 2 * pad)) & ~1;
    if (w <= 0 || h <= 0) return {};

    QImage img(w + 2, h + 2, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    auto lineLeft = [&](int line) { return pad + (contentW - lineWidth[size_t(line)]) / 2.0; }; // centred
    auto baseline = [&](int line) { return pad + line * lineH + (lineH - fontH) / 2 + ascent; };

    // Which words show (reveal) and how many letters of each (typewriter).
    auto shownText = [&](int i) -> QString {
        const QString& t = placed[size_t(i)].text;
        if (fr.visibleWords >= 0 && i >= fr.visibleWords) return {};
        if (fr.visibleChars < 0) return t;
        int before = 0;
        for (int k = 0; k < i; ++k) before += int(placed[size_t(k)].text.size()) + 1;
        return t.left(std::max(0, fr.visibleChars - before));
    };

    if (s.box) {   // one rounded box per line (and one per bilingual line)
        QColor bc = s.boxColor;
        bc.setAlphaF(float(s.boxOpacity));
        p.setPen(Qt::NoPen);
        p.setBrush(bc);
        for (int l = 0; l < int(lineWidth.size()); ++l) {
            const QRectF r(lineLeft(l) - px * 0.3, pad + l * lineH - px * 0.08, lineWidth[size_t(l)] + px * 0.6, lineH + px * 0.1);
            p.drawRoundedRect(r, px * 0.22, px * 0.22);
        }
        for (int l = 0; l < int(secLines.size()); ++l) {
            const double lw = sfm.horizontalAdvance(secLines[l]);
            const QRectF r(pad + (contentW - lw) / 2 - px * 0.2, pad + mainH + secGap + l * secLineH - px * 0.04,
                           lw + px * 0.4, secLineH + px * 0.08);
            p.drawRoundedRect(r, px * 0.15, px * 0.15);
        }
    }

    for (int i = 0; i < int(placed.size()); ++i) {
        const Placed& pw = placed[size_t(i)];
        const QString shown = shownText(i);
        if (shown.isEmpty()) continue;
        double x = lineLeft(pw.line) + pw.x;
        const double y = baseline(pw.line);
        const QFont& f = fontFor(pw.text);
        if (shown.size() < pw.text.size() && isArabic(pw.text))   // an RTL word is typed from its right edge
            x += pw.w - QFontMetricsF(f).horizontalAdvance(shown);
        QPainterPath path;
        path.addText(QPointF(x, y), f, shown);   // HarfBuzz shaping: Arabic letters join
        const bool active = i == highlight;
        const bool said = highlight >= 0 && i < highlight;

        if (active && mode == QLatin1String("grow")) {   // the spoken word bigger, around its centre
            const QPointF c(x + pw.w / 2, y - ascent / 2);
            QTransform tr;
            tr.translate(c.x(), c.y());
            tr.scale(1.0 + grow, 1.0 + grow);
            tr.translate(-c.x(), -c.y());
            path = tr.map(path);
        }
        if (active && mode == QLatin1String("box")) {
            const QRectF r(x - px * 0.14, y - ascent - px * 0.04, pw.w + px * 0.28, fontH + px * 0.08);
            p.setPen(Qt::NoPen);
            p.setBrush(s.highlight);
            p.drawRoundedRect(r, px * 0.18, px * 0.18);
        }
        if (s.shadow) {
            QPainterPath sh = path.translated(px * 0.05, px * 0.07);
            if (stroke > 0)
                p.strokePath(sh, QPen(QColor(0, 0, 0, 140), 2 * stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.fillPath(sh, QColor(0, 0, 0, 150));
        }
        if (stroke > 0) {
            if (s.preset == QLatin1String("neon")) {   // soft glow in the outline colour
                for (int g = 3; g >= 1; --g) {
                    QColor c = s.outline;
                    c.setAlpha(55);
                    p.strokePath(path, QPen(c, 2 * stroke * (1 + g * 0.8), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                }
            }
            p.strokePath(path, QPen(s.outline, 2 * stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
        QColor fill = s.color;
        if (active && (mode == QLatin1String("color") || mode == QLatin1String("grow") || mode == QLatin1String("spoken")))
            fill = s.highlight;
        if (said && (mode == QLatin1String("spoken") || mode == QLatin1String("karaoke"))) fill = s.highlight;
        if (active && mode == QLatin1String("karaoke")) {
            // The colour sweeps through the word as it is sung (from where it starts reading).
            p.fillPath(path, s.color);
            const double sw = pw.w * std::clamp(fr.progress, 0.0, 1.0);
            const QRectF clip = isArabic(pw.text) ? QRectF(x + pw.w - sw, y - ascent - px, sw, fontH + 2 * px)
                                                  : QRectF(x, y - ascent - px, sw, fontH + 2 * px);
            p.save();
            p.setClipRect(clip);
            p.fillPath(path, s.highlight);
            p.restore();
        } else {
            p.fillPath(path, fill);
        }
        if (active && mode == QLatin1String("underline")) {
            const QRectF r(x, y + px * 0.12, pw.w, std::max(2.0, px * 0.09));
            p.setPen(Qt::NoPen);
            p.setBrush(s.highlight);
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
    }

    // Bilingual line (once the main words are all there)
    if (!secLines.isEmpty() && (fr.visibleWords < 0 || fr.visibleWords >= int(placed.size())) && fr.visibleChars < 0) {
        QColor sc = s.color;
        sc.setAlphaF(0.92f);
        for (int l = 0; l < int(secLines.size()); ++l) {
            const double lw = sfm.horizontalAdvance(secLines[l]);
            QPainterPath path;
            path.addText(QPointF(pad + (contentW - lw) / 2, pad + mainH + secGap + l * secLineH + sfm.ascent()), secFont, secLines[l]);
            if (stroke > 0)
                p.strokePath(path, QPen(s.outline, 1.4 * stroke * 0.62, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            else if (s.shadow)
                p.fillPath(path.translated(px * 0.04, px * 0.05), QColor(0, 0, 0, 150));
            p.fillPath(path, sc);
        }
    }
    p.end();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

namespace {
double easeOutBounce(double p)
{
    p = std::clamp(p, 0.0, 1.0);
    const double n = 7.5625, d = 2.75;
    if (p < 1 / d) return n * p * p;
    if (p < 2 / d) { p -= 1.5 / d; return n * p * p + 0.75; }
    if (p < 2.5 / d) { p -= 2.25 / d; return n * p * p + 0.9375; }
    p -= 2.625 / d;
    return n * p * p + 0.984375;
}
} // namespace

bool CaptionRenderer::layerAt(const TimelineSnapshot& s, TimeUs t, Layer& out)
{
    const auto& chunks = s.captionChunks;
    if (!s.captions.enabled || chunks.empty()) return false;
    // The caption showing at t (latest start <= t)
    auto it = std::upper_bound(chunks.begin(), chunks.end(), t, [](TimeUs v, const CaptionChunk& c) { return v < c.start; });
    if (it == chunks.begin()) return false;
    --it;
    const CaptionChunk& c = *it;
    if (t >= c.end) return false;
    const CaptionStyle& st = s.captions;

    Frame fr;
    for (const auto& w : c.words) fr.words << w.text;
    fr.secondary = c.secondary;
    int said = 0;
    for (int i = 0; i < int(c.words.size()); ++i)
        if (c.words[size_t(i)].start <= t) {
            fr.highlight = i;
            said = i + 1;
        }
    if (fr.highlight >= 0 && st.highlightMode == QLatin1String("karaoke")) {
        const auto& w = c.words[size_t(fr.highlight)];
        const double p = double(t - w.start) / double(std::max<TimeUs>(1, w.end - w.start));
        fr.progress = std::round(std::clamp(p, 0.0, 1.0) * 12.0) / 12.0;   // 12 steps per word (cache)
    }
    if (st.highlightMode == QLatin1String("none")) fr.highlight = -1;
    const double e = usToSeconds(t - c.start), left = usToSeconds(c.end - t);
    if (st.animation == QLatin1String("reveal")) fr.visibleWords = std::max(1, said);
    if (st.animation == QLatin1String("typewriter")) {
        const int chars = int(fr.words.join(QLatin1Char(' ')).size());
        const double typing = std::min(usToSeconds(c.end - c.start) * 0.6, chars * 0.045);
        if (typing > 0 && e < typing) fr.visibleChars = std::max(1, int(std::ceil(e / typing * chars)));
    }

    const int cw = s.settings.width, ch = s.settings.height;
    const QString key = QString::fromUtf8(QJsonDocument(st.toJson()).toJson(QJsonDocument::Compact)) +
                        QStringLiteral("|%1x%2|%3|%4|%5|%6|").arg(cw).arg(ch).arg(fr.highlight).arg(fr.progress)
                            .arg(fr.visibleWords).arg(fr.visibleChars) +
                        fr.words.join(QLatin1Char('\x1f')) + QLatin1Char('\x1e') + fr.secondary;
    VideoFramePtr frame = m_cache.value(key);
    if (!frame) {
        const QImage img = render(st, fr, cw, ch);
        if (img.isNull()) return false;
        auto f = std::make_shared<VideoFrame>();
        f->width = img.width();
        f->height = img.height();
        f->rgba.resize(size_t(img.width()) * img.height() * 4);
        for (int y = 0; y < img.height(); ++y)
            std::memcpy(f->rgba.data() + size_t(y) * img.width() * 4, img.constScanLine(y), size_t(img.width()) * 4);
        frame = f;
        m_cache.insert(key, frame);
        m_order << key;
        while (m_order.size() > 96) m_cache.remove(m_order.takeFirst());
    }

    // 1 caption pixel = 1 canvas pixel; animation via scale / opacity / position.
    const double fit = std::min(double(cw) / frame->width, double(ch) / frame->height);
    double scale = 1.0 / fit;
    double opacity = 1.0;
    double dy = 0.0;
    const QString& anim = st.animation;
    if (anim == QLatin1String("pop")) {
        if (e < 0.10) scale *= 0.78 + 0.30 * (e / 0.10);        // grow past full size...
        else if (e < 0.18) scale *= 1.08 - 0.08 * ((e - 0.10) / 0.08); // ...and settle
        opacity = std::min(1.0, e / 0.06);
    } else if (anim == QLatin1String("fade")) {
        opacity = std::clamp(std::min(e, left) / 0.15, 0.0, 1.0);
    } else if (anim == QLatin1String("slide")) {               // rise into place
        const double p = std::clamp(e / 0.22, 0.0, 1.0);
        dy = std::pow(1.0 - p, 3.0) * ch * 0.035;
        opacity = std::min(1.0, e / 0.12) * std::clamp(left / 0.08, 0.0, 1.0);
    } else if (anim == QLatin1String("bounce")) {
        scale *= 0.55 + 0.45 * easeOutBounce(e / 0.35);
        opacity = std::min(1.0, e / 0.05);
    } else if (anim == QLatin1String("zoom")) {
        const double p = std::clamp(e / 0.16, 0.0, 1.0);
        scale *= 1.35 - 0.35 * (1.0 - std::pow(1.0 - p, 3.0));
        opacity = std::min(1.0, e / 0.1);
    } else if (anim == QLatin1String("reveal") || anim == QLatin1String("typewriter")) {
        opacity = std::clamp(left / 0.08, 0.0, 1.0);
    }
    const double h = frame->height;
    double cy;  // caption centre, canvas pixels
    if (st.position == QLatin1String("top")) cy = ch * st.offset + h / 2;
    else if (st.position == QLatin1String("middle")) cy = ch / 2.0 + ch * st.offset;
    else cy = ch * (1.0 - st.offset) - h / 2;

    out = Layer{};
    out.frame = frame;
    out.transform.scale = scale;
    out.transform.x = 0;
    out.transform.y = cy - ch / 2.0 + dy;
    out.opacity = opacity;
    return true;
}

} // namespace ve
