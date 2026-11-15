// End-to-end smoke test: media probing/decoding, timeline editing + undo,
// SQLite/JSON persistence, GPU compositing (offscreen OpenGL) and export.
//
// usage: ve_smoke_test <testdata dir> <output dir>

#include "core/Project.h"
#include "core/ProjectStore.h"
#include "media/AudioDecoder.h"
#include "app/MixerPanel.h"
#include "media/AudioMixer.h"
#include "media/Exporter.h"
#include "media/FrameSource.h"
#include "media/HwAccel.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"
#include "media/FFmpegUtil.h"
#include "render/CaptionRenderer.h"
#include "render/GLCompositor.h"

#ifdef VE_HAVE_VULKAN
#include "render/VulkanPreview.h"
#endif

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ve;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                            \
    } while (0)

static void section(const char* name) { std::printf("== %s\n", name); }

static MediaItem importMedia(const QString& path)
{
    MediaItem m;
    m.path = path;
    QString err;
    auto info = MediaProbe::probe(path, &err);
    if (info) m.info = *info;
    else std::fprintf(stderr, "  probe failed for %s: %s\n", qPrintable(path), qPrintable(err));
    return m;
}

int main(int argc, char** argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QGuiApplication app(argc, argv);
    av_log_set_level(AV_LOG_ERROR);

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <testdata> <outdir>\n", argv[0]);
        return 2;
    }
    const QDir data(QString::fromLocal8Bit(argv[1]));
    const QString outDir = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(outDir);
    const QString clipA = data.filePath(QStringLiteral("clip_a.mp4"));
    const QString clipB = data.filePath(QStringLiteral("clip_b.mp4"));
    const QString logo = data.filePath(QStringLiteral("logo.png"));
    const QString tone = data.filePath(QStringLiteral("tone.mp3"));

    // ------------------------------------------------------------------
    section("probe");
    const MediaItem a = importMedia(clipA);
    CHECK(a.info.hasVideo && a.info.hasAudio && !a.info.isImage);
    CHECK(a.info.width == 1280 && a.info.height == 720);
    CHECK(std::fabs(a.info.fps - 30.0) < 0.01);
    CHECK(std::llabs(a.info.duration - 4 * kUsPerSecond) < 100'000);
    const MediaItem img = importMedia(logo);
    CHECK(img.info.hasVideo && img.info.isImage);
    const MediaItem snd = importMedia(tone);
    CHECK(snd.info.hasAudio && !snd.info.hasVideo);
    std::printf("  clip_a: %dx%d %.2f fps %.2fs %s/%s\n", a.info.width, a.info.height, a.info.fps,
                usToSeconds(a.info.duration), qPrintable(a.info.videoCodec), qPrintable(a.info.audioCodec));

    // ------------------------------------------------------------------
    section("video decode + seek");
    {
        VideoDecoder dec;
        CHECK(dec.open(clipA));
        auto f0 = dec.frameAt(0);
        CHECK(f0 && f0->width == 1280 && f0->height == 720 && f0->rgba.size() == size_t(1280) * 720 * 4);
        auto f2 = dec.frameAt(2 * kUsPerSecond);
        CHECK(f2 && std::llabs(f2->pts - 2 * kUsPerSecond) <= 34'000);
        auto f1 = dec.frameAt(kUsPerSecond); // backwards seek
        CHECK(f1 && std::llabs(f1->pts - kUsPerSecond) <= 34'000);
        TimeUs last = f1 ? f1->pts : 0;
        bool monotonic = true;
        for (int i = 1; i <= 30; ++i) {
            auto f = dec.frameAt(kUsPerSecond + i * 33'333);
            if (!f || f->pts < last) monotonic = false;
            if (f) last = f->pts;
        }
        CHECK(monotonic);
        CHECK(std::llabs(last - 2 * kUsPerSecond) <= 34'000);
        // Different frames really differ (testsrc2 animates).
        CHECK(f0 && f2 && f0->rgba != f2->rgba);

        VideoDecoder still;
        CHECK(still.open(logo));
        auto s = still.frameAt(3 * kUsPerSecond);
        CHECK(s && s->width == 400 && s->height == 400);
    }

    // ------------------------------------------------------------------
    section("audio decode");
    {
        AudioDecoder dec;
        CHECK(dec.open(clipA, 48000));
        std::vector<float> buf(48000 * 2);
        dec.read(kUsPerSecond / 2, 48000, buf.data());
        double sum = 0;
        for (float v : buf) sum += double(v) * v;
        const double rms = std::sqrt(sum / buf.size());
        std::printf("  rms of 440 Hz tone: %.4f\n", rms);
        CHECK(rms > 0.01);
    }

    // ------------------------------------------------------------------
    section("timeline editing + undo");
    Project project;
    QString idA, idB, idImg, idTone;
    {
        MediaItem b = importMedia(clipB);
        idA = project.addMedia(a);
        idB = project.addMedia(b);
        idImg = project.addMedia(img);
        idTone = project.addMedia(snd);
        CHECK(project.trackCount() == 4); // V2, V1, A1, A2
        CHECK(!project.tracks()[1].isAudio() && project.tracks()[2].isAudio() && project.firstAudioTrack() == 2);

        QString c1, c2;
        project.edit("add", [&] { c1 = project.addClipFromMedia(idA, 1, 0); });
        project.edit("add", [&] { c2 = project.addClipFromMedia(idB, 1, 4 * kUsPerSecond); });
        CHECK(project.duration() == 7 * kUsPerSecond);
        project.edit("add logo", [&] { project.addClipFromMedia(idImg, 0, kUsPerSecond); });
        project.edit("add tone", [&] { project.addClipFromMedia(idTone, 3, 0); }); // A2 (A1 has clip A's sound)
        CHECK(project.tracks()[1].clips.size() == 2);

        // Split clip A at 2s
        QString right;
        project.edit("split", [&] { right = project.splitClip(c1, 2 * kUsPerSecond); });
        CHECK(!right.isEmpty());
        CHECK(project.tracks()[1].clips.size() == 3);
        const Clip* r = project.findClip(right);
        CHECK(r && r->start == 2 * kUsPerSecond && r->in == 2 * kUsPerSecond && r->duration == 2 * kUsPerSecond);

        // Overwrite: move clip B onto 1.5s -> covers the right half of A entirely
        project.edit("move", [&] { project.moveClip(c2, 1, 1'500'000); });
        const Clip* left = project.findClip(c1);
        CHECK(left && left->duration == 1'500'000); // trimmed by the overwrite
        CHECK(project.findClip(right) == nullptr);  // fully covered -> removed

        project.undoStack()->undo();
        CHECK(project.findClip(right) != nullptr);
        CHECK(project.findClip(c1)->duration == 2 * kUsPerSecond);
        project.undoStack()->redo();
        CHECK(project.findClip(right) == nullptr);
        project.undoStack()->undo(); // keep the split version for later tests

        // Trim right edge cannot exceed the source length
        project.edit("trim", [&] { project.trimClip(right, 2 * kUsPerSecond, 2 * kUsPerSecond, 10 * kUsPerSecond); });
        CHECK(project.findClip(right)->duration == 2 * kUsPerSecond);

        // Properties + merged undo
        Clip edited = *project.findClip(right);
        edited.effects.grayscale = true;
        edited.effects.blur = 2.0;
        edited.fadeOut = 500'000;
        const int before = project.undoStack()->index();
        project.edit("prop", [&] { project.updateClip(edited); }, "k");
        edited.effects.blur = 3.0;
        project.edit("prop", [&] { project.updateClip(edited); }, "k");
        CHECK(project.undoStack()->index() == before + 1); // two edits merged into one step
        CHECK(project.findClip(right)->effects.blur == 3.0);
    }

    // ------------------------------------------------------------------
    section("linked audio + video");
    {
        constexpr TimeUs s = kUsPerSecond;
        Project pr;
        const QString ma = pr.addMedia(a), mb = pr.addMedia(importMedia(clipB)), mt = pr.addMedia(snd),
                      mi = pr.addMedia(img);
        QString v;
        pr.edit("add", [&] { v = pr.addClipFromMedia(ma, 1, 0); });
        const QStringList partners = pr.linkedClips(v);
        CHECK(partners.size() == 1);                          // a video with sound -> picture + linked sound
        const QString au = partners.value(0);
        int vt = -1, at = -1;
        const Clip* vc = pr.findClip(v, &vt);
        const Clip* ac = pr.findClip(au, &at);
        CHECK(vc && ac && vt == 1 && at == 2 && pr.tracks()[at].isAudio()); // V1 + A1
        CHECK(ac && vc && ac->start == vc->start && ac->in == vc->in && ac->duration == vc->duration);

        QString vb, im, tn;
        pr.edit("add", [&] {
            vb = pr.addClipFromMedia(mb, 1, 10 * s);          // silent video: no audio clip
            im = pr.addClipFromMedia(mi, 0, 0);               // image: video track only
            tn = pr.addClipFromMedia(mt, 0, 0);               // audio file asked for a video track
        });
        int tnt = -1;
        pr.findClip(tn, &tnt);
        CHECK(pr.linkedClips(vb).isEmpty() && pr.linkedClips(im).isEmpty());
        CHECK(tnt == 3);                                      // -> first free audio track (A2)

        pr.edit("move", [&] { pr.moveClip(v, 1, 1 * s); });
        CHECK(pr.findClip(au)->start == 1 * s);               // the sound follows the picture
        pr.edit("move", [&] { pr.moveClip(au, 2, 1500'000, false); });
        CHECK(pr.findClip(v)->start == 1 * s && pr.findClip(au)->start == 1500'000); // linked = false (Alt)
        pr.undoStack()->undo();
        CHECK(!pr.moveClip(v, 2, 0));                         // pictures can't go onto audio tracks

        pr.edit("trim", [&] { pr.trimClip(v, 1500'000, 500'000, 3500'000); });
        const Clip* t2 = pr.findClip(au);
        CHECK(t2 && t2->start == 1500'000 && t2->in == 500'000 && t2->duration == 3500'000);

        QString vr;
        pr.edit("split", [&] { vr = pr.splitClip(v, 3 * s); });
        const QStringList rp = pr.linkedClips(vr);
        CHECK(rp.size() == 1 && pr.findClip(rp.value(0)) && pr.findClip(rp.value(0))->start == 3 * s);
        CHECK(pr.linkedClips(v) == QStringList{au});          // left halves stay linked to each other

        pr.edit("del", [&] { pr.removeClip(vr); });
        CHECK(!pr.findClip(vr) && !pr.findClip(rp.value(0))); // delete takes the linked sound along
        pr.edit("del", [&] { pr.removeClip(au, false); });
        CHECK(pr.findClip(v) && !pr.findClip(au));
        pr.undoStack()->undo();
        pr.edit("unlink", [&] { pr.unlinkClip(v); });
        CHECK(pr.linkedClips(v).isEmpty() && pr.linkedClips(au).isEmpty());
        CHECK(pr.linkCandidate(v) == au && pr.linkCandidate(au) == v); // "Link Audio and Video" finds the pair
        pr.edit("link", [&] { pr.linkClips({v, au}); });
        CHECK(pr.linkedClips(v) == QStringList{au} && pr.linkCandidate(v).isEmpty());

        // "Link audio to video when adding" off: the sound arrives as an independent clip.
        QString v2, a2;
        pr.edit("add unlinked", [&] { v2 = pr.addMediaClips(ma, 0, -1, 20 * s, &a2, false); });
        int a2t = -1;
        CHECK(!v2.isEmpty() && !a2.isEmpty() && pr.findClip(a2, &a2t) && pr.tracks()[a2t].isAudio());
        CHECK(pr.linkedClips(v2).isEmpty() && pr.linkCandidate(v2) == a2);
        pr.edit("move", [&] { pr.moveClip(v2, 0, 21 * s); });
        CHECK(pr.findClip(a2)->start == 20 * s);              // unlinked audio stays put

        // Pictures come from video tracks only, sound from audio tracks only.
        const TimelineSnapshot snapAv = pr.snapshot();
        FrameSource fs;
        auto f = fs.build(snapAv, 2 * s);                     // image (V2) + clip A (V1)
        CHECK(f && f->layers.size() == 2);
        const MixLevels lv = analyzeMix(snapAv, 2 * s, 2500'000);
        CHECK(lv.tracks.value(pr.tracks()[2].id).peak[0] > 0.01f); // clip A's sound on A1
        CHECK(!lv.tracks.contains(pr.tracks()[1].id));        // video tracks are not mixed

        // Projects from before track kinds: picture and sound get separated + linked.
        const QJsonObject legacy{
            {"format", "video-editor-project"}, {"version", 1}, {"settings", QJsonObject{}},
            {"media", QJsonArray{QJsonObject{{"id", ma}, {"path", clipA}, {"name", "clip_a.mp4"}, {"info", a.info.toJson()}},
                                 QJsonObject{{"id", mt}, {"path", tone}, {"name", "tone.mp3"}, {"info", snd.info.toJson()}}}},
            {"tracks", QJsonArray{
                QJsonObject{{"id", "t1"}, {"name", "V1"}, {"clips", QJsonArray{QJsonObject{
                    {"id", "c1"}, {"mediaId", ma}, {"start", 0}, {"in", 0}, {"duration", 4e6}}}}},
                QJsonObject{{"id", "t2"}, {"name", "A1"}, {"clips", QJsonArray{QJsonObject{
                    {"id", "c2"}, {"mediaId", mt}, {"start", 0}, {"in", 0}, {"duration", 2e6}}}}}}},
        };
        Project old;
        CHECK(old.fromJson(legacy));
        const QStringList lp = old.linkedClips(QStringLiteral("c1"));
        int lt = -1;
        if (!lp.isEmpty()) old.findClip(lp[0], &lt);
        CHECK(lp.size() == 1 && lt >= 0 && old.tracks()[lt].isAudio()); // c1's sound now on its own audio track
        CHECK(!old.tracks()[0].isAudio() && old.tracks()[1].isAudio() && old.trackCount() == 3);
    }

    // ------------------------------------------------------------------
    section("speed, curves, reverse, freeze, duplicate, ripple, cut range");
    {
        constexpr TimeUs s = kUsPerSecond;
        Project pr;
        const QString ma = pr.addMedia(a), mb = pr.addMedia(importMedia(clipB));
        QString v, w;
        pr.edit("add", [&] {
            v = pr.addClipFromMedia(ma, 1, 0);                 // 0-4 s (+ linked audio on A1)
            w = pr.addClipFromMedia(mb, 1, 4 * s);             // 4-7 s
        });
        const QString au = pr.linkedClips(v).value(0);

        // Speed 2x: half as long, same footage; the next clip and the audio follow.
        pr.edit("speed", [&] { pr.setClipSpeed(v, 2.0, {}, {}); });
        CHECK(pr.findClip(v)->duration == 2 * s && pr.findClip(v)->sourceSpan() == 4 * s);
        CHECK(pr.findClip(w)->start == 2 * s && pr.findClip(au)->duration == 2 * s);
        CHECK(pr.findClip(v)->sourceTimeAt(1 * s) == 2 * s);  // 1 s into the clip shows second 2
        pr.edit("slow", [&] { pr.setClipSpeed(v, 0.5, {}, {}); });
        CHECK(pr.findClip(v)->duration == 8 * s && pr.findClip(w)->start == 8 * s);
        pr.edit("normal", [&] { pr.setClipSpeed(v, 1.0, {}, {}); });
        CHECK(pr.findClip(v)->duration == 4 * s && pr.findClip(w)->start == 4 * s);

        // Speed curve: same footage, monotonic mapping, slow part in the middle.
        pr.edit("curve", [&] { pr.setClipSpeed(v, 1.0, speedCurvePreset("bullet"), "bullet"); });
        const Clip* cv = pr.findClip(v);
        CHECK(std::llabs(cv->sourceSpan() - 4 * s) < 40'000 && cv->duration < 4 * s);
        bool monotonic = true;
        for (TimeUs t = 0; t + 10'000 < cv->duration; t += 10'000)
            monotonic &= cv->sourceTimeAt(t + 10'000) >= cv->sourceTimeAt(t);
        CHECK(monotonic);
        CHECK(cv->curveAt(0.5) < 0.3 && cv->curveAt(0.0) > 2.0);
        // Splitting a ramped clip keeps every frame where it was.
        const TimeUs mid = cv->duration / 2, probe = cv->duration / 2 + 100'000;
        const TimeUs before = cv->sourceTimeAt(probe);
        QString rr;
        pr.edit("split", [&] { rr = pr.splitClip(v, mid, false); });
        CHECK(!rr.isEmpty() && std::llabs(pr.findClip(rr)->sourceTimeAt(probe) - before) < 2'000);
        pr.undoStack()->undo();
        pr.undoStack()->undo();                                // back to plain 1x

        // Reverse: the first frame shown is the end of the footage; split still works.
        pr.edit("reverse", [&] { pr.setClipReverse(v, true); });
        CHECK(pr.findClip(v)->sourceTimeAt(0) > 3900'000 && pr.findClip(v)->sourceTimeAt(4 * s - 1) < 50'000);
        CHECK(pr.findClip(au)->reverse);                       // the sound too
        const TimeUs shownAt3 = pr.findClip(v)->sourceTimeAt(3 * s);
        QString rv;
        pr.edit("split", [&] { rv = pr.splitClip(v, 1 * s, false); });
        CHECK(std::llabs(pr.findClip(rv)->sourceTimeAt(3 * s) - shownAt3) < 2'000);
        pr.undoStack()->undo();
        pr.edit("forward", [&] { pr.setClipReverse(v, false); });

        // Edge trims follow the speed: trimming 1 s off a 2x clip removes 2 s of footage.
        pr.edit("speed", [&] { pr.setClipSpeed(v, 2.0, {}, {}); });
        pr.edit("trim", [&] { pr.trimEdges(v, 1 * s, 2 * s); });
        CHECK(pr.findClip(v)->in == 2 * s && pr.findClip(v)->duration == 1 * s);
        pr.undoStack()->undo();
        pr.undoStack()->undo();

        // Freeze frame: a 2 s still of second 1; the rest moves 2 s later.
        QString still;
        pr.edit("freeze", [&] { still = pr.freezeFrame(v, 1 * s, 2 * s); });
        const Clip* st = pr.findClip(still);
        CHECK(st && st->freeze && st->start == 1 * s && st->duration == 2 * s && st->in == 1 * s);
        CHECK(st && st->sourceTimeAt(2500'000) == 1 * s);       // the same frame the whole time
        CHECK(pr.findClip(w)->start == 6 * s);
        int stillTrack = -1;
        pr.findClip(still, &stillTrack);
        CHECK(stillTrack == 1 && pr.linkedClips(still).isEmpty());
        pr.undoStack()->undo();

        // Duplicate: copy (with its linked sound) right after the original.
        QString dup;
        pr.edit("dup", [&] { dup = pr.duplicateClip(w); });
        CHECK(pr.findClip(dup) && pr.findClip(dup)->start == 7 * s);
        QString dupA;
        pr.edit("dup", [&] { dupA = pr.duplicateClip(v); });   // 0-4 -> placed at 4 s
        int dt = -1;
        pr.findClip(dupA, &dt);
        CHECK(pr.findClip(dupA)->start == 4 * s && dt != 1 && pr.linkedClips(dupA).size() == 1); // V1 busy -> V2
        pr.undoStack()->undo();

        // Ripple delete closes the gap; close gap; cut range on all tracks.
        pr.edit("ripple", [&] { pr.rippleDelete(w); });
        CHECK(!pr.findClip(w) && pr.findClip(dup)->start == 4 * s);
        pr.undoStack()->undo();
        pr.edit("del", [&] { pr.removeClip(w); });
        TimeUs closed = 0;
        pr.edit("gap", [&] { closed = pr.closeGap(1, 5 * s); });
        CHECK(closed == 3 * s && pr.findClip(dup)->start == 4 * s);
        pr.undoStack()->undo();
        pr.undoStack()->undo();
        pr.edit("cut", [&] { pr.removeRange(1 * s, 2 * s, true); });
        CHECK(pr.duration() == 9 * s);                         // 10 s timeline minus 1 s
        bool paired = true;                                    // halves keep their own partners
        for (const auto& t : pr.tracks())
            for (const auto& c : t.clips)
                if (c.mediaId == ma) paired &= pr.linkedClips(c.id).size() == 1;
        CHECK(paired);
    }

    // ------------------------------------------------------------------
    section("transcripts");
    {
        constexpr TimeUs s = kUsPerSecond;
        Project pr;
        const QString mt = pr.addMedia(snd);
        QString c;
        pr.edit("add", [&] { c = pr.addClipFromMedia(mt, 2, 1 * s); });   // A1 at 1 s
        Transcript t;
        t.mediaId = mt;
        t.language = "en";
        t.segments.push_back({500'000, 1500'000, "hello world", {{500'000, 900'000, "hello"}, {1000'000, 1500'000, "world"}}, false});
        t.segments.push_back({2 * s, 3 * s, "second line", {}, false});
        pr.setTranscript(t);
        auto lines = pr.transcriptLines();
        CHECK(lines.size() == 2 && lines[0].start == 1500'000 && lines[0].end == 2500'000 && lines[1].start == 3 * s);
        CHECK(lines.size() == 2 && lines[0].words.size() == 2 && lines[0].words[1].start == 2 * s);
        pr.edit("move", [&] { pr.moveClip(c, 2, 4 * s); });
        CHECK(pr.transcriptLines().at(0).start == 4500'000);                // follows the clip
        pr.edit("speed", [&] { pr.setClipSpeed(c, 2.0, {}, {}); });
        lines = pr.transcriptLines();
        CHECK(std::llabs(lines.at(1).start - (4 * s + s)) < 2'000);         // source 2 s at 2x -> 1 s in
        pr.edit("trim", [&] { pr.trimEdges(c, 4 * s + s, pr.findClip(c)->end()); });
        CHECK(pr.transcriptLines().size() == 1);                           // first line trimmed away
        pr.undoStack()->undo();
        pr.undoStack()->undo();
        pr.edit("reverse", [&] { pr.setClipReverse(c, true); });
        lines = pr.transcriptLines();
        CHECK(lines.size() == 2 && lines[0].text == "second line");         // backwards: last line first
        pr.undoStack()->undo();
        CHECK(pr.updateTranscriptSegment(mt, 0, "hello there"));
        CHECK(pr.transcript(mt)->segments[0].edited && pr.transcript(mt)->segments[0].words.empty());
        const int ins = pr.insertTranscriptSegment(mt, 1 * s, 1500'000, "inserted");
        CHECK(ins == 1 && pr.transcript(mt)->segments.size() == 3);
        CHECK(pr.removeTranscriptSegment(mt, 1) && pr.transcript(mt)->segments.size() == 2);
        // saved with the project (JSON and SQLite)
        Project back;
        CHECK(back.fromJson(pr.toJson()) && back.transcript(mt) && back.transcript(mt)->segments[0].text == "hello there");
        const QString db = outDir + QStringLiteral("/transcript.veproj");
        QString err;
        Project fromDb;
        CHECK(ProjectStore::save(pr, db, &err) && ProjectStore::load(fromDb, db, &err));
        CHECK(fromDb.transcript(mt) && fromDb.transcript(mt)->segments.size() == 2 &&
              fromDb.transcript(mt)->segments[1].text == "second line");

        // Captions: chunks of N words with timeline times, rendered as a top layer.
        Transcript lyr;
        lyr.mediaId = mt;
        TranscriptSegment line{0, 3 * s, "one two three four five six", {}, false};
        for (int i = 0; i < 6; ++i)
            line.words.push_back({i * 500'000, i * 500'000 + 400'000, QStringList{"one", "two", "three", "four", "five", "six"}[i]});
        lyr.segments.push_back(line);
        pr.setTranscript(lyr);
        CaptionStyle cs = captionPreset("classic");
        cs.enabled = true;
        cs.wordsPerCaption = 3;
        pr.setCaptionStyle(cs);
        const auto chunks = pr.captionChunks();
        CHECK(chunks.size() == 2 && chunks[0].words.size() == 3 && chunks[1].words[0].text == "four");
        CHECK(chunks.size() == 2 && chunks[0].start == pr.findClip(c)->start && chunks[0].end == chunks[1].start);
        const QImage a1 = CaptionRenderer::render(cs, {"one", "two", "three"}, 0, 1920, 1080);
        const QImage a2 = CaptionRenderer::render(cs, {"one", "two", "three"}, 2, 1920, 1080);
        CHECK(!a1.isNull() && a1.width() < 1920 && a1.height() > 40 && a1 != a2);   // highlight moved
        const QImage big = CaptionRenderer::render(cs, QString("a very long caption that has to wrap onto more than one "
                                                               "line because it is far wider than the frame").split(' '),
                                                   -1, 1920, 1080);
        CHECK(!big.isNull() && big.width() <= 1920 && big.height() > a1.height());   // wrapped onto 2 lines
        CaptionRenderer cr;
        Layer layer;
        const TimelineSnapshot snapCap = pr.snapshot();
        CHECK(cr.layerAt(snapCap, chunks[0].start + 600'000, layer) && layer.frame && layer.transform.y > 0); // bottom half
        CHECK(!cr.layerAt(snapCap, chunks.back().end + s, layer));
        cs.enabled = false;
        pr.setCaptionStyle(cs);
        CHECK(pr.snapshot().captionChunks.empty());
        Project capBack;
        cs.enabled = true;
        cs.position = "top";
        pr.setCaptionStyle(cs);
        CHECK(capBack.fromJson(pr.toJson()) && capBack.captionStyle().enabled && capBack.captionStyle().position == "top");

        // Fonts: a different family changes the rendering; non-font files are refused.
        CaptionStyle f1 = captionPreset("classic"), f2 = f1;
        f1.font = "Segoe UI";
        f2.font = "Georgia";
        f2.italic = true;
        CHECK(CaptionRenderer::render(f1, {"Font", "test"}, -1, 1920, 1080) != CaptionRenderer::render(f2, {"Font", "test"}, -1, 1920, 1080));
        CHECK(CaptionRenderer::loadFontFile(tone).isEmpty() && CaptionRenderer::loadFontFile("C:/no/such/font.ttf").isEmpty());
        CHECK(!CaptionRenderer::suggestedFonts().isEmpty());

        // Arabic: detected, right-to-left (the first, highlighted word is on the right), Arabic font.
        CHECK(CaptionRenderer::isArabic(QStringLiteral("النهارده")) &&
              !CaptionRenderer::isArabic("hello"));
        CaptionStyle ar = captionPreset("bold");     // Arial Black has no Arabic letters
        CHECK(QFontDatabase::writingSystems(CaptionRenderer::arabicFamily(ar)).contains(QFontDatabase::Arabic));
        ar.highlightMode = "box";
        ar.highlight = QColor(255, 0, 255);
        ar.shadow = false;
        const QStringList arWords{QStringLiteral("إزيك"), QStringLiteral("يا"),
                                  QStringLiteral("صاحبي")};
        const QImage arImg = CaptionRenderer::render(ar, arWords, 0, 1920, 1080);
        int leftHits = 0, rightHits = 0;                 // where is the magenta highlight box?
        for (int y = 0; y < arImg.height(); y += 2)
            for (int x = 0; x < arImg.width(); x += 2) {
                const QColor c = arImg.pixelColor(x, y);
                if (c.red() > 220 && c.green() < 40 && c.blue() > 220) (x < arImg.width() / 2 ? leftHits : rightHits)++;
            }
        CHECK(!arImg.isNull() && rightHits > 50 && leftHits == 0);   // first Arabic word = rightmost
        const QImage enImg = CaptionRenderer::render(ar, {"one", "two", "three"}, 0, 1920, 1080);
        leftHits = rightHits = 0;
        for (int y = 0; y < enImg.height(); y += 2)
            for (int x = 0; x < enImg.width(); x += 2) {
                const QColor c = enImg.pixelColor(x, y);
                if (c.red() > 220 && c.green() < 40 && c.blue() > 220) (x < enImg.width() / 2 ? leftHits : rightHits)++;
            }
        CHECK(leftHits > 50 && rightHits == 0);                     // English: first word on the left
        // Arabic punctuation ends a caption
        Transcript art;
        art.mediaId = mt;
        TranscriptSegment arLine{0, 2 * s, "", {}, false};
        const QStringList aw{QStringLiteral("إزيك"), QStringLiteral("يا"),
                             QStringLiteral("صاحبي؟"), QStringLiteral("كويس")};
        for (int i = 0; i < aw.size(); ++i) arLine.words.push_back({i * 400'000, i * 400'000 + 350'000, aw[i]});
        art.segments.push_back(arLine);
        pr.setTranscript(art);
        CaptionStyle five = captionPreset("arabic");
        five.enabled = true;
        five.wordsPerCaption = 5;
        pr.setCaptionStyle(five);
        const auto arChunks = pr.captionChunks();
        CHECK(arChunks.size() == 2 && arChunks[0].words.size() == 3);  // broke after "؟"
        f2.arabicFontFile = "C:/fonts/cairo.ttf";
        CHECK(CaptionStyle::fromJson(f2.toJson()).arabicFontFile == "C:/fonts/cairo.ttf");
        f2.fontFile = "C:/fonts/custom.ttf";
        CHECK(CaptionStyle::fromJson(f2.toJson()).fontFile == "C:/fonts/custom.ttf" && CaptionStyle::fromJson(f2.toJson()).italic);
    }

    // ------------------------------------------------------------------
    section("audio mixer");
    {
        // Tracks: 0 = V2 (logo), 1 = V1 (clip A/B pictures), 2 = A1 (clip A's sound), 3 = A2 (tone).
        const auto peakOver = [&](double seconds) {
            return analyzeMix(project.snapshot(), 0, secondsToUs(seconds));
        };
        const QString a1 = project.tracks()[3].id, v1 = project.tracks()[2].id; // tone track, clip A sound track

        project.edit("solo", [&] { project.setTrackSolo(3, true); });
        MixLevels lv = peakOver(1.0);
        CHECK(lv.tracks.value(v1).peak[0] == 0.0f);          // silenced by solo on A2
        const float ref = lv.master.peak[0];
        CHECK(ref > 0.05f && lv.tracks.value(a1).peak[0] == ref);
        CHECK(lv.clippedSamples == 0);

        project.edit("gain", [&] { project.setTrackGain(3, -6.0); });
        lv = peakOver(1.0);
        const double ratio = lv.master.peak[0] / ref;
        std::printf("  -6 dB fader ratio: %.4f\n", ratio);
        CHECK(std::fabs(ratio - dbToGain(-6.0)) < 0.01);

        project.edit("pan", [&] { project.setTrackPan(3, 1.0); });
        lv = peakOver(1.0);
        CHECK(lv.master.peak[0] == 0.0f && lv.master.peak[1] > 0.0f); // hard right

        project.edit("master", [&] { project.setMaster({.gainDb = 0.0, .muted = true}); });
        CHECK(peakOver(1.0).master.peak[1] == 0.0f);
        project.undoStack()->undo();                          // master mute is undoable
        CHECK(!project.master().muted);

        project.edit("master", [&] { project.setMaster({.gainDb = 12.0, .muted = false}); });
        project.edit("gain", [&] { project.setTrackGain(3, 12.0); });
        CHECK(peakOver(1.0).clippedSamples > 0);              // +24 dB clips

        // Mixer state for the persistence round trip below.
        project.edit("mix", [&] {
            project.setTrackSolo(3, false);
            project.setTrackGain(3, -3.5);
            project.setTrackPan(3, -0.25);
            project.setTrackGain(2, 2.0);
            project.setMaster({.gainDb = -1.5, .muted = false});
        });
        CHECK(project.snapshot().master.gainDb == -1.5);
        bool taperOk = ChannelStrip::dbToPos(0.0) > 500 && ChannelStrip::posToDb(0) == kMinGainDb;
        for (double db = -59.0; db <= kMaxGainDb; db += 0.5)
            taperOk = taperOk && std::fabs(ChannelStrip::posToDb(ChannelStrip::dbToPos(db)) - db) < 0.25;
        CHECK(taperOk); // mixer fader taper round-trips
    }

    // ------------------------------------------------------------------
    section("SQLite + JSON persistence");
    {
        const QString dbPath = outDir + QStringLiteral("/roundtrip.veproj");
        const QString jsonPath = outDir + QStringLiteral("/roundtrip.json");
        QFile::remove(dbPath);
        QString err;
        CHECK(ProjectStore::save(project, dbPath, &err));
        CHECK(ProjectStore::save(project, dbPath, &err)); // overwrite existing db
        CHECK(ProjectStore::save(project, jsonPath, &err));
        if (!err.isEmpty()) std::fprintf(stderr, "  error: %s\n", qPrintable(err));

        Project fromDb, fromJson;
        CHECK(ProjectStore::load(fromDb, dbPath, &err));
        CHECK(ProjectStore::load(fromJson, jsonPath, &err));
        const QByteArray ref = QJsonDocument(project.toJson()).toJson();
        CHECK(QJsonDocument(fromDb.toJson()).toJson() == ref);
        CHECK(QJsonDocument(fromJson.toJson()).toJson() == ref);
        CHECK(!fromDb.isModified());
        CHECK(fromDb.master().gainDb == -1.5 && fromDb.tracks()[3].pan == -0.25 && fromDb.tracks()[3].gainDb == -3.5 && fromDb.tracks()[3].isAudio());

        Project bogus;
        CHECK(!ProjectStore::load(bogus, clipA, &err)); // not a project
    }

    // ------------------------------------------------------------------
    section("frame source (layer stack)");
    const TimelineSnapshot snap = project.snapshot();
    {
        FrameSource src;
        auto f = src.build(snap, 1'500'000);   // clip A + logo on top
        CHECK(f && f->layers.size() == 2);
        auto g = src.build(snap, 6'500'000); // clip B only (logo ends at 6s)
        CHECK(g && g->layers.size() == 1);
    }

    // ------------------------------------------------------------------
    section("OpenGL compositor (offscreen)");
    {
        QOpenGLContext ctx;
        CHECK(ctx.create());
        QOffscreenSurface surf;
        surf.setFormat(ctx.format());
        surf.create();
        CHECK(ctx.makeCurrent(&surf));
        std::printf("  GL: %s\n", reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_RENDERER)));
        QOpenGLFramebufferObject fbo(320, 180);
        GLCompositor comp;
        QString err;
        CHECK(comp.initialize(&err));
        if (!err.isEmpty()) std::fprintf(stderr, "  %s\n", qPrintable(err));

        // A solid red frame through the grayscale effect must come out grey.
        auto red = std::make_shared<VideoFrame>();
        red->width = red->height = 4;
        red->rgba.assign(4 * 4 * 4, 0);
        for (int i = 0; i < 16; ++i) { red->rgba[i * 4] = 255; red->rgba[i * 4 + 3] = 255; }
        CompositeFrame cf;
        cf.canvasWidth = 320;
        cf.canvasHeight = 180;
        cf.background = Qt::blue;
        Layer l;
        l.frame = red;
        l.effects.grayscale = true;
        cf.layers.push_back(l);

        fbo.bind();
        comp.render(cf, QRect(0, 0, 320, 180), 180);
        const QImage out = fbo.toImage();
        fbo.release();
        const QColor centre = out.pixelColor(160, 90);
        const QColor edge = out.pixelColor(5, 90);   // 4x4 square fits as 180x180 centred -> edges show bg
        std::printf("  centre rgb(%d,%d,%d) edge rgb(%d,%d,%d)\n", centre.red(), centre.green(), centre.blue(),
                    edge.red(), edge.green(), edge.blue());
        CHECK(std::abs(centre.red() - centre.green()) <= 2 && std::abs(centre.green() - centre.blue()) <= 2);
        CHECK(std::abs(centre.red() - 54) <= 3); // 0.2126 * 255
        CHECK(edge.blue() > 240 && edge.red() < 10);
        comp.cleanup();
        ctx.doneCurrent();
    }

    // ------------------------------------------------------------------
    section("export (GPU render + FFmpeg encode)");
    {
        const QStringList encoders = Exporter::availableVideoEncoders();
        std::printf("  encoders: %s\n", qPrintable(encoders.join(", ")));
        ExportSettings es;
        es.path = outDir + QStringLiteral("/export.mp4");
        es.width = 640;
        es.height = 360;
        es.fps = 30;
        es.videoEncoder = encoders.contains("libx264") ? QStringLiteral("libx264") : QStringLiteral("mpeg4");
        QString err;
        Exporter ex;
        int calls = 0;
        const bool ok = ex.run(snap, es, [&](double) { ++calls; return true; }, &err);
        if (!ok) std::fprintf(stderr, "  export error: %s\n", qPrintable(err));
        CHECK(ok);
        CHECK(calls == 210); // 7 s * 30 fps

        auto info = MediaProbe::probe(es.path, &err);
        CHECK(info.has_value());
        if (info) {
            std::printf("  output: %dx%d %.2f fps %.3fs v=%s a=%s\n", info->width, info->height, info->fps,
                        usToSeconds(info->duration), qPrintable(info->videoCodec), qPrintable(info->audioCodec));
            CHECK(info->width == 640 && info->height == 360);
            CHECK(info->hasAudio);
            CHECK(std::llabs(info->duration - 7 * kUsPerSecond) < 150'000);
        }
        // Frame at 3s comes from the grayscale clip -> R == G == B
        VideoDecoder dec;
        CHECK(dec.open(es.path));
        if (auto f = dec.frameAt(3 * kUsPerSecond)) {
            VideoDecoder::toImage(*f).save(outDir + QStringLiteral("/frame_3s.png"));
            int maxDiff = 0;
            // Sample the left strip only: the (coloured) logo layer covers the centre.
            for (int y = 0; y < f->height; y += 7)
                for (int x = 0; x < 120; x += 5) {
                    const size_t i = (size_t(y) * f->width + x) * 4;
                    maxDiff = std::max({maxDiff, std::abs(f->rgba[i] - f->rgba[i + 1]), std::abs(f->rgba[i + 1] - f->rgba[i + 2])});
                }
            std::printf("  max channel diff in grayscale section: %d\n", maxDiff);
            CHECK(maxDiff <= 12); // allow for YUV 4:2:0 rounding
        } else {
            CHECK(false);
        }
        if (auto f = dec.frameAt(1'500'000)) VideoDecoder::toImage(*f).save(outDir + QStringLiteral("/frame_1_5s.png"));
    }

    // ------------------------------------------------------------------
    section("GPU hardware decoding");
    {
        const QStringList devices = hw::availableDecodeDevices();
        std::printf("  decode devices: %s\n", qPrintable(devices.join(", ")));
        hw::setDecodeMode(QStringLiteral("none"));
        VideoDecoder cpu;
        CHECK(cpu.open(clipA));
        CHECK(!cpu.usesHardware());
        const auto ref = cpu.frameAt(2 * kUsPerSecond);
        CHECK(ref && ref->hwDevice == nullptr);

        for (const QString& dev : devices) {
            hw::setDecodeMode(dev);
            VideoDecoder gpu;
            CHECK(gpu.open(clipA));
            const auto f = gpu.frameAt(2 * kUsPerSecond);
            const bool onGpu = gpu.usesHardware() && f && f->hwDevice;
            double diff = 1e9;
            if (f && ref && f->rgba.size() == ref->rgba.size()) {
                double sum = 0;
                for (size_t i = 0; i < f->rgba.size(); i += 4) sum += std::abs(int(f->rgba[i]) - int(ref->rgba[i]));
                diff = sum / (f->rgba.size() / 4);
            }
            std::printf("  %-8s gpu=%d  mean |diff| vs CPU = %.2f\n", qPrintable(dev), onGpu ? 1 : 0, diff);
            CHECK(onGpu);
            CHECK(diff < 3.0);               // same picture as the CPU decoder
            // Sequential decode + seek still works on the GPU path.
            CHECK(gpu.frameAt(kUsPerSecond) && gpu.frameAt(kUsPerSecond + 33'333));
        }
        hw::setDecodeMode(QStringLiteral("auto"));
    }

    section("GPU hardware encoding");
    {
        QString gpuEncoder;
        for (const auto& e : hw::availableEncoders())
            if (e.hardware) {
                std::printf("  gpu encoder: %s\n", qPrintable(e.name));
                if (gpuEncoder.isEmpty()) gpuEncoder = e.name;
            }
        if (gpuEncoder.isEmpty()) {
            std::printf("  (no GPU encoder on this machine - skipped)\n");
        } else {
            ExportSettings es;
            es.path = outDir + QStringLiteral("/export_gpu.mp4");
            es.width = 1280;
            es.height = 720;
            es.fps = 30;
            es.videoEncoder = gpuEncoder;
            es.end = 3 * kUsPerSecond;
            QString err;
            Exporter ex;
            const bool ok = ex.run(snap, es, {}, &err);
            if (!ok) std::fprintf(stderr, "  gpu export error: %s\n", qPrintable(err));
            CHECK(ok);
            const auto info = MediaProbe::probe(es.path, &err);
            CHECK(info && info->width == 1280 && info->height == 720 && info->hasAudio);
            if (info) std::printf("  output: %s %.2fs\n", qPrintable(info->videoCodec), usToSeconds(info->duration));
            VideoDecoder dec;
            CHECK(dec.open(es.path) && dec.frameAt(kUsPerSecond));
        }
    }

    // ------------------------------------------------------------------
#ifdef VE_HAVE_VULKAN
    section("vulkan");
    {
        QString err;
        QVulkanInstance* inst = sharedVulkanInstance(&err);
        if (inst) {
            VulkanPreviewWindow w;
            w.setVulkanInstance(inst);
            const auto devs = w.availablePhysicalDevices();
            for (const auto& d : devs) std::printf("  device: %s\n", d.deviceName);
            CHECK(!devs.isEmpty());
        } else {
            std::printf("  Vulkan instance unavailable: %s\n", qPrintable(err));
        }
    }
#endif

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
