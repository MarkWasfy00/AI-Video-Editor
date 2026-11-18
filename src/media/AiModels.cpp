#include "media/AiModels.h"

#include "media/Speech.h"
#include "media/StemSeparator.h"
#include "media/VideoUpscaler.h"

#include <QJsonArray>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace ve::ai {

namespace {

QString gb(double v)
{
    return std::abs(v - std::round(v)) < 0.05 ? QString::number(std::lround(v)) : QString::number(v, 'f', 1);
}

AiAssessment good(const QString& why = {}) { return {AiFit::Good, why}; }
AiAssessment slow(const QString& why) { return {AiFit::Slow, why}; }
AiAssessment heavy(const QString& why) { return {AiFit::TooHeavy, why}; }
AiAssessment unsupported(const QString& why) { return {AiFit::Unsupported, why}; }

// FlashVSR: diffusion model (Wan 2.1 1.3B) + Triton "sparse sage" attention, CUDA only.
AiAssessment assessFlashVsr(const QString& mode, double ram, double vram, double cc, const SystemProfile& hw)
{
    if (!hw.hasNvidia()) return unsupported(QStringLiteral("Needs an NVIDIA GPU (RTX 20-series or newer)"));
    if (cc < 7.5) return unsupported(QStringLiteral("Needs an RTX 20-series or newer GPU (this one is too old)"));
    if (ram < 12) return heavy(QStringLiteral("Needs 16 GB of RAM (this computer has %1 GB)").arg(gb(hw.ramGB)));
    const double goodVram = mode == QLatin1String("full") ? 20 : mode == QLatin1String("tiny") ? 12 : 10;
    const double minVram = mode == QLatin1String("full") ? 12 : 8;
    if (vram < minVram)
        return heavy(QStringLiteral("Needs %1 GB of VRAM, %2 GB to run well (this GPU has %3 GB)")
                         .arg(gb(minVram), gb(goodVram), gb(hw.vramGB())));
    if (vram < goodVram) return slow(QStringLiteral("Low-VRAM mode (processed in tiles): slow"));
    if (ram < 16) return slow(QStringLiteral("Only %1 GB of RAM: may swap, 16 GB recommended").arg(gb(hw.ramGB)));
    if (cc < 8.0) return slow(QStringLiteral("RTX 20-series: works, but slower (no bfloat16)"));
    return good();
}

AiAssessment assessImpl(const AiModel& m, const SystemProfile& hw)
{
    // Small margins: a "16 GB" PC reports 15.9 GB, an "8 GB" GPU 7.99 GB.
    const double ram = hw.ramGB + 0.6;
    const double vram = hw.vramGB() + 0.3;
    const double cc = hw.computeCapability();
    const bool nv = hw.hasNvidia();

    if (m.id == QLatin1String("flashvsr-full")) return assessFlashVsr(QStringLiteral("full"), ram, vram, cc, hw);
    if (m.id == QLatin1String("flashvsr-tiny")) return assessFlashVsr(QStringLiteral("tiny"), ram, vram, cc, hw);
    if (m.id == QLatin1String("flashvsr-tiny-long")) return assessFlashVsr(QStringLiteral("tiny-long"), ram, vram, cc, hw);
    if (m.id == QLatin1String("realesrgan-x4plus")) {
        if (nv && vram >= 4) return good();
        if (nv) return slow(QStringLiteral("Small GPU: slower"));
        if (hw.appleSilicon && ram >= 8) return good(QStringLiteral("Uses the Apple GPU"));
        if (ram >= 8) return slow(QStringLiteral("No NVIDIA GPU: runs on the CPU, several seconds per frame"));
        return heavy(QStringLiteral("Needs 8 GB of RAM without a GPU"));
    }
    if (m.id == QLatin1String("realesrgan-general")) {
        if (nv || hw.appleSilicon || ram >= 4) return good(nv ? QString() : QStringLiteral("Light enough for the CPU"));
        return slow(QStringLiteral("Very little RAM"));
    }
    if (m.id == QLatin1String("whisper-turbo")) {
        if (nv && vram >= 6) return good();
        if (nv && vram >= 4.5) return slow(QStringLiteral("Tight on VRAM: may fall back to the CPU"));
        if (ram >= 16) return slow(QStringLiteral("No suitable GPU: runs on the CPU, about real time"));
        return heavy(QStringLiteral("Needs a GPU with 6 GB VRAM, or 16 GB of RAM"));
    }
    if (m.id == QLatin1String("whisper-small")) {
        if ((nv && vram >= 2) || ram >= 8) return good(nv ? QString() : QStringLiteral("Fine on the CPU"));
        if (ram >= 4) return slow(QStringLiteral("Little RAM: slow"));
        return heavy(QStringLiteral("Needs 4 GB of RAM"));
    }
    if (m.id == QLatin1String("whisper-base")) {
        if (ram >= 4) return good(QStringLiteral("Fast on any computer"));
        return slow(QStringLiteral("Very little RAM"));
    }
    if (m.id == QLatin1String("htdemucs_ft")) {
        if (nv && vram >= 4) return good();
        if (ram >= 8) return slow(QStringLiteral("No suitable GPU: about 4x slower than Demucs on the CPU"));
        return heavy(QStringLiteral("Needs 8 GB of RAM without a GPU"));
    }
    if (m.id == QLatin1String("htdemucs")) {
        if (nv || ram >= 4) return good(nv ? QString() : QStringLiteral("Fine on the CPU"));
        return slow(QStringLiteral("Very little RAM"));
    }
    if (m.id == QLatin1String("nllb-1.3b")) {
        if (nv && vram >= 6) return good();
        if (ram >= 16) return slow(QStringLiteral("No suitable GPU: runs on the CPU, a few seconds per line"));
        return heavy(QStringLiteral("Needs a GPU with 6 GB VRAM, or 16 GB of RAM"));
    }
    if (m.id == QLatin1String("nllb-600m")) {
        if ((nv && vram >= 3) || ram >= 8) return good(nv ? QString() : QStringLiteral("Fine on the CPU"));
        if (ram >= 4) return slow(QStringLiteral("Little RAM: slow"));
        return heavy(QStringLiteral("Needs 4 GB of RAM"));
    }
    return good();
}

QString settingsKey(AiTask task) { return QStringLiteral("ai/models/") + taskId(task); }

} // namespace

QString taskId(AiTask task)
{
    switch (task) {
    case AiTask::Upscale: return QStringLiteral("upscale");
    case AiTask::Transcribe: return QStringLiteral("transcribe");
    case AiTask::Separate: return QStringLiteral("separate");
    case AiTask::Translate: return QStringLiteral("translate");
    }
    return {};
}

QString taskName(AiTask task)
{
    switch (task) {
    case AiTask::Upscale: return QObject::tr("Video upscaling");
    case AiTask::Transcribe: return QObject::tr("Transcription");
    case AiTask::Separate: return QObject::tr("Voice / music separation");
    case AiTask::Translate: return QObject::tr("Subtitle translation");
    }
    return {};
}

bool taskFromId(const QString& id, AiTask* task)
{
    for (AiTask t : tasks())
        if (taskId(t) == id) {
            if (task) *task = t;
            return true;
        }
    return false;
}

QVector<AiTask> tasks() { return {AiTask::Upscale, AiTask::Transcribe, AiTask::Separate, AiTask::Translate}; }

const QVector<AiModel>& models()
{
    static const QVector<AiModel> all = {
        {QStringLiteral("flashvsr-full"), AiTask::Upscale, QStringLiteral("FlashVSR v1.1 (Full)"),
         QObject::tr("Diffusion-based video super-resolution with the Wan VAE decoder: the sharpest, most "
                     "detailed results."),
         QObject::tr("NVIDIA RTX GPU with 20 GB+ VRAM (12 GB in low-VRAM mode), 16 GB RAM"), 7.0, 50},
        {QStringLiteral("flashvsr-tiny"), AiTask::Upscale, QStringLiteral("FlashVSR v1.1 (Tiny)"),
         QObject::tr("FlashVSR with its small decoder: nearly the same quality, faster, less VRAM."),
         QObject::tr("NVIDIA RTX GPU with 12 GB+ VRAM (8 GB in low-VRAM mode), 16 GB RAM"), 7.0, 45},
        {QStringLiteral("flashvsr-tiny-long"), AiTask::Upscale, QStringLiteral("FlashVSR v1.1 (Tiny, long video)"),
         QObject::tr("FlashVSR streamed frame-block by frame-block: the lowest-VRAM FlashVSR, for long videos."),
         QObject::tr("NVIDIA RTX GPU with 10 GB+ VRAM (8 GB in low-VRAM mode), 16 GB RAM"), 7.0, 40},
        {QStringLiteral("realesrgan-x4plus"), AiTask::Upscale, QStringLiteral("Real-ESRGAN x4plus"),
         QObject::tr("Classic GAN upscaler, frame by frame: good detail on real footage, no temporal model."),
         QObject::tr("Any NVIDIA GPU with 4 GB+ VRAM (or the CPU, slowly)"), 0.07, 20},
        {QStringLiteral("realesrgan-general"), AiTask::Upscale, QStringLiteral("Real-ESRGAN General (compact)"),
         QObject::tr("Small, fast upscaler that runs on any computer, even without a GPU."),
         QObject::tr("Any computer (4 GB RAM)"), 0.005, 10},

        {QStringLiteral("whisper-turbo"), AiTask::Transcribe, QStringLiteral("Whisper large-v3 turbo"),
         QObject::tr("The most accurate transcription and lyrics, all languages."),
         QObject::tr("NVIDIA GPU with 6 GB+ VRAM, or 16 GB RAM (slow on the CPU)"), 1.6, 30},
        {QStringLiteral("whisper-small"), AiTask::Transcribe, QStringLiteral("Whisper small"),
         QObject::tr("Good accuracy for clear speech; fine without a GPU."),
         QObject::tr("GPU with 2 GB VRAM, or 8 GB RAM"), 0.46, 20},
        {QStringLiteral("whisper-base"), AiTask::Transcribe, QStringLiteral("Whisper base"),
         QObject::tr("Fast and light; less accurate on songs, accents and noise."),
         QObject::tr("Any computer (4 GB RAM)"), 0.14, 10},

        {QStringLiteral("htdemucs_ft"), AiTask::Separate, QStringLiteral("Demucs (fine-tuned)"),
         QObject::tr("Four fine-tuned Demucs models: the cleanest voice / music split, 4x the work."),
         QObject::tr("NVIDIA GPU with 4 GB+ VRAM (or 8 GB RAM, slowly)"), 0.32, 20},
        {QStringLiteral("htdemucs"), AiTask::Separate, QStringLiteral("Demucs"),
         QObject::tr("Hybrid Transformer Demucs: clean separation, quick even on the CPU."),
         QObject::tr("Any computer (4 GB RAM)"), 0.08, 10},

        {QStringLiteral("nllb-1.3b"), AiTask::Translate, QStringLiteral("NLLB-200 1.3B (distilled)"),
         QObject::tr("Meta's No Language Left Behind: the better subtitle translations, 200 languages incl. Egyptian Arabic."),
         QObject::tr("NVIDIA GPU with 6 GB+ VRAM, or 16 GB RAM"), 5.5, 20},
        {QStringLiteral("nllb-600m"), AiTask::Translate, QStringLiteral("NLLB-200 600M (distilled)"),
         QObject::tr("Smaller NLLB: good subtitle translations, light enough for the CPU."),
         QObject::tr("GPU with 3 GB VRAM, or 8 GB RAM"), 2.5, 10},
    };
    return all;
}

QVector<AiModel> models(AiTask task)
{
    QVector<AiModel> out;
    for (const AiModel& m : models())
        if (m.task == task) out << m;
    std::stable_sort(out.begin(), out.end(), [](const AiModel& a, const AiModel& b) { return a.quality > b.quality; });
    return out;
}

const AiModel* find(const QString& id)
{
    for (const AiModel& m : models())
        if (m.id == id) return &m;
    return nullptr;
}

AiAssessment assess(const AiModel& model, const SystemProfile& hw)
{
    AiAssessment a = assessImpl(model, hw);
    // Not downloaded yet and no room for it (models plus PyTorch).
    if (a.fit <= AiFit::Slow && hw.diskFreeGB > 0 && !isInstalled(model.id) && hw.diskFreeGB < model.downloadGB + 3.5)
        a = heavy(QStringLiteral("Needs about %1 GB of free disk space (%2 GB free)")
                      .arg(gb(model.downloadGB + 3.5), gb(hw.diskFreeGB)));
    return a;
}

QString fitLabel(AiFit fit)
{
    switch (fit) {
    case AiFit::Good: return QObject::tr("Runs well");
    case AiFit::Slow: return QObject::tr("Runs slowly");
    case AiFit::TooHeavy: return QObject::tr("Too heavy for this computer");
    case AiFit::Unsupported: return QObject::tr("Not supported on this computer");
    }
    return {};
}

QString recommended(AiTask task, const SystemProfile& hw)
{
    const QVector<AiModel> list = models(task);
    for (AiFit want : {AiFit::Good, AiFit::Slow})
        for (const AiModel& m : list)
            if (assess(m, hw).fit == want) return m.id;
    return list.isEmpty() ? QString() : list.last().id;
}

bool hasExplicitChoice(AiTask task)
{
    const AiModel* m = find(QSettings().value(settingsKey(task)).toString());
    return m && m->task == task;
}

QString selected(AiTask task)
{
    const QString saved = QSettings().value(settingsKey(task)).toString();
    if (const AiModel* m = find(saved); m && m->task == task) return saved;
    // No choice yet: keep an installed model that runs well here (no surprise download),
    // or an installed slower one that is at least as good as the recommendation.
    const QString rec = recommended(task);
    const AiModel* recModel = find(rec);
    const QVector<AiModel> list = models(task);   // best first
    for (const AiModel& m : list)
        if (isInstalled(m.id) && assess(m).fit == AiFit::Good) return m.id;
    for (const AiModel& m : list)
        if (isInstalled(m.id) && assess(m).fit == AiFit::Slow && (!recModel || m.quality >= recModel->quality)) return m.id;
    return rec;
}

void setSelected(AiTask task, const QString& modelId)
{
    if (const AiModel* m = find(modelId); m && m->task == task) QSettings().setValue(settingsKey(task), modelId);
}

bool isInstalled(const QString& modelId)
{
    const AiModel* m = find(modelId);
    if (!m) return false;
    switch (m->task) {
    case AiTask::Upscale: return VideoUpscaler::isInstalled(modelId);
    case AiTask::Transcribe: return Transcriber::isInstalled(modelId);
    case AiTask::Separate: return StemSeparator::isInstalled(modelId);
    case AiTask::Translate: return Translator::isInstalled(modelId);
    }
    return false;
}

QVector<Advice> limitedTasks(const SystemProfile& hw)
{
    QVector<Advice> out;
    for (AiTask t : tasks()) {
        const QVector<AiModel> list = models(t);
        if (list.isEmpty()) continue;
        Advice a{t, list.first().id, assess(list.first(), hw), recommended(t, hw), selected(t), {}};
        const AiModel* sel = find(a.selected);
        a.selectedFit = sel ? assess(*sel, hw) : AiAssessment{};
        if (a.bestFit.fit != AiFit::Good || a.selectedFit.fit > AiFit::Slow) out << a;
    }
    return out;
}

UpscaleOptions upscaleDefaults(const QString& modelId, int width, int height, const SystemProfile& hw)
{
    UpscaleOptions o;
    o.model = modelId;
    const int maxDim = std::max(width, height);
    o.scale = maxDim > 0 && maxDim * 4 <= 3840 ? 4 : 2;   // 4x up to 4K output
    const double vram = hw.vramGB() + 0.3;
    if (const NvidiaGpu* g = hw.bestGpu()) o.gpuIndex = g->index;
    if (modelId.startsWith(QLatin1String("flashvsr"))) {
        if (vram < 12) o.scale = 2;                         // FlashVSR's own advice for limited VRAM
        const double outPixels = double(width) * o.scale * double(height) * o.scale;
        if (vram < 12) {
            o.tileDit = o.tileVae = o.unloadDit = true;
        } else if (vram < 16) {
            o.tileDit = o.tileVae = true;
        } else if (vram < 24 && outPixels > 1920.0 * 1080.0 * 1.1) {
            o.tileVae = true;
            o.tileDit = vram < 20;
        }
        if (modelId == QLatin1String("flashvsr-full") && vram < 20) o.tileVae = true;
    }
    return o;
}

QJsonObject toJson(const SystemProfile& hw)
{
    QJsonArray tasksJson;
    for (AiTask t : tasks()) {
        const QString rec = recommended(t, hw);
        QJsonArray list;
        for (const AiModel& m : models(t)) {
            const AiAssessment a = assess(m, hw);
            list.append(QJsonObject{{"id", m.id}, {"name", m.name}, {"description", m.description},
                                    {"requirements", m.requirements}, {"downloadGB", m.downloadGB},
                                    {"installed", isInstalled(m.id)}, {"fit", fitLabel(a.fit)}, {"reason", a.reason},
                                    {"recommended", m.id == rec}});
        }
        tasksJson.append(QJsonObject{{"task", taskId(t)}, {"name", taskName(t)}, {"selected", selected(t)},
                                     {"recommended", rec}, {"models", list}});
    }
    return {{"hardware", hw.toJson()}, {"tasks", tasksJson}};
}

} // namespace ve::ai
