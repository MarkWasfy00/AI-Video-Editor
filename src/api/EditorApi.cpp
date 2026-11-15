#include "api/EditorApi.h"

#include "core/ProjectStore.h"
#include "media/Exporter.h"
#include "media/FrameSource.h"
#include "media/HwAccel.h"
#include "media/MediaProbe.h"
#include "media/StemSeparator.h"
#include "render/CaptionRenderer.h"
#include "render/GpuInfo.h"
#include "render/OffscreenRenderer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>
#include <QUndoStack>

#include <algorithm>
#include <cmath>

namespace ve {

bool EditorHost::setPreviewRenderer(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("The preview renderer can only be changed in the GUI app.");
    return false;
}

namespace {

TimeUs toUs(double seconds) { return secondsToUs(seconds); }
double toSec(TimeUs us) { return usToSeconds(us); }

double clampNum(const QJsonObject& o, const char* key, double current, double lo, double hi)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) return current;
    if (!v.isDouble()) throw ApiError(QStringLiteral("'%1' must be a number").arg(QLatin1String(key)));
    return std::clamp(v.toDouble(), lo, hi);
}

bool boolVal(const QJsonObject& o, const char* key, bool current)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) return current;
    if (!v.isBool()) throw ApiError(QStringLiteral("'%1' must be a boolean").arg(QLatin1String(key)));
    return v.toBool();
}

} // namespace

EditorApi::EditorApi(Project* project, EditorHost* host)
    : m_project(project)
    , m_host(host)
{
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
const Clip& EditorApi::requireClip(const QString& id, int* track) const
{
    const Clip* c = m_project->findClip(id, track);
    if (!c) throw ApiError(QStringLiteral("No clip with id '%1'").arg(id));
    return *c;
}

int EditorApi::requireTrack(int index) const
{
    if (index < 0 || index >= m_project->trackCount())
        throw ApiError(QStringLiteral("Track index %1 out of range (0..%2)").arg(index).arg(m_project->trackCount() - 1));
    return index;
}

QJsonObject EditorApi::clipToJson(const Clip& c, int track) const
{
    const MediaItem* m = m_project->mediaById(c.mediaId);
    return {
        {"id", c.id},
        {"name", c.name},
        {"mediaId", c.mediaId},
        {"mediaPath", m ? m->path : QString()},
        {"track", track},
        {"trackKind", m_project->tracks()[track].isAudio() ? "audio" : "video"},
        {"linkId", c.linkId},
        {"linkedClips", QJsonArray::fromStringList(m_project->linkedClips(c.id))},
        {"start", toSec(c.start)},
        {"end", toSec(c.end())},
        {"duration", toSec(c.duration)},
        {"in", toSec(c.in)},
        {"transform", c.transform.toJson()},
        {"effects", c.effects.toJson()},
        {"volume", c.volume},
        {"speed", c.speed},
        {"speedCurve", [&c] {
            QJsonArray a;
            for (const auto& p : c.speedCurve) a.append(QJsonArray{p.pos, p.speed});
            return a;
        }()},
        {"curvePreset", c.curvePreset},
        {"reverse", c.reverse},
        {"freeze", c.freeze},
        {"sourceSpan", toSec(c.sourceSpan())},
        {"fadeIn", toSec(c.fadeIn)},
        {"fadeOut", toSec(c.fadeOut)},
    };
}

QJsonObject EditorApi::mediaToJson(const MediaItem& m) const
{
    QJsonObject info = m.info.toJson();
    info["duration"] = toSec(m.info.duration);
    return {{"id", m.id}, {"name", m.name}, {"path", m.path}, {"info", info}, {"used", m_project->isMediaUsed(m.id)}};
}

// ---------------------------------------------------------------------------
// project
// ---------------------------------------------------------------------------
QJsonObject EditorApi::projectNew()
{
    m_host->setPlaying(false);
    m_project->clear();
    m_host->seek(0);
    m_host->projectReplaced();
    return projectInfo();
}

QJsonObject EditorApi::projectOpen(const QString& path)
{
    if (path.isEmpty()) throw ApiError(QStringLiteral("path is required"));
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!QFileInfo::exists(abs)) throw ApiError(QStringLiteral("File not found: %1").arg(abs));
    m_host->setPlaying(false);
    QString err;
    if (!ProjectStore::load(*m_project, abs, &err)) throw ApiError(QStringLiteral("Could not open project: %1").arg(err));
    QJsonArray missing;
    for (const auto& m : m_project->media())
        if (!QFileInfo::exists(m.path)) missing.append(m.path);
    m_host->seek(0);
    m_host->projectReplaced();
    QJsonObject r = projectInfo();
    r["missingMedia"] = missing;
    return r;
}

QJsonObject EditorApi::projectSave(const QString& pathIn)
{
    QString path = pathIn.isEmpty() ? m_project->filePath() : QFileInfo(pathIn).absoluteFilePath();
    if (path.isEmpty()) throw ApiError(QStringLiteral("The project has no file yet; pass a path (.veproj or .json)"));
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".veproj");
    QString err;
    if (!ProjectStore::save(*m_project, path, &err)) throw ApiError(QStringLiteral("Save failed: %1").arg(err));
    m_project->setFilePath(path);
    m_project->undoStack()->setClean();
    m_project->setModified(false);
    return {{"path", path}, {"format", path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) ? "json" : "sqlite"}};
}

QJsonObject EditorApi::projectInfo() const
{
    QJsonObject s = m_project->settings().toJson();
    int clips = 0;
    for (const auto& t : m_project->tracks()) clips += int(t.clips.size());
    return {
        {"path", m_project->filePath()},
        {"modified", m_project->isModified()},
        {"settings", s},
        {"duration", toSec(m_project->duration())},
        {"trackCount", m_project->trackCount()},
        {"clipCount", clips},
        {"mediaCount", int(m_project->media().size())},
        {"host", m_host->hostKind()},
    };
}

QJsonObject EditorApi::projectJson() const
{
    // Seconds-based, fully expanded view of the whole project.
    QJsonArray tracks;
    for (int ti = 0; ti < m_project->trackCount(); ++ti) {
        const Track& t = m_project->tracks()[ti];
        QJsonArray clips;
        for (const auto& c : t.clips) clips.append(clipToJson(c, ti));
        tracks.append(QJsonObject{{"index", ti}, {"id", t.id}, {"name", t.name}, {"muted", t.muted},
                                  {"hidden", t.hidden}, {"clips", clips}});
    }
    QJsonArray media;
    for (const auto& m : m_project->media()) media.append(mediaToJson(m));
    QJsonObject r = projectInfo();
    r["media"] = media;
    r["tracks"] = tracks;
    return r;
}

QJsonObject EditorApi::projectSetSettings(const QJsonObject& o)
{
    ProjectSettings s = m_project->settings();
    if (o.contains("name")) s.name = o.value("name").toString(s.name);
    s.width = int(clampNum(o, "width", s.width, 16, 8192)) & ~1;
    s.height = int(clampNum(o, "height", s.height, 16, 8192)) & ~1;
    if (o.contains("aspectRatio")) {        // e.g. "16:9", "9:16", "1:1", "4:5" -> 1080p-class size
        const QStringList parts = o.value("aspectRatio").toString().split(QLatin1Char(':'));
        const double aw = parts.size() == 2 ? parts[0].toDouble() : 0, ah = parts.size() == 2 ? parts[1].toDouble() : 0;
        if (aw <= 0 || ah <= 0) throw ApiError(QStringLiteral("aspectRatio must look like 16:9"));
        const int base = int(clampNum(o, "shortSide", 1080, 144, 4320));
        if (aw >= ah) { s.height = base; s.width = int(std::lround(base * aw / ah)) & ~1; }
        else { s.width = base; s.height = int(std::lround(base * ah / aw)) & ~1; }
    }
    s.fps = clampNum(o, "fps", s.fps, 1, 240);
    if (o.contains("background")) {
        const QColor c(o.value("background").toString());
        if (!c.isValid()) throw ApiError(QStringLiteral("background must be a color like #000000"));
        s.background = c;
    }
    m_project->setSettings(s);
    return projectInfo();
}

// ---------------------------------------------------------------------------
// media
// ---------------------------------------------------------------------------
QJsonObject EditorApi::mediaImport(const QStringList& paths)
{
    if (paths.isEmpty()) throw ApiError(QStringLiteral("paths must contain at least one file"));
    QJsonArray imported, errors;
    for (const QString& p : paths) {
        const QString abs = QFileInfo(p).absoluteFilePath();
        if (!QFileInfo(abs).isFile()) {
            errors.append(QJsonObject{{"path", p}, {"error", "file not found"}});
            continue;
        }
        QString err;
        const auto info = MediaProbe::probe(abs, &err);
        if (!info) {
            errors.append(QJsonObject{{"path", p}, {"error", err}});
            continue;
        }
        MediaItem item;
        item.path = abs;
        item.info = *info;
        if (m_host->wantsThumbnails()) item.thumbnail = MediaProbe::thumbnail(abs, item.info);
        const QString id = m_project->addMedia(std::move(item));
        imported.append(mediaToJson(*m_project->mediaById(id)));
    }
    return {{"imported", imported}, {"errors", errors}};
}

QJsonObject EditorApi::mediaList() const
{
    QJsonArray arr;
    for (const auto& m : m_project->media()) arr.append(mediaToJson(m));
    return {{"media", arr}};
}

QJsonObject EditorApi::mediaRemove(const QString& id)
{
    if (!m_project->mediaById(id)) throw ApiError(QStringLiteral("No media with id '%1'").arg(id));
    if (m_project->isMediaUsed(id)) throw ApiError(QStringLiteral("Media is used by clips on the timeline; remove those clips first"));
    m_project->removeMedia(id);
    return {{"removed", id}};
}

QJsonObject EditorApi::mediaProbe(const QString& path) const
{
    QString err;
    const auto info = MediaProbe::probe(path, &err);
    if (!info) throw ApiError(QStringLiteral("Cannot read media: %1").arg(err));
    QJsonObject o = info->toJson();
    o["duration"] = toSec(info->duration);
    o["path"] = QFileInfo(path).absoluteFilePath();
    return o;
}

// ---------------------------------------------------------------------------
// tracks
// ---------------------------------------------------------------------------
QJsonObject EditorApi::trackToJson(int i) const
{
    const Track& t = m_project->tracks()[i];
    TimelineSnapshot s;
    s.tracks = m_project->tracks();
    bool hasAudio = false;
    for (const auto& c : t.clips)
        if (const MediaItem* m = m_project->mediaById(c.mediaId); m && m->info.hasAudio) hasAudio = true;
    return {{"index", i}, {"id", t.id}, {"name", t.name}, {"kind", t.isAudio() ? "audio" : "video"},
            {"muted", t.muted}, {"hidden", t.hidden},
            {"solo", t.solo}, {"gainDb", t.gainDb}, {"pan", t.pan}, {"audible", trackAudible(s, t)},
            {"hasAudioClips", hasAudio}, {"clipCount", int(t.clips.size())}};
}

QJsonObject EditorApi::trackList() const
{
    QJsonArray arr;
    for (int i = 0; i < m_project->trackCount(); ++i) arr.append(trackToJson(i));
    return {{"tracks", arr},
            {"note", "Video tracks come first (index 0 is the top layer of the picture); audio tracks follow. "
                     "Video tracks are silent: a video's sound is a linked clip on an audio track."}};
}

QJsonObject EditorApi::trackAdd(const QString& name, const QString& kind)
{
    if (kind != QLatin1String("video") && kind != QLatin1String("audio"))
        throw ApiError(QStringLiteral("kind must be 'video' or 'audio'"));
    m_project->edit(QStringLiteral("Add track"), [&] {
        m_project->addTrack(name, kind == QLatin1String("audio") ? TrackKind::Audio : TrackKind::Video);
    });
    return trackList();
}

QJsonObject EditorApi::trackRemove(int index)
{
    requireTrack(index);
    if (m_project->trackCount() <= 1) throw ApiError(QStringLiteral("Cannot remove the last track"));
    m_project->edit(QStringLiteral("Remove track"), [&] { m_project->removeTrack(index); });
    return trackList();
}

void EditorApi::applyTrackChanges(int index, const QJsonObject& o, const QString& undoText)
{
    requireTrack(index);
    const Track& t = m_project->tracks()[index];
    const bool muted = boolVal(o, "muted", t.muted);
    const bool hidden = boolVal(o, "hidden", t.hidden);
    const bool solo = boolVal(o, "solo", t.solo);
    const double gainDb = clampNum(o, "gainDb", t.gainDb, kMinGainDb, kMaxGainDb);
    const double pan = clampNum(o, "pan", t.pan, -1.0, 1.0);
    const QString name = o.contains("name") ? o.value("name").toString() : t.name;
    m_project->edit(undoText, [&] {
        m_project->setTrackMuted(index, muted);
        m_project->setTrackHidden(index, hidden);
        m_project->setTrackSolo(index, solo);
        m_project->setTrackGain(index, gainDb);
        m_project->setTrackPan(index, pan);
        m_project->renameTrack(index, name);
    });
}

QJsonObject EditorApi::trackUpdate(int index, const QJsonObject& o)
{
    applyTrackChanges(index, o, QStringLiteral("Change track"));
    return trackList();
}

// ---------------------------------------------------------------------------
// audio mixer
// ---------------------------------------------------------------------------
namespace {

double round1(double v) { return std::round(v * 10.0) / 10.0; }

QJsonObject levelsJson(const ChannelLevels& l)
{
    return {{"peakDb", QJsonArray{round1(l.peakDb(0)), round1(l.peakDb(1))}},
            {"rmsDb", QJsonArray{round1(l.rmsDb(0)), round1(l.rmsDb(1))}}};
}

} // namespace

QJsonObject EditorApi::mixerGet() const
{
    QJsonArray tracks;
    bool soloActive = false;
    for (int i = 0; i < m_project->trackCount(); ++i) {
        if (!m_project->tracks()[i].isAudio()) continue;
        tracks.append(trackToJson(i));
        soloActive |= m_project->tracks()[i].solo;
    }
    QJsonObject r{{"tracks", tracks},
                  {"master", m_project->master().toJson()},
                  {"soloActive", soloActive},
                  {"range", QJsonObject{{"gainDb", QJsonArray{kMinGainDb, kMaxGainDb}}, {"pan", QJsonArray{-1, 1}}}},
                  {"signalFlow", "clip volume x fades -> track gainDb + pan (mute/solo) -> sum -> master gainDb/mute "
                                 "-> clip at 0 dBFS"}};
    MixLevels lv;
    if (m_host->audioLevels(lv)) {
        QJsonObject tm;
        for (int i = 0; i < m_project->trackCount(); ++i) {
            if (!m_project->tracks()[i].isAudio()) continue;
            const QString id = m_project->tracks()[i].id;
            tm[id] = levelsJson(lv.tracks.value(id));
        }
        QJsonObject master = levelsJson(lv.master);
        master["clipping"] = lv.clippedSamples > 0;
        r["meters"] = QJsonObject{{"tracks", tm}, {"master", master}};
    }
    return r;
}

QJsonObject EditorApi::mixerSetTrack(int index, const QJsonObject& o)
{
    for (const QString& k : o.keys())
        if (k != "gainDb" && k != "pan" && k != "muted" && k != "solo")
            throw ApiError(QStringLiteral("Unknown mixer field '%1' (use gainDb, pan, muted, solo)").arg(k));
    requireTrack(index);
    if (!m_project->tracks()[index].isAudio())
        throw ApiError(QStringLiteral("Track %1 is a video track; the mixer has audio tracks only (see mixer_get)").arg(index));
    applyTrackChanges(index, o, QStringLiteral("Mixer: %1").arg(m_project->tracks()[index].name));
    return mixerGet();
}

QJsonObject EditorApi::mixerSetMaster(const QJsonObject& o)
{
    MasterBus m = m_project->master();
    m.gainDb = clampNum(o, "gainDb", m.gainDb, kMinGainDb, kMaxGainDb);
    m.muted = boolVal(o, "muted", m.muted);
    m_project->edit(QStringLiteral("Mixer: Master"), [&] { m_project->setMaster(m); });
    return mixerGet();
}

QJsonObject EditorApi::mixerAnalyze(double start, double end) const
{
    const TimelineSnapshot s = m_project->snapshot();
    const TimeUs t0 = std::max<TimeUs>(0, toUs(start));
    const TimeUs t1 = end < 0 ? s.duration() : toUs(end);
    if (t1 <= t0) throw ApiError(QStringLiteral("Empty range (the timeline is %1 s long)").arg(toSec(s.duration())));
    const MixLevels lv = analyzeMix(s, t0, t1, s.settings.sampleRate > 0 ? s.settings.sampleRate : 48000);

    QJsonArray tracks;
    for (int i = 0; i < int(s.tracks.size()); ++i) {
        const Track& t = s.tracks[i];
        if (!t.isAudio()) continue;
        QJsonObject o = levelsJson(lv.tracks.value(t.id));
        o["index"] = i;
        o["id"] = t.id;
        o["name"] = t.name;
        o["audible"] = trackAudible(s, t);
        tracks.append(o);
    }
    QJsonObject master = levelsJson(lv.master);
    const double peak = std::max(lv.master.peakDb(0), lv.master.peakDb(1));
    master["clippedSamples"] = double(lv.clippedSamples);
    master["headroomDb"] = round1(-peak);
    QString advice;
    if (lv.master.peak[0] == 0 && lv.master.peak[1] == 0) advice = QStringLiteral("The mix is silent in this range.");
    else if (lv.clippedSamples > 0)
        advice = QStringLiteral("The master clips: lower the master or track gain by at least %1 dB.").arg(round1(peak + 0.1));
    else if (peak < -12) advice = QStringLiteral("The mix is quiet: there is %1 dB of headroom.").arg(round1(-peak));
    else advice = QStringLiteral("Levels are OK (no clipping).");
    return {{"start", toSec(t0)}, {"end", toSec(t1)}, {"tracks", tracks}, {"master", master}, {"advice", advice},
            {"note", "dBFS, post-fader; -60 means silence. Values are [left, right]."}};
}

// ---------------------------------------------------------------------------
// clips
// ---------------------------------------------------------------------------
QJsonObject EditorApi::clipList(int track) const
{
    QJsonArray arr;
    for (int ti = 0; ti < m_project->trackCount(); ++ti) {
        if (track >= 0 && ti != track) continue;
        for (const auto& c : m_project->tracks()[ti].clips) arr.append(clipToJson(c, ti));
    }
    return {{"clips", arr}};
}

QJsonObject EditorApi::clipGet(const QString& id) const
{
    int ti = -1;
    const Clip& c = requireClip(id, &ti);
    return clipToJson(c, ti);
}

QJsonObject EditorApi::clipAdd(const QString& mediaId, int track, double start, int audioTrack, bool link)
{
    const MediaItem* m = m_project->mediaById(mediaId);
    if (!m) throw ApiError(QStringLiteral("No media with id '%1'").arg(mediaId));
    if (track >= 0) requireTrack(track);
    if (audioTrack >= 0) {
        requireTrack(audioTrack);
        if (!m_project->tracks()[audioTrack].isAudio())
            throw ApiError(QStringLiteral("audioTrack %1 is not an audio track").arg(audioTrack));
    }
    const TimeUs t = snapToFrame(start < 0 ? m_host->playhead() : toUs(start), m_project->settings().fps);

    int videoTrack = -1;
    if (track >= 0) {
        if (m_project->tracks()[track].isAudio()) {
            if (audioTrack < 0) audioTrack = track;
        } else {
            videoTrack = track;
        }
    }
    QString id, audioId;
    m_project->edit(QStringLiteral("Add clip"), [&] {
        id = m_project->addMediaClips(mediaId, videoTrack, audioTrack, t, &audioId, link);
    });
    if (id.isEmpty()) throw ApiError(QStringLiteral("Could not add clip (the media has no video or audio)"));
    QJsonObject r = clipGet(id);
    QJsonArray linked;
    for (const QString& p : m_project->linkedClips(id)) linked.append(clipGet(p));
    r["linked"] = linked;
    if (!audioId.isEmpty() && audioId != id) r["audioClip"] = clipGet(audioId);
    return r;
}

QJsonObject EditorApi::clipMove(const QString& id, int track, double start, bool linked)
{
    int ti = -1;
    const Clip& c = requireClip(id, &ti);
    const int target = track < 0 ? ti : requireTrack(track);
    if (m_project->tracks()[target].kind != m_project->tracks()[ti].kind)
        throw ApiError(QStringLiteral("Clips stay on their kind of track: track %1 is a %2 track")
                           .arg(target).arg(m_project->tracks()[target].isAudio() ? "audio" : "video"));
    const TimeUs t = start < 0 ? c.start : snapToFrame(toUs(start), m_project->settings().fps);
    m_project->edit(QStringLiteral("Move clip"), [&] { m_project->moveClip(id, target, t, linked); });
    return clipGet(id);
}

QJsonObject EditorApi::clipTrim(const QString& id, const QJsonObject& o)
{
    const Clip& c = requireClip(id);
    TimeUs s = c.start, in = c.in, d = c.duration;
    if (o.contains("start")) {          // move the left edge, keep the end
        const TimeUs ns = toUs(clampNum(o, "start", 0, 0, 1e7));
        const TimeUs delta = ns - s;
        s = ns;
        in += delta;
        d -= delta;
    }
    if (o.contains("in")) in = toUs(clampNum(o, "in", 0, 0, 1e7));
    if (o.contains("end")) d = toUs(clampNum(o, "end", 0, 0, 1e7)) - s;
    if (o.contains("duration")) d = toUs(clampNum(o, "duration", 0, 0, 1e7));
    if (d <= 0) throw ApiError(QStringLiteral("Resulting duration must be positive"));
    const bool linked = boolVal(o, "linked", true);
    if (o.contains("in"))
        m_project->edit(QStringLiteral("Trim clip"), [&] { m_project->trimClip(id, s, in, d, linked); });
    else
        m_project->edit(QStringLiteral("Trim clip"), [&] { m_project->trimEdges(id, s, s + d, linked); });
    return clipGet(id);
}

QJsonObject EditorApi::clipSplit(const QString& id, double time, bool linked)
{
    const Clip& c = requireClip(id);
    const TimeUs t = time < 0 ? m_host->playhead() : toUs(time);
    if (!c.contains(t) || t == c.start) throw ApiError(QStringLiteral("Split time must lie inside the clip (%1 - %2 s)")
                                                         .arg(toSec(c.start)).arg(toSec(c.end())));
    QString right;
    m_project->edit(QStringLiteral("Split clip"), [&] { right = m_project->splitClip(id, t, linked); });
    if (right.isEmpty()) throw ApiError(QStringLiteral("Split point too close to the clip edge"));
    return {{"left", clipGet(id)}, {"right", clipGet(right)}};
}

QJsonObject EditorApi::clipRemove(const QString& id, bool linked)
{
    requireClip(id);
    const QStringList removed = QStringList{id} + (linked ? m_project->linkedClips(id) : QStringList());
    m_project->edit(QStringLiteral("Delete clip"), [&] { m_project->removeClip(id, linked); });
    return {{"removed", QJsonArray::fromStringList(removed)}};
}

QJsonObject EditorApi::clipLink(const QStringList& ids)
{
    if (ids.size() < 2) throw ApiError(QStringLiteral("Give at least two clip ids to link"));
    for (const QString& id : ids) requireClip(id);
    m_project->edit(QStringLiteral("Link clips"), [&] { m_project->linkClips(ids); });
    return clipGet(ids.first());
}

QJsonObject EditorApi::clipSetSpeed(const QString& id, const QJsonObject& o)
{
    const Clip& c = requireClip(id);
    if (c.freeze) throw ApiError(QStringLiteral("A freeze frame has no speed"));
    const double speed = clampNum(o, "speed", c.speed, 0.05, 16);
    std::vector<SpeedPoint> curve = c.speedCurve;
    QString preset = c.curvePreset;
    const QJsonValue cv = o.value("curve");
    if (cv.isString()) {
        preset = cv.toString();
        if (!speedCurvePresetNames().contains(preset))
            throw ApiError(QStringLiteral("Unknown curve '%1' (use %2 or a list of [pos, speed] points)")
                               .arg(preset, speedCurvePresetNames().join(", ")));
        curve = speedCurvePreset(preset);
        if (preset == QLatin1String("none")) preset.clear();
    } else if (cv.isArray()) {
        curve.clear();
        preset = QStringLiteral("custom");
        for (const auto& v : cv.toArray()) {
            const QJsonArray p = v.toArray();
            if (p.size() != 2 || !p[0].isDouble() || !p[1].isDouble())
                throw ApiError(QStringLiteral("curve points are [pos 0..1, speed]"));
            curve.push_back({std::clamp(p[0].toDouble(), 0.0, 1.0), std::clamp(p[1].toDouble(), 0.05, 16.0)});
        }
        std::sort(curve.begin(), curve.end(), [](const SpeedPoint& a, const SpeedPoint& b) { return a.pos < b.pos; });
        if (curve.size() == 1) throw ApiError(QStringLiteral("A curve needs at least two points"));
    } else if (!cv.isUndefined() && !cv.isNull()) {
        throw ApiError(QStringLiteral("curve must be a preset name or a list of points"));
    }
    const bool linked = boolVal(o, "linked", true);
    m_project->edit(QStringLiteral("Change speed"), [&] { m_project->setClipSpeed(id, speed, curve, preset, linked); });
    return clipGet(id);
}

QJsonObject EditorApi::clipReverse(const QString& id, bool reverse, bool linked)
{
    requireClip(id);
    m_project->edit(reverse ? QStringLiteral("Reverse clip") : QStringLiteral("Play forward"),
                    [&] { m_project->setClipReverse(id, reverse, linked); });
    return clipGet(id);
}

QJsonObject EditorApi::clipFreezeFrame(const QString& id, double time, double hold)
{
    int ti = -1;
    const Clip& c = requireClip(id, &ti);
    if (m_project->tracks()[ti].isAudio()) throw ApiError(QStringLiteral("Freeze frames are made from video clips"));
    const TimeUs t = time < 0 ? m_host->playhead() : toUs(time);
    if (!c.contains(t)) throw ApiError(QStringLiteral("Time must lie inside the clip (%1 - %2 s)")
                                           .arg(toSec(c.start)).arg(toSec(c.end())));
    QString still;
    m_project->edit(QStringLiteral("Freeze frame"), [&] { still = m_project->freezeFrame(id, t, toUs(std::clamp(hold, 0.04, 600.0))); });
    if (still.isEmpty()) throw ApiError(QStringLiteral("Could not create the freeze frame"));
    return clipGet(still);
}

QJsonObject EditorApi::clipDuplicate(const QString& id, bool linked)
{
    requireClip(id);
    QString copy;
    m_project->edit(QStringLiteral("Duplicate clip"), [&] { copy = m_project->duplicateClip(id, linked); });
    QJsonObject r = clipGet(copy);
    QJsonArray partners;
    for (const QString& p : m_project->linkedClips(copy)) partners.append(clipGet(p));
    r["linked"] = partners;
    return r;
}

QJsonObject EditorApi::clipRippleDelete(const QString& id, bool linked)
{
    requireClip(id);
    const QStringList removed = QStringList{id} + (linked ? m_project->linkedClips(id) : QStringList());
    m_project->edit(QStringLiteral("Ripple delete"), [&] { m_project->rippleDelete(id, linked); });
    return {{"removed", QJsonArray::fromStringList(removed)}, {"duration", toSec(m_project->duration())}};
}

QJsonObject EditorApi::timelineCloseGap(int track, double time)
{
    requireTrack(track);
    TimeUs closed = 0;
    m_project->edit(QStringLiteral("Close gap"), [&] { closed = m_project->closeGap(track, toUs(time)); });
    if (closed <= 0) throw ApiError(QStringLiteral("No gap with clips after it at %1 s on track %2").arg(time).arg(track));
    return {{"closed", toSec(closed)}, {"duration", toSec(m_project->duration())}};
}

QJsonObject EditorApi::timelineRemoveRange(double start, double end, bool closeGap)
{
    if (end <= start) throw ApiError(QStringLiteral("end must be after start"));
    m_project->edit(closeGap ? QStringLiteral("Cut range") : QStringLiteral("Delete range"),
                    [&] { m_project->removeRange(toUs(start), toUs(end), closeGap); });
    return {{"removed", end - start}, {"closedGap", closeGap}, {"duration", toSec(m_project->duration())}};
}

QJsonObject EditorApi::audioSeparationStatus() const
{
    return {{"installed", StemSeparator::isInstalled()}, {"environment", StemSeparator::envDir()},
            {"script", StemSeparator::scriptPath()}, {"uv", StemSeparator::uvPath()},
            {"cache", StemSeparator::cacheDir()}};
}

QString EditorApi::audioClipFor(const QString& clipId) const
{
    int ti = -1;
    requireClip(clipId, &ti);
    if (m_project->tracks()[ti].isAudio()) return clipId;
    for (const QString& p : m_project->linkedClips(clipId)) {
        int pt = -1;
        if (m_project->findClip(p, &pt) && m_project->tracks()[pt].isAudio()) return p;
    }
    throw ApiError(QStringLiteral("This video clip has no audio clip linked to it"));
}

std::pair<QString, QString> EditorApi::stemFiles(const QString& mediaPath, TimeUs mediaDuration, bool* cachedOut,
                                                 QString* device, double* seconds)
{
    const auto progress = [this](const QString& t, double f) { return m_host->reportProgress(t, f); };
    // Stems are cached per source file (path + size + modification time).
    const QFileInfo fi(mediaPath);
    const QByteArray key = QCryptographicHash::hash(
        (fi.absoluteFilePath() + QString::number(fi.size()) + fi.lastModified().toString(Qt::ISODate)).toUtf8(),
        QCryptographicHash::Sha1).toHex().left(16);
    const QString dir = StemSeparator::cacheDir() + QLatin1Char('/') + QString::fromLatin1(key);
    const QString base = fi.completeBaseName();
    const QString voicePath = dir + QLatin1Char('/') + base + QStringLiteral(" - voice.flac");
    const QString musicPath = dir + QLatin1Char('/') + base + QStringLiteral(" - music.flac");
    const bool cached = QFileInfo::exists(voicePath) && QFileInfo::exists(musicPath);
    if (cachedOut) *cachedOut = cached;
    if (!cached) {
        QDir().mkpath(dir);
        QTemporaryDir tmp;
        const QString wav = tmp.filePath(QStringLiteral("input.wav"));
        QString err;
        if (!StemSeparator::decodeToWav(mediaPath, mediaDuration, wav, progress, &err))
            throw ApiError(QStringLiteral("Could not read the audio: %1").arg(err));
        const StemSeparator::Result r = StemSeparator::separate(wav, tmp.path(), [&](const QString& t, double f) {
            return progress(t, 0.1 + 0.9 * std::max(0.0, f));
        });
        if (!r.ok) throw ApiError(r.cancelled ? QStringLiteral("Separation cancelled") : QStringLiteral("Separation failed: %1").arg(r.error));
        QFile::remove(voicePath);
        QFile::remove(musicPath);
        if (!QFile::rename(r.voicePath, voicePath) && !QFile::copy(r.voicePath, voicePath))
            throw ApiError(QStringLiteral("Could not save %1").arg(voicePath));
        if (!QFile::rename(r.musicPath, musicPath) && !QFile::copy(r.musicPath, musicPath))
            throw ApiError(QStringLiteral("Could not save %1").arg(musicPath));
        if (device) *device = r.device;
        if (seconds) *seconds = r.seconds;
    }
    return {voicePath, musicPath};
}

QJsonObject EditorApi::audioSeparate(const QString& clipId, const QString& keep, bool install)
{
    if (keep != QLatin1String("both") && keep != QLatin1String("voice") && keep != QLatin1String("music"))
        throw ApiError(QStringLiteral("keep must be 'both', 'voice' or 'music'"));
    const QString audioId = audioClipFor(clipId);
    const Clip src = *m_project->findClip(audioId);
    const MediaItem* media = m_project->mediaById(src.mediaId);
    if (!media || !media->info.hasAudio) throw ApiError(QStringLiteral("The clip's media has no audio"));
    const QString mediaPath = media->path;
    const TimeUs mediaDuration = media->info.duration;
    const QString originalMedia = src.mediaId;

    struct Done { EditorHost* h; ~Done() { h->progressDone(); } } done{m_host};
    const auto progress = [this](const QString& t, double f) { return m_host->reportProgress(t, f); };

    if (!StemSeparator::isInstalled()) {
        if (!install)
            throw ApiError(QStringLiteral("Voice/music separation is not installed. Call again with install=true to "
                                          "set it up (one-time download: Demucs ~5 MB + model ~80 MB, plus PyTorch "
                                          "if it is not cached, up to ~2.5 GB for the GPU build)."));
        QString err;
        if (!StemSeparator::install(progress, &err)) throw ApiError(QStringLiteral("Setup failed: %1").arg(err));
    }
    bool cached = false;
    QString device;
    double seconds = 0;
    const auto [voicePath, musicPath] = stemFiles(mediaPath, mediaDuration, &cached, &device, &seconds);

    // Media items for the stems (reuse ones already imported).
    const auto mediaFor = [this](const QString& path) {
        for (const MediaItem& m : m_project->media())
            if (QFileInfo(m.path) == QFileInfo(path)) return m.id;
        const QJsonObject imp = mediaImport({path});
        const QJsonArray ok = imp.value("imported").toArray();
        if (ok.isEmpty()) throw ApiError(QStringLiteral("Could not import %1").arg(path));
        return ok.first().toObject().value("id").toString();
    };
    const QString voiceMedia = keep != QLatin1String("music") ? mediaFor(voicePath) : QString();
    const QString musicMedia = keep != QLatin1String("voice") ? mediaFor(musicPath) : QString();

    // Replace the clip's sound with the stems: same timing, speed and volume;
    // linked to the video (or to each other).
    QString voiceClip, musicClip;
    m_project->edit(keep == QLatin1String("both") ? QStringLiteral("Separate voice and music")
                    : keep == QLatin1String("voice") ? QStringLiteral("Remove music")
                                                     : QStringLiteral("Remove voice"),
                    [&] {
        int at = -1;
        const Clip orig = *m_project->findClip(audioId, &at);
        const QString trackId = m_project->tracks()[at].id;
        m_project->removeClip(audioId, false);
        QString link = orig.linkId;
        if (link.isEmpty() && keep == QLatin1String("both")) link = newId();
        const auto place = [&](const QString& mediaId, const QString& suffix, bool sameTrack) {
            Clip c = orig;
            c.id = newId();
            c.mediaId = mediaId;
            c.name = orig.name + suffix;
            c.linkId = link;
            const int track = sameTrack ? m_project->trackIndexById(trackId)
                                        : m_project->findFreeTrack(TrackKind::Audio, c.start, c.end(), true);
            return m_project->addClip(track, c);
        };
        if (!voiceMedia.isEmpty()) voiceClip = place(voiceMedia, QStringLiteral(" (voice)"), true);
        if (!musicMedia.isEmpty()) musicClip = place(musicMedia, QStringLiteral(" (music)"), voiceMedia.isEmpty());
    });

    // The lyrics follow the voice.
    if (const Transcript* t = m_project->transcript(originalMedia); t && !voiceMedia.isEmpty() && !m_project->transcript(voiceMedia)) {
        Transcript copy = *t;
        copy.mediaId = voiceMedia;
        m_project->setTranscript(copy);
    }
    QJsonObject r{{"keep", keep}, {"cached", cached}, {"voiceFile", voicePath}, {"musicFile", musicPath}};
    if (!device.isEmpty()) r["device"] = device;
    if (seconds > 0) r["seconds"] = seconds;
    if (!voiceClip.isEmpty()) r["voice"] = clipGet(voiceClip);
    if (!musicClip.isEmpty()) r["music"] = clipGet(musicClip);
    return r;
}

// ---------------------------------------------------------------------------
// transcripts
// ---------------------------------------------------------------------------
namespace {
QJsonObject lineJson(const TimelineLine& l, int index)
{
    QJsonArray words;
    for (const auto& w : l.words) words.append(QJsonArray{usToSeconds(w.start), usToSeconds(w.end), w.text});
    return {{"index", index}, {"clipId", l.clipId}, {"mediaId", l.mediaId}, {"segment", l.segment},
            {"start", usToSeconds(l.start)}, {"end", usToSeconds(l.end)}, {"text", l.text}, {"words", words}};
}

QString srtTime(TimeUs t, QChar sep)
{
    t = std::max<TimeUs>(0, t);
    const qint64 ms = t / 1000;
    return QStringLiteral("%1:%2:%3%4%5").arg(ms / 3600000, 2, 10, QLatin1Char('0')).arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')).arg(sep).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
} // namespace

QString EditorApi::formatTranscript(const std::vector<TimelineLine>& lines, const QString& format)
{
    QString out;
    if (format == QLatin1String("srt")) {
        for (size_t i = 0; i < lines.size(); ++i)
            out += QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(i + 1).arg(srtTime(lines[i].start, QLatin1Char(',')),
                                                                     srtTime(lines[i].end, QLatin1Char(',')), lines[i].text);
    } else if (format == QLatin1String("vtt")) {
        out = QStringLiteral("WEBVTT\n\n");
        for (const auto& l : lines)
            out += QStringLiteral("%1 --> %2\n%3\n\n").arg(srtTime(l.start, QLatin1Char('.')), srtTime(l.end, QLatin1Char('.')), l.text);
    } else if (format == QLatin1String("lrc")) {  // synced lyrics
        for (const auto& l : lines) {
            const qint64 cs = l.start / 10000;
            out += QStringLiteral("[%1:%2.%3]%4\n").arg(cs / 6000, 2, 10, QLatin1Char('0')).arg(cs / 100 % 60, 2, 10, QLatin1Char('0'))
                       .arg(cs % 100, 2, 10, QLatin1Char('0')).arg(l.text);
        }
    } else {                                      // plain text
        for (const auto& l : lines) out += l.text + QLatin1Char('\n');
    }
    return out;
}

QJsonObject EditorApi::transcriptCreate(const QString& clipId, const QString& language, const QString& isolateVoice,
                                        bool install, const QString& hints)
{
    const QString audioId = audioClipFor(clipId);
    const Clip src = *m_project->findClip(audioId);
    const MediaItem* media = m_project->mediaById(src.mediaId);
    if (!media || !media->info.hasAudio) throw ApiError(QStringLiteral("The clip's media has no audio"));
    const QString mediaId = media->id, mediaPath = media->path;
    const TimeUs duration = media->info.duration;

    struct Done { EditorHost* h; ~Done() { h->progressDone(); } } done{m_host};
    const auto progress = [this](const QString& t, double f) { return m_host->reportProgress(t, f); };
    if (!Transcriber::isInstalled()) {
        if (!install)
            throw ApiError(QStringLiteral("Transcription is not installed. Call again with install=true to set it up "
                                          "(one-time download: Whisper ~5 MB + the turbo model ~1.6 GB)."));
        QString err;
        if (!Transcriber::install(progress, &err)) throw ApiError(QStringLiteral("Setup failed: %1").arg(err));
    }

    // Songs: transcribe the separated voice, without the music in the way.
    const bool isolate = isolateVoice == QLatin1String("yes") ||
                         (isolateVoice != QLatin1String("no") && StemSeparator::isInstalled() &&
                          !media->name.endsWith(QLatin1String(" - voice.flac")));
    QString input;
    QTemporaryDir tmp;
    if (isolate) {
        bool cached = false;
        input = stemFiles(mediaPath, duration, &cached, nullptr, nullptr).first;
    } else {
        input = tmp.filePath(QStringLiteral("input.wav"));
        QString err;
        if (!StemSeparator::decodeToWav(mediaPath, duration, input, progress, &err))
            throw ApiError(QStringLiteral("Could not read the audio: %1").arg(err));
    }
    const QString json = tmp.filePath(QStringLiteral("transcript.json"));
    const Transcriber::Result r = Transcriber::transcribe(input, json, language, progress, hints);
    if (!r.ok) throw ApiError(r.cancelled ? QStringLiteral("Transcription cancelled") : QStringLiteral("Transcription failed: %1").arg(r.error));

    QFile f(json);
    if (!f.open(QIODevice::ReadOnly)) throw ApiError(QStringLiteral("Transcription produced no output"));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    Transcript t;
    t.mediaId = mediaId;
    t.language = o.value("language").toString();
    t.model = o.value("model").toString();
    for (const auto& v : o.value("segments").toArray()) {
        const QJsonObject so = v.toObject();
        TranscriptSegment seg;
        seg.start = toUs(so.value("start").toDouble());
        seg.end = toUs(so.value("end").toDouble());
        seg.text = so.value("text").toString();
        for (const auto& wv : so.value("words").toArray()) {
            const QJsonObject w = wv.toObject();
            seg.words.push_back({toUs(w.value("start").toDouble()), toUs(w.value("end").toDouble()), w.value("word").toString()});
        }
        t.segments.push_back(std::move(seg));
    }
    m_project->setTranscript(t);
    QJsonObject res = transcriptGet(mediaId);
    res["seconds"] = r.seconds;
    res["device"] = r.device;
    res["isolatedVoice"] = isolate;
    return res;
}

QJsonObject EditorApi::transcriptGet(const QString& id) const
{
    QString mediaId = id;
    if (!id.isEmpty() && !m_project->mediaById(id)) {
        if (!m_project->findClip(id)) throw ApiError(QStringLiteral("No clip or media with id '%1'").arg(id));
        const Clip* c = m_project->findClip(audioClipFor(id));
        mediaId = c->mediaId;
    }
    QJsonArray lines;
    const auto all = m_project->transcriptLines();
    for (size_t i = 0; i < all.size(); ++i)
        if (mediaId.isEmpty() || all[i].mediaId == mediaId) lines.append(lineJson(all[i], int(i)));
    QJsonArray transcripts;
    for (const QString& m : m_project->transcribedMedia()) {
        if (!mediaId.isEmpty() && m != mediaId) continue;
        const Transcript* t = m_project->transcript(m);
        QJsonArray segs;
        for (int i = 0; i < int(t->segments.size()); ++i) {
            const auto& s = t->segments[size_t(i)];
            segs.append(QJsonObject{{"segment", i}, {"start", toSec(s.start)}, {"end", toSec(s.end)},
                                    {"text", s.text}, {"edited", s.edited}});
        }
        const MediaItem* mi = m_project->mediaById(m);
        transcripts.append(QJsonObject{{"mediaId", m}, {"media", mi ? mi->name : QString()}, {"language", t->language},
                                       {"model", t->model}, {"segments", segs}});
    }
    if (!mediaId.isEmpty() && transcripts.isEmpty())
        throw ApiError(QStringLiteral("No transcript yet for this media (use transcript_create)"));
    return {{"lines", lines}, {"transcripts", transcripts},
            {"note", "lines: timeline seconds (where it is heard); segments: source seconds of the media"}};
}

QJsonObject EditorApi::transcriptUpdate(const QJsonObject& a)
{
    const QString mediaId = a.value("mediaId").toString();
    if (!m_project->mediaById(mediaId)) throw ApiError(QStringLiteral("No media with id '%1'").arg(mediaId));
    if (a.contains("insert")) {
        const QJsonObject ins = a.value("insert").toObject();
        const double s0 = ins.value("start").toDouble(-1), s1 = ins.value("end").toDouble(-1);
        if (s0 < 0 || s1 <= s0 || ins.value("text").toString().isEmpty())
            throw ApiError(QStringLiteral("insert needs start < end (source seconds) and text"));
        m_project->insertTranscriptSegment(mediaId, toUs(s0), toUs(s1), ins.value("text").toString());
        return transcriptGet(mediaId);
    }
    const Transcript* t = m_project->transcript(mediaId);
    if (!t) throw ApiError(QStringLiteral("No transcript for this media"));
    const int seg = a.value("segment").toInt(-1);
    if (seg < 0 || seg >= int(t->segments.size()))
        throw ApiError(QStringLiteral("segment must be 0..%1").arg(int(t->segments.size()) - 1));
    if (a.value("delete").toBool()) {
        m_project->removeTranscriptSegment(mediaId, seg);
        return transcriptGet(mediaId);
    }
    const auto& cur = t->segments[size_t(seg)];
    const QString text = a.contains("text") ? a.value("text").toString() : cur.text;
    const TimeUs s0 = a.contains("start") ? toUs(a.value("start").toDouble()) : -1;
    const TimeUs s1 = a.contains("end") ? toUs(a.value("end").toDouble()) : -1;
    m_project->updateTranscriptSegment(mediaId, seg, text, s0, s1);
    return transcriptGet(mediaId);
}

QJsonObject EditorApi::transcriptExport(const QString& path, const QString& format) const
{
    QString fmt = format.toLower();
    if (fmt.isEmpty()) fmt = QFileInfo(path).suffix().toLower();
    if (fmt != QLatin1String("srt") && fmt != QLatin1String("vtt") && fmt != QLatin1String("lrc") && fmt != QLatin1String("txt"))
        throw ApiError(QStringLiteral("format must be srt, vtt, lrc or txt"));
    const auto lines = m_project->transcriptLines();
    if (lines.empty()) throw ApiError(QStringLiteral("There is no transcript on the timeline to export"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        throw ApiError(QStringLiteral("Cannot write %1: %2").arg(path, f.errorString()));
    f.write(formatTranscript(lines, fmt).toUtf8());
    return {{"path", QFileInfo(path).absoluteFilePath()}, {"format", fmt}, {"lines", int(lines.size())}};
}

QJsonObject EditorApi::captionsGet() const
{
    const auto chunks = m_project->captionChunks();
    QJsonArray preview;
    for (size_t i = 0; i < std::min<size_t>(chunks.size(), 8); ++i) {
        QStringList w;
        for (const auto& x : chunks[i].words) w << x.text;
        preview.append(QJsonObject{{"start", toSec(chunks[i].start)}, {"end", toSec(chunks[i].end)}, {"text", w.join(' ')}});
    }
    return {{"style", m_project->captionStyle().toJson()}, {"presets", QJsonArray::fromStringList(captionPresetNames())},
            {"suggestedFonts", QJsonArray::fromStringList(CaptionRenderer::suggestedFonts())},
            {"suggestedArabicFonts", QJsonArray::fromStringList(CaptionRenderer::suggestedArabicFonts())},
            {"arabicFontInUse", CaptionRenderer::arabicFamily(m_project->captionStyle())},
            {"captionCount", int(chunks.size())}, {"firstCaptions", preview},
            {"note", "Captions come from the transcripts (transcript_create / transcript_update) and show over the "
                     "video in the preview, render_frame and export."}};
}

QJsonObject EditorApi::captionsSet(const QJsonObject& o)
{
    CaptionStyle s = m_project->captionStyle();
    if (o.contains("preset")) {
        const QString p = o.value("preset").toString();
        if (!captionPresetNames().contains(p))
            throw ApiError(QStringLiteral("Unknown preset '%1' (%2)").arg(p, captionPresetNames().join(", ")));
        s = captionPreset(p, s);
    }
    // Any style field overrides (same names as in captions_get "style").
    QJsonObject merged = s.toJson();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.key() != QLatin1String("preset")) merged[it.key()] = it.value();
    const auto checkColor = [&](const char* k) {
        if (o.contains(QLatin1String(k)) && !QColor(o.value(QLatin1String(k)).toString()).isValid())
            throw ApiError(QStringLiteral("'%1' must be a color like #FFD400").arg(QLatin1String(k)));
    };
    for (const char* k : {"color", "highlight", "outline", "boxColor"}) checkColor(k);
    const auto checkEnum = [&](const char* k, const QStringList& allowed) {
        if (o.contains(QLatin1String(k)) && !allowed.contains(o.value(QLatin1String(k)).toString()))
            throw ApiError(QStringLiteral("'%1' must be one of %2").arg(QLatin1String(k), allowed.join(", ")));
    };
    checkEnum("position", {"bottom", "middle", "top"});
    checkEnum("animation", {"pop", "fade", "none"});
    checkEnum("highlightMode", {"color", "box", "none"});
    s = CaptionStyle::fromJson(merged);
    if (o.contains("fontFile") && !s.fontFile.isEmpty()) {     // a downloaded font, not installed
        const QString family = CaptionRenderer::loadFontFile(s.fontFile);
        if (family.isEmpty()) throw ApiError(QStringLiteral("'%1' is not a font file (.ttf / .otf)").arg(s.fontFile));
        if (!o.contains("font")) s.font = family;
    } else if (o.contains("font")) {
        s.fontFile.clear();
        if (!QFontDatabase::families().contains(s.font, Qt::CaseInsensitive))
            throw ApiError(QStringLiteral("Font '%1' is not installed. Use one of suggestedFonts (captions_get), any "
                                          "installed font, or fontFile for a .ttf / .otf file").arg(s.font));
    }
    if (o.contains("arabicFontFile") && !s.arabicFontFile.isEmpty()) {
        const QString family = CaptionRenderer::loadFontFile(s.arabicFontFile);
        if (family.isEmpty()) throw ApiError(QStringLiteral("'%1' is not a font file (.ttf / .otf)").arg(s.arabicFontFile));
        if (!QFontDatabase::writingSystems(family).contains(QFontDatabase::Arabic))
            throw ApiError(QStringLiteral("'%1' has no Arabic letters").arg(family));
        if (!o.contains("arabicFont")) s.arabicFont = family;
    } else if (o.contains("arabicFont") && !s.arabicFont.isEmpty()) {
        s.arabicFontFile.clear();
        if (!QFontDatabase::families().contains(s.arabicFont, Qt::CaseInsensitive))
            throw ApiError(QStringLiteral("Arabic font '%1' is not installed (see suggestedArabicFonts, or use arabicFontFile)")
                               .arg(s.arabicFont));
        if (!QFontDatabase::writingSystems(s.arabicFont).contains(QFontDatabase::Arabic))
            throw ApiError(QStringLiteral("'%1' has no Arabic letters (see suggestedArabicFonts)").arg(s.arabicFont));
    }
    if (o.contains("preset") && !o.contains("enabled")) s.enabled = true;   // picking a style turns captions on
    m_project->setCaptionStyle(s);
    QJsonObject r = captionsGet();
    if (s.enabled && r.value("captionCount").toInt() == 0)
        r["warning"] = QStringLiteral("Captions are on but there is no transcript on the timeline yet (transcript_create).");
    return r;
}

QJsonObject EditorApi::clipLinkMatching(const QString& id)
{
    requireClip(id);
    if (!m_project->linkedClips(id).isEmpty()) throw ApiError(QStringLiteral("Clip %1 is already linked").arg(id));
    const QString mate = m_project->linkCandidate(id);
    if (mate.isEmpty())
        throw ApiError(QStringLiteral("No unlinked clip of the same media overlaps it on the other kind of track"));
    return clipLink({id, mate});
}

QJsonObject EditorApi::clipUnlink(const QString& id)
{
    requireClip(id);
    const QStringList group = QStringList{id} + m_project->linkedClips(id);
    m_project->edit(QStringLiteral("Unlink clips"), [&] { m_project->unlinkClip(id); });
    return {{"unlinked", QJsonArray::fromStringList(group)}};
}

QJsonObject EditorApi::clipUpdate(const QString& id, const QJsonObject& o)
{
    Clip c = requireClip(id);
    if (o.contains("name")) c.name = o.value("name").toString();
    const QJsonObject t = o.value("transform").toObject();
    c.transform.x = clampNum(t, "x", c.transform.x, -16384, 16384);
    c.transform.y = clampNum(t, "y", c.transform.y, -16384, 16384);
    c.transform.scale = clampNum(t, "scale", c.transform.scale, 0.01, 20);
    c.transform.rotation = clampNum(t, "rotation", c.transform.rotation, -3600, 3600);
    c.transform.opacity = clampNum(t, "opacity", c.transform.opacity, 0, 1);
    c.transform.cropLeft = clampNum(t, "cropLeft", c.transform.cropLeft, 0, 0.95);
    c.transform.cropTop = clampNum(t, "cropTop", c.transform.cropTop, 0, 0.95);
    c.transform.cropRight = clampNum(t, "cropRight", c.transform.cropRight, 0, 0.95);
    c.transform.cropBottom = clampNum(t, "cropBottom", c.transform.cropBottom, 0, 0.95);
    if (c.transform.cropLeft + c.transform.cropRight > 0.95 || c.transform.cropTop + c.transform.cropBottom > 0.95)
        throw ApiError(QStringLiteral("Crop would leave nothing visible"));
    if (o.contains("fit")) {                // "fit" = whole frame visible, "fill" = cover the canvas
        const QString mode = o.value("fit").toString();
        const MediaItem* m = m_project->mediaById(c.mediaId);
        if (mode == QLatin1String("fit")) c.transform.scale = 1.0;
        else if (mode == QLatin1String("fill") && m && m->info.width > 0 && m->info.height > 0) {
            const double cw = m_project->settings().width, ch = m_project->settings().height;
            const double fx = cw / m->info.width, fy = ch / m->info.height;
            c.transform.scale = std::max(fx, fy) / std::min(fx, fy);
        } else throw ApiError(QStringLiteral("fit must be 'fit' or 'fill'"));
        c.transform.x = c.transform.y = 0;
    }
    const QJsonObject e = o.value("effects").toObject();
    c.effects.brightness = clampNum(e, "brightness", c.effects.brightness, -1, 1);
    c.effects.contrast = clampNum(e, "contrast", c.effects.contrast, 0, 5);
    c.effects.saturation = clampNum(e, "saturation", c.effects.saturation, 0, 5);
    c.effects.hue = clampNum(e, "hue", c.effects.hue, -180, 180);
    c.effects.blur = clampNum(e, "blur", c.effects.blur, 0, 50);
    c.effects.vignette = clampNum(e, "vignette", c.effects.vignette, 0, 1);
    c.effects.grayscale = boolVal(e, "grayscale", c.effects.grayscale);
    c.effects.invert = boolVal(e, "invert", c.effects.invert);
    c.volume = clampNum(o, "volume", c.volume, 0, 4);
    c.fadeIn = toUs(clampNum(o, "fadeIn", toSec(c.fadeIn), 0, 3600));
    c.fadeOut = toUs(clampNum(o, "fadeOut", toSec(c.fadeOut), 0, 3600));
    if (o.value("resetEffects").toBool()) c.effects = {};
    if (o.value("resetTransform").toBool()) c.transform = {};
    m_project->edit(QStringLiteral("Change clip properties"), [&] { m_project->updateClip(c); });
    return clipGet(id);
}

// ---------------------------------------------------------------------------
// history
// ---------------------------------------------------------------------------
QJsonObject EditorApi::undo()
{
    QUndoStack* s = m_project->undoStack();
    if (!s->canUndo()) throw ApiError(QStringLiteral("Nothing to undo"));
    const QString text = s->undoText();
    s->undo();
    return {{"undone", text}, {"history", history()}};
}

QJsonObject EditorApi::redo()
{
    QUndoStack* s = m_project->undoStack();
    if (!s->canRedo()) throw ApiError(QStringLiteral("Nothing to redo"));
    const QString text = s->redoText();
    s->redo();
    return {{"redone", text}, {"history", history()}};
}

QJsonObject EditorApi::history() const
{
    const QUndoStack* s = m_project->undoStack();
    QJsonArray entries;
    for (int i = 0; i < s->count(); ++i)
        entries.append(QJsonObject{{"index", i + 1}, {"text", s->text(i)}, {"applied", i < s->index()}});
    return {{"index", s->index()}, {"count", s->count()}, {"canUndo", s->canUndo()}, {"canRedo", s->canRedo()},
            {"entries", entries}};
}

// ---------------------------------------------------------------------------
// playback / selection
// ---------------------------------------------------------------------------
QJsonObject EditorApi::playbackState() const
{
    const double fps = m_project->settings().fps;
    return {{"position", toSec(m_host->playhead())}, {"timecode", formatTimecode(m_host->playhead(), fps)},
            {"duration", toSec(m_project->duration())}, {"playing", m_host->isPlaying()}, {"fps", fps}};
}

QJsonObject EditorApi::playbackSeek(double time)
{
    m_host->seek(std::max<TimeUs>(0, toUs(time)));
    return playbackState();
}

QJsonObject EditorApi::playbackPlay()
{
    if (m_host->hostKind() != QLatin1String("gui")) throw ApiError(QStringLiteral("Playback needs the GUI app"));
    m_host->setPlaying(true);
    return playbackState();
}

QJsonObject EditorApi::playbackPause()
{
    m_host->setPlaying(false);
    return playbackState();
}

QJsonObject EditorApi::selectionGet() const
{
    const QString id = m_host->selectedClip();
    if (id.isEmpty() || !m_project->findClip(id)) return {{"selected", QJsonValue::Null}};
    return {{"selected", clipGet(id)}};
}

QJsonObject EditorApi::selectionSet(const QString& clipId)
{
    if (!clipId.isEmpty()) requireClip(clipId);
    m_host->selectClip(clipId);
    return selectionGet();
}

// ---------------------------------------------------------------------------
// rendering / export
// ---------------------------------------------------------------------------
QJsonObject EditorApi::renderFrame(double time, const QString& path, int maxWidth, QByteArray* pngOut)
{
    const TimelineSnapshot snap = m_project->snapshot();
    const TimeUs t = time < 0 ? m_host->playhead() : toUs(time);
    FrameSource source;
    const CompositeFramePtr frame = source.build(snap, t);

    OffscreenRenderer renderer;
    QString err;
    if (!renderer.initialize(snap.settings.width, snap.settings.height, &err)) throw ApiError(err);
    const QImage img = renderer.render(*frame);
    if (img.isNull()) throw ApiError(QStringLiteral("Rendering failed"));

    QJsonObject r{{"time", toSec(t)}, {"width", img.width()}, {"height", img.height()},
                  {"layers", int(frame->layers.size())}};
    if (!path.isEmpty()) {
        const QString abs = QFileInfo(path).absoluteFilePath();
        if (!img.save(abs)) throw ApiError(QStringLiteral("Could not write %1").arg(abs));
        r["path"] = abs;
    }
    if (pngOut) {
        const QImage small = (maxWidth > 0 && img.width() > maxWidth)
            ? img.scaledToWidth(maxWidth, Qt::SmoothTransformation) : img;
        QBuffer buf(pngOut);
        buf.open(QIODevice::WriteOnly);
        small.convertToFormat(QImage::Format_RGB888).save(&buf, "PNG");
    }
    return r;
}

QJsonObject EditorApi::exportVideo(const QJsonObject& o)
{
    const ProjectSettings& ps = m_project->settings();
    if (m_project->duration() <= 0) throw ApiError(QStringLiteral("The timeline is empty"));
    ExportSettings s;
    s.path = o.value("path").toString();
    if (s.path.isEmpty()) throw ApiError(QStringLiteral("path is required"));
    s.path = QFileInfo(s.path).absoluteFilePath();
    s.width = int(clampNum(o, "width", ps.width, 16, 8192));
    s.height = int(clampNum(o, "height", ps.height, 16, 8192));
    s.fps = clampNum(o, "fps", ps.fps, 1, 240);
    s.crf = int(clampNum(o, "quality", 20, 0, 51));
    s.includeAudio = boolVal(o, "includeAudio", true);
    s.start = toUs(clampNum(o, "start", 0, 0, 1e7));
    s.end = toUs(clampNum(o, "end", 0, 0, 1e7));

    const auto encoders = hw::availableEncoders();
    s.videoEncoder = o.value("encoder").toString();
    if (s.videoEncoder.isEmpty() || s.videoEncoder == QLatin1String("auto")) {
        // Default: GPU H.264 if the machine has one, else x264.
        s.videoEncoder.clear();
        for (const auto& e : encoders)
            if (e.hardware && e.name.startsWith(QLatin1String("h264_"))) { s.videoEncoder = e.name; break; }
        if (s.videoEncoder.isEmpty()) s.videoEncoder = QStringLiteral("libx264");
    } else {
        const bool known = std::any_of(encoders.begin(), encoders.end(), [&](const auto& e) { return e.name == s.videoEncoder; });
        if (!known) throw ApiError(QStringLiteral("Encoder '%1' is not available here; see export_encoders").arg(s.videoEncoder));
    }

    m_host->setPlaying(false);
    QElapsedTimer timer;
    timer.start();
    QString err;
    Exporter exporter;
    const bool ok = exporter.run(m_project->snapshot(), s, [](double) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        return true;
    }, &err);
    if (!ok) throw ApiError(QStringLiteral("Export failed: %1").arg(err));
    return {{"path", s.path}, {"encoder", s.videoEncoder}, {"gpuEncoder", hw::isHardwareEncoder(s.videoEncoder)},
            {"width", s.width & ~1}, {"height", s.height & ~1}, {"fps", s.fps},
            {"bytes", double(QFileInfo(s.path).size())}, {"elapsedSeconds", timer.elapsed() / 1000.0}};
}

QJsonObject EditorApi::exportEncoders() const
{
    QJsonArray arr;
    for (const auto& e : hw::availableEncoders())
        arr.append(QJsonObject{{"name", e.name}, {"label", e.label}, {"gpu", e.hardware}});
    return {{"encoders", arr}};
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------
QJsonObject EditorApi::gpuInfo() const
{
    QJsonObject o = collectGpuInfo();
    o["previewRenderer"] = m_host->previewRenderer();
    return o;
}

QJsonObject EditorApi::gpuSetDecodeMode(const QString& mode)
{
    const QStringList available = hw::availableDecodeDevices();
    if (mode != QLatin1String("auto") && mode != QLatin1String("none") && !available.contains(mode))
        throw ApiError(QStringLiteral("Unknown decode mode '%1'. Use auto, none, or one of: %2")
                           .arg(mode, available.join(QStringLiteral(", "))));
    hw::setDecodeMode(mode);
    QSettings().setValue(QStringLiteral("gpu/decode"), mode);
    m_host->decodeSettingsChanged();
    return gpuInfo();
}

QJsonObject EditorApi::previewSetRenderer(const QString& name)
{
    QString err;
    if (!m_host->setPreviewRenderer(name, &err)) throw ApiError(err);
    return {{"previewRenderer", m_host->previewRenderer()}};
}

} // namespace ve
