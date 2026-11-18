#pragma once

#include "media/StemSeparator.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace ve {

// Text to speech. Three engines:
//   system  Windows voices (SAPI, offline, nothing to install)
//   piper   Piper neural voices (offline; ~60 MB per voice, in the AI environment)
//   edge    Microsoft Edge neural voices (online: the text is sent to Microsoft;
//           has Egyptian Arabic voices)
// Results are cached in <GenericData>/VideoEditor/tts.
class TextToSpeech {
public:
    using Progress = StemSeparator::Progress;

    struct Voice {
        QString id;            // engine-specific voice name
        QString name;          // shown to the user
        QString language;      // "en", "ar-EG", ...
        QString gender;
        QString engine;
        double downloadMB = 0; // piper: model download
        bool installed = true;
    };
    struct Result {
        bool ok = false;
        bool cancelled = false;
        bool cached = false;
        QString path;          // .wav (system, piper) or .mp3 (edge)
        QString error;
    };

    static QStringList engines();                       // system, piper, edge
    static QString engineName(const QString& engine);
    static bool engineAvailable(const QString& engine); // usable on this computer (maybe after setup)
    static bool isInstalled(const QString& engine, const QString& voice = {});
    static bool isOnline(const QString& engine) { return engine == QLatin1String("edge"); }
    static QVector<Voice> voices(const QString& engine);
    // A voice for the language ("" = any), or empty.
    static QString defaultVoice(const QString& engine, const QString& language);
    static QString cacheDir();

    // Installs the engine's package (and the voice for piper) in the AI environment.
    static bool install(const QString& engine, const QString& voice, const Progress& progress, QString* error);
    // rate: 0.5 .. 2.0 (1 = normal).
    static Result synthesize(const QString& text, const QString& engine, const QString& voice, double rate,
                             const Progress& progress);
};

// Subtitle translation with Meta's NLLB-200 (separator/translate.py), offline,
// in the AI environment (PyTorch + transformers). Models in <env>/hf.
class Translator {
public:
    using Progress = StemSeparator::Progress;
    struct Result {
        bool ok = false;
        bool cancelled = false;
        QStringList texts;
        QString device;
        double seconds = 0.0;
        QString error;
    };

    static QString modelId();                            // AiModels id chosen in AI Models ("nllb-600m")
    static QString hfModel(const QString& modelId = {}); // "facebook/nllb-200-distilled-600M"
    static bool isInstalled();
    static bool isInstalled(const QString& modelId);
    static double downloadGB();
    static bool install(const Progress& progress, QString* error);
    // source / target: codes like "en", "ar", "ar-EG" (source may be "" = guess from the text).
    static Result translate(const QStringList& texts, const QString& source, const QString& target,
                            const Progress& progress);
    static bool supports(const QString& language);
};

} // namespace ve
