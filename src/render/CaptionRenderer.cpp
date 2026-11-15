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

QImage CaptionRenderer::render(const CaptionStyle& s, const QStringList& wordsIn, int highlight, int canvasW, int canvasH)
{
    QStringList words = wordsIn;
    if (s.uppercase)
        for (QString& w : words) w = w.toUpper();   // no effect on Arabic
    if (words.isEmpty()) return {};

    // Arabic captions run right to left; Arabic words get an Arabic-capable font.
    int arabicWords = 0;
    for (const QString& w : words) arabicWords += isArabic(w) ? 1 : 0;
    const bool rtl = arabicWords * 2 >= words.size() && arabicWords > 0;
    const QString arFamily = arabicWords > 0 ? arabicFamily(s) : QString();

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
    const double pad = std::ceil(stroke + px * (s.box ? 0.35 : 0.12) + (s.shadow ? px * 0.08 : 0.0) + 2);
    const double contentW = *std::max_element(lineWidth.begin(), lineWidth.end());
    const int w = int(std::ceil(contentW + 2 * pad)) & ~1;
    const int h = int(std::ceil(lineH * lineWidth.size() + 2 * pad)) & ~1;
    if (w <= 0 || h <= 0) return {};

    QImage img(w + 2, h + 2, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    auto lineLeft = [&](int line) { return pad + (contentW - lineWidth[size_t(line)]) / 2.0; }; // centred
    auto baseline = [&](int line) { return pad + line * lineH + (lineH - fontH) / 2 + ascent; };

    if (s.box) {   // one rounded box per line
        QColor bc = s.boxColor;
        bc.setAlphaF(float(s.boxOpacity));
        for (int l = 0; l < int(lineWidth.size()); ++l) {
            const QRectF r(lineLeft(l) - px * 0.3, pad + l * lineH - px * 0.08, lineWidth[size_t(l)] + px * 0.6, lineH + px * 0.1);
            p.setPen(Qt::NoPen);
            p.setBrush(bc);
            p.drawRoundedRect(r, px * 0.22, px * 0.22);
        }
    }

    for (int i = 0; i < int(placed.size()); ++i) {
        const Placed& pw = placed[size_t(i)];
        const double x = lineLeft(pw.line) + pw.x;
        const double y = baseline(pw.line);
        const QFont& f = fontFor(pw.text);
        QPainterPath path;
        path.addText(QPointF(x, y), f, pw.text);   // HarfBuzz shaping: Arabic letters join
        const bool active = i == highlight;

        if (active && s.highlightMode == QLatin1String("box")) {
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
        const QColor fill = (active && s.highlightMode == QLatin1String("color")) ? s.highlight : s.color;
        p.fillPath(path, fill);
    }
    p.end();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

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

    int highlight = -1;
    for (int i = 0; i < int(c.words.size()); ++i)
        if (c.words[size_t(i)].start <= t) highlight = i;
    if (s.captions.highlightMode == QLatin1String("none")) highlight = -1;

    const int cw = s.settings.width, ch = s.settings.height;
    QStringList words;
    for (const auto& w : c.words) words << w.text;
    const QString key = QString::fromUtf8(QJsonDocument(s.captions.toJson()).toJson(QJsonDocument::Compact)) +
                        QStringLiteral("|%1x%2|%3|").arg(cw).arg(ch).arg(highlight) + words.join(QLatin1Char('\x1f'));
    VideoFramePtr frame = m_cache.value(key);
    if (!frame) {
        const QImage img = render(s.captions, words, highlight, cw, ch);
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
        while (m_order.size() > 48) m_cache.remove(m_order.takeFirst());
    }

    // 1 caption pixel = 1 canvas pixel; animation via scale / opacity.
    const double fit = std::min(double(cw) / frame->width, double(ch) / frame->height);
    double scale = 1.0 / fit;
    double opacity = 1.0;
    const double e = usToSeconds(t - c.start), left = usToSeconds(c.end - t);
    if (s.captions.animation == QLatin1String("pop")) {
        if (e < 0.10) scale *= 0.78 + 0.30 * (e / 0.10);        // grow past full size...
        else if (e < 0.18) scale *= 1.08 - 0.08 * ((e - 0.10) / 0.08); // ...and settle
        opacity = std::min(1.0, e / 0.06);
    } else if (s.captions.animation == QLatin1String("fade")) {
        opacity = std::clamp(std::min(e, left) / 0.15, 0.0, 1.0);
    }
    const double h = frame->height;
    double cy;  // caption centre, canvas pixels
    if (s.captions.position == QLatin1String("top")) cy = ch * s.captions.offset + h / 2;
    else if (s.captions.position == QLatin1String("middle")) cy = ch / 2.0 + ch * s.captions.offset;
    else cy = ch * (1.0 - s.captions.offset) - h / 2;

    out = Layer{};
    out.frame = frame;
    out.transform.scale = scale;
    out.transform.x = 0;
    out.transform.y = cy - ch / 2.0;
    out.opacity = opacity;
    return true;
}

} // namespace ve
