#pragma once

#include "media/AiModels.h"
#include "media/PythonTask.h"

#include <QString>

namespace ve {

// AI video upscaling (FlashVSR / Real-ESRGAN), run by separator/upscale.py in
// its own Python environment, created with `uv` on first use:
//   <GenericData>/VideoEditor/upscaler/.venv     Python 3.12 + PyTorch (+ Triton for FlashVSR)
//   <GenericData>/VideoEditor/upscaler/models    model weights
//   <GenericData>/VideoEditor/upscaler/flashvsr_code   pinned FlashVSR runtime
// The PyTorch build is chosen for the GPU (see torchFlavor()). It is separate from
// the voice/music environment because FlashVSR needs Triton matched to PyTorch.
class VideoUpscaler {
public:
    using Progress = TaskProgress;

    struct Result {
        bool ok = false;
        bool cancelled = false;
        QString path;
        int width = 0;
        int height = 0;
        int frames = 0;
        QString device;
        double seconds = 0.0;
        QString error;
    };

    static QString envDir();
    static QString pythonPath();
    static QString cacheDir();                 // upscaled files
    static bool isEnvInstalled();
    static bool isInstalled(const QString& modelId);
    // "cu128" (RTX 30+), "cu124" (RTX 20), "cu126" (older NVIDIA) or "cpu".
    static QString torchFlavor(const SystemProfile& hw = SystemProfile::current());

    // Creates / updates the environment if needed and downloads the model.
    static bool install(const QString& modelId, const Progress& progress, QString* error);

    // Upscales [start, start + duration) of `input` (duration 0 = to the end) into `output` (MP4, H.264,
    // with the source's sound).
    static Result upscale(const QString& input, const QString& output, double start, double duration,
                          const UpscaleOptions& options, const Progress& progress);
};

} // namespace ve
