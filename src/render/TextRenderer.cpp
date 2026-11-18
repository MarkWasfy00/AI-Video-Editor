#include "render/TextRenderer.h"

#include "render/CaptionRenderer.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

namespace {

constexpr double kPi = 3.14159265358979323846;

QFont textFont(const TextStyle& s, double px)
{
    QString family = s.font;
    if (!s.fontFile.isEmpty()) {
        const QString loaded = CaptionRenderer::loadFontFile(s.fontFile);
        if (!loaded.isEmpty() && (family.isEmpty() || !QFontDatabase::families().contains(family))) family = loaded;
    }
    QFont f(family);
    f.setPixelSize(std::max(4, int(std::lround(px))));
    f.setBold(s.bold);
    f.setItalic(s.italic);
    if (s.letterSpacing != 0.0) f.setLetterSpacing(QFont::AbsoluteSpacing, s.letterSpacing * px);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

// Separable box blur (3 passes ~ gaussian) on a premultiplied ARGB image.
void blurImage(QImage& img, int radius)
{
    if (radius < 1 || img.isNull()) return;
    const int w = img.width(), h = img.height();
    std::vector<std::uint32_t> tmp(size_t(std::max(w, h)));
    auto pass = [&](bool horizontal) {
        const int lines = horizontal ? h : w, len = horizontal ? w : h;
        for (int l = 0; l < lines; ++l) {
            auto px = [&](int i) -> std::uint32_t& {
                return horizontal ? reinterpret_cast<std::uint32_t*>(img.scanLine(l))[i]
                                  : reinterpret_cast<std::uint32_t*>(img.scanLine(i))[l];
            };
            for (int i = 0; i < len; ++i) tmp[size_t(i)] = px(i);
            int acc[4] = {0, 0, 0, 0};
            const int win = 2 * radius + 1;
            auto add = [&](int i, int sign) {
                if (i < 0 || i >= len) return;
                const std::uint32_t v = tmp[size_t(i)];
                for (int c = 0; c < 4; ++c) acc[c] += sign * int((v >> (8 * c)) & 0xff);
            };
            for (int i = -radius; i <= radius; ++i) add(i, 1);
            for (int i = 0; i < len; ++i) {
                std::uint32_t out = 0;
                for (int c = 0; c < 4; ++c) out |= std::uint32_t(std::clamp(acc[c] / win, 0, 255)) << (8 * c);
                px(i) = out;
                add(i - radius, -1);
                add(i + radius + 1, 1);
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(true);
        pass(false);
    }
}

double easeOutCubic(double p) { return 1.0 - std::pow(1.0 - std::clamp(p, 0.0, 1.0), 3.0); }
double easeOutBack(double p)
{
    p = std::clamp(p, 0.0, 1.0);
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(p - 1.0, 3.0) + c1 * std::pow(p - 1.0, 2.0);
}
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

// Index just past the n-th word of `text` (n >= 1).
int charsForWords(const QString& text, int n)
{
    int words = 0;
    bool inWord = false;
    for (int i = 0; i < text.size(); ++i) {
        const bool space = text[i].isSpace();
        if (!space && !inWord) {
            if (words == n) return i;
            ++words;
        }
        inWord = !space;
    }
    return int(text.size());
}

int countWords(const QString& text) { return int(text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size()); }

struct Line {
    QString text;
    int start = 0;          // index of the first character in the full text
    double width = 0;
    bool rtl = false;
};

} // namespace

QString TextRenderer::displayText(const TextData& d)
{
    return d.style.uppercase ? d.text.toUpper() : d.text;
}

QImage TextRenderer::render(const TextData& d, int canvasW, int canvasH, double scale, int visibleChars)
{
    const TextStyle& s = d.style;
    const QString text = displayText(d);
    if (text.trimmed().isEmpty() || canvasW <= 0 || canvasH <= 0) return {};
    const double px = std::max(4.0, s.size * canvasH * scale);
    const QFont font = textFont(s, px);
    const QFontMetricsF fm(font);
    const double wrap = std::max(px * 2, s.maxWidth * canvasW * scale);

    // Lay out: paragraphs at '\n', wrapped at word boundaries (bidi-aware).
    std::vector<Line> lines;
    int offset = 0;
    for (const QString& para : text.split(QLatin1Char('\n'))) {
        if (para.trimmed().isEmpty()) {
            lines.push_back({QString(), offset, 0.0, false});
        } else {
            QTextLayout layout(para, font);
            QTextOption opt;
            opt.setWrapMode(QTextOption::WordWrap);
            opt.setTextDirection(para.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight);
            layout.setTextOption(opt);
            layout.beginLayout();
            for (QTextLine tl = layout.createLine(); tl.isValid(); tl = layout.createLine()) {
                tl.setLineWidth(wrap);
                QString part = para.mid(tl.textStart(), tl.textLength());
                int lead = 0;
                while (lead < part.size() && part[lead].isSpace()) ++lead;
                part = part.mid(lead);
                while (!part.isEmpty() && part.back().isSpace()) part.chop(1);
                lines.push_back({part, offset + tl.textStart() + lead, fm.horizontalAdvance(part), part.isRightToLeft()});
            }
            layout.endLayout();
        }
        offset += int(para.size()) + 1;
    }
    if (lines.empty()) return {};

    const double lineH = fm.height() * s.lineSpacing;
    double contentW = 0;
    for (const Line& l : lines) contentW = std::max(contentW, l.width);
    if (contentW <= 0) return {};
    const double contentH = lineH * double(lines.size()) - (lineH - fm.height());
    const double stroke = s.outlineWidth * px;
    const double pad = s.background ? s.backgroundPadding * px : 0.0;
    const double blurR = s.shadow ? s.shadowBlur * px : 0.0;
    const double shadowReach = s.shadow ? std::max(std::abs(s.shadowX), std::abs(s.shadowY)) * px + 2 * blurR : 0.0;
    const double margin = std::ceil(std::max(stroke + 2, pad + 2) + shadowReach + 2);
    const int w = (int(std::ceil(contentW + 2 * margin)) + 1) & ~1;
    const int h = (int(std::ceil(contentH + 2 * margin)) + 1) & ~1;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return {};

    // Left/right alignment follows the reading direction for RTL lines (Arabic reads from the right).
    auto alignedX = [&](const Line& l) -> double {
        const bool left = s.align == QLatin1String("left"), right = s.align == QLatin1String("right");
        if (left) return l.rtl ? margin + contentW - l.width : margin;     // RTL: the start edge is the right
        if (right) return l.rtl ? margin : margin + contentW - l.width;
        return margin + (contentW - l.width) / 2.0;
    };
    auto lineTop = [&](int i) { return margin + i * lineH; };

    // Text outline path, limited to the visible characters.
    QPainterPath textPath;
    std::vector<QRectF> underlines;
    for (int i = 0; i < int(lines.size()); ++i) {
        const Line& l = lines[size_t(i)];
        if (l.text.isEmpty()) continue;
        int n = int(l.text.size());
        if (visibleChars >= 0) n = std::clamp(visibleChars - l.start, 0, int(l.text.size()));
        if (n <= 0) continue;
        const QString shown = l.text.left(n);
        const double shownW = n == l.text.size() ? l.width : fm.horizontalAdvance(shown);
        double x = alignedX(l);
        if (l.rtl) x += l.width - shownW;                   // RTL types from the right edge
        const double baseline = lineTop(i) + fm.ascent();
        textPath.addText(QPointF(x, baseline), font, shown);
        if (s.underline) underlines.emplace_back(x, baseline + px * 0.1, shownW, std::max(1.0, px * 0.06));
    }
    for (const QRectF& r : underlines) textPath.addRect(r);

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    if (s.background) {
        QColor bc = s.backgroundColor;
        bc.setAlphaF(float(std::clamp(s.backgroundOpacity, 0.0, 1.0) * s.backgroundColor.alphaF()));
        p.setPen(Qt::NoPen);
        p.setBrush(bc);
        const double radius = s.backgroundRadius * px;
        if (s.backgroundMode == QLatin1String("lines")) {
            for (int i = 0; i < int(lines.size()); ++i) {
                const Line& l = lines[size_t(i)];
                if (l.text.isEmpty()) continue;
                const QRectF r(alignedX(l) - pad, lineTop(i) - pad * 0.5, l.width + 2 * pad, fm.height() + pad);
                p.drawRoundedRect(r, radius, radius);
            }
        } else {
            p.drawRoundedRect(QRectF(margin - pad, margin - pad, contentW + 2 * pad, contentH + 2 * pad), radius, radius);
        }
    }

    const QPen strokePen(s.outline, 2 * stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (s.shadow && !textPath.isEmpty()) {
        QImage sh(w, h, QImage::Format_ARGB32_Premultiplied);
        sh.fill(Qt::transparent);
        {
            QPainter sp(&sh);
            sp.setRenderHint(QPainter::Antialiasing);
            sp.translate(s.shadowX * px, s.shadowY * px);
            if (stroke > 0) sp.strokePath(textPath, QPen(s.shadowColor, 2 * stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            sp.fillPath(textPath, s.shadowColor);
        }
        blurImage(sh, int(std::lround(blurR / 2.2)));
        p.drawImage(0, 0, sh);
    }
    if (stroke > 0) p.strokePath(textPath, strokePen);
    if (s.gradient) {
        const QRectF b = textPath.boundingRect();
        QLinearGradient g(b.topLeft(), b.bottomLeft());
        g.setColorAt(0, s.color);
        g.setColorAt(1, s.color2);
        p.fillPath(textPath, g);
    } else {
        p.fillPath(textPath, s.color);
    }
    p.end();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

TextRenderer::AnimState TextRenderer::animate(const TextAnimation& a, const QString& text, double local, double duration,
                                              int canvasW, int canvasH)
{
    AnimState st;
    double inD = a.in == QLatin1String("none") ? 0.0 : a.inDuration;
    double outD = a.out == QLatin1String("none") ? 0.0 : a.outDuration;
    if (inD + outD > duration && inD + outD > 0) {          // short clip: share its length
        const double k = duration / (inD + outD);
        inD *= k;
        outD *= k;
    }
    const int chars = int(text.size());
    const double W = canvasW, H = canvasH;

    if (inD > 0 && local < inD) {
        const double p = std::clamp(local / inD, 0.0, 1.0), e = easeOutCubic(p);
        const QString& m = a.in;
        if (m == QLatin1String("fade")) st.opacity *= e;
        else if (m == QLatin1String("pop")) { st.scale *= 0.5 + 0.5 * easeOutBack(p); st.opacity *= std::min(1.0, p * 3); }
        else if (m == QLatin1String("zoom")) { st.scale *= 0.3 + 0.7 * e; st.opacity *= p; }
        else if (m == QLatin1String("slide-up")) { st.dy += (1 - e) * 0.12 * H; st.opacity *= std::min(1.0, p * 2); }
        else if (m == QLatin1String("slide-down")) { st.dy -= (1 - e) * 0.12 * H; st.opacity *= std::min(1.0, p * 2); }
        else if (m == QLatin1String("slide-left")) { st.dx += (1 - e) * 0.3 * W; st.opacity *= std::min(1.0, p * 2); }
        else if (m == QLatin1String("slide-right")) { st.dx -= (1 - e) * 0.3 * W; st.opacity *= std::min(1.0, p * 2); }
        else if (m == QLatin1String("typewriter")) st.visibleChars = int(std::floor(p * chars));
        else if (m == QLatin1String("words")) {
            const int words = countWords(text);
            const int n = std::clamp(int(std::floor(p * words)) + 1, 1, std::max(1, words));
            st.visibleChars = charsForWords(text, n);
        } else if (m == QLatin1String("blur")) { st.blur = (1 - e) * 12.0; st.opacity *= e; }
        else if (m == QLatin1String("bounce")) { st.dy -= (1 - easeOutBounce(p)) * 0.3 * H; st.opacity *= std::min(1.0, p * 4); }
        else if (m == QLatin1String("drop")) { st.dy -= (1 - easeOutBack(p)) * 0.2 * H; st.opacity *= std::min(1.0, p * 3); }
        else if (m == QLatin1String("spin")) { st.rotation -= (1 - e) * 180; st.scale *= 0.3 + 0.7 * e; st.opacity *= p; }
    }
    const double left = duration - local;
    if (outD > 0 && left < outD) {
        const double p = std::clamp(1.0 - left / outD, 0.0, 1.0);   // 0 -> 1 while leaving
        const double e = p * p * p;                                  // ease in
        const QString& m = a.out;
        if (m == QLatin1String("fade")) st.opacity *= 1 - p;
        else if (m == QLatin1String("pop")) { st.scale *= 1.0 - 0.6 * e; st.opacity *= 1 - e; }
        else if (m == QLatin1String("zoom")) { st.scale *= 1.0 + 0.6 * e; st.opacity *= 1 - p; }
        else if (m == QLatin1String("slide-up")) { st.dy -= e * 0.12 * H; st.opacity *= 1 - p; }
        else if (m == QLatin1String("slide-down")) { st.dy += e * 0.12 * H; st.opacity *= 1 - p; }
        else if (m == QLatin1String("slide-left")) { st.dx -= e * 0.3 * W; st.opacity *= 1 - p; }
        else if (m == QLatin1String("slide-right")) { st.dx += e * 0.3 * W; st.opacity *= 1 - p; }
        else if (m == QLatin1String("typewriter")) {
            const int n = int(std::ceil((1 - p) * chars));
            st.visibleChars = st.visibleChars < 0 ? n : std::min(st.visibleChars, n);
        } else if (m == QLatin1String("blur")) { st.blur = std::max(st.blur, e * 12.0); st.opacity *= 1 - p; }
        else if (m == QLatin1String("spin")) { st.rotation += e * 180; st.scale *= 1.0 - 0.7 * e; st.opacity *= 1 - p; }
    }
    if (a.loop != QLatin1String("none")) {
        const double t = local * a.loopSpeed, ph = 2 * kPi * t;
        const QString& m = a.loop;
        if (m == QLatin1String("pulse")) st.scale *= 1.0 + 0.06 * std::sin(ph);
        else if (m == QLatin1String("float")) st.dy += std::sin(ph) * 0.012 * H;
        else if (m == QLatin1String("wiggle")) st.rotation += 4.0 * std::sin(ph);
        else if (m == QLatin1String("blink")) st.opacity *= 0.35 + 0.65 * (0.5 + 0.5 * std::cos(ph));
        else if (m == QLatin1String("shake")) { st.dx += std::sin(ph * 7.3) * 0.004 * W; st.dy += std::cos(ph * 9.1) * 0.004 * H; }
        else if (m == QLatin1String("rainbow")) st.hue = std::fmod(t * 360.0, 360.0) - 180.0;
    }
    st.opacity = std::clamp(st.opacity, 0.0, 1.0);
    return st;
}

bool TextRenderer::layerFor(const Clip& clip, const ProjectSettings& settings, TimeUs t, Layer& out)
{
    if (!clip.text || !clip.contains(t)) return false;
    const TextData& d = *clip.text;
    const int cw = settings.width, ch = settings.height;
    const QString shown = displayText(d);
    const AnimState st = animate(d.animation, shown, usToSeconds(t - clip.start), usToSeconds(clip.duration), cw, ch);
    const double opacity = clip.transform.opacity * clip.fadeFactor(t) * st.opacity;
    if (opacity <= 0.002 || st.visibleChars == 0) return false;

    // Render at the clip's scale so scaled-up text stays sharp (quantised to keep the cache useful).
    const double renderScale = std::clamp(std::ceil(std::max(1.0, clip.transform.scale) * 4.0) / 4.0, 1.0, 4.0);
    const int visible = st.visibleChars >= int(shown.size()) ? -1 : st.visibleChars;
    const QString key = QString::fromUtf8(QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact)) +
                        QStringLiteral("|%1x%2|%3|%4").arg(cw).arg(ch).arg(renderScale).arg(visible);
    VideoFramePtr frame = m_cache.value(key);
    if (!frame) {
        // A partly typed text keeps the size of the whole text, so it does not jump around.
        const QImage img = render(d, cw, ch, renderScale, visible);
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

    const double fit = std::min(double(cw) / frame->width, double(ch) / frame->height);
    out = Layer{};
    out.frame = frame;
    out.transform = clip.transform;
    out.transform.x += st.dx;
    out.transform.y += st.dy;
    out.transform.rotation += st.rotation;
    out.transform.scale = clip.transform.scale * st.scale / (fit * renderScale);
    out.effects = clip.effects;
    out.effects.blur += st.blur;
    out.effects.hue = std::fmod(out.effects.hue + st.hue + 540.0, 360.0) - 180.0;
    out.opacity = opacity;
    return true;
}

} // namespace ve
