#pragma once

#include "core/Project.h"
#include "media/AudioMixer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <stdexcept>

namespace ve {

// Thrown by EditorApi for invalid arguments or failed operations. The MCP
// layer turns it into a tool error result.
class ApiError : public std::runtime_error {
public:
    explicit ApiError(const QString& msg) : std::runtime_error(msg.toStdString()), m_msg(msg) {}
    QString message() const { return m_msg; }

private:
    QString m_msg;
};

// What only the hosting application can do (GUI window or headless server).
class EditorHost {
public:
    virtual ~EditorHost() = default;
    virtual QString hostKind() const = 0;                 // "gui" or "headless"
    virtual TimeUs playhead() const = 0;
    virtual void seek(TimeUs t) = 0;
    virtual void setPlaying(bool playing) { Q_UNUSED(playing); }
    virtual bool isPlaying() const { return false; }
    virtual QString selectedClip() const { return {}; }
    virtual void selectClip(const QString& id) { Q_UNUSED(id); }
    virtual QString previewRenderer() const { return {}; }
    virtual bool setPreviewRenderer(const QString& name, QString* error);
    virtual void projectReplaced() {}                     // after new/open: refresh views
    virtual void decodeSettingsChanged() {}
    virtual bool wantsThumbnails() const { return false; }
    // Live mixer meter levels while playing (GUI only).
    virtual bool audioLevels(MixLevels& out) const { Q_UNUSED(out); return false; }
    // Long operations report here (GUI: progress dialog). Return false to cancel.
    virtual bool reportProgress(const QString& text, double fraction) { Q_UNUSED(text); Q_UNUSED(fraction); return true; }
    virtual void progressDone() {}
};

// The editor's complete programmatic surface. Every user-facing feature is
// reachable from here; the MCP server (and anything else, e.g. scripting)
// is a thin layer on top. All times are in SECONDS (double) at this level.
// Mutations are recorded on the project's undo stack.
// Must be used from the GUI/main thread.
class EditorApi {
public:
    EditorApi(Project* project, EditorHost* host);

    Project* project() const { return m_project; }
    EditorHost* host() const { return m_host; }

    // --- project
    QJsonObject projectNew();
    QJsonObject projectOpen(const QString& path);
    QJsonObject projectSave(const QString& path = {});
    QJsonObject projectInfo() const;
    QJsonObject projectJson() const;
    QJsonObject projectSetSettings(const QJsonObject& settings);

    // --- media
    QJsonObject mediaImport(const QStringList& paths);
    QJsonObject mediaList() const;
    QJsonObject mediaRemove(const QString& id);
    QJsonObject mediaProbe(const QString& path) const;

    // --- tracks (index 0 = top of the stack)
    QJsonObject trackList() const;
    QJsonObject trackAdd(const QString& name, const QString& kind = QStringLiteral("video"));
    QJsonObject trackRemove(int index);
    QJsonObject trackUpdate(int index, const QJsonObject& changes);

    // --- audio mixer (gain in dB, pan -1..1; -60 dB = silence)
    QJsonObject mixerGet() const;
    QJsonObject mixerSetTrack(int index, const QJsonObject& changes);
    QJsonObject mixerSetMaster(const QJsonObject& changes);
    QJsonObject mixerAnalyze(double start, double end) const;   // end < 0 = timeline end

    // --- clips
    QJsonObject clipList(int track = -1) const;
    QJsonObject clipGet(const QString& id) const;
    // Video with sound becomes a video clip plus a linked audio clip.
    // link = false: the sound is added as an independent clip ("audioClip" in the result).
    QJsonObject clipAdd(const QString& mediaId, int track, double start, int audioTrack = -1, bool link = true);
    // `linked` = also apply to the clips linked with it (video <-> its audio).
    QJsonObject clipMove(const QString& id, int track, double start, bool linked = true);
    QJsonObject clipTrim(const QString& id, const QJsonObject& changes);   // "linked" key, default true
    QJsonObject clipSplit(const QString& id, double time, bool linked = true);
    QJsonObject clipRemove(const QString& id, bool linked = true);
    QJsonObject clipLink(const QStringList& ids);
    QJsonObject clipLinkMatching(const QString& id);   // re-link a video with its own sound
    QJsonObject clipUnlink(const QString& id);
    // speed: 0.05 .. 16; curve: preset name or [[pos, speed], ...] (null = keep, "none" = remove)
    QJsonObject clipSetSpeed(const QString& id, const QJsonObject& args);
    QJsonObject clipReverse(const QString& id, bool reverse, bool linked = true);
    QJsonObject clipFreezeFrame(const QString& id, double time, double hold);
    QJsonObject clipDuplicate(const QString& id, bool linked = true);
    QJsonObject clipRippleDelete(const QString& id, bool linked = true);
    QJsonObject timelineCloseGap(int track, double time);
    QJsonObject timelineRemoveRange(double start, double end, bool closeGap);

    // AI voice / music separation (Demucs). keep = "both" | "voice" | "music".
    // The clip's sound is replaced by the kept stem(s) on audio tracks, linked
    // to the video. install = run the one-time setup if needed (downloads).
    QJsonObject audioSeparate(const QString& clipId, const QString& keep, bool install);
    QJsonObject audioSeparationStatus() const;

    // AI video upscaling (FlashVSR / Real-ESRGAN) of the part of the source a video
    // clip shows. model: an upscale model id, empty = the one chosen in AI Models.
    // scale: 2 or 4 (0 = automatic). replace: put the upscaled file in the clip
    // (else it is only imported). force: run even if the model is too heavy for
    // this computer. install: run the one-time setup if needed (downloads).
    QJsonObject videoUpscale(const QString& clipId, const QString& model, int scale, bool replace, bool install,
                             bool force = false);

    // --- local AI models: hardware, recommendations, choices
    QJsonObject aiModels() const;
    QJsonObject aiModelSelect(const QString& task, const QString& model);

    // --- transcripts (Whisper). Segment times in the API are SOURCE seconds of
    // the media; "lines" also give the TIMELINE seconds where they are heard.
    // isolateVoice: transcribe the separated voice (better for songs); "auto"
    // = when voice separation is installed.
    QJsonObject transcriptCreate(const QString& clipId, const QString& language, const QString& isolateVoice,
                                 bool install, const QString& hints = {});
    QJsonObject transcriptGet(const QString& mediaOrClipId) const;
    QJsonObject transcriptUpdate(const QJsonObject& args);
    // language: export a translation instead of the transcript ("" = the transcript).
    QJsonObject transcriptExport(const QString& path, const QString& format, const QString& language = {}) const;
    static QString formatTranscript(const std::vector<TimelineLine>& lines, const QString& format);
    // Moves subtitle lines in time (seconds, + = later): one media's transcript (or all), lines [from, to].
    QJsonObject transcriptShift(const QJsonObject& args);
    // Reads subtitles (.srt / .vtt / .lrc, timeline times) into the transcript of the clip's sound.
    QJsonObject transcriptImport(const QString& clipId, const QString& path, bool replace);
    // Translates every transcript on the timeline (or one media's) with NLLB. Stored next to the
    // transcript; shown as captions when showCaptions (captions language). install = set up if needed.
    QJsonObject transcriptTranslate(const QString& language, const QString& mediaId, bool install, bool showCaptions);
    static QJsonArray parseSubtitles(const QString& content, const QString& format, QString* error); // [[start, end, text]] seconds

    // --- on-screen captions from the transcript (CapCut-style)
    QJsonObject captionsGet() const;
    QJsonObject captionsSet(const QJsonObject& changes);
    QJsonObject clipUpdate(const QString& id, const QJsonObject& changes);

    // --- text overlays (titles, lower thirds, ...): clips on video tracks
    QJsonObject textTemplates() const;
    // text, template, start (default playhead), duration, track, style{}, animation{}, transform{}
    QJsonObject textAdd(const QJsonObject& args);
    // text, template (re-style from a template), style{}, animation{}
    QJsonObject textUpdate(const QString& clipId, const QJsonObject& args);
    QJsonObject textTemplateSave(const QString& clipId, const QString& name);
    QJsonObject textTemplateDelete(const QString& id);

    // --- text to speech (voice-over)
    QJsonObject ttsVoices(const QString& engine) const;
    // text (or clipId of a text clip), engine, voice, rate, start, track, install. Adds an audio clip.
    QJsonObject ttsSpeak(const QJsonObject& args);
    // A voice-over of the subtitles: one clip per line at its time. language: a translation ("" = transcript).
    QJsonObject ttsSubtitles(const QJsonObject& args);

    // --- history
    QJsonObject undo();
    QJsonObject redo();
    QJsonObject history() const;

    // --- playback / selection
    QJsonObject playbackState() const;
    QJsonObject playbackSeek(double time);
    QJsonObject playbackPlay();
    QJsonObject playbackPause();
    QJsonObject selectionGet() const;
    QJsonObject selectionSet(const QString& clipId);

    // --- rendering / export (GPU)
    // Renders the composited frame at `time`. Saves a PNG if `path` is set.
    // `pngOut` receives PNG bytes scaled to at most maxWidth (for previews).
    QJsonObject renderFrame(double time, const QString& path, int maxWidth, QByteArray* pngOut);
    QJsonObject exportVideo(const QJsonObject& settings);
    QJsonObject exportEncoders() const;

    // --- GPU
    QJsonObject gpuInfo() const;
    QJsonObject gpuSetDecodeMode(const QString& mode);
    QJsonObject previewSetRenderer(const QString& name);

    // JSON helpers (seconds-based) shared with the MCP layer.
    QJsonObject clipToJson(const Clip& c, int track) const;
    QJsonObject mediaToJson(const MediaItem& m) const;

private:
    const Clip& requireClip(const QString& id, int* track = nullptr) const;
    int requireTrack(int index) const;
    void applyTrackChanges(int index, const QJsonObject& changes, const QString& undoText);
    QJsonObject trackToJson(int index) const;
    // Separated stems of a media file (cached). Runs the separation if needed.
    std::pair<QString, QString> stemFiles(const QString& mediaPath, TimeUs duration, bool* cached, QString* device,
                                          double* seconds);
    QString audioClipFor(const QString& clipId) const;   // the clip itself, or the audio linked to a video clip
    TextData textDataFrom(const QJsonObject& args, const TextData& base) const;   // validates style / animation
    // Speech for `text` as a media item (imported once per file). Returns the media id.
    QString speechMedia(const QString& text, const QString& engine, const QString& voice, double rate, bool install,
                        bool* cached);

    Project* m_project;
    EditorHost* m_host;
};

} // namespace ve
