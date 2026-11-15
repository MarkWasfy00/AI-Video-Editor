#pragma once

#include "core/Time.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QUndoStack;

namespace ve {

// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------
struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    bool isImage = false;          // still image -> infinite source, default clip length
    int width = 0;
    int height = 0;
    double fps = 0.0;
    TimeUs duration = 0;           // 0 for still images
    int sampleRate = 0;
    int channels = 0;
    QString videoCodec;
    QString audioCodec;
    QString container;

    QJsonObject toJson() const;
    static MediaInfo fromJson(const QJsonObject& o);
};

struct MediaItem {
    QString id;
    QString path;
    QString name;
    MediaInfo info;
    QImage thumbnail;              // runtime only, not serialized
    // Runtime only: audio peak (0..1) per 10 ms of source, for timeline waveforms.
    std::shared_ptr<const std::vector<float>> waveform;
};

constexpr int kWaveformBucketsPerSecond = 100;

// ---------------------------------------------------------------------------
// Clips / tracks
// ---------------------------------------------------------------------------
struct ClipTransform {
    double x = 0.0;                // offset from canvas centre, in canvas pixels
    double y = 0.0;
    double scale = 1.0;            // 1.0 = fit into canvas
    double rotation = 0.0;         // degrees, clockwise
    double opacity = 1.0;
    // Crop: fraction of the source removed from each edge (0 .. 0.95).
    double cropLeft = 0.0;
    double cropTop = 0.0;
    double cropRight = 0.0;
    double cropBottom = 0.0;

    QJsonObject toJson() const;
    static ClipTransform fromJson(const QJsonObject& o);
    bool operator==(const ClipTransform&) const = default;
};

struct ClipEffects {
    double brightness = 0.0;       // -1 .. 1
    double contrast = 1.0;         //  0 .. 3
    double saturation = 1.0;       //  0 .. 3
    double hue = 0.0;              // degrees -180 .. 180
    double blur = 0.0;             //  0 .. 10 (texel radius multiplier)
    double vignette = 0.0;         //  0 .. 1
    bool grayscale = false;
    bool invert = false;

    QJsonObject toJson() const;
    static ClipEffects fromJson(const QJsonObject& o);
    bool operator==(const ClipEffects&) const = default;
};

// One point of a speed curve: at `pos` (0 = clip start, 1 = clip end) the
// clip plays at `speed` x its base speed. Speed varies linearly in between.
struct SpeedPoint {
    double pos = 0.0;
    double speed = 1.0;
    bool operator==(const SpeedPoint&) const = default;
};

// Named speed-curve shapes ("ease-in", "ease-out", "ease-in-out", "bullet", "hero").
QStringList speedCurvePresetNames();
std::vector<SpeedPoint> speedCurvePreset(const QString& name);   // empty for unknown / "none"

struct Clip {
    QString id;
    QString mediaId;
    QString name;
    TimeUs start = 0;              // position on the timeline
    TimeUs in = 0;                 // offset into the source media
    TimeUs duration = 0;           // length on the timeline
    ClipTransform transform;
    ClipEffects effects;
    double volume = 1.0;
    TimeUs fadeIn = 0;
    TimeUs fadeOut = 0;
    // Clips sharing a non-empty linkId (a video clip and its audio) move, trim,
    // split and delete together.
    QString linkId;

    // Time remapping. The clip shows `sourceSpan()` of source starting at `in`.
    double speed = 1.0;                    // 0.05 .. 16
    std::vector<SpeedPoint> speedCurve;    // optional ramp (multiplies `speed`)
    QString curvePreset;                   // name of the preset the curve came from
    bool reverse = false;                  // play the source span backwards
    bool freeze = false;                   // hold the frame at `in` (no sound)

    TimeUs end() const { return start + duration; }
    bool plainTiming() const { return speed == 1.0 && speedCurve.empty() && !reverse && !freeze; }
    double curveAt(double u) const;        // curve multiplier at u in 0..1 (1 without a curve)
    double meanRate() const;               // average source seconds per timeline second
    // Source microseconds consumed after `local` microseconds of the clip
    // (local may lie outside 0..duration: extrapolated at the edge speed).
    TimeUs sourceOffset(TimeUs local) const;
    TimeUs sourceSpan() const { return sourceOffset(duration); }
    TimeUs sourceTimeAt(TimeUs timelineT) const;   // source frame shown at timeline time t
    // Timeline time at which source time `src` is played (-1 if the clip does not play it).
    TimeUs timelineTimeOf(TimeUs src) const;
    bool contains(TimeUs t) const { return t >= start && t < end(); }

    // Opacity/volume envelope from fades at timeline time t (0..1).
    double fadeFactor(TimeUs t) const;

    QJsonObject toJson() const;
    static Clip fromJson(const QJsonObject& o);
};

// Mixer fader range. kMinGainDb means silence (-inf).
constexpr double kMinGainDb = -60.0;
constexpr double kMaxGainDb = 12.0;
double dbToGain(double db);                      // 0 at/below kMinGainDb
double gainToDb(double gain);                    // clamps to kMinGainDb

// Video tracks only show pictures; audio tracks only play sound. Video tracks
// come first in Project::tracks() (top of the composite), audio tracks after.
enum class TrackKind { Video, Audio };

struct Track {
    QString id;
    QString name;
    TrackKind kind = TrackKind::Video;
    bool muted = false;
    bool hidden = false;
    // Audio mixer channel strip
    bool solo = false;
    double gainDb = 0.0;           // kMinGainDb .. kMaxGainDb
    double pan = 0.0;              // -1 (left) .. 1 (right), balance law
    std::vector<Clip> clips;       // sorted by start, non-overlapping

    bool isAudio() const { return kind == TrackKind::Audio; }
    QJsonObject toJson() const;
    static Track fromJson(const QJsonObject& o);
};

// Master bus of the audio mixer (applied after all tracks are summed).
struct MasterBus {
    double gainDb = 0.0;
    bool muted = false;

    QJsonObject toJson() const;
    static MasterBus fromJson(const QJsonObject& o);
    bool operator==(const MasterBus&) const = default;
};

// ---------------------------------------------------------------------------
// Transcripts (speech / lyrics), stored per media item in SOURCE time
// ---------------------------------------------------------------------------
struct TranscriptWord {
    TimeUs start = 0;
    TimeUs end = 0;
    QString text;
};

struct TranscriptSegment {
    TimeUs start = 0;
    TimeUs end = 0;
    QString text;
    std::vector<TranscriptWord> words;   // cleared when the text is edited
    bool edited = false;
};

struct Transcript {
    QString mediaId;
    QString language;
    QString model;
    std::vector<TranscriptSegment> segments;   // sorted by start

    QJsonObject toJson() const;
    static Transcript fromJson(const QJsonObject& o);
};

// One transcript line placed on the timeline (through an audio clip).
struct TimelineLine {
    QString clipId;
    QString mediaId;
    int segment = -1;
    TimeUs start = 0;              // timeline time
    TimeUs end = 0;
    QString text;
    std::vector<TranscriptWord> words; // timeline times
};

// ---------------------------------------------------------------------------
// On-screen captions (CapCut-style), generated from the transcripts
// ---------------------------------------------------------------------------
struct CaptionStyle {
    bool enabled = false;
    QString preset = QStringLiteral("classic");
    QString font = QStringLiteral("Segoe UI");
    QString fontFile;                       // optional .ttf / .otf that provides `font` (not installed)
    QString arabicFont;                     // font for Arabic words ("" = automatic)
    QString arabicFontFile;                 // optional .ttf / .otf for it
    bool bold = true;
    bool italic = false;
    bool uppercase = false;
    double size = 0.065;                    // text height, fraction of the canvas height
    QColor color = Qt::white;
    QColor highlight = QColor(255, 212, 0); // the word being spoken
    QString highlightMode = QStringLiteral("color"); // color | box | none
    QColor outline = Qt::black;
    double outlineWidth = 0.14;             // fraction of the font size (0 = none)
    bool shadow = true;
    bool box = false;                       // background box behind the caption
    QColor boxColor = Qt::black;
    double boxOpacity = 0.55;
    QString position = QStringLiteral("bottom"); // bottom | middle | top
    double offset = 0.10;                   // distance from the edge, fraction of the canvas height
    int wordsPerCaption = 4;                // 0 = the whole line
    int maxLines = 2;
    QString animation = QStringLiteral("pop"); // pop | fade | none

    QJsonObject toJson() const;
    static CaptionStyle fromJson(const QJsonObject& o);
    bool operator==(const CaptionStyle&) const = default;
};

QStringList captionPresetNames();           // classic, bold, box, neon, minimal, karaoke, arabic
CaptionStyle captionPreset(const QString& name, const CaptionStyle& base = {});

// A caption on screen: a few words with their timeline times.
struct CaptionChunk {
    TimeUs start = 0;
    TimeUs end = 0;
    std::vector<TranscriptWord> words;     // timeline times
};

struct ProjectSettings {
    QString name = QStringLiteral("Untitled");
    int width = 1920;
    int height = 1080;
    double fps = 30.0;
    int sampleRate = 48000;
    QColor background = Qt::black;

    QJsonObject toJson() const;
    static ProjectSettings fromJson(const QJsonObject& o);
};

// Immutable copy of everything needed to render/mix a frame on another thread.
struct TimelineSnapshot {
    ProjectSettings settings;
    std::vector<Track> tracks;
    QHash<QString, MediaItem> media;
    MasterBus master;
    CaptionStyle captions;
    std::vector<CaptionChunk> captionChunks; // empty unless captions are enabled

    TimeUs duration() const;
};

// ---------------------------------------------------------------------------
// Project: owns media + timeline, emits change signals, supports undo.
// Mutators do NOT record undo by themselves; wrap them in edit().
// ---------------------------------------------------------------------------
class Project : public QObject {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    // --- settings
    const ProjectSettings& settings() const { return m_settings; }
    void setSettings(const ProjectSettings& s);

    // --- media
    const std::vector<MediaItem>& media() const { return m_media; }
    const MediaItem* mediaById(const QString& id) const;
    QString addMedia(MediaItem item);
    void setMediaThumbnail(const QString& id, const QImage& img);
    void setMediaWaveform(const QString& id, std::shared_ptr<const std::vector<float>> peaks);
    bool isMediaUsed(const QString& id) const;
    bool removeMedia(const QString& id);

    // --- tracks
    const std::vector<Track>& tracks() const { return m_tracks; }
    int trackCount() const { return static_cast<int>(m_tracks.size()); }
    // New video tracks go on top, new audio tracks at the bottom. Returns the index.
    int addTrack(const QString& name = {}, TrackKind kind = TrackKind::Video);
    int firstAudioTrack() const;       // == trackCount() when there is none
    int trackIndexById(const QString& id) const;
    // A track of `kind` with nothing in [start, end): audio searches A1 down,
    // video searches the bottom video track up. Creates one if needed and allowed.
    int findFreeTrack(TrackKind kind, TimeUs start, TimeUs end, bool create);
    bool isTrackFree(int track, TimeUs start, TimeUs end, const QStringList& ignore = {}) const;
    void removeTrack(int index);
    void setTrackMuted(int index, bool muted);
    void setTrackHidden(int index, bool hidden);
    void renameTrack(int index, const QString& name);
    void setTrackSolo(int index, bool solo);
    void setTrackGain(int index, double gainDb);
    void setTrackPan(int index, double pan);

    // --- audio mixer master bus (part of the undo state)
    const MasterBus& master() const { return m_master; }
    void setMaster(const MasterBus& m);

    // --- clips
    const Clip* findClip(const QString& id, int* trackIndex = nullptr) const;
    QString addClip(int trackIndex, Clip clip);           // overwrites what it covers
    // Adds media at `start`: the picture on a video track and the sound, as a
    // linked clip, on an audio track. `trackIndex` may be either kind; the other
    // part goes to a free track of its kind. Returns the video clip id (or the
    // audio clip id for audio-only media).
    // link = false keeps the sound as an independent (unlinked) clip.
    QString addClipFromMedia(const QString& mediaId, int trackIndex, TimeUs start, bool link = true);
    QString addMediaClips(const QString& mediaId, int videoTrack, int audioTrack, TimeUs start,
                          QString* audioClipId = nullptr, bool link = true);
    QStringList linkedClips(const QString& id) const;     // partners, without id itself
    // An unlinked clip of the same media on the other kind of track that
    // overlaps `id` (in sync if possible): what "Link Audio and Video" pairs it with.
    QString linkCandidate(const QString& id) const;
    // Editing operations act on the whole linked group unless linked == false.
    void removeClip(const QString& id, bool linked = true);
    bool moveClip(const QString& id, int newTrack, TimeUs newStart, bool linked = true);
    bool trimClip(const QString& id, TimeUs newStart, TimeUs newIn, TimeUs newDuration, bool linked = true);
    QString splitClip(const QString& id, TimeUs at, bool linked = true);
    bool linkClips(const QStringList& ids);
    void unlinkClip(const QString& id);

    // Moves clip edges on the timeline; the source in-point follows the clip's
    // speed / reverse mapping. Linked clips get the same edge changes.
    bool trimEdges(const QString& id, TimeUs newStart, TimeUs newEnd, bool linked = true);
    // Speed (and optional curve) for the clip and its linked clips. The clip
    // keeps showing the same source; its length changes and later clips on
    // those tracks shift so nothing is overwritten or left with a gap.
    bool setClipSpeed(const QString& id, double speed, const std::vector<SpeedPoint>& curve,
                      const QString& curvePreset, bool linked = true);
    bool setClipReverse(const QString& id, bool reverse, bool linked = true);
    // Splits the clip at `at`, inserts a still of that frame for `hold` and
    // pushes the rest (and its linked clips) later. Returns the still's id.
    QString freezeFrame(const QString& id, TimeUs at, TimeUs hold);
    // Copies the clip (and its linked clips) right after the group. Returns the copy's id.
    QString duplicateClip(const QString& id, bool linked = true);
    // Deletes the clip(s) and closes the gap they leave on their tracks.
    bool rippleDelete(const QString& id, bool linked = true);
    // Closes the empty space at `at` on a track (later clips + linked clips move left).
    TimeUs closeGap(int track, TimeUs at);
    // Removes [a, b) on every track; closeGap = shift everything after b left.
    bool removeRange(TimeUs a, TimeUs b, bool closeGap);
    bool updateClip(const Clip& clip);                    // properties only (not position)
    TimeUs maxClipDuration(const Clip& clip) const;       // limited by source length

    TimeUs duration() const;
    TimelineSnapshot snapshot() const;

    // --- undo
    QUndoStack* undoStack() const { return m_undo; }
    QJsonArray captureState() const;
    void restoreState(const QJsonArray& tracks);
    void commitUndo(const QString& text, const QJsonArray& before, const QString& mergeKey = {});
    void edit(const QString& text, const std::function<void()>& fn, const QString& mergeKey = {});

    // --- transcripts (not part of the undo history)
    const Transcript* transcript(const QString& mediaId) const;
    void setTranscript(const Transcript& t);
    void removeTranscript(const QString& mediaId);
    bool updateTranscriptSegment(const QString& mediaId, int index, const QString& text, TimeUs start = -1, TimeUs end = -1);
    bool removeTranscriptSegment(const QString& mediaId, int index);
    int insertTranscriptSegment(const QString& mediaId, TimeUs start, TimeUs end, const QString& text);
    QStringList transcribedMedia() const { return m_transcripts.keys(); }
    // Every transcript line heard on the timeline (audio clips), in timeline order.
    std::vector<TimelineLine> transcriptLines() const;

    // --- on-screen captions (not part of the undo history)
    const CaptionStyle& captionStyle() const { return m_captions; }
    void setCaptionStyle(const CaptionStyle& s);
    // Transcript lines cut into on-screen captions (style.wordsPerCaption words each).
    std::vector<CaptionChunk> captionChunks() const;

    // --- (de)serialization
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& root, QString* error = nullptr);
    void clear();

    QString filePath() const { return m_filePath; }
    void setFilePath(const QString& p) { m_filePath = p; }
    bool isModified() const { return m_modified; }
    void setModified(bool m);

signals:
    void changed();             // timeline content changed
    void mediaChanged();
    void settingsChanged();
    void modifiedChanged(bool);
    void transcriptsChanged();
    void captionsChanged();

private:
    Clip* findClipMutable(const QString& id, int* trackIndex = nullptr);
    void sortTrack(Track& t);
    void resolveOverlaps(Track& t, const QString& keepId);
    void notifyChanged();
    QJsonArray tracksJson() const;
    bool moveSingle(const QString& id, int newTrack, TimeUs newStart);
    bool trimSingle(const QString& id, TimeUs newStart, TimeUs newIn, TimeUs newDuration);
    QString splitSingle(const QString& id, TimeUs at);
    bool trimEdgesSingle(const QString& id, TimeUs newStart, TimeUs newEnd);
    // Shifts every clip that starts at/after `from` on the given tracks, plus
    // the clips linked to them, by `delta`. A negative delta is limited so
    // nothing overlaps. Returns the delta applied.
    TimeUs rippleShift(const QStringList& trackIds, TimeUs from, TimeUs delta);
    void migrateLegacyTracks();

    ProjectSettings m_settings;
    std::vector<MediaItem> m_media;
    std::vector<Track> m_tracks;
    MasterBus m_master;
    QHash<QString, Transcript> m_transcripts;   // by media id
    CaptionStyle m_captions;
    QUndoStack* m_undo = nullptr;
    QString m_filePath;
    bool m_modified = false;
};

QString newId();

} // namespace ve
