#pragma once

#include "media/SystemProfile.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ve {

// Every local AI model the editor can run, what each needs, and which one to
// recommend on this computer. A new model is added here (and to the script that
// runs it) so it gets a recommendation like the others.
enum class AiTask { Upscale, Transcribe, Separate, Translate };

enum class AiFit {
    Good,          // runs well here
    Slow,          // runs, but slowly or in a low-memory mode
    TooHeavy,      // not enough memory / VRAM
    Unsupported,   // needs hardware this computer does not have (e.g. an NVIDIA GPU)
};

struct AiAssessment {
    AiFit fit = AiFit::Good;
    QString reason;                // short, shown next to the model
};

struct AiModel {
    QString id;                    // stable id used in settings, scripts and the API
    AiTask task;
    QString name;
    QString description;
    QString requirements;          // human-readable hardware needs
    double downloadGB = 0.0;
    int quality = 0;               // higher = better results (and heavier)
};

// Settings derived from the hardware for one upscaling run.
struct UpscaleOptions {
    QString model;
    int scale = 2;                 // 2 or 4
    bool tileDit = false;          // FlashVSR low-VRAM switches
    bool tileVae = false;
    bool unloadDit = false;
    int tileSize = 256;
    int gpuIndex = -1;             // nvidia-smi index, -1 = automatic
};

namespace ai {

QString taskId(AiTask task);                     // "upscale", "transcribe", "separate"
QString taskName(AiTask task);                   // "Video upscaling", ...
bool taskFromId(const QString& id, AiTask* task);
QVector<AiTask> tasks();

const QVector<AiModel>& models();
QVector<AiModel> models(AiTask task);            // best quality first
const AiModel* find(const QString& id);

AiAssessment assess(const AiModel& model, const SystemProfile& hw = SystemProfile::current());
QString fitLabel(AiFit fit);                     // "Recommended", "Runs slowly", ...

// The best model that runs well here (else the best that runs at all, else the lightest).
QString recommended(AiTask task, const SystemProfile& hw = SystemProfile::current());

// The user's choice (settings), defaulting to an installed model that fits, then
// the recommendation.
QString selected(AiTask task);
void setSelected(AiTask task, const QString& modelId);
bool hasExplicitChoice(AiTask task);

bool isInstalled(const QString& modelId);

// Tasks where this computer cannot run the best model well, with the
// recommendation for each: used to prompt the user on start-up.
struct Advice {
    AiTask task;
    QString best;                  // the top model of the task
    AiAssessment bestFit;
    QString recommended;
    QString selected;
    AiAssessment selectedFit;
};
QVector<Advice> limitedTasks(const SystemProfile& hw = SystemProfile::current());

// Default upscaling settings for a source of the given size on this computer.
UpscaleOptions upscaleDefaults(const QString& modelId, int width, int height,
                               const SystemProfile& hw = SystemProfile::current());

QJsonObject toJson(const SystemProfile& hw = SystemProfile::current());   // for the API / MCP

} // namespace ai
} // namespace ve
