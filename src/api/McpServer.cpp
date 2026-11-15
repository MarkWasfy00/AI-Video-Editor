#include "api/McpServer.h"

#include "api/EditorApi.h"

#include <QCoreApplication>
#include <QJsonDocument>

namespace ve {

namespace {

// ---- JSON-Schema builders ---------------------------------------------------
QJsonObject str(const QString& d) { return {{"type", "string"}, {"description", d}}; }
QJsonObject num(const QString& d) { return {{"type", "number"}, {"description", d}}; }
QJsonObject integer(const QString& d) { return {{"type", "integer"}, {"description", d}}; }
QJsonObject boolean(const QString& d) { return {{"type", "boolean"}, {"description", d}}; }
QJsonObject strArray(const QString& d) { return {{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", d}}; }
QJsonObject enumStr(const QStringList& values, const QString& d)
{
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}, {"description", d}};
}

QJsonObject object(const QJsonObject& props, const QStringList& required = {}, const QString& d = {})
{
    QJsonObject o{{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
    if (!required.isEmpty()) o["required"] = QJsonArray::fromStringList(required);
    if (!d.isEmpty()) o["description"] = d;
    return o;
}

const QJsonObject kTransformSchema = object({
    {"x", num("Horizontal offset from canvas centre, in canvas pixels")},
    {"y", num("Vertical offset from canvas centre, in canvas pixels (positive = down)")},
    {"scale", num("1.0 = fit into the canvas (0.01 - 20)")},
    {"rotation", num("Degrees clockwise")},
    {"opacity", num("0 - 1")},
    {"cropLeft", num("Crop: fraction of the picture removed from the left (0 - 0.95)")},
    {"cropTop", num("Crop from the top (0 - 0.95)")},
    {"cropRight", num("Crop from the right (0 - 0.95)")},
    {"cropBottom", num("Crop from the bottom (0 - 0.95)")},
}, {}, "Partial transform; omitted fields keep their value. rotation in degrees rotates the clip.");

const QJsonObject kEffectsSchema = object({
    {"brightness", num("-1 .. 1 (0 = unchanged)")},
    {"contrast", num("0 .. 5 (1 = unchanged)")},
    {"saturation", num("0 .. 5 (1 = unchanged, 0 = grey)")},
    {"hue", num("Hue rotation in degrees, -180 .. 180")},
    {"blur", num("Gaussian blur radius multiplier, 0 .. 50")},
    {"vignette", num("0 .. 1")},
    {"grayscale", boolean("Black & white")},
    {"invert", boolean("Invert colors")},
}, {}, "Partial GPU effect settings; omitted fields keep their value");

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QString argStr(const QJsonObject& a, const char* k, const QString& def = {})
{
    const QJsonValue v = a.value(QLatin1String(k));
    if (v.isUndefined() || v.isNull()) return def;
    if (!v.isString()) throw ApiError(QStringLiteral("'%1' must be a string").arg(QLatin1String(k)));
    return v.toString();
}

double argNum(const QJsonObject& a, const char* k, double def)
{
    const QJsonValue v = a.value(QLatin1String(k));
    if (v.isUndefined() || v.isNull()) return def;
    if (!v.isDouble()) throw ApiError(QStringLiteral("'%1' must be a number").arg(QLatin1String(k)));
    return v.toDouble();
}

QString requireStr(const QJsonObject& a, const char* k)
{
    const QString s = argStr(a, k);
    if (s.isEmpty()) throw ApiError(QStringLiteral("'%1' is required").arg(QLatin1String(k)));
    return s;
}

bool unsavedGuard(EditorApi* api, const QJsonObject& a)
{
    if (api->project()->isModified() && !a.value("discardChanges").toBool())
        throw ApiError(QStringLiteral("The current project has unsaved changes. Call project_save first, "
                                      "or pass discardChanges=true to throw them away."));
    return true;
}

} // namespace

McpServer::McpServer(EditorApi* api, QObject* parent)
    : QObject(parent)
    , m_api(api)
{
    registerTools();
}

// ---------------------------------------------------------------------------
// Tool registry: one tool per EditorApi operation
// ---------------------------------------------------------------------------
void McpServer::registerTools()
{
    EditorApi* api = m_api;
    const QString timeNote = QStringLiteral("All times are in seconds.");

    // ---- project
    add({"project_get", "Get project",
         "Returns the whole project: settings, media library, tracks (index 0 = top layer) and every clip with its "
         "timing, transform, effects and audio settings. Call this first to understand the timeline. " + timeNote,
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->projectJson(); }});
    add({"project_new", "New project", "Starts a new empty project (tracks V2, V1, A1).",
         object({{"discardChanges", boolean("Allow discarding unsaved changes")}}), false, true, false,
         [api](const QJsonObject& a, QJsonArray&) { unsavedGuard(api, a); return api->projectNew(); }});
    add({"project_open", "Open project", "Opens a .veproj (SQLite) or .json project file.",
         object({{"path", str("Project file path")}, {"discardChanges", boolean("Allow discarding unsaved changes")}}, {"path"}),
         false, true, false,
         [api](const QJsonObject& a, QJsonArray&) { unsavedGuard(api, a); return api->projectOpen(requireStr(a, "path")); }});
    add({"project_save", "Save project",
         "Saves the project. '.veproj' = SQLite database, '.json' = JSON. Without path, saves to the current file.",
         object({{"path", str("Optional destination (.veproj or .json)")}}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->projectSave(argStr(a, "path")); }});
    add({"project_set_settings", "Project settings",
         "Changes the canvas size / aspect ratio (resize the video), frame rate, background color or name.",
         object({{"name", str("Project name")}, {"width", integer("Canvas width in pixels")},
                 {"height", integer("Canvas height in pixels")},
                 {"aspectRatio", str("Resize the canvas to an aspect ratio, e.g. 16:9, 9:16, 1:1, 4:5, 4:3, 21:9")},
                 {"shortSide", integer("Short side in pixels for aspectRatio (default 1080)")},
                 {"fps", num("Frames per second")},
                 {"background", str("Background color, e.g. #000000")}}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->projectSetSettings(a); }});

    // ---- media
    add({"media_import", "Import media", "Imports video, audio or image files into the media library. Returns media ids.",
         object({{"paths", strArray("Absolute file paths")}}, {"paths"}), false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             QStringList paths;
             for (const auto& v : a.value("paths").toArray()) paths << v.toString();
             return api->mediaImport(paths);
         }});
    add({"media_list", "List media", "Lists the media library (id, path, streams, duration, whether used).",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->mediaList(); }});
    add({"media_remove", "Remove media", "Removes an unused media item from the library.",
         object({{"mediaId", str("Media id")}}, {"mediaId"}), false, true, false,
         [api](const QJsonObject& a, QJsonArray&) { return api->mediaRemove(requireStr(a, "mediaId")); }});
    add({"media_probe", "Probe file", "Reads stream information of a media file without importing it.",
         object({{"path", str("File path")}}, {"path"}), true, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->mediaProbe(requireStr(a, "path")); }});

    // ---- tracks
    add({"track_list", "List tracks",
         "Lists tracks with their kind. Video tracks come first (index 0 is the top layer of the picture) and are "
         "silent; audio tracks follow and hold the sound.",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->trackList(); }});
    add({"track_add", "Add track",
         "Adds a video track (on top of the video tracks, index 0) or an audio track (after the last audio track).",
         object({{"name", str("Optional track name")}, {"kind", enumStr({"video", "audio"}, "Track kind (default video)")}}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) { return api->trackAdd(argStr(a, "name"), argStr(a, "kind", "video")); }});
    add({"track_remove", "Remove track", "Removes a track and all its clips.",
         object({{"track", integer("Track index")}}, {"track"}), false, true, false,
         [api](const QJsonObject& a, QJsonArray&) { return api->trackRemove(int(argNum(a, "track", -1))); }});
    add({"track_update", "Update track",
         "Renames a track, toggles mute (audio) / hidden (video), or sets its mixer gain, pan and solo.",
         object({{"track", integer("Track index")}, {"name", str("New name")}, {"muted", boolean("Mute audio")},
                 {"hidden", boolean("Hide video")}, {"solo", boolean("Solo the track's audio")},
                 {"gainDb", num("Track fader in dB, -60 (silence) .. +12")},
                 {"pan", num("-1 (left) .. 0 (centre) .. 1 (right)")}}, {"track"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->trackUpdate(int(argNum(a, "track", -1)), a); }});

    // ---- audio mixer
    add({"mixer_get", "Get audio mixer",
         "Returns the audio mixer: every track's fader (gainDb), pan, mute, solo and whether it is audible, plus the "
         "master bus. While the GUI is playing it also includes live peak/RMS meter levels.",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->mixerGet(); }});
    add({"mixer_set_track", "Set track mixer",
         "Sets a track's channel strip. Omitted fields keep their value. Solo on any track silences all non-solo "
         "tracks. Clip-level volume and fades are set with clip_update.",
         object({{"track", integer("Track index")},
                 {"gainDb", num("Fader in dB, -60 (silence) .. +12; 0 = unity")},
                 {"pan", num("-1 (left) .. 0 (centre) .. 1 (right)")},
                 {"muted", boolean("Mute")}, {"solo", boolean("Solo")}}, {"track"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             QJsonObject c = a;
             c.remove("track");
             return api->mixerSetTrack(int(argNum(a, "track", -1)), c);
         }});
    add({"mixer_set_master", "Set master bus", "Sets the master fader (dB) and/or mutes the whole mix.",
         object({{"gainDb", num("Master fader in dB, -60 .. +12; 0 = unity")}, {"muted", boolean("Mute everything")}}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->mixerSetMaster(a); }});
    add({"mixer_analyze", "Analyze audio levels",
         "Renders the mix offline and measures peak and RMS levels (dBFS) per track and on the master bus, counts "
         "clipped samples and suggests a gain change. Use it to balance and normalize audio, since you cannot listen. "
         + timeNote,
         object({{"start", num("Range start (default 0)")}, {"end", num("Range end (default: end of timeline)")}}),
         true, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->mixerAnalyze(argNum(a, "start", 0), argNum(a, "end", -1)); }});

    // ---- clips
    add({"clip_list", "List clips", "Lists clips, optionally only those on one track. " + timeNote,
         object({{"track", integer("Optional track index")}}), true, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipList(int(argNum(a, "track", -1))); }});
    add({"clip_get", "Get clip", "Returns one clip's full properties.",
         object({{"clipId", str("Clip id")}}, {"clipId"}), true, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipGet(requireStr(a, "clipId")); }});
    add({"clip_add", "Add clip",
         "Places media on the timeline. A video with sound becomes two linked clips: the picture on a video track "
         "and the sound on an audio track (returned under 'linked'). Images go on video tracks, audio files on "
         "audio tracks. Without 'track' the bottom-most free video track / the first free audio track is used "
         "(a new one is created if needed). Placing over existing clips overwrites them. " + timeNote,
         object({{"mediaId", str("Media id from media_import / media_list")},
                 {"track", integer("Optional track: a video track for the picture, or an audio track for the sound")},
                 {"audioTrack", integer("Optional audio track for the sound of a video")},
                 {"start", num("Timeline position; default = playhead")},
                 {"link", boolean("Keep a video's sound linked to its picture (default true). false = independent "
                                  "audio clip, returned as 'audioClip'")}}, {"mediaId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipAdd(requireStr(a, "mediaId"), int(argNum(a, "track", -1)), argNum(a, "start", -1),
                                 int(argNum(a, "audioTrack", -1)), !a.contains("link") || a.value("link").toBool());
         }});
    add({"clip_move", "Move clip",
         "Moves a clip in time and/or to another track of the same kind. Its linked clips (a video's audio) move by "
         "the same amount unless linked=false. " + timeNote,
         object({{"clipId", str("Clip id")}, {"track", integer("Target track (same kind; default: same track)")},
                 {"start", num("New start time (default: unchanged)")},
                 {"linked", boolean("Also move linked clips (default true)")}}, {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipMove(requireStr(a, "clipId"), int(argNum(a, "track", -1)), argNum(a, "start", -1),
                                  !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"clip_trim", "Trim clip",
         "Trims a clip (removes unwanted beginning or ending). start = move the left edge (end stays), end = move "
         "the right edge, duration = new length; the footage follows the clip's speed/reverse. in = set the source "
         "in-point directly. Limited by the source media length. " + timeNote,
         object({{"clipId", str("Clip id")}, {"start", num("New left edge on the timeline")},
                 {"end", num("New right edge on the timeline")}, {"in", num("Source in-point")},
                 {"duration", num("New length")},
                 {"linked", boolean("Also trim linked clips the same way (default true)")}}, {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipTrim(requireStr(a, "clipId"), a); }});
    add({"clip_split", "Split clip",
         "Splits a clip (and its linked clips, unless linked=false) at a timeline time. The right halves stay "
         "linked to each other. " + timeNote,
         object({{"clipId", str("Clip id")}, {"time", num("Split time (default: playhead)")},
                 {"linked", boolean("Also split linked clips (default true)")}}, {"clipId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipSplit(requireStr(a, "clipId"), argNum(a, "time", -1),
                                   !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"clip_remove", "Delete clip",
         "Deletes a clip and, unless linked=false, its linked clips (a video's audio).",
         object({{"clipId", str("Clip id")}, {"linked", boolean("Also delete linked clips (default true)")}}, {"clipId"}),
         false, true, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipRemove(requireStr(a, "clipId"), !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"clip_link", "Link clips",
         "Links clips so they move, trim, split and delete together (e.g. a picture and its sound). Pass clipIds, "
         "or a single clipId to re-link a video clip with its own unlinked sound (or the reverse).",
         object({{"clipIds", strArray("Two or more clip ids")},
                 {"clipId", str("One clip: link it with the matching clip of the same media")}}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             if (a.contains("clipId")) return api->clipLinkMatching(requireStr(a, "clipId"));
             QStringList ids;
             for (const auto& v : a.value("clipIds").toArray()) ids << v.toString();
             return api->clipLink(ids);
         }});
    add({"clip_unlink", "Unlink clips",
         "Unlinks a clip from its group (e.g. to move a video's sound on its own).",
         object({{"clipId", str("Any clip of the linked group")}}, {"clipId"}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipUnlink(requireStr(a, "clipId")); }});
    add({"clip_set_speed", "Change clip speed",
         "Sets playback speed (0.5 = half speed / slow motion, 2 = twice as fast) and an optional speed curve "
         "(ramp). The clip keeps the same footage, so its length changes; later clips on its tracks (and the "
         "linked audio) shift so nothing is overwritten. Curve presets: ease-in (speeds up), ease-out (slows "
         "down), ease-in-out (slow-fast-slow), bullet (fast, slow motion, fast), hero (slow-motion moment), "
         "none. Or pass points [[pos 0..1, speed multiplier], ...]. Audio pitch follows the speed.",
         object({{"clipId", str("Clip id")}, {"speed", num("Base speed 0.05 .. 16 (default: unchanged)")},
                 {"curve", QJsonObject{{"description", "Preset name, 'none', or [[pos, speed], ...]"}}},
                 {"linked", boolean("Apply to linked clips too (default true)")}}, {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipSetSpeed(requireStr(a, "clipId"), a); }});
    add({"clip_reverse", "Reverse clip", "Plays a clip (and its linked audio) backwards, or forwards again.",
         object({{"clipId", str("Clip id")}, {"reverse", boolean("true = backwards (default), false = forwards")},
                 {"linked", boolean("Apply to linked clips too (default true)")}}, {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipReverse(requireStr(a, "clipId"), !a.contains("reverse") || a.value("reverse").toBool(),
                                     !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"clip_freeze_frame", "Freeze frame",
         "Holds the frame at `time` for `duration` seconds: the clip is split there, a still of that frame is "
         "inserted and the rest of the clip (with its audio) moves later. " + timeNote,
         object({{"clipId", str("Video clip id")}, {"time", num("Frame to hold (default: playhead)")},
                 {"duration", num("How long to hold it (default 2)")}}, {"clipId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipFreezeFrame(requireStr(a, "clipId"), argNum(a, "time", -1), argNum(a, "duration", 2));
         }});
    add({"clip_duplicate", "Duplicate clip",
         "Copies a clip (and its linked audio) with all its settings, placed right after the original.",
         object({{"clipId", str("Clip id")}, {"linked", boolean("Copy linked clips too (default true)")}}, {"clipId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipDuplicate(requireStr(a, "clipId"), !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"clip_ripple_delete", "Ripple delete",
         "Deletes a clip (and its linked audio) and closes the gap: later clips on those tracks move left.",
         object({{"clipId", str("Clip id")}, {"linked", boolean("Also delete linked clips (default true)")}}, {"clipId"}),
         false, true, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->clipRippleDelete(requireStr(a, "clipId"), !a.contains("linked") || a.value("linked").toBool());
         }});
    add({"timeline_close_gap", "Close gap",
         "Closes the empty space at `time` on a track: the clips after it (and their linked clips) move left.",
         object({{"track", integer("Track index")}, {"time", num("A time inside the gap")}}, {"track", "time"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->timelineCloseGap(int(argNum(a, "track", -1)), argNum(a, "time", 0));
         }});
    add({"timeline_cut_range", "Cut range",
         "Removes the time range [start, end) from every track (cuts clips at both ends). closeGap = true "
         "(default) pulls everything after it left; false leaves an empty gap. " + timeNote,
         object({{"start", num("Range start")}, {"end", num("Range end")},
                 {"closeGap", boolean("Close the gap afterwards (default true)")}}, {"start", "end"}),
         false, true, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->timelineRemoveRange(argNum(a, "start", 0), argNum(a, "end", 0),
                                             !a.contains("closeGap") || a.value("closeGap").toBool());
         }});
    add({"audio_separate", "Separate voice and music",
         "AI source separation (Demucs, runs locally on the GPU): splits a clip's audio into a voice stem and a "
         "music stem and puts them on audio tracks in place of the original sound, linked to the video. "
         "keep = both (default; adjust or delete either one later), voice (removes the music), music (removes "
         "the voice / karaoke). Works on an audio clip or on a video clip with linked audio. Takes a few seconds "
         "per minute of audio on a GPU; results are cached per file. Needs a one-time setup: pass install=true "
         "only if the user agreed to the download (about 90 MB, more if PyTorch is not cached).",
         object({{"clipId", str("Audio clip, or a video clip with linked audio")},
                 {"keep", enumStr({"both", "voice", "music"}, "What to keep (default both)")},
                 {"install", boolean("Run the one-time setup (downloads) if needed (default false)")}}, {"clipId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->audioSeparate(requireStr(a, "clipId"), argStr(a, "keep", "both"), a.value("install").toBool());
         }});
    add({"transcript_create", "Transcribe audio",
         "Transcribes speech or song lyrics of a clip with OpenAI Whisper (turbo, local GPU), with timestamps for "
         "every line and word. Works on an audio clip or a video clip with linked audio. For songs the voice is "
         "separated from the music first when separation is installed (isolateVoice auto). The transcript belongs "
         "to the media file and appears wherever its clips are on the timeline. Pass install=true only if the user "
         "agreed to the one-time download (~1.6 GB).",
         object({{"clipId", str("Audio clip, or a video clip with linked audio")},
                 {"language", str("auto (default), a language code such as en, ar, es, fr, de, or ar-EG for "
                                  "Egyptian Arabic (dialect-aware spelling)")},
                 {"hints", str("Optional names, slang or spellings to expect (helps lyrics and dialects)")},
                 {"isolateVoice", enumStr({"auto", "yes", "no"}, "Separate the voice first (default auto)")},
                 {"install", boolean("Run the one-time setup if needed (default false)")}}, {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) {
             return api->transcriptCreate(requireStr(a, "clipId"), argStr(a, "language", "auto"),
                                          argStr(a, "isolateVoice", "auto"), a.value("install").toBool(),
                                          argStr(a, "hints"));
         }});
    add({"transcript_get", "Get transcript / lyrics",
         "Returns the transcript lines in timeline order (timeline seconds, text, word timings) and the per-media "
         "segments (source seconds, used by transcript_update). Without an id: everything on the timeline.",
         object({{"id", str("Optional clip id or media id")}}), true, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->transcriptGet(argStr(a, "id")); }});
    add({"transcript_update", "Edit transcript",
         "Edits one transcript segment of a media file: new text (fix a word / lyric), new start/end (source "
         "seconds), delete=true, or insert={start, end, text} to add a line.",
         object({{"mediaId", str("Media id (see transcript_get)")}, {"segment", integer("Segment index")},
                 {"text", str("New text")}, {"start", num("New start, source seconds")},
                 {"end", num("New end, source seconds")}, {"delete", boolean("Remove the segment")},
                 {"insert", QJsonObject{{"type", "object"}, {"description", "{start, end, text} in source seconds"}}}},
                {"mediaId"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) { return api->transcriptUpdate(a); }});
    add({"transcript_export", "Export transcript",
         "Writes the timeline's transcript as subtitles or lyrics: srt, vtt, lrc (synced lyrics) or txt.",
         object({{"path", str("Output file")}, {"format", enumStr({"srt", "vtt", "lrc", "txt"}, "Default: from the file extension")}},
                {"path"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->transcriptExport(requireStr(a, "path"), argStr(a, "format")); }});
    add({"captions_get", "Get captions",
         "On-screen captions (CapCut-style text over the video) generated from the transcript: current style, "
         "presets and the first captions.",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->captionsGet(); }});
    add({"captions_set", "Set captions",
         "Turns on-screen captions on/off and styles them. Arabic captions are laid out right to left automatically "
         "with an Arabic font. preset: arabic (tuned for Arabic), classic (white, black outline, yellow word), "
         "bold (big caps, thick outline, centred), box (text on a dark box), neon (glow), minimal (whole line, "
         "no highlight), karaoke (coloured box behind the spoken word). Any field overrides the preset. Text comes "
         "from the transcript (fix words with transcript_update). Check the result with render_frame.",
         object({{"enabled", boolean("Show captions on the video")},
                 {"preset", enumStr(captionPresetNames(), "Style preset (also turns captions on)")},
                 {"font", str("Installed font family, e.g. Arial Black, Impact, Segoe UI Black (see suggestedFonts in captions_get)")},
                 {"fontFile", str("Path to a .ttf / .otf font file to use without installing it")},
                 {"arabicFont", str("Font for Arabic words (see suggestedArabicFonts; empty = automatic)")},
                 {"arabicFontFile", str("Path to a .ttf / .otf Arabic font (e.g. Cairo, Tajawal) to use without installing it")},
                 {"bold", boolean("Bold")}, {"italic", boolean("Italic")}, {"uppercase", boolean("ALL CAPS")},
                 {"size", num("Text height as a fraction of the frame height (0.015 - 0.3, default 0.065)")},
                 {"color", str("Text color, e.g. #FFFFFF")}, {"highlight", str("Color of the spoken word")},
                 {"highlightMode", enumStr({"color", "box", "none"}, "How the spoken word is shown")},
                 {"outline", str("Outline color")}, {"outlineWidth", num("Outline thickness, fraction of the font size (0 - 0.5)")},
                 {"shadow", boolean("Drop shadow")}, {"box", boolean("Background box")}, {"boxColor", str("Box color")},
                 {"boxOpacity", num("Box opacity 0 - 1")},
                 {"position", enumStr({"bottom", "middle", "top"}, "Where on screen")},
                 {"offset", num("Distance from that edge (bottom/top) or shift down from centre, fraction of frame height")},
                 {"wordsPerCaption", integer("Words shown at a time (0 = the whole line)")},
                 {"maxLines", integer("Lines per caption 1 - 4")},
                 {"animation", enumStr({"pop", "fade", "none"}, "How each caption appears")}}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->captionsSet(a); }});
    add({"clip_update", "Update clip properties",
         "Changes a clip's name, transform (position/scale/rotation/opacity), GPU effects (brightness, contrast, "
         "saturation, hue, blur, vignette, grayscale, invert), volume and fades. Partial updates; omitted fields keep "
         "their value. " + timeNote,
         object({{"clipId", str("Clip id")}, {"name", str("Display name")}, {"transform", kTransformSchema},
                 {"effects", kEffectsSchema}, {"volume", num("Audio gain 0 .. 4 (1 = unchanged)")},
                 {"fit", enumStr({"fit", "fill"}, "Scale the picture to fit inside or fill the canvas (resets position)")},
                 {"fadeIn", num("Fade-in length in seconds")}, {"fadeOut", num("Fade-out length in seconds")},
                 {"resetEffects", boolean("Reset all effects first")}, {"resetTransform", boolean("Reset transform first")}},
                {"clipId"}),
         false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->clipUpdate(requireStr(a, "clipId"), a); }});

    // ---- history
    add({"edit_undo", "Undo", "Undoes the last timeline edit.", object({}), false, false, false,
         [api](const QJsonObject&, QJsonArray&) { return api->undo(); }});
    add({"edit_redo", "Redo", "Redoes the last undone edit.", object({}), false, false, false,
         [api](const QJsonObject&, QJsonArray&) { return api->redo(); }});
    add({"edit_history", "Undo history", "Lists the undo history.", object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->history(); }});

    // ---- playback / selection
    add({"playback_state", "Playback state", "Playhead position, duration and whether playing.", object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->playbackState(); }});
    add({"playback_seek", "Seek", "Moves the playhead. " + timeNote,
         object({{"time", num("Position in seconds")}}, {"time"}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->playbackSeek(argNum(a, "time", 0)); }});
    add({"playback_play", "Play", "Starts real-time playback in the GUI (with audio).", object({}), false, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->playbackPlay(); }});
    add({"playback_pause", "Pause", "Pauses playback.", object({}), false, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->playbackPause(); }});
    add({"selection_get", "Get selection", "Returns the clip selected in the GUI timeline, if any.", object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->selectionGet(); }});
    add({"selection_set", "Select clip", "Selects a clip in the GUI (shows it in the inspector). Empty id clears.",
         object({{"clipId", str("Clip id, or empty to clear")}}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->selectionSet(argStr(a, "clipId")); }});

    // ---- render / export
    add({"render_frame", "Render frame",
         "Renders the composited frame at a time on the GPU and returns it as an image, so you can see the result "
         "of edits. Optionally also saves a full-resolution PNG. " + timeNote,
         object({{"time", num("Timeline time in seconds (default: playhead)")},
                 {"path", str("Optional PNG/JPG path to save the full-resolution frame")},
                 {"maxWidth", integer("Width of the returned preview image (default 960, 0 = full size)")},
                 {"returnImage", boolean("Include the image in the result (default true)")}}),
         true, false, true,
         [api](const QJsonObject& a, QJsonArray& extra) {
             const bool wantImage = a.value("returnImage").toBool(true);
             QByteArray png;
             QJsonObject r = api->renderFrame(argNum(a, "time", -1), argStr(a, "path"),
                                              int(argNum(a, "maxWidth", 960)), wantImage ? &png : nullptr);
             if (wantImage && !png.isEmpty())
                 extra.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"},
                                          {"data", QString::fromLatin1(png.toBase64())}});
             return r;
         }});
    add({"export_video", "Export video",
         "Renders the timeline on the GPU and encodes it to a video file (blocks until done). Encoder 'auto' picks a "
         "GPU H.264 encoder (NVENC/AMF/QSV) when available. Container follows the extension (.mp4, .mov, .mkv, .webm). "
         + timeNote,
         object({{"path", str("Output file path")},
                 {"encoder", str("FFmpeg encoder name from export_encoders, or 'auto'")},
                 {"width", integer("Output width (default: project)")}, {"height", integer("Output height (default: project)")},
                 {"fps", num("Output frame rate (default: project)")},
                 {"quality", integer("Constant quality 0-51, lower = better (default 20)")},
                 {"includeAudio", boolean("Include mixed audio (default true)")},
                 {"start", num("Range start in seconds (default 0)")}, {"end", num("Range end in seconds (default: end)")}},
                {"path"}),
         false, false, false,
         [api](const QJsonObject& a, QJsonArray&) { return api->exportVideo(a); }});
    add({"export_encoders", "List encoders", "Video encoders that work on this machine (GPU ones flagged).",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->exportEncoders(); }});

    // ---- GPU
    add({"gpu_info", "GPU info",
         "Reports the graphics hardware: OpenGL renderer, Vulkan devices, hardware video decoders, current decode "
         "mode, GPU/CPU encoders and the active preview renderer.",
         object({}), true, false, true,
         [api](const QJsonObject&, QJsonArray&) { return api->gpuInfo(); }});
    add({"gpu_set_decode_mode", "Set GPU decoding",
         "Chooses hardware video decoding: 'auto', 'none' (CPU) or a device such as d3d11va, cuda, dxva2, vulkan, "
         "vaapi, videotoolbox (see gpu_info).",
         object({{"mode", str("auto | none | device type")}}, {"mode"}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->gpuSetDecodeMode(requireStr(a, "mode")); }});
    add({"preview_set_renderer", "Set preview renderer", "Switches the GUI preview between OpenGL and Vulkan.",
         object({{"renderer", enumStr({"opengl", "vulkan"}, "Renderer")}}, {"renderer"}), false, false, true,
         [api](const QJsonObject& a, QJsonArray&) { return api->previewSetRenderer(requireStr(a, "renderer")); }});
}

QJsonArray McpServer::toolDefinitions() const
{
    QJsonArray arr;
    for (const auto& t : m_tools) {
        arr.append(QJsonObject{
            {"name", t.name},
            {"title", t.title},
            {"description", t.description},
            {"inputSchema", t.inputSchema},
            {"annotations", QJsonObject{{"title", t.title}, {"readOnlyHint", t.readOnly},
                                        {"destructiveHint", t.destructive}, {"idempotentHint", t.idempotent},
                                        {"openWorldHint", false}}},
        });
    }
    return arr;
}

QJsonObject McpServer::callTool(const QString& name, const QJsonObject& args)
{
    for (const auto& t : m_tools) {
        if (t.name != name) continue;
        QJsonArray content;
        try {
            QJsonArray extra;
            const QJsonObject result = t.run(args, extra);
            content.append(QJsonObject{{"type", "text"},
                                       {"text", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}});
            for (const auto& e : extra) content.append(e);
            emit toolCalled(name, true);
            return {{"content", content}, {"structuredContent", result}, {"isError", false}};
        } catch (const ApiError& e) {
            content.append(QJsonObject{{"type", "text"}, {"text", e.message()}});
        } catch (const std::exception& e) {
            content.append(QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(e.what())}});
        }
        emit toolCalled(name, false);
        return {{"content", content}, {"isError", true}};
    }
    return {};
}

// ---------------------------------------------------------------------------
// JSON-RPC
// ---------------------------------------------------------------------------
QJsonObject McpServer::handleMessage(const QJsonObject& msg)
{
    const QJsonValue id = msg.value("id");
    const bool isNotification = !msg.contains("id");
    const QString method = msg.value("method").toString();

    if (msg.value("jsonrpc").toString() != QLatin1String("2.0") || method.isEmpty()) {
        if (isNotification) return {};
        return rpcError(id, -32600, QStringLiteral("Invalid Request"));
    }
    if (isNotification) return {}; // notifications/initialized, cancelled, ... need no reply

    const QJsonObject params = msg.value("params").toObject();
    if (method == QLatin1String("initialize")) {
        const QString requested = params.value("protocolVersion").toString();
        static const QStringList supported = {kLatestProtocol, "2025-03-26", "2024-11-05"};
        const QString version = supported.contains(requested) ? requested : QString::fromLatin1(kLatestProtocol);
        return rpcResult(id, {
            {"protocolVersion", version},
            {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
            {"serverInfo", QJsonObject{{"name", "video-editor"}, {"title", "Video Editor"},
                                       {"version", QCoreApplication::applicationVersion()}}},
            {"instructions",
             "Controls a GPU-accelerated non-linear video editor. Typical flow: project_get -> media_import -> "
             "clip_add / clip_trim / clip_split / clip_update (effects, transform, fades) -> render_frame to look at "
             "the result -> export_video. Times are in seconds. Track 0 is the top video layer. Every edit is undoable "
             "with edit_undo. Save with project_save."},
        });
    }
    if (method == QLatin1String("ping")) return rpcResult(id, {});
    if (method == QLatin1String("tools/list")) return rpcResult(id, {{"tools", toolDefinitions()}});
    if (method == QLatin1String("tools/call")) {
        const QString name = params.value("name").toString();
        const QJsonObject result = callTool(name, params.value("arguments").toObject());
        if (result.isEmpty()) return rpcError(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
        return rpcResult(id, result);
    }
    if (method == QLatin1String("resources/list")) return rpcResult(id, {{"resources", QJsonArray()}});
    if (method == QLatin1String("prompts/list")) return rpcResult(id, {{"prompts", QJsonArray()}});
    return rpcError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

QByteArray McpServer::handle(const QByteArray& message)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(message, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonDocument(rpcError(QJsonValue::Null, -32700, QStringLiteral("Parse error"))).toJson(QJsonDocument::Compact);
    const QJsonObject response = handleMessage(doc.object());
    if (response.isEmpty()) return {};
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

} // namespace ve
