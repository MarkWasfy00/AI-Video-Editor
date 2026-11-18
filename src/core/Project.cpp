#include "core/Project.h"

#include <QRegularExpression>

#include "core/Commands.h"

#include <QFileInfo>
#include <QUndoStack>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ve {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

namespace {

constexpr TimeUs kDefaultImageDuration = 5 * kUsPerSecond;
constexpr TimeUs kMinClipDuration = 10'000; // 10 ms

double num(const QJsonObject& o, const char* key, double def)
{
    const auto v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : def;
}

TimeUs timeVal(const QJsonObject& o, const char* key, TimeUs def = 0)
{
    const auto v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<TimeUs>(v.toDouble()) : def;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
QJsonObject MediaInfo::toJson() const
{
    return {
        {"hasVideo", hasVideo}, {"hasAudio", hasAudio}, {"isImage", isImage},
        {"width", width}, {"height", height}, {"fps", fps},
        {"duration", static_cast<double>(duration)},
        {"sampleRate", sampleRate}, {"channels", channels},
        {"videoCodec", videoCodec}, {"audioCodec", audioCodec}, {"container", container},
    };
}

MediaInfo MediaInfo::fromJson(const QJsonObject& o)
{
    MediaInfo m;
    m.hasVideo = o.value("hasVideo").toBool();
    m.hasAudio = o.value("hasAudio").toBool();
    m.isImage = o.value("isImage").toBool();
    m.width = o.value("width").toInt();
    m.height = o.value("height").toInt();
    m.fps = num(o, "fps", 0);
    m.duration = timeVal(o, "duration");
    m.sampleRate = o.value("sampleRate").toInt();
    m.channels = o.value("channels").toInt();
    m.videoCodec = o.value("videoCodec").toString();
    m.audioCodec = o.value("audioCodec").toString();
    m.container = o.value("container").toString();
    return m;
}

QJsonObject ClipTransform::toJson() const
{
    return {{"x", x}, {"y", y}, {"scale", scale}, {"rotation", rotation}, {"opacity", opacity},
            {"cropLeft", cropLeft}, {"cropTop", cropTop}, {"cropRight", cropRight}, {"cropBottom", cropBottom}};
}

ClipTransform ClipTransform::fromJson(const QJsonObject& o)
{
    ClipTransform t;
    t.x = num(o, "x", 0);
    t.y = num(o, "y", 0);
    t.scale = num(o, "scale", 1);
    t.rotation = num(o, "rotation", 0);
    t.opacity = num(o, "opacity", 1);
    t.cropLeft = std::clamp(num(o, "cropLeft", 0), 0.0, 0.95);
    t.cropTop = std::clamp(num(o, "cropTop", 0), 0.0, 0.95);
    t.cropRight = std::clamp(num(o, "cropRight", 0), 0.0, 0.95);
    t.cropBottom = std::clamp(num(o, "cropBottom", 0), 0.0, 0.95);
    return t;
}

QJsonObject ClipEffects::toJson() const
{
    return {
        {"brightness", brightness}, {"contrast", contrast}, {"saturation", saturation},
        {"hue", hue}, {"blur", blur}, {"vignette", vignette},
        {"grayscale", grayscale}, {"invert", invert},
    };
}

ClipEffects ClipEffects::fromJson(const QJsonObject& o)
{
    ClipEffects e;
    e.brightness = num(o, "brightness", 0);
    e.contrast = num(o, "contrast", 1);
    e.saturation = num(o, "saturation", 1);
    e.hue = num(o, "hue", 0);
    e.blur = num(o, "blur", 0);
    e.vignette = num(o, "vignette", 0);
    e.grayscale = o.value("grayscale").toBool();
    e.invert = o.value("invert").toBool();
    return e;
}

// ---------------------------------------------------------------------------
// Time remapping
// ---------------------------------------------------------------------------
QStringList speedCurvePresetNames()
{
    return {QStringLiteral("none"), QStringLiteral("ease-in"), QStringLiteral("ease-out"),
            QStringLiteral("ease-in-out"), QStringLiteral("bullet"), QStringLiteral("hero")};
}

std::vector<SpeedPoint> speedCurvePreset(const QString& name)
{
    if (name == QLatin1String("ease-in"))      // speeds up
        return {{0.0, 0.3}, {0.5, 0.9}, {1.0, 2.0}};
    if (name == QLatin1String("ease-out"))     // slows down
        return {{0.0, 2.0}, {0.5, 0.9}, {1.0, 0.3}};
    if (name == QLatin1String("ease-in-out"))  // slow, fast, slow
        return {{0.0, 0.4}, {0.5, 2.2}, {1.0, 0.4}};
    if (name == QLatin1String("bullet"))       // fast, slow motion, fast
        return {{0.0, 2.5}, {0.35, 2.5}, {0.5, 0.25}, {0.65, 2.5}, {1.0, 2.5}};
    if (name == QLatin1String("hero"))         // normal, slow-motion moment, normal
        return {{0.0, 1.0}, {0.3, 1.0}, {0.45, 0.2}, {0.65, 0.2}, {0.8, 1.0}, {1.0, 1.0}};
    return {};
}

double Clip::curveAt(double u) const
{
    if (speedCurve.empty()) return 1.0;
    if (u <= speedCurve.front().pos) return speedCurve.front().speed;
    for (size_t i = 1; i < speedCurve.size(); ++i) {
        const auto& a = speedCurve[i - 1];
        const auto& b = speedCurve[i];
        if (u <= b.pos) {
            const double span = b.pos - a.pos;
            return span <= 0 ? b.speed : a.speed + (b.speed - a.speed) * (u - a.pos) / span;
        }
    }
    return speedCurve.back().speed;
}

namespace {
// Integral of the curve multiplier from 0 to u (u in 0..1).
double curveIntegral(const std::vector<SpeedPoint>& c, double u)
{
    if (c.empty()) return u;
    double acc = 0.0, prevPos = 0.0, prevSpeed = c.front().speed;
    for (const auto& p : c) {
        if (p.pos <= prevPos) { prevSpeed = p.speed; continue; }
        const double end = std::min(u, p.pos);
        if (end > prevPos) {
            const double sEnd = prevSpeed + (p.speed - prevSpeed) * (end - prevPos) / (p.pos - prevPos);
            acc += (end - prevPos) * (prevSpeed + sEnd) * 0.5;
        }
        if (u <= p.pos) return acc;
        prevPos = p.pos;
        prevSpeed = p.speed;
    }
    return acc + std::max(0.0, u - prevPos) * prevSpeed;
}

// The part [u0, u1] of a curve, stretched back to 0..1 (for split clips).
std::vector<SpeedPoint> subCurve(const Clip& c, double u0, double u1)
{
    if (c.speedCurve.empty() || u1 <= u0) return {};
    std::vector<SpeedPoint> out{{0.0, c.curveAt(u0)}};
    for (const auto& p : c.speedCurve)
        if (p.pos > u0 && p.pos < u1) out.push_back({(p.pos - u0) / (u1 - u0), p.speed});
    out.push_back({1.0, c.curveAt(u1)});
    return out;
}
} // namespace

double Clip::meanRate() const
{
    if (freeze) return 0.0;
    return speed * curveIntegral(speedCurve, 1.0);
}

TimeUs Clip::sourceOffset(TimeUs local) const
{
    if (freeze) return 0;
    if (speedCurve.empty()) return static_cast<TimeUs>(std::llround(double(local) * speed));
    const double d = std::max<double>(1.0, double(duration));
    if (local <= 0) return static_cast<TimeUs>(std::llround(double(local) * speed * curveAt(0.0)));
    if (local >= duration)
        return static_cast<TimeUs>(std::llround(d * speed * curveIntegral(speedCurve, 1.0) +
                                                double(local - duration) * speed * curveAt(1.0)));
    return static_cast<TimeUs>(std::llround(d * speed * curveIntegral(speedCurve, double(local) / d)));
}

TimeUs Clip::sourceTimeAt(TimeUs timelineT) const
{
    if (freeze) return in;
    const TimeUs local = std::clamp<TimeUs>(timelineT - start, 0, std::max<TimeUs>(0, duration));
    if (!reverse) return in + sourceOffset(local);
    return in + std::max<TimeUs>(0, sourceSpan() - sourceOffset(local) - 1);
}

TimeUs Clip::timelineTimeOf(TimeUs src) const
{
    if (freeze) return -1;
    const TimeUs span = sourceSpan();
    if (src < in || src > in + span) return -1;
    if (plainTiming()) return start + (src - in);
    // sourceTimeAt is monotonic (increasing, or decreasing when reversed): bisect.
    TimeUs lo = 0, hi = duration;
    for (int i = 0; i < 48 && hi - lo > 1; ++i) {
        const TimeUs mid = (lo + hi) / 2;
        const TimeUs s = sourceTimeAt(start + mid);
        if ((s < src) != reverse) lo = mid;
        else hi = mid;
    }
    return start + lo;
}

double Clip::fadeFactor(TimeUs t) const
{
    double f = 1.0;
    const TimeUs local = t - start;
    if (fadeIn > 0 && local < fadeIn)
        f = std::min(f, static_cast<double>(std::max<TimeUs>(local, 0)) / fadeIn);
    const TimeUs remaining = end() - t;
    if (fadeOut > 0 && remaining < fadeOut)
        f = std::min(f, static_cast<double>(std::max<TimeUs>(remaining, 0)) / fadeOut);
    return std::clamp(f, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Text overlays
// ---------------------------------------------------------------------------
namespace {
QString argb(const QColor& c) { return c.name(QColor::HexArgb); }
QColor colorVal(const QJsonObject& o, const char* k, const QColor& def)
{
    const QColor c(o.value(QLatin1String(k)).toString());
    return c.isValid() ? c : def;
}
QString oneOf(const QJsonObject& o, const char* k, const QStringList& allowed, const QString& def)
{
    const QString v = o.value(QLatin1String(k)).toString(def);
    return allowed.contains(v) ? v : def;
}
} // namespace

QJsonObject TextStyle::toJson() const
{
    return {{"font", font}, {"fontFile", fontFile}, {"bold", bold}, {"italic", italic}, {"underline", underline},
            {"uppercase", uppercase}, {"size", size}, {"color", argb(color)}, {"gradient", gradient},
            {"color2", argb(color2)}, {"align", align}, {"letterSpacing", letterSpacing}, {"lineSpacing", lineSpacing},
            {"maxWidth", maxWidth}, {"outline", argb(outline)}, {"outlineWidth", outlineWidth}, {"shadow", shadow},
            {"shadowColor", argb(shadowColor)}, {"shadowX", shadowX}, {"shadowY", shadowY}, {"shadowBlur", shadowBlur},
            {"background", background}, {"backgroundColor", argb(backgroundColor)},
            {"backgroundOpacity", backgroundOpacity}, {"backgroundPadding", backgroundPadding},
            {"backgroundRadius", backgroundRadius}, {"backgroundMode", backgroundMode}};
}

TextStyle TextStyle::fromJson(const QJsonObject& o) { return fromJson(o, TextStyle{}); }

TextStyle TextStyle::fromJson(const QJsonObject& o, const TextStyle& b)
{
    TextStyle s = b;
    s.font = o.value("font").toString(b.font);
    s.fontFile = o.value("fontFile").toString(b.fontFile);
    s.bold = o.value("bold").toBool(b.bold);
    s.italic = o.value("italic").toBool(b.italic);
    s.underline = o.value("underline").toBool(b.underline);
    s.uppercase = o.value("uppercase").toBool(b.uppercase);
    s.size = std::clamp(num(o, "size", b.size), 0.01, 0.6);
    s.color = colorVal(o, "color", b.color);
    s.gradient = o.value("gradient").toBool(b.gradient);
    s.color2 = colorVal(o, "color2", b.color2);
    s.align = oneOf(o, "align", {"left", "center", "right"}, b.align);
    s.letterSpacing = std::clamp(num(o, "letterSpacing", b.letterSpacing), -0.2, 1.5);
    s.lineSpacing = std::clamp(num(o, "lineSpacing", b.lineSpacing), 0.6, 3.0);
    s.maxWidth = std::clamp(num(o, "maxWidth", b.maxWidth), 0.1, 1.0);
    s.outline = colorVal(o, "outline", b.outline);
    s.outlineWidth = std::clamp(num(o, "outlineWidth", b.outlineWidth), 0.0, 0.5);
    s.shadow = o.value("shadow").toBool(b.shadow);
    s.shadowColor = colorVal(o, "shadowColor", b.shadowColor);
    s.shadowX = std::clamp(num(o, "shadowX", b.shadowX), -1.0, 1.0);
    s.shadowY = std::clamp(num(o, "shadowY", b.shadowY), -1.0, 1.0);
    s.shadowBlur = std::clamp(num(o, "shadowBlur", b.shadowBlur), 0.0, 1.0);
    s.background = o.value("background").toBool(b.background);
    s.backgroundColor = colorVal(o, "backgroundColor", b.backgroundColor);
    s.backgroundOpacity = std::clamp(num(o, "backgroundOpacity", b.backgroundOpacity), 0.0, 1.0);
    s.backgroundPadding = std::clamp(num(o, "backgroundPadding", b.backgroundPadding), 0.0, 2.0);
    s.backgroundRadius = std::clamp(num(o, "backgroundRadius", b.backgroundRadius), 0.0, 2.0);
    s.backgroundMode = oneOf(o, "backgroundMode", {"block", "lines"}, b.backgroundMode);
    return s;
}

QStringList textAnimationsIn()
{
    return {"none", "fade", "pop", "zoom", "slide-up", "slide-down", "slide-left", "slide-right",
            "typewriter", "words", "blur", "bounce", "drop", "spin"};
}

QStringList textAnimationsOut()
{
    return {"none", "fade", "pop", "zoom", "slide-up", "slide-down", "slide-left", "slide-right",
            "typewriter", "blur", "spin"};
}

QStringList textAnimationsLoop() { return {"none", "pulse", "float", "wiggle", "blink", "shake", "rainbow"}; }

QJsonObject TextAnimation::toJson() const
{
    return {{"in", in}, {"inDuration", inDuration}, {"out", out}, {"outDuration", outDuration},
            {"loop", loop}, {"loopSpeed", loopSpeed}};
}

TextAnimation TextAnimation::fromJson(const QJsonObject& o) { return fromJson(o, TextAnimation{}); }

TextAnimation TextAnimation::fromJson(const QJsonObject& o, const TextAnimation& b)
{
    TextAnimation a = b;
    a.in = oneOf(o, "in", textAnimationsIn(), b.in);
    a.inDuration = std::clamp(num(o, "inDuration", b.inDuration), 0.0, 10.0);
    a.out = oneOf(o, "out", textAnimationsOut(), b.out);
    a.outDuration = std::clamp(num(o, "outDuration", b.outDuration), 0.0, 10.0);
    a.loop = oneOf(o, "loop", textAnimationsLoop(), b.loop);
    a.loopSpeed = std::clamp(num(o, "loopSpeed", b.loopSpeed), 0.05, 10.0);
    return a;
}

QJsonObject TextData::toJson() const
{
    return {{"text", text}, {"style", style.toJson()}, {"animation", animation.toJson()}, {"templateId", templateId}};
}

TextData TextData::fromJson(const QJsonObject& o)
{
    TextData d;
    d.text = o.value("text").toString();
    d.style = TextStyle::fromJson(o.value("style").toObject());
    d.animation = TextAnimation::fromJson(o.value("animation").toObject());
    d.templateId = o.value("templateId").toString();
    return d;
}

QJsonObject Clip::toJson() const
{
    QJsonObject o = toJsonBase();
    if (text) o["text"] = text->toJson();
    return o;
}

QJsonObject Clip::toJsonBase() const
{
    return {
        {"id", id}, {"mediaId", mediaId}, {"name", name},
        {"start", static_cast<double>(start)},
        {"in", static_cast<double>(in)},
        {"duration", static_cast<double>(duration)},
        {"transform", transform.toJson()},
        {"effects", effects.toJson()},
        {"volume", volume},
        {"fadeIn", static_cast<double>(fadeIn)},
        {"fadeOut", static_cast<double>(fadeOut)},
        {"linkId", linkId},
        {"speed", speed},
        {"speedCurve", [this] {
            QJsonArray a;
            for (const auto& p : speedCurve) a.append(QJsonArray{p.pos, p.speed});
            return a;
        }()},
        {"curvePreset", curvePreset},
        {"reverse", reverse},
        {"freeze", freeze},
    };
}

Clip Clip::fromJson(const QJsonObject& o)
{
    Clip c;
    c.id = o.value("id").toString();
    if (c.id.isEmpty()) c.id = newId();
    c.mediaId = o.value("mediaId").toString();
    c.name = o.value("name").toString();
    c.start = timeVal(o, "start");
    c.in = timeVal(o, "in");
    c.duration = timeVal(o, "duration");
    c.transform = ClipTransform::fromJson(o.value("transform").toObject());
    c.effects = ClipEffects::fromJson(o.value("effects").toObject());
    c.volume = num(o, "volume", 1);
    c.fadeIn = timeVal(o, "fadeIn");
    c.fadeOut = timeVal(o, "fadeOut");
    c.linkId = o.value("linkId").toString();
    c.speed = std::clamp(num(o, "speed", 1), 0.05, 16.0);
    for (const auto& v : o.value("speedCurve").toArray()) {
        const QJsonArray p = v.toArray();
        if (p.size() == 2) c.speedCurve.push_back({std::clamp(p[0].toDouble(), 0.0, 1.0), std::clamp(p[1].toDouble(), 0.05, 16.0)});
    }
    c.curvePreset = o.value("curvePreset").toString();
    c.reverse = o.value("reverse").toBool();
    c.freeze = o.value("freeze").toBool();
    if (o.value("text").isObject()) {
        c.text = TextData::fromJson(o.value("text").toObject());
        c.mediaId.clear();
        c.in = 0;
        c.speed = 1.0;
        c.speedCurve.clear();
        c.reverse = c.freeze = false;
    }
    return c;
}

double dbToGain(double db)
{
    if (db <= kMinGainDb) return 0.0;
    return std::pow(10.0, std::min(db, kMaxGainDb) / 20.0);
}

double gainToDb(double gain)
{
    if (gain <= 0.0) return kMinGainDb;
    return std::clamp(20.0 * std::log10(gain), kMinGainDb, kMaxGainDb);
}

QJsonObject MasterBus::toJson() const
{
    return {{"gainDb", gainDb}, {"muted", muted}};
}

MasterBus MasterBus::fromJson(const QJsonObject& o)
{
    MasterBus m;
    m.gainDb = std::clamp(num(o, "gainDb", 0), kMinGainDb, kMaxGainDb);
    m.muted = o.value("muted").toBool();
    return m;
}

QJsonObject Track::toJson() const
{
    QJsonArray arr;
    for (const auto& c : clips) arr.append(c.toJson());
    return {{"id", id}, {"name", name}, {"kind", isAudio() ? "audio" : "video"}, {"muted", muted}, {"hidden", hidden},
            {"solo", solo}, {"gainDb", gainDb}, {"pan", pan}, {"clips", arr}};
}

Track Track::fromJson(const QJsonObject& o)
{
    Track t;
    t.id = o.value("id").toString();
    if (t.id.isEmpty()) t.id = newId();
    t.name = o.value("name").toString();
    const QString kind = o.value("kind").toString();
    if (kind == QLatin1String("audio")) t.kind = TrackKind::Audio;
    else if (kind == QLatin1String("video")) t.kind = TrackKind::Video;
    else // projects from before track kinds: "A1", "A2", ... were the audio tracks
        t.kind = QRegularExpression(QStringLiteral("^A\\d*$"), QRegularExpression::CaseInsensitiveOption)
                         .match(t.name).hasMatch() ? TrackKind::Audio : TrackKind::Video;
    t.muted = o.value("muted").toBool();
    t.hidden = o.value("hidden").toBool();
    t.solo = o.value("solo").toBool();
    t.gainDb = std::clamp(num(o, "gainDb", 0), kMinGainDb, kMaxGainDb);
    t.pan = std::clamp(num(o, "pan", 0), -1.0, 1.0);
    for (const auto& v : o.value("clips").toArray())
        t.clips.push_back(Clip::fromJson(v.toObject()));
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return t;
}

QJsonObject ProjectSettings::toJson() const
{
    return {
        {"name", name}, {"width", width}, {"height", height}, {"fps", fps},
        {"sampleRate", sampleRate}, {"background", background.name()},
    };
}

ProjectSettings ProjectSettings::fromJson(const QJsonObject& o)
{
    ProjectSettings s;
    s.name = o.value("name").toString(s.name);
    s.width = std::max(16, o.value("width").toInt(s.width));
    s.height = std::max(16, o.value("height").toInt(s.height));
    s.fps = std::clamp(num(o, "fps", s.fps), 1.0, 240.0);
    s.sampleRate = o.value("sampleRate").toInt(s.sampleRate);
    const QColor bg(o.value("background").toString());
    if (bg.isValid()) s.background = bg;
    return s;
}

TimeUs TimelineSnapshot::duration() const
{
    TimeUs d = 0;
    for (const auto& t : tracks)
        if (!t.clips.empty()) d = std::max(d, t.clips.back().end());
    return d;
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------
Project::Project(QObject* parent)
    : QObject(parent)
    , m_undo(new QUndoStack(this))
{
    connect(m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) { setModified(!clean); });
    clear();
}

Project::~Project() = default;

void Project::setModified(bool m)
{
    if (m_modified == m) return;
    m_modified = m;
    emit modifiedChanged(m);
}

void Project::clear()
{
    m_settings = {};
    m_media.clear();
    m_tracks.clear();
    m_master = {};
    m_transcripts.clear();
    m_captions = {};
    m_undo->clear();
    m_filePath.clear();
    // Video tracks are inserted on top, audio tracks appended: V2, V1, A1, A2.
    addTrack(QStringLiteral("V1"), TrackKind::Video);
    addTrack(QStringLiteral("V2"), TrackKind::Video);
    addTrack(QStringLiteral("A1"), TrackKind::Audio);
    addTrack(QStringLiteral("A2"), TrackKind::Audio);
    m_undo->setClean();
    m_modified = false;
    emit settingsChanged();
    emit mediaChanged();
    emit changed();
    emit modifiedChanged(false);
}

void Project::setSettings(const ProjectSettings& s)
{
    m_settings = s;
    setModified(true);
    emit settingsChanged();
    emit changed();
}

const MediaItem* Project::mediaById(const QString& id) const
{
    for (const auto& m : m_media)
        if (m.id == id) return &m;
    return nullptr;
}

QString Project::addMedia(MediaItem item)
{
    if (item.id.isEmpty()) item.id = newId();
    if (item.name.isEmpty()) item.name = QFileInfo(item.path).fileName();
    const QString id = item.id;
    m_media.push_back(std::move(item));
    setModified(true);
    emit mediaChanged();
    return id;
}

void Project::setMediaThumbnail(const QString& id, const QImage& img)
{
    for (auto& m : m_media)
        if (m.id == id) m.thumbnail = img;
    emit mediaChanged();
}

void Project::setMediaWaveform(const QString& id, std::shared_ptr<const std::vector<float>> peaks)
{
    bool found = false;
    for (auto& m : m_media)
        if (m.id == id) {
            m.waveform = peaks;
            found = true;
        }
    if (found) emit mediaChanged();
}

// ---------------------------------------------------------------------------
// Transcripts
// ---------------------------------------------------------------------------
QJsonObject Transcript::toJson() const
{
    QJsonArray segs;
    for (const auto& s : segments) {
        QJsonArray words;
        for (const auto& w : s.words)
            words.append(QJsonArray{double(w.start), double(w.end), w.text});
        QJsonObject o{{"start", double(s.start)}, {"end", double(s.end)}, {"text", s.text}, {"words", words}};
        if (s.edited) o["edited"] = true;
        if (!s.translations.isEmpty()) {
            QJsonObject tr;
            for (auto it = s.translations.begin(); it != s.translations.end(); ++it) tr[it.key()] = it.value();
            o["translations"] = tr;
        }
        segs.append(o);
    }
    return {{"mediaId", mediaId}, {"language", language}, {"model", model}, {"segments", segs}};
}

Transcript Transcript::fromJson(const QJsonObject& o)
{
    Transcript t;
    t.mediaId = o.value("mediaId").toString();
    t.language = o.value("language").toString();
    t.model = o.value("model").toString();
    for (const auto& v : o.value("segments").toArray()) {
        const QJsonObject so = v.toObject();
        TranscriptSegment s;
        s.start = timeVal(so, "start");
        s.end = timeVal(so, "end");
        s.text = so.value("text").toString();
        s.edited = so.value("edited").toBool();
        const QJsonObject tr = so.value("translations").toObject();
        for (auto it = tr.begin(); it != tr.end(); ++it) s.translations.insert(it.key(), it.value().toString());
        for (const auto& wv : so.value("words").toArray()) {
            const QJsonArray w = wv.toArray();
            if (w.size() == 3) s.words.push_back({TimeUs(w[0].toDouble()), TimeUs(w[1].toDouble()), w[2].toString()});
        }
        t.segments.push_back(std::move(s));
    }
    std::sort(t.segments.begin(), t.segments.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return t;
}

const Transcript* Project::transcript(const QString& mediaId) const
{
    const auto it = m_transcripts.constFind(mediaId);
    return it == m_transcripts.constEnd() ? nullptr : &it.value();
}

void Project::setTranscript(const Transcript& t)
{
    Transcript copy = t;
    std::sort(copy.segments.begin(), copy.segments.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    m_transcripts.insert(t.mediaId, copy);
    setModified(true);
    emit transcriptsChanged();
}

void Project::removeTranscript(const QString& mediaId)
{
    if (m_transcripts.remove(mediaId)) {
        setModified(true);
        emit transcriptsChanged();
    }
}

bool Project::updateTranscriptSegment(const QString& mediaId, int index, const QString& text, TimeUs start, TimeUs end)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || index < 0 || index >= int(it->segments.size())) return false;
    TranscriptSegment& s = it->segments[size_t(index)];
    if (text != s.text) {
        s.text = text;
        s.edited = true;
        s.words.clear();                     // word timings no longer match the text
    }
    if (start >= 0) s.start = start;
    if (end >= 0) s.end = std::max(end, s.start + 1);
    std::sort(it->segments.begin(), it->segments.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    setModified(true);
    emit transcriptsChanged();
    return true;
}

bool Project::removeTranscriptSegment(const QString& mediaId, int index)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || index < 0 || index >= int(it->segments.size())) return false;
    it->segments.erase(it->segments.begin() + index);
    setModified(true);
    emit transcriptsChanged();
    return true;
}

int Project::insertTranscriptSegment(const QString& mediaId, TimeUs start, TimeUs end, const QString& text)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end()) {
        Transcript t;
        t.mediaId = mediaId;
        t.model = QStringLiteral("manual");
        it = m_transcripts.insert(mediaId, t);
    }
    TranscriptSegment s;
    s.start = start;
    s.end = std::max(end, start + 1);
    s.text = text;
    s.edited = true;
    it->segments.push_back(s);
    std::sort(it->segments.begin(), it->segments.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    int index = 0;
    for (int i = 0; i < int(it->segments.size()); ++i)
        if (it->segments[size_t(i)].start == start && it->segments[size_t(i)].text == text) index = i;
    setModified(true);
    emit transcriptsChanged();
    return index;
}

bool Project::shiftTranscript(const QString& mediaId, TimeUs delta, int from, int to)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || it->segments.empty()) return false;
    const int n = int(it->segments.size());
    if (to < 0 || to >= n) to = n - 1;
    from = std::max(0, from);
    if (from > to) return false;
    // Never before the start of the media.
    delta = std::max(delta, -it->segments[size_t(from)].start);
    for (int i = from; i <= to; ++i) {
        auto& s = it->segments[size_t(i)];
        s.start += delta;
        s.end += delta;
        for (auto& w : s.words) {
            w.start += delta;
            w.end += delta;
        }
    }
    std::stable_sort(it->segments.begin(), it->segments.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    setModified(true);
    emit transcriptsChanged();
    return true;
}

namespace {
// Splits text into two parts at the word nearest to fraction u (0..1).
std::pair<QString, QString> splitWordsAt(const QString& text, double u)
{
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 2) return {text, QString()};
    const int k = std::clamp(int(std::lround(u * parts.size())), 1, int(parts.size()) - 1);
    return {parts.mid(0, k).join(QLatin1Char(' ')), parts.mid(k).join(QLatin1Char(' '))};
}
} // namespace

int Project::splitTranscriptSegment(const QString& mediaId, int index, TimeUs at)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || index < 0 || index >= int(it->segments.size())) return -1;
    TranscriptSegment& s = it->segments[size_t(index)];
    if (at <= s.start || at >= s.end) return -1;
    TranscriptSegment right;
    right.end = s.end;
    right.edited = true;
    if (!s.words.empty()) {
        // Between the words: everything starting before `at` stays on the left.
        size_t k = 0;
        while (k < s.words.size() && s.words[k].start + (s.words[k].end - s.words[k].start) / 2 < at) ++k;
        if (k == 0 || k == s.words.size()) return -1;
        right.words.assign(s.words.begin() + qsizetype(k), s.words.end());
        s.words.erase(s.words.begin() + qsizetype(k), s.words.end());
        QStringList l, r;
        for (const auto& w : s.words) l << w.text;
        for (const auto& w : right.words) r << w.text;
        s.text = l.join(QLatin1Char(' '));
        right.text = r.join(QLatin1Char(' '));
        right.start = right.words.front().start;
        s.end = std::max(s.words.back().end, s.start + 1);
    } else {
        const double u = double(at - s.start) / double(std::max<TimeUs>(1, s.end - s.start));
        auto [l, r] = splitWordsAt(s.text, u);
        if (r.isEmpty()) return -1;
        s.text = l;
        right.text = r;
        right.start = at;
        s.end = at;
    }
    const double u = double(right.start - s.start) / double(std::max<TimeUs>(1, right.end - s.start));
    for (auto t = s.translations.begin(); t != s.translations.end(); ++t) {
        auto [l, r] = splitWordsAt(t.value(), u);
        t.value() = l;
        right.translations.insert(t.key(), r);
    }
    s.edited = true;
    it->segments.insert(it->segments.begin() + index + 1, std::move(right));
    setModified(true);
    emit transcriptsChanged();
    return index + 1;
}

bool Project::mergeTranscriptSegments(const QString& mediaId, int index)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || index < 0 || index + 1 >= int(it->segments.size())) return false;
    TranscriptSegment& a = it->segments[size_t(index)];
    const TranscriptSegment b = it->segments[size_t(index) + 1];
    const bool keepWords = !a.words.empty() && !b.words.empty();
    a.text = (a.text + QLatin1Char(' ') + b.text).trimmed();
    a.end = std::max(a.end, b.end);
    if (keepWords) a.words.insert(a.words.end(), b.words.begin(), b.words.end());
    else a.words.clear();
    for (auto t = b.translations.begin(); t != b.translations.end(); ++t)
        a.translations[t.key()] = (a.translations.value(t.key()) + QLatin1Char(' ') + t.value()).trimmed();
    a.edited = true;
    it->segments.erase(it->segments.begin() + index + 1);
    setModified(true);
    emit transcriptsChanged();
    return true;
}

bool Project::setTranscriptTranslation(const QString& mediaId, const QString& language, const QStringList& texts)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || language.isEmpty() || texts.size() != qsizetype(it->segments.size())) return false;
    for (size_t i = 0; i < it->segments.size(); ++i) {
        const QString t = texts[qsizetype(i)].trimmed();
        if (t.isEmpty()) it->segments[i].translations.remove(language);
        else it->segments[i].translations.insert(language, t);
    }
    setModified(true);
    emit transcriptsChanged();
    return true;
}

bool Project::setSegmentTranslation(const QString& mediaId, int index, const QString& language, const QString& text)
{
    auto it = m_transcripts.find(mediaId);
    if (it == m_transcripts.end() || index < 0 || index >= int(it->segments.size()) || language.isEmpty()) return false;
    auto& tr = it->segments[size_t(index)].translations;
    if (text.trimmed().isEmpty()) tr.remove(language);
    else tr.insert(language, text.trimmed());
    setModified(true);
    emit transcriptsChanged();
    return true;
}

QStringList Project::translationLanguages() const
{
    QStringList out;
    for (const auto& t : m_transcripts)
        for (const auto& s : t.segments)
            for (auto it = s.translations.begin(); it != s.translations.end(); ++it)
                if (!out.contains(it.key())) out << it.key();
    out.sort();
    return out;
}

QList<QPair<QString, QString>> subtitleLanguages()
{
    return {{"en", "English"},     {"ar-EG", "Arabic (Egyptian)"}, {"ar", "Arabic (Standard)"},
            {"es", "Spanish"},     {"fr", "French"},               {"de", "German"},
            {"it", "Italian"},     {"pt", "Portuguese"},           {"ru", "Russian"},
            {"tr", "Turkish"},     {"hi", "Hindi"},                {"ur", "Urdu"},
            {"fa", "Persian"},     {"id", "Indonesian"},           {"nl", "Dutch"},
            {"pl", "Polish"},      {"uk", "Ukrainian"},            {"ja", "Japanese"},
            {"ko", "Korean"},      {"zh", "Chinese"},              {"he", "Hebrew"},
            {"sv", "Swedish"},     {"el", "Greek"},                {"vi", "Vietnamese"},
            {"th", "Thai"},        {"bn", "Bengali"},              {"sw", "Swahili"}};
}

QString languageName(const QString& code)
{
    for (const auto& [c, n] : subtitleLanguages())
        if (c == code) return n;
    return code;
}

std::vector<TimelineLine> Project::transcriptLines(const QString& language) const
{
    std::vector<TimelineLine> out;
    for (const auto& track : m_tracks) {
        if (!track.isAudio()) continue;
        for (const auto& c : track.clips) {
            const Transcript* t = transcript(c.mediaId);
            if (!t || c.freeze) continue;
            const TimeUs s0 = c.in, s1 = c.in + c.sourceSpan();
            for (int i = 0; i < int(t->segments.size()); ++i) {
                const auto& seg = t->segments[size_t(i)];
                if (seg.end <= s0 || seg.start >= s1) continue;      // not in the part the clip plays
                TimeUs a = c.timelineTimeOf(std::clamp(seg.start, s0, s1));
                TimeUs b = c.timelineTimeOf(std::clamp(seg.end, s0, s1));
                if (a < 0 || b < 0) continue;
                if (b < a) std::swap(a, b);                           // reversed clip
                TimelineLine line{c.id, c.mediaId, i, a, b, seg.text, {}, seg.text};
                if (!language.isEmpty() && seg.translations.contains(language)) {
                    line.text = seg.translations.value(language);    // no word timings for a translation
                    out.push_back(std::move(line));
                    continue;
                }
                for (const auto& w : seg.words) {
                    if (w.end <= s0 || w.start >= s1) continue;
                    TimeUs wa = c.timelineTimeOf(std::clamp(w.start, s0, s1));
                    TimeUs wb = c.timelineTimeOf(std::clamp(w.end, s0, s1));
                    if (wb < wa) std::swap(wa, wb);
                    line.words.push_back({wa, wb, w.text});
                }
                out.push_back(std::move(line));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const TimelineLine& a, const TimelineLine& b) { return a.start < b.start; });
    return out;
}

// ---------------------------------------------------------------------------
// Captions
// ---------------------------------------------------------------------------
QJsonObject CaptionStyle::toJson() const
{
    return {{"enabled", enabled}, {"preset", preset}, {"font", font}, {"fontFile", fontFile},
            {"arabicFont", arabicFont}, {"arabicFontFile", arabicFontFile}, {"bold", bold}, {"italic", italic},
            {"uppercase", uppercase}, {"size", size}, {"color", color.name(QColor::HexArgb)},
            {"highlight", highlight.name(QColor::HexArgb)}, {"highlightMode", highlightMode},
            {"outline", outline.name(QColor::HexArgb)}, {"outlineWidth", outlineWidth}, {"shadow", shadow},
            {"box", box}, {"boxColor", boxColor.name(QColor::HexArgb)}, {"boxOpacity", boxOpacity},
            {"position", position}, {"offset", offset}, {"wordsPerCaption", wordsPerCaption},
            {"maxLines", maxLines}, {"animation", animation}, {"language", language}, {"bilingual", bilingual},
            {"delay", delay}};
}

CaptionStyle CaptionStyle::fromJson(const QJsonObject& o)
{
    CaptionStyle s;
    const auto color = [&](const char* k, const QColor& def) {
        const QColor c(o.value(QLatin1String(k)).toString());
        return c.isValid() ? c : def;
    };
    s.enabled = o.value("enabled").toBool(s.enabled);
    s.preset = o.value("preset").toString(s.preset);
    s.font = o.value("font").toString(s.font);
    s.fontFile = o.value("fontFile").toString();
    s.arabicFont = o.value("arabicFont").toString();
    s.arabicFontFile = o.value("arabicFontFile").toString();
    s.bold = o.value("bold").toBool(s.bold);
    s.italic = o.value("italic").toBool(s.italic);
    s.uppercase = o.value("uppercase").toBool(s.uppercase);
    s.size = std::clamp(num(o, "size", s.size), 0.015, 0.3);
    s.color = color("color", s.color);
    s.highlight = color("highlight", s.highlight);
    s.highlightMode = oneOf(o, "highlightMode", captionHighlightModes(), s.highlightMode);
    s.outline = color("outline", s.outline);
    s.outlineWidth = std::clamp(num(o, "outlineWidth", s.outlineWidth), 0.0, 0.5);
    s.shadow = o.value("shadow").toBool(s.shadow);
    s.box = o.value("box").toBool(s.box);
    s.boxColor = color("boxColor", s.boxColor);
    s.boxOpacity = std::clamp(num(o, "boxOpacity", s.boxOpacity), 0.0, 1.0);
    s.position = o.value("position").toString(s.position);
    s.offset = std::clamp(num(o, "offset", s.offset), 0.0, 0.45);
    s.wordsPerCaption = std::clamp(o.value("wordsPerCaption").toInt(s.wordsPerCaption), 0, 20);
    s.maxLines = std::clamp(o.value("maxLines").toInt(s.maxLines), 1, 4);
    s.animation = oneOf(o, "animation", captionAnimations(), s.animation);
    s.language = o.value("language").toString();
    s.bilingual = o.value("bilingual").toBool();
    s.delay = std::clamp(num(o, "delay", 0.0), -30.0, 30.0);
    return s;
}

QStringList captionHighlightModes() { return {"color", "box", "karaoke", "spoken", "grow", "underline", "none"}; }
QStringList captionAnimations() { return {"pop", "fade", "slide", "bounce", "zoom", "reveal", "typewriter", "none"}; }

QStringList captionPresetNames()
{
    return {QStringLiteral("classic"), QStringLiteral("bold"), QStringLiteral("box"), QStringLiteral("neon"),
            QStringLiteral("minimal"), QStringLiteral("karaoke"), QStringLiteral("arabic"), QStringLiteral("sweep"),
            QStringLiteral("wordpop"), QStringLiteral("typewriter"), QStringLiteral("cinema")};
}

CaptionStyle captionPreset(const QString& name, const CaptionStyle& base)
{
    CaptionStyle s;                          // classic: white, black outline, yellow word
    s.enabled = base.enabled;
    s.fontFile.clear();
    s.arabicFont = base.arabicFont;           // the chosen Arabic face survives preset changes
    s.arabicFontFile = base.arabicFontFile;
    s.language = base.language;               // so are the language and timing
    s.bilingual = base.bilingual;
    s.delay = base.delay;
    s.preset = name;
    if (name == QLatin1String("bold")) {     // the viral look: big caps, thick outline
        s.font = QStringLiteral("Arial Black");
        s.uppercase = true;
        s.size = 0.075;
        s.highlight = QColor(255, 225, 0);
        s.outlineWidth = 0.22;
        s.wordsPerCaption = 3;
        s.position = QStringLiteral("middle");
        s.offset = 0.0;
    } else if (name == QLatin1String("box")) {
        s.font = QStringLiteral("Segoe UI Semibold");
        s.bold = false;
        s.box = true;
        s.boxOpacity = 0.6;
        s.outlineWidth = 0.0;
        s.shadow = false;
        s.highlight = QColor(80, 200, 255);
        s.wordsPerCaption = 6;
        s.animation = QStringLiteral("fade");
    } else if (name == QLatin1String("neon")) {
        s.font = QStringLiteral("Segoe UI Black");
        s.color = QColor(235, 255, 255);
        s.highlight = QColor(255, 60, 220);
        s.outline = QColor(0, 220, 255);
        s.outlineWidth = 0.10;
        s.uppercase = true;
        s.wordsPerCaption = 3;
    } else if (name == QLatin1String("minimal")) {
        s.font = QStringLiteral("Segoe UI");
        s.bold = false;
        s.size = 0.05;
        s.outlineWidth = 0.0;
        s.shadow = true;
        s.highlightMode = QStringLiteral("none");
        s.wordsPerCaption = 0;
        s.animation = QStringLiteral("fade");
    } else if (name == QLatin1String("arabic")) {   // tuned for Arabic: larger, bold Arabic face
        s.font = QStringLiteral("Segoe UI");
        s.arabicFont = QStringLiteral("Segoe UI");
        s.size = 0.075;
        s.outlineWidth = 0.12;
        s.highlight = QColor(255, 200, 40);
        s.wordsPerCaption = 3;
    } else if (name == QLatin1String("karaoke")) {
        s.font = QStringLiteral("Segoe UI Black");
        s.highlightMode = QStringLiteral("box");
        s.highlight = QColor(130, 70, 255);
        s.outlineWidth = 0.08;
        s.wordsPerCaption = 5;
    } else if (name == QLatin1String("sweep")) {     // karaoke: colour sweeps through each word as it is sung
        s.font = QStringLiteral("Segoe UI Black");
        s.highlightMode = QStringLiteral("karaoke");
        s.highlight = QColor(60, 200, 255);
        s.outlineWidth = 0.12;
        s.wordsPerCaption = 0;
        s.maxLines = 2;
        s.animation = QStringLiteral("fade");
    } else if (name == QLatin1String("wordpop")) {   // words pop in one by one, the spoken one bigger
        s.font = QStringLiteral("Arial Black");
        s.uppercase = true;
        s.size = 0.07;
        s.highlightMode = QStringLiteral("grow");
        s.highlight = QColor(120, 255, 90);
        s.outlineWidth = 0.2;
        s.wordsPerCaption = 4;
        s.position = QStringLiteral("middle");
        s.offset = 0.08;
        s.animation = QStringLiteral("reveal");
    } else if (name == QLatin1String("typewriter")) {
        s.font = QStringLiteral("Consolas");
        s.bold = false;
        s.box = true;
        s.boxOpacity = 0.75;
        s.outlineWidth = 0.0;
        s.shadow = false;
        s.highlightMode = QStringLiteral("none");
        s.wordsPerCaption = 6;
        s.animation = QStringLiteral("typewriter");
    } else if (name == QLatin1String("cinema")) {    // film subtitles: whole line, plain, soft shadow
        s.font = QStringLiteral("Segoe UI Semibold");
        s.bold = false;
        s.size = 0.048;
        s.outlineWidth = 0.06;
        s.highlightMode = QStringLiteral("none");
        s.wordsPerCaption = 0;
        s.offset = 0.07;
        s.animation = QStringLiteral("none");
    } else {
        s.preset = QStringLiteral("classic");
    }
    return s;
}

void Project::setCaptionStyle(const CaptionStyle& s)
{
    if (s == m_captions) return;
    m_captions = s;
    setModified(true);
    emit captionsChanged();
}

std::vector<CaptionChunk> Project::captionChunks() const
{
    std::vector<CaptionChunk> out;
    const auto lines = transcriptLines(m_captions.language);
    // Bilingual captions show whole lines (the original under its translation).
    const bool bilingual = m_captions.bilingual && !m_captions.language.isEmpty();
    const int per = bilingual ? 0 : m_captions.wordsPerCaption;
    for (size_t li = 0; li < lines.size(); ++li) {
        const TimelineLine& l = lines[li];
        // Words with times: from Whisper, or spread evenly over an edited line.
        std::vector<TranscriptWord> words = l.words;
        if (words.empty()) {
            const QStringList parts = l.text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            const TimeUs d = std::max<TimeUs>(1, l.end - l.start);
            for (int i = 0; i < parts.size(); ++i)
                words.push_back({l.start + d * i / parts.size(), l.start + d * (i + 1) / parts.size(), parts[i]});
        }
        if (words.empty()) continue;
        const TimeUs lineEnd = li + 1 < lines.size() ? std::min(l.end + 400'000, lines[li + 1].start) : l.end + 400'000;
        CaptionChunk cur;
        auto flush = [&] {
            if (cur.words.empty()) return;
            cur.start = cur.words.front().start;
            cur.end = cur.words.back().end;
            if (bilingual && l.original != l.text) cur.secondary = l.original;
            out.push_back(cur);
            cur = {};
        };
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& w = words[i];
            if (per > 0 && !cur.words.empty() &&
                (int(cur.words.size()) >= per || w.start - cur.words.back().end > 600'000))
                flush();
            cur.words.push_back(w);
            const QChar last = w.text.isEmpty() ? QChar() : w.text.back();
            static const QString kBreaks = QStringLiteral(".?!,\u060C\u061F\u061B\u06D4");  // + Arabic ، ؟ ؛ ۔
            if (per > 0 && kBreaks.contains(last) && cur.words.size() >= 2)
                flush();
        }
        flush();
        // Stay on screen until the next caption (short pauses), at most a little past the line.
        const size_t first = out.size() - std::min(out.size(), size_t(1 + (per > 0 ? words.size() : 0)));
        for (size_t i = first; i < out.size(); ++i) {
            const TimeUs next = i + 1 < out.size() ? out[i + 1].start : lineEnd;
            if (next - out[i].end < 700'000) out[i].end = std::max(out[i].end, next);
            else out[i].end = out[i].end + 200'000;
        }
    }
    std::sort(out.begin(), out.end(), [](const CaptionChunk& a, const CaptionChunk& b) { return a.start < b.start; });
    if (const TimeUs d = secondsToUs(m_captions.delay); d != 0) {   // sync fix for every caption
        for (auto& c : out) {
            c.start += d;
            c.end += d;
            for (auto& w : c.words) {
                w.start += d;
                w.end += d;
            }
        }
        out.erase(std::remove_if(out.begin(), out.end(), [](const CaptionChunk& c) { return c.end <= 0; }), out.end());
    }
    return out;
}

bool Project::isMediaUsed(const QString& id) const
{
    for (const auto& t : m_tracks)
        for (const auto& c : t.clips)
            if (c.mediaId == id) return true;
    return false;
}

bool Project::removeMedia(const QString& id)
{
    if (isMediaUsed(id)) return false;
    const auto it = std::remove_if(m_media.begin(), m_media.end(), [&](const MediaItem& m) { return m.id == id; });
    if (it == m_media.end()) return false;
    m_media.erase(it, m_media.end());
    if (m_transcripts.remove(id)) emit transcriptsChanged();
    setModified(true);
    emit mediaChanged();
    return true;
}

int Project::addTrack(const QString& name, TrackKind kind)
{
    Track t;
    t.id = newId();
    t.kind = kind;
    t.name = name;
    if (t.name.isEmpty()) {
        const QChar prefix = kind == TrackKind::Audio ? QLatin1Char('A') : QLatin1Char('V');
        int n = 1 + int(std::count_if(m_tracks.begin(), m_tracks.end(), [&](const Track& x) { return x.kind == kind; }));
        const auto used = [&](const QString& s) {
            return std::any_of(m_tracks.begin(), m_tracks.end(), [&](const Track& x) { return x.name == s; });
        };
        while (used(QString(prefix) + QString::number(n))) ++n;
        t.name = QString(prefix) + QString::number(n);
    }
    int index = 0;
    if (kind == TrackKind::Video) {
        m_tracks.insert(m_tracks.begin(), std::move(t)); // new video tracks go on top
    } else {
        m_tracks.push_back(std::move(t));                 // new audio tracks at the bottom
        index = trackCount() - 1;
    }
    notifyChanged();
    return index;
}

int Project::firstAudioTrack() const
{
    for (int i = 0; i < trackCount(); ++i)
        if (m_tracks[i].isAudio()) return i;
    return trackCount();
}

int Project::trackIndexById(const QString& id) const
{
    for (int i = 0; i < trackCount(); ++i)
        if (m_tracks[i].id == id) return i;
    return -1;
}

bool Project::isTrackFree(int track, TimeUs start, TimeUs end, const QStringList& ignore) const
{
    if (track < 0 || track >= trackCount()) return false;
    for (const auto& c : m_tracks[track].clips)
        if (!ignore.contains(c.id) && c.start < end && c.end() > start) return false;
    return true;
}

int Project::findFreeTrack(TrackKind kind, TimeUs start, TimeUs end, bool create)
{
    const auto isFree = [&](int i) {
        for (const auto& c : m_tracks[i].clips)
            if (c.start < end && c.end() > start) return false;
        return true;
    };
    const int firstAudio = firstAudioTrack();
    if (kind == TrackKind::Audio) {
        for (int i = firstAudio; i < trackCount(); ++i)
            if (isFree(i)) return i;
    } else {
        for (int i = firstAudio - 1; i >= 0; --i)
            if (isFree(i)) return i;
    }
    return create ? addTrack({}, kind) : -1;
}
void Project::removeTrack(int index)
{
    if (index < 0 || index >= trackCount() || trackCount() <= 1) return;
    m_tracks.erase(m_tracks.begin() + index);
    notifyChanged();
}

void Project::setTrackMuted(int index, bool muted)
{
    if (index < 0 || index >= trackCount()) return;
    m_tracks[index].muted = muted;
    notifyChanged();
}

void Project::setTrackHidden(int index, bool hidden)
{
    if (index < 0 || index >= trackCount()) return;
    m_tracks[index].hidden = hidden;
    notifyChanged();
}

void Project::setTrackSolo(int index, bool solo)
{
    if (index < 0 || index >= trackCount() || m_tracks[index].solo == solo) return;
    m_tracks[index].solo = solo;
    notifyChanged();
}

void Project::setTrackGain(int index, double gainDb)
{
    if (index < 0 || index >= trackCount()) return;
    gainDb = std::clamp(gainDb, kMinGainDb, kMaxGainDb);
    if (m_tracks[index].gainDb == gainDb) return;
    m_tracks[index].gainDb = gainDb;
    notifyChanged();
}

void Project::setTrackPan(int index, double pan)
{
    if (index < 0 || index >= trackCount()) return;
    pan = std::clamp(pan, -1.0, 1.0);
    if (m_tracks[index].pan == pan) return;
    m_tracks[index].pan = pan;
    notifyChanged();
}

void Project::setMaster(const MasterBus& m)
{
    MasterBus v = m;
    v.gainDb = std::clamp(v.gainDb, kMinGainDb, kMaxGainDb);
    if (v == m_master) return;
    m_master = v;
    notifyChanged();
}

void Project::renameTrack(int index, const QString& name)
{
    if (index < 0 || index >= trackCount() || name.isEmpty() || m_tracks[index].name == name) return;
    m_tracks[index].name = name;
    notifyChanged();
}

const Clip* Project::findClip(const QString& id, int* trackIndex) const
{
    return const_cast<Project*>(this)->findClipMutable(id, trackIndex);
}

Clip* Project::findClipMutable(const QString& id, int* trackIndex)
{
    for (int ti = 0; ti < trackCount(); ++ti) {
        for (auto& c : m_tracks[ti].clips) {
            if (c.id == id) {
                if (trackIndex) *trackIndex = ti;
                return &c;
            }
        }
    }
    return nullptr;
}

void Project::sortTrack(Track& t)
{
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

// "Overwrite" edit: the clip `keepId` wins; anything it covers is trimmed,
// split or removed.
void Project::resolveOverlaps(Track& t, const QString& keepId)
{
    const auto keepIt = std::find_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.id == keepId; });
    if (keepIt == t.clips.end()) return;
    const TimeUs ks = keepIt->start;
    const TimeUs ke = keepIt->end();

    std::vector<Clip> result;
    result.reserve(t.clips.size() + 1);
    for (const auto& c : t.clips) {
        if (c.id == keepId || c.end() <= ks || c.start >= ke) {
            result.push_back(c);
            continue;
        }
        // Left remainder
        if (c.start < ks) {
            Clip left = c;
            left.duration = ks - c.start;
            if (left.duration >= kMinClipDuration) result.push_back(left);
        }
        // Right remainder
        if (c.end() > ke) {
            Clip right = c;
            const TimeUs cut = ke - c.start;
            right.start = ke;
            right.in = c.in + cut;
            right.duration = c.end() - ke;
            if (c.start < ks) right.id = newId(); // split: left keeps the original id
            if (right.duration >= kMinClipDuration) result.push_back(right);
        }
    }
    t.clips = std::move(result);
    sortTrack(t);
}

TimeUs Project::maxClipDuration(const Clip& clip) const
{
    const MediaItem* m = mediaById(clip.mediaId);
    if (!m || m->info.isImage || m->info.duration <= 0 || clip.freeze)
        return std::numeric_limits<TimeUs>::max() / 4;
    const double rate = std::max(1e-3, clip.meanRate());
    return std::max<TimeUs>(TimeUs(double(m->info.duration - clip.in) / rate), kMinClipDuration);
}

QString Project::addClip(int trackIndex, Clip clip)
{
    if (trackIndex < 0 || trackIndex >= trackCount()) return {};
    if (clip.id.isEmpty()) clip.id = newId();
    clip.start = std::max<TimeUs>(0, clip.start);
    clip.duration = std::max(clip.duration, kMinClipDuration);
    const QString id = clip.id;
    auto& t = m_tracks[trackIndex];
    t.clips.push_back(std::move(clip));
    sortTrack(t);
    resolveOverlaps(t, id);
    notifyChanged();
    return id;
}

QString Project::addClipFromMedia(const QString& mediaId, int trackIndex, TimeUs start, bool link)
{
    if (trackIndex >= 0 && trackIndex < trackCount() && m_tracks[trackIndex].isAudio())
        return addMediaClips(mediaId, -1, trackIndex, start, nullptr, link);
    return addMediaClips(mediaId, trackIndex, -1, start, nullptr, link);
}

QString Project::addMediaClips(const QString& mediaId, int videoTrack, int audioTrack, TimeUs start,
                               QString* audioClipId, bool link)
{
    const MediaItem* m = mediaById(mediaId);
    if (!m) return {};
    const bool wantVideo = m->info.hasVideo;
    const bool wantAudio = m->info.hasAudio && !m->info.isImage;
    if (!wantVideo && !wantAudio) return {};

    Clip base;
    base.mediaId = mediaId;
    base.name = m->name;
    base.start = snapToFrame(std::max<TimeUs>(0, start), m_settings.fps);
    base.in = 0;
    base.duration = (m->info.isImage || m->info.duration <= 0) ? kDefaultImageDuration : m->info.duration;
    if (wantVideo && wantAudio && link) base.linkId = newId();
    const TimeUs end = base.start + base.duration;

    // Remember the requested audio track by id: adding a video track shifts indices.
    const bool audioOk = audioTrack >= 0 && audioTrack < trackCount() && m_tracks[audioTrack].isAudio();
    const QString audioTrackId = audioOk ? m_tracks[audioTrack].id : QString();
    const bool videoOk = videoTrack >= 0 && videoTrack < trackCount() && !m_tracks[videoTrack].isAudio();

    QString vid, aid;
    if (wantVideo) {
        const int vt = videoOk ? videoTrack : findFreeTrack(TrackKind::Video, base.start, end, true);
        Clip c = base;
        c.id = newId();
        vid = addClip(vt, std::move(c));
    }
    if (wantAudio) {
        const int at = audioOk ? trackIndexById(audioTrackId) : findFreeTrack(TrackKind::Audio, base.start, end, true);
        Clip c = base;
        c.id = newId();
        aid = addClip(at, std::move(c));
    }
    if (audioClipId) *audioClipId = aid;
    return vid.isEmpty() ? aid : vid;
}

QStringList Project::linkedClips(const QString& id) const
{
    const Clip* c = findClip(id);
    if (!c || c->linkId.isEmpty()) return {};
    QStringList out;
    for (const auto& t : m_tracks)
        for (const auto& x : t.clips)
            if (x.linkId == c->linkId && x.id != id) out << x.id;
    return out;
}

QString Project::linkCandidate(const QString& id) const
{
    int ti = -1;
    const Clip* c = findClip(id, &ti);
    if (!c || !c->linkId.isEmpty()) return {};
    QString best;
    TimeUs bestOffset = -1;
    for (const auto& t : m_tracks) {
        if (t.isAudio() == m_tracks[ti].isAudio()) continue;
        for (const auto& x : t.clips) {
            if (x.mediaId != c->mediaId || !x.linkId.isEmpty() || x.start >= c->end() || x.end() <= c->start) continue;
            // 0 = perfectly in sync (same source frame at the same timeline time)
            const TimeUs offset = std::llabs((x.start - x.in) - (c->start - c->in));
            if (bestOffset < 0 || offset < bestOffset) {
                bestOffset = offset;
                best = x.id;
            }
        }
    }
    return best;
}

bool Project::linkClips(const QStringList& ids)
{
    if (ids.size() < 2) return false;
    for (const QString& id : ids)
        if (!findClip(id)) return false;
    const QString link = newId();
    for (const QString& id : ids) findClipMutable(id)->linkId = link;
    notifyChanged();
    return true;
}

void Project::unlinkClip(const QString& id)
{
    const QStringList group = QStringList{id} + linkedClips(id);
    for (const QString& g : group)
        if (Clip* c = findClipMutable(g)) c->linkId.clear();
    notifyChanged();
}
void Project::removeClip(const QString& id, bool linked)
{
    const QStringList group = QStringList{id} + (linked ? linkedClips(id) : QStringList());
    bool removed = false;
    for (auto& t : m_tracks) {
        const auto it = std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return group.contains(c.id); });
        if (it != t.clips.end()) {
            t.clips.erase(it, t.clips.end());
            removed = true;
        }
    }
    if (removed) notifyChanged();
}
bool Project::moveClip(const QString& id, int newTrack, TimeUs newStart, bool linked)
{
    int oldTrack = -1;
    const Clip* c = findClip(id, &oldTrack);
    if (!c || newTrack < 0 || newTrack >= trackCount()) return false;
    if (m_tracks[newTrack].kind != m_tracks[oldTrack].kind) return false; // video stays video, audio stays audio

    struct Partner { QString id; QString trackId; TimeUs start; };
    std::vector<Partner> partners;
    TimeUs minStart = c->start;
    if (linked) {
        for (const QString& p : linkedClips(id)) {
            int pt = -1;
            const Clip* pc = findClip(p, &pt);
            partners.push_back({p, m_tracks[pt].id, pc->start});
            minStart = std::min(minStart, pc->start);
        }
    }
    const TimeUs delta = std::max(std::max<TimeUs>(0, newStart) - c->start, -minStart); // nothing before 0
    const TimeUs start = c->start + delta;
    if (!moveSingle(id, newTrack, start)) return false;
    for (const auto& p : partners) moveSingle(p.id, trackIndexById(p.trackId), p.start + delta);
    return true;
}

bool Project::moveSingle(const QString& id, int newTrack, TimeUs newStart)
{
    int oldTrack = -1;
    Clip* c = findClipMutable(id, &oldTrack);
    if (!c || newTrack < 0 || newTrack >= trackCount()) return false;
    Clip copy = *c;
    copy.start = std::max<TimeUs>(0, newStart);
    auto& src = m_tracks[oldTrack].clips;
    src.erase(std::remove_if(src.begin(), src.end(), [&](const Clip& x) { return x.id == id; }), src.end());
    auto& dst = m_tracks[newTrack];
    dst.clips.push_back(copy);
    sortTrack(dst);
    resolveOverlaps(dst, id);
    notifyChanged();
    return true;
}
bool Project::trimClip(const QString& id, TimeUs newStart, TimeUs newIn, TimeUs newDuration, bool linked)
{
    const Clip* c = findClip(id);
    if (!c) return false;
    const TimeUs os = c->start, oi = c->in, od = c->duration;
    const QStringList partners = linked ? linkedClips(id) : QStringList();
    if (!trimSingle(id, newStart, newIn, newDuration)) return false;
    const Clip* n = findClip(id);
    if (!n) return true;
    // Apply the same edge changes to the linked clips.
    const TimeUs ds = n->start - os, di = n->in - oi, dd = n->duration - od;
    for (const QString& p : partners)
        if (const Clip* pc = findClip(p)) trimSingle(p, pc->start + ds, pc->in + di, pc->duration + dd);
    return true;
}

bool Project::trimSingle(const QString& id, TimeUs newStart, TimeUs newIn, TimeUs newDuration)
{
    int ti = -1;
    Clip* c = findClipMutable(id, &ti);
    if (!c) return false;
    if (newIn < 0) {                         // can't go before the source start
        newStart -= newIn;
        newDuration += newIn;
        newIn = 0;
    }
    c->start = std::max<TimeUs>(0, newStart);
    c->in = newIn;
    Clip probe = *c;
    c->duration = std::clamp(newDuration, kMinClipDuration, maxClipDuration(probe));
    auto& t = m_tracks[ti];
    sortTrack(t);
    resolveOverlaps(t, id);
    notifyChanged();
    return true;
}
QString Project::splitClip(const QString& id, TimeUs at, bool linked)
{
    const QStringList partners = linked ? linkedClips(id) : QStringList();
    const QString right = splitSingle(id, at);
    if (right.isEmpty()) return {};
    QStringList rights{right};
    for (const QString& p : partners) {
        const QString r = splitSingle(p, at);
        if (!r.isEmpty()) rights << r;
    }
    // Left halves keep the old link; the right halves form a new linked group.
    const QString link = rights.size() > 1 ? newId() : QString();
    for (const QString& r : rights)
        if (Clip* c = findClipMutable(r)) c->linkId = link;
    return right;
}

QString Project::splitSingle(const QString& id, TimeUs at)
{
    int ti = -1;
    Clip* c = findClipMutable(id, &ti);
    if (!c) return {};
    if (at <= c->start + kMinClipDuration || at >= c->end() - kMinClipDuration) return {};
    Clip right = *c;
    const TimeUs cut = at - c->start;
    const TimeUs off = c->sourceOffset(cut);
    const TimeUs span = c->sourceSpan();
    const double u = double(cut) / double(std::max<TimeUs>(1, c->duration));
    right.id = newId();
    right.start = at;
    right.duration = c->duration - cut;
    right.fadeIn = 0;
    if (c->freeze) {
        right.in = c->in;                    // the same still on both sides
    } else if (!c->reverse) {
        right.in = c->in + off;
    } else {                                 // backwards: the left part shows the source's end
        right.in = c->in;
        c->in = c->in + std::max<TimeUs>(0, span - off);
    }
    if (!c->speedCurve.empty()) {
        right.speedCurve = subCurve(*c, u, 1.0);
        c->speedCurve = subCurve(*c, 0.0, u);
    }
    c->duration = cut;
    c->fadeOut = 0;
    const QString rid = right.id;
    m_tracks[ti].clips.push_back(std::move(right));
    sortTrack(m_tracks[ti]);
    notifyChanged();
    return rid;
}
bool Project::updateClip(const Clip& clip)
{
    Clip* c = findClipMutable(clip.id);
    if (!c) return false;
    const TimeUs start = c->start, in = c->in, dur = c->duration;
    const QString link = c->linkId;
    *c = clip;
    c->start = start; c->in = in; c->duration = dur; // position edits go through move/trim
    c->linkId = link;                                // linking goes through link/unlinkClip
    c->fadeIn = std::clamp<TimeUs>(c->fadeIn, 0, dur);
    c->fadeOut = std::clamp<TimeUs>(c->fadeOut, 0, dur);
    notifyChanged();
    return true;
}

QString Project::addTextClip(int trackIndex, TimeUs start, TimeUs duration, const TextData& text,
                             const ClipTransform& transform)
{
    Clip c;
    c.id = newId();
    c.text = text;
    const QString first = text.text.section(QChar(u'\n'), 0, 0).trimmed();
    c.name = first.isEmpty() ? QStringLiteral("Text") : first.left(60);
    c.start = snapToFrame(std::max<TimeUs>(0, start), m_settings.fps);
    c.duration = std::max(duration, kMinClipDuration);
    c.transform = transform;
    if (trackIndex < 0 || trackIndex >= trackCount() || m_tracks[trackIndex].isAudio()) {
        // Text goes above the pictures: the top video track if it is free there, else a new one on top.
        trackIndex = -1;
        if (firstAudioTrack() > 0 && isTrackFree(0, c.start, c.end())) trackIndex = 0;
        if (trackIndex < 0) trackIndex = addTrack({}, TrackKind::Video);
    }
    return addClip(trackIndex, std::move(c));
}

TimeUs Project::duration() const
{
    TimeUs d = 0;
    for (const auto& t : m_tracks)
        if (!t.clips.empty()) d = std::max(d, t.clips.back().end());
    return d;
}

TimelineSnapshot Project::snapshot() const
{
    TimelineSnapshot s;
    s.settings = m_settings;
    s.tracks = m_tracks;
    s.master = m_master;
    s.captions = m_captions;
    if (m_captions.enabled) s.captionChunks = captionChunks();
    for (const auto& m : m_media) {
        MediaItem copy = m;
        copy.thumbnail = {};
        s.media.insert(m.id, std::move(copy));
    }
    return s;
}

void Project::notifyChanged()
{
    setModified(true);
    emit changed();
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------
QJsonArray Project::tracksJson() const
{
    QJsonArray arr;
    for (const auto& t : m_tracks) arr.append(t.toJson());
    return arr;
}

// Undo state = every track plus one trailing {"master": ...} entry.
QJsonArray Project::captureState() const
{
    QJsonArray arr = tracksJson();
    arr.append(QJsonObject{{"master", m_master.toJson()}});
    return arr;
}

void Project::restoreState(const QJsonArray& tracks)
{
    m_tracks.clear();
    for (const auto& v : tracks) {
        const QJsonObject o = v.toObject();
        if (o.contains("master")) m_master = MasterBus::fromJson(o.value("master").toObject());
        else m_tracks.push_back(Track::fromJson(o));
    }
    if (m_tracks.empty()) addTrack();
    notifyChanged();
}

void Project::commitUndo(const QString& text, const QJsonArray& before, const QString& mergeKey)
{
    QJsonArray after = captureState();
    if (after == before) return;
    m_undo->push(new SnapshotCommand(this, text, before, std::move(after), mergeKey));
}

void Project::edit(const QString& text, const std::function<void()>& fn, const QString& mergeKey)
{
    const QJsonArray before = captureState();
    fn();
    commitUndo(text, before, mergeKey);
}

// ---------------------------------------------------------------------------
// Edge trims, speed, reverse, freeze frame, duplicate, ripple editing
// ---------------------------------------------------------------------------
bool Project::trimEdges(const QString& id, TimeUs newStart, TimeUs newEnd, bool linked)
{
    const Clip* c = findClip(id);
    if (!c) return false;
    const TimeUs ds = newStart - c->start, de = newEnd - c->end();
    const QStringList partners = linked ? linkedClips(id) : QStringList();
    if (!trimEdgesSingle(id, newStart, newEnd)) return false;
    for (const QString& p : partners)
        if (const Clip* pc = findClip(p)) trimEdgesSingle(p, pc->start + ds, pc->end() + de);
    return true;
}

bool Project::trimEdgesSingle(const QString& id, TimeUs ns, TimeUs ne)
{
    int ti = -1;
    Clip* c = findClipMutable(id, &ti);
    if (!c) return false;
    ns = std::max<TimeUs>(0, ns);
    if (ne - ns < kMinClipDuration) ne = ns + kMinClipDuration;
    const TimeUs dl = ns - c->start, dr = ne - c->end(), d = c->duration;
    TimeUs newIn = c->in;
    if (!c->freeze && !c->isText()) {          // text has no source: any length either way
        if (!c->reverse) newIn = c->in + c->sourceOffset(dl);                         // source at the new start
        else newIn = c->in + c->sourceSpan() - c->sourceOffset(d + dr);               // source at the new end
        if (newIn < 0) {                     // ran past the start of the source: give the time back
            const TimeUs back = TimeUs(double(-newIn) / std::max(1e-3, c->speed * c->curveAt(c->reverse ? 1.0 : 0.0)));
            if (!c->reverse) ns += back; else ne -= back;
            newIn = 0;
        }
    }
    c->start = ns;
    c->in = newIn;
    Clip probe = *c;
    probe.duration = ne - ns;
    c->duration = std::clamp(ne - ns, kMinClipDuration, maxClipDuration(probe));
    sortTrack(m_tracks[ti]);
    resolveOverlaps(m_tracks[ti], id);
    notifyChanged();
    return true;
}

TimeUs Project::rippleShift(const QStringList& trackIds, TimeUs from, TimeUs delta)
{
    if (delta == 0) return 0;
    // Everything that moves: clips at/after `from` on the tracks + their linked clips.
    QStringList moving;
    for (const auto& t : m_tracks) {
        if (!trackIds.contains(t.id)) continue;
        for (const auto& c : t.clips)
            if (c.start >= from) moving << c.id;
    }
    for (int i = 0; i < moving.size(); ++i)
        for (const QString& p : linkedClips(moving[i]))
            if (!moving.contains(p)) moving << p;
    if (moving.isEmpty()) return 0;

    if (delta < 0) { // never overlap a clip that stays, never go before 0
        for (const auto& t : m_tracks)
            for (const auto& c : t.clips) {
                if (!moving.contains(c.id)) continue;
                TimeUs room = c.start;
                for (const auto& o : t.clips)
                    if (!moving.contains(o.id) && o.end() <= c.start) room = std::min(room, c.start - o.end());
                delta = std::max(delta, -room);
            }
        if (delta == 0) return 0;
    }
    for (auto& t : m_tracks) {
        for (auto& c : t.clips)
            if (moving.contains(c.id)) c.start += delta;
        sortTrack(t);
    }
    notifyChanged();
    return delta;
}

bool Project::setClipSpeed(const QString& id, double speed, const std::vector<SpeedPoint>& curve,
                           const QString& curvePreset, bool linked)
{
    const Clip* c = findClip(id);
    if (!c || c->freeze || c->isText()) return false;
    speed = std::clamp(speed, 0.05, 16.0);
    const TimeUs oldEnd = c->end();
    const QStringList group = QStringList{id} + (linked ? linkedClips(id) : QStringList());
    QStringList tracks;
    TimeUs newEnd = oldEnd;
    std::vector<std::pair<QString, TimeUs>> newDur;
    for (const QString& g : group) {
        int ti = -1;
        Clip probe = *findClip(g, &ti);
        const TimeUs span = probe.sourceSpan();        // keep showing the same source
        probe.speed = speed;
        probe.speedCurve = curve;
        // Whole frames, and never more footage than the source has.
        TimeUs d = snapToFrame(TimeUs(std::llround(double(span) / std::max(1e-3, probe.meanRate()))), m_settings.fps);
        d = std::clamp(d, kMinClipDuration, std::max(kMinClipDuration, maxClipDuration(probe)));
        newDur.emplace_back(g, d);
        if (!tracks.contains(m_tracks[ti].id)) tracks << m_tracks[ti].id;
        if (g == id) newEnd = probe.start + d;
    }
    // Make room first (longer) so nothing gets overwritten; close up afterwards (shorter).
    if (newEnd > oldEnd) rippleShift(tracks, oldEnd, newEnd - oldEnd);
    for (const auto& [g, d] : newDur) {
        Clip* x = findClipMutable(g);
        x->speed = speed;
        x->speedCurve = curve;
        x->curvePreset = curve.empty() ? QString() : curvePreset;
        x->duration = d;
        x->fadeIn = std::min(x->fadeIn, d);
        x->fadeOut = std::min(x->fadeOut, d);
    }
    for (auto& t : m_tracks) sortTrack(t);
    if (newEnd < oldEnd) rippleShift(tracks, oldEnd, newEnd - oldEnd);
    notifyChanged();
    return true;
}

bool Project::setClipReverse(const QString& id, bool reverse, bool linked)
{
    if (!findClip(id)) return false;
    const QStringList group = QStringList{id} + (linked ? linkedClips(id) : QStringList());
    for (const QString& g : group)
        if (Clip* c = findClipMutable(g); c && !c->isText()) c->reverse = reverse;
    notifyChanged();
    return true;
}

QString Project::freezeFrame(const QString& id, TimeUs at, TimeUs hold)
{
    int ti = -1;
    const Clip* c = findClip(id, &ti);
    if (!c || m_tracks[ti].isAudio() || !c->contains(at) || hold <= 0) return {};
    const MediaItem* m = mediaById(c->mediaId);
    if (!m || !m->info.hasVideo) return {};
    Clip still = *c;                         // keeps transform, crop and effects
    still.id = newId();
    still.linkId.clear();
    still.in = c->sourceTimeAt(at);
    still.start = at;
    still.duration = hold;
    still.freeze = true;
    still.reverse = false;
    still.speed = 1.0;
    still.speedCurve.clear();
    still.curvePreset.clear();
    still.fadeIn = still.fadeOut = 0;
    const QString trackId = m_tracks[ti].id;

    QStringList tracks{trackId};
    for (const QString& p : linkedClips(id)) {
        int pt = -1;
        if (findClip(p, &pt) && !tracks.contains(m_tracks[pt].id)) tracks << m_tracks[pt].id;
    }
    if (at > c->start) splitClip(id, at);    // the part after `at` (and its sound) moves on
    rippleShift(tracks, at, hold);
    addClip(trackIndexById(trackId), still);
    return still.id;
}

QString Project::duplicateClip(const QString& id, bool linked)
{
    if (!findClip(id)) return {};
    const QStringList group = QStringList{id} + (linked ? linkedClips(id) : QStringList());
    TimeUs s = std::numeric_limits<TimeUs>::max(), e = 0;
    for (const QString& g : group) {
        const Clip* c = findClip(g);
        s = std::min(s, c->start);
        e = std::max(e, c->end());
    }
    const TimeUs offset = e - s;             // right after the original
    const QString link = group.size() > 1 ? newId() : QString();
    QString mainCopy;
    for (const QString& g : group) {
        int ti = -1;
        Clip copy = *findClip(g, &ti);
        copy.id = newId();
        copy.linkId = link;
        copy.start += offset;
        int target = ti;
        if (!isTrackFree(target, copy.start, copy.end()))
            target = findFreeTrack(m_tracks[ti].kind, copy.start, copy.end(), true);
        addClip(target, copy);
        if (g == id) mainCopy = copy.id;
    }
    return mainCopy;
}

bool Project::rippleDelete(const QString& id, bool linked)
{
    if (!findClip(id)) return false;
    const QStringList group = QStringList{id} + (linked ? linkedClips(id) : QStringList());
    QStringList tracks;
    TimeUs s = std::numeric_limits<TimeUs>::max(), e = 0;
    for (const QString& g : group) {
        int ti = -1;
        const Clip* c = findClip(g, &ti);
        s = std::min(s, c->start);
        e = std::max(e, c->end());
        if (!tracks.contains(m_tracks[ti].id)) tracks << m_tracks[ti].id;
    }
    removeClip(id, linked);
    rippleShift(tracks, e, s - e);
    return true;
}

TimeUs Project::closeGap(int track, TimeUs at)
{
    if (track < 0 || track >= trackCount()) return 0;
    TimeUs gs = 0, ge = -1;
    for (const auto& c : m_tracks[track].clips) {
        if (c.contains(at)) return 0;        // not a gap
        if (c.end() <= at) gs = std::max(gs, c.end());
        else if (ge < 0 || c.start < ge) ge = c.start;
    }
    if (ge < 0 || ge <= gs) return 0;        // nothing after the gap
    return -rippleShift({m_tracks[track].id}, ge, gs - ge);
}

bool Project::removeRange(TimeUs a, TimeUs b, bool closeGapAfter)
{
    if (b <= a) return false;
    QStringList all;
    for (const auto& t : m_tracks) all << t.id;
    // Cut every clip at both ends of the range, then drop what lies inside.
    for (int pass = 0; pass < 2; ++pass) {
        const TimeUs at = pass == 0 ? a : b;
        QStringList ids;
        for (const auto& t : m_tracks)
            for (const auto& c : t.clips)
                if (c.start < at && c.end() > at) ids << c.id;
        for (const QString& id : ids)       // linked split keeps picture/sound halves paired
            if (const Clip* c = findClip(id); c && c->start < at && c->end() > at) splitClip(id, at, true);
    }
    for (auto& t : m_tracks)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                     [&](const Clip& c) { return c.start >= a && c.end() <= b; }),
                      t.clips.end());
    if (closeGapAfter) rippleShift(all, b, a - b);
    notifyChanged();
    return true;
}

// Projects saved before video/audio tracks existed played every clip's picture
// and sound on whatever track it was on. Convert them: video tracks first, and
// each clip's other half placed, linked, on a track of the matching kind.
void Project::migrateLegacyTracks()
{
    std::stable_partition(m_tracks.begin(), m_tracks.end(), [](const Track& t) { return !t.isAudio(); });
    struct Job { QString clipId; bool toAudio; bool move; };
    std::vector<Job> jobs;
    for (const auto& t : m_tracks)
        for (const auto& c : t.clips) {
            const MediaItem* m = mediaById(c.mediaId);
            if (!m) continue;
            const bool audio = m->info.hasAudio && !m->info.isImage;
            if (!t.isAudio() && audio) jobs.push_back({c.id, true, !m->info.hasVideo});   // sound -> audio track
            if (t.isAudio() && m->info.hasVideo) jobs.push_back({c.id, false, !audio});  // picture -> video track
        }
    for (const auto& j : jobs) {
        int ti = -1;
        const Clip* c = findClip(j.clipId, &ti);
        if (!c) continue;
        Clip copy = *c;
        const int target = findFreeTrack(j.toAudio ? TrackKind::Audio : TrackKind::Video, copy.start, copy.end(), true);
        if (j.move) {
            removeClip(copy.id, false);
            addClip(target, copy);
        } else {
            const QString link = newId();
            findClipMutable(copy.id)->linkId = link;
            copy.id = newId();
            copy.linkId = link;
            addClip(target, copy);
        }
    }
    if (firstAudioTrack() == trackCount()) addTrack(QStringLiteral("A1"), TrackKind::Audio);
    if (firstAudioTrack() == 0) addTrack(QStringLiteral("V1"), TrackKind::Video);
}

// ---------------------------------------------------------------------------
// Whole-project JSON (used for .json import/export and inside the SQLite file)
// ---------------------------------------------------------------------------
QJsonObject Project::toJson() const
{
    QJsonArray media;
    for (const auto& m : m_media)
        media.append(QJsonObject{{"id", m.id}, {"path", m.path}, {"name", m.name}, {"info", m.info.toJson()}});
    return {
        {"format", "video-editor-project"},
        {"version", 2},
        {"settings", m_settings.toJson()},
        {"media", media},
        {"master", m_master.toJson()},
        {"tracks", tracksJson()},
        {"captions", m_captions.toJson()},
        {"transcripts", [this] {
            QJsonArray a;
            for (const auto& t : m_transcripts) a.append(t.toJson());
            return a;
        }()},
    };
}

bool Project::fromJson(const QJsonObject& root, QString* error)
{
    if (root.value("format").toString() != QLatin1String("video-editor-project")) {
        if (error) *error = tr("Not a video editor project file");
        return false;
    }
    m_settings = ProjectSettings::fromJson(root.value("settings").toObject());
    m_media.clear();
    for (const auto& v : root.value("media").toArray()) {
        const auto o = v.toObject();
        MediaItem m;
        m.id = o.value("id").toString();
        m.path = o.value("path").toString();
        m.name = o.value("name").toString();
        m.info = MediaInfo::fromJson(o.value("info").toObject());
        m_media.push_back(std::move(m));
    }
    m_master = MasterBus::fromJson(root.value("master").toObject());
    m_tracks.clear();
    bool legacy = false;
    for (const auto& v : root.value("tracks").toArray()) {
        legacy |= !v.toObject().contains("kind");
        m_tracks.push_back(Track::fromJson(v.toObject()));
    }
    if (legacy) migrateLegacyTracks();
    if (m_tracks.empty()) addTrack();
    m_captions = CaptionStyle::fromJson(root.value("captions").toObject());
    m_transcripts.clear();
    for (const auto& v : root.value("transcripts").toArray()) {
        const Transcript t = Transcript::fromJson(v.toObject());
        if (!t.mediaId.isEmpty()) m_transcripts.insert(t.mediaId, t);
    }
    m_undo->clear();
    m_modified = false;
    emit settingsChanged();
    emit mediaChanged();
    emit changed();
    emit transcriptsChanged();
    emit captionsChanged();
    emit modifiedChanged(false);
    return true;
}

} // namespace ve
