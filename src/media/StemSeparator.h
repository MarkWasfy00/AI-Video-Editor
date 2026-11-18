#pragma once

#include "core/Time.h"

#include <QString>

#include <functional>

namespace ve {

// Voice / music separation with Demucs (htdemucs), run by separator/separate.py
// in a private Python environment that `uv` creates on first use:
//   <GenericData>/VideoEditor/separator/.venv     Python 3.12 + PyTorch + demucs
//   <GenericData>/VideoEditor/separator/torch     model weights (~80 MB)
// Uses the GPU (CUDA) when PyTorch can, otherwise the CPU.
//
// All calls block, but keep the Qt event loop running; `progress` receives a
// status text and a fraction (0..1, or -1 when unknown) and returns false to cancel.
class StemSeparator {
public:
    using Progress = std::function<bool(const QString& text, double fraction)>;

    struct Result {
        bool ok = false;
        bool cancelled = false;
        QString voicePath;
        QString musicPath;
        QString device;           // "cuda" or "cpu"
        double seconds = 0.0;
        QString error;
    };

    static QString envDir();
    static QString pythonPath();
    static QString scriptPath();  // separate.py next to the editor (or in the source tree)
    static QString uvPath();
    static QString cacheDir();    // separated stems, one folder per source file
    // The Demucs model chosen in Tools > AI Models ("htdemucs" or "htdemucs_ft").
    static QString model();
    static bool isInstalled();                           // the chosen model
    static bool isInstalled(const QString& model);

    // One-time setup: creates the environment with uv, installs PyTorch (CUDA
    // build when an NVIDIA GPU is present) and demucs, and downloads the model.
    static bool install(const Progress& progress, QString* error);

    // Separates `input` (WAV/FLAC at 44.1 kHz) into outDir/voice.flac and outDir/music.flac.
    static Result separate(const QString& input, const QString& outDir, const Progress& progress);

    // Decodes the audio of any media file to a 44.1 kHz stereo float WAV.
    static bool decodeToWav(const QString& mediaPath, TimeUs duration, const QString& wavPath,
                            const Progress& progress, QString* error);
    static QString findScript(const QString& fileName);   // separator/<fileName>
};

// Speech / lyrics transcription with OpenAI Whisper (separator/transcribe.py),
// in the same private environment. Model weights: <env>/whisper.
class Transcriber {
public:
    using Progress = StemSeparator::Progress;
    struct Result {
        bool ok = false;
        bool cancelled = false;
        QString jsonPath;
        QString language;
        QString device;
        int segments = 0;
        double seconds = 0.0;
        QString error;
    };

    // The model chosen in Tools > AI Models: AiModels id ("whisper-turbo") and
    // Whisper's own name for it ("turbo" = large-v3-turbo).
    static QString modelId();
    static QString model();
    static QString modelFile(const QString& modelId = {});
    static double downloadGB();
    static bool isInstalled();                           // the chosen model
    static bool isInstalled(const QString& modelId);
    // Installs openai-whisper into the environment (creating it if needed) and
    // downloads the model (~1.6 GB for turbo).
    static bool install(const Progress& progress, QString* error);
    // language: "auto" or a code such as "en". Writes the JSON described in transcribe.py.
    // language: "auto", a code such as "en", or "ar-EG" (Egyptian Arabic). hints: names / slang /
    // spellings to expect.
    static Result transcribe(const QString& input, const QString& outJson, const QString& language,
                             const Progress& progress, const QString& hints = {});
};

} // namespace ve
