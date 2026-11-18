#include "media/Speech.h"

#include "core/Project.h"
#include "media/AiModels.h"
#include "media/PythonTask.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

namespace ve {

namespace {

QProcessEnvironment aiEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString dir = StemSeparator::envDir();
    env.insert(QStringLiteral("TORCH_HOME"), QDir::toNativeSeparators(dir + QStringLiteral("/torch")));
    env.insert(QStringLiteral("HF_HOME"), QDir::toNativeSeparators(dir + QStringLiteral("/hf")));
    env.insert(QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PIPER_VOICES"), QDir::toNativeSeparators(dir + QStringLiteral("/piper")));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.remove(QStringLiteral("PYTHONHOME"));
    return env;
}

bool sitePackage(const QString& package)
{
#ifdef Q_OS_WIN
    return QFileInfo::exists(StemSeparator::envDir() + QStringLiteral("/.venv/Lib/site-packages/") + package + QStringLiteral("/__init__.py"));
#else
    QDirIterator it(StemSeparator::envDir() + QStringLiteral("/.venv/lib"), {QStringLiteral("__init__.py")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        if (it.next().endsWith(QLatin1Char('/') + package + QStringLiteral("/__init__.py"))) return true;
    return false;
#endif
}

QString marker(const QString& name) { return StemSeparator::envDir() + QStringLiteral("/models/") + name + QStringLiteral(".ok"); }

void writeMarker(const QString& name)
{
    QDir().mkpath(StemSeparator::envDir() + QStringLiteral("/models"));
    QFile f(marker(name));
    if (f.open(QIODevice::WriteOnly)) f.write("ok");
}

// Creates the private Python environment (no packages) if it is not there yet.
bool ensureEnv(const TextToSpeech::Progress& progress, QString* error)
{
    if (QFileInfo::exists(StemSeparator::pythonPath())) return true;
    const QString uv = StemSeparator::uvPath();
    if (uv.isEmpty()) {
        if (error) *error = QObject::tr("The installer needs 'uv' (https://docs.astral.sh/uv/). Install it, then try again.");
        return false;
    }
    QDir().mkpath(StemSeparator::envDir());
    QString status = QObject::tr("Creating the Python environment...");
    double fraction = 0.05;
    const PythonTaskResult r = runPythonTask(uv, {QStringLiteral("venv"), QStringLiteral("--allow-existing"), QStringLiteral("--python"),
                                                  QStringLiteral("3.12"), StemSeparator::envDir() + QStringLiteral("/.venv")},
                                             aiEnv(), [&](const QString& l) { status = l.left(160); }, progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (error) *error = r.cancelled ? QObject::tr("Cancelled") : r.stderrTail.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

bool pipInstall(const QStringList& packages, const QString& what, const TextToSpeech::Progress& progress, QString* error)
{
    const QString uv = StemSeparator::uvPath();
    if (uv.isEmpty()) {
        if (error) *error = QObject::tr("The installer needs 'uv' (https://docs.astral.sh/uv/). Install it, then try again.");
        return false;
    }
    QString status = QObject::tr("Installing %1...").arg(what);
    double fraction = 0.2;
    const PythonTaskResult r = runPythonTask(
        uv, QStringList{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), StemSeparator::pythonPath()} + packages,
        aiEnv(), [&](const QString& l) { status = l.left(160); }, progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (error) *error = r.cancelled ? QObject::tr("Cancelled")
                                        : QObject::tr("Installing %1 failed:\n%2").arg(what, r.stderrTail.join(QLatin1Char('\n')));
        return false;
    }
    return true;
}

// Runs one of the separator/*.py scripts; collects its JSON lines.
struct ScriptRun {
    bool ok = false;
    bool cancelled = false;
    QJsonObject done;
    QString error;
};

ScriptRun runScript(const QString& script, const QStringList& args, const QString& startStatus,
                    const std::function<QString(const QJsonObject&, double*)>& onProgress,
                    const TextToSpeech::Progress& progress)
{
    ScriptRun res;
    const QString path = StemSeparator::findScript(script);
    if (path.isEmpty()) {
        res.error = QObject::tr("separator/%1 is missing next to the editor.").arg(script);
        return res;
    }
    QString status = startStatus;
    double fraction = -1;
    QString pyError;
    const PythonTaskResult r = runPythonTask(
        StemSeparator::pythonPath(), QStringList{QStringLiteral("-I"), path} + args, aiEnv(),
        [&](const QString& l) {
            const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
            const QString type = o.value("type").toString();
            if (type == QLatin1String("progress")) {
                if (onProgress) status = onProgress(o, &fraction);
            } else if (type == QLatin1String("done")) {
                res.ok = true;
                res.done = o;
            } else if (type == QLatin1String("error")) {
                pyError = o.value("message").toString();
            }
        },
        progress, &status, &fraction);
    res.cancelled = r.cancelled;
    if (r.cancelled) {
        res.ok = false;
        res.error = QObject::tr("Cancelled");
    } else if (!res.ok) {
        res.error = !pyError.isEmpty() ? pyError : r.stderrTail.join(QLatin1Char('\n'));
    }
    return res;
}

// --- Piper voices (rhasspy/piper-voices on Hugging Face)
struct PiperVoiceInfo {
    const char* id;
    const char* name;
    const char* language;
    const char* gender;
    double mb;
};
const PiperVoiceInfo kPiperVoices[] = {
    {"en_US-lessac-medium", "Lessac (US English)", "en", "female", 63},
    {"en_US-ryan-high", "Ryan (US English)", "en", "male", 121},
    {"en_GB-alba-medium", "Alba (British English)", "en", "female", 63},
    {"ar_JO-kareem-medium", "Kareem (Arabic)", "ar", "male", 63},
    {"es_ES-davefx-medium", "Davefx (Spanish)", "es", "male", 63},
    {"fr_FR-siwis-medium", "Siwis (French)", "fr", "female", 63},
    {"de_DE-thorsten-medium", "Thorsten (German)", "de", "male", 63},
    {"it_IT-paola-medium", "Paola (Italian)", "it", "female", 63},
    {"pt_BR-faber-medium", "Faber (Brazilian Portuguese)", "pt", "male", 63},
    {"ru_RU-irina-medium", "Irina (Russian)", "ru", "female", 63},
    {"tr_TR-dfki-medium", "DFKI (Turkish)", "tr", "male", 63},
    {"zh_CN-huayan-medium", "Huayan (Chinese)", "zh", "female", 63},
};

// --- Edge neural voices (a selection; any Edge voice name works)
struct EdgeVoiceInfo {
    const char* id;
    const char* language;
    const char* gender;
};
const EdgeVoiceInfo kEdgeVoices[] = {
    {"en-US-AriaNeural", "en", "female"},      {"en-US-GuyNeural", "en", "male"},
    {"en-GB-SoniaNeural", "en", "female"},     {"ar-EG-SalmaNeural", "ar-EG", "female"},
    {"ar-EG-ShakirNeural", "ar-EG", "male"},   {"ar-SA-ZariyahNeural", "ar", "female"},
    {"ar-SA-HamedNeural", "ar", "male"},       {"es-ES-ElviraNeural", "es", "female"},
    {"es-MX-JorgeNeural", "es", "male"},       {"fr-FR-DeniseNeural", "fr", "female"},
    {"fr-FR-HenriNeural", "fr", "male"},       {"de-DE-KatjaNeural", "de", "female"},
    {"de-DE-ConradNeural", "de", "male"},      {"it-IT-ElsaNeural", "it", "female"},
    {"pt-BR-FranciscaNeural", "pt", "female"}, {"ru-RU-SvetlanaNeural", "ru", "female"},
    {"tr-TR-EmelNeural", "tr", "female"},      {"hi-IN-SwaraNeural", "hi", "female"},
    {"ja-JP-NanamiNeural", "ja", "female"},    {"ko-KR-SunHiNeural", "ko", "female"},
    {"zh-CN-XiaoxiaoNeural", "zh", "female"},  {"zh-CN-YunxiNeural", "zh", "male"},
};

QString piperDir() { return StemSeparator::envDir() + QStringLiteral("/piper"); }
bool piperVoiceInstalled(const QString& v) { return QFileInfo::exists(piperDir() + QLatin1Char('/') + v + QStringLiteral(".onnx")); }

// Windows voices, listed once per run.
QVector<TextToSpeech::Voice> systemVoices()
{
    static QMutex mutex;
    static bool listed = false;
    static QVector<TextToSpeech::Voice> cache;
    QMutexLocker lock(&mutex);
    if (listed) return cache;
    listed = true;
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
             QStringLiteral("[Console]::OutputEncoding=[Text.Encoding]::UTF8; Add-Type -AssemblyName System.Speech; "
                            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
                            "$s.GetInstalledVoices() | Where-Object { $_.Enabled } | ForEach-Object { $_.VoiceInfo } | "
                            "ForEach-Object { \"$($_.Name)`t$($_.Culture)`t$($_.Gender)\" }")});
    if (p.waitForFinished(20000)) {
        for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList f = line.trimmed().split(QLatin1Char('\t'));
            if (f.size() < 3 || f[0].isEmpty()) continue;
            QString lang = f[1].section(QLatin1Char('-'), 0, 0).toLower();
            if (f[1].compare(QLatin1String("ar-EG"), Qt::CaseInsensitive) == 0) lang = QStringLiteral("ar-EG");
            cache.push_back({f[0], f[0] + QStringLiteral(" (") + f[1] + QLatin1Char(')'), lang, f[2].toLower(),
                             QStringLiteral("system"), 0, true});
        }
    }
#endif
    return cache;
}

bool sameLanguage(const QString& voiceLang, const QString& wanted)
{
    if (wanted.isEmpty()) return true;
    if (voiceLang == wanted) return true;
    return voiceLang.section(QLatin1Char('-'), 0, 0) == wanted.section(QLatin1Char('-'), 0, 0);
}

} // namespace

// ---------------------------------------------------------------------------
// Text to speech
// ---------------------------------------------------------------------------
QStringList TextToSpeech::engines() { return {QStringLiteral("system"), QStringLiteral("piper"), QStringLiteral("edge")}; }

QString TextToSpeech::engineName(const QString& e)
{
    if (e == QLatin1String("system")) return QObject::tr("Windows voices (offline)");
    if (e == QLatin1String("piper")) return QObject::tr("Piper neural voices (offline)");
    if (e == QLatin1String("edge")) return QObject::tr("Microsoft Edge neural voices (online)");
    return e;
}

bool TextToSpeech::engineAvailable(const QString& e)
{
#ifdef Q_OS_WIN
    if (e == QLatin1String("system")) return true;
#else
    if (e == QLatin1String("system")) return false;
#endif
    return (e == QLatin1String("piper") || e == QLatin1String("edge")) &&
           (QFileInfo::exists(StemSeparator::pythonPath()) || !StemSeparator::uvPath().isEmpty());
}

bool TextToSpeech::isInstalled(const QString& e, const QString& voice)
{
    if (e == QLatin1String("system")) return engineAvailable(e);
    if (!QFileInfo::exists(StemSeparator::pythonPath())) return false;
    if (e == QLatin1String("piper"))
        return sitePackage(QStringLiteral("piper")) && (voice.isEmpty() || piperVoiceInstalled(voice));
    if (e == QLatin1String("edge")) return sitePackage(QStringLiteral("edge_tts"));
    return false;
}

QVector<TextToSpeech::Voice> TextToSpeech::voices(const QString& e)
{
    QVector<Voice> out;
    if (e == QLatin1String("system")) return systemVoices();
    if (e == QLatin1String("piper")) {
        for (const auto& v : kPiperVoices)
            out.push_back({QLatin1String(v.id), QLatin1String(v.name), QLatin1String(v.language), QLatin1String(v.gender),
                           e, v.mb, piperVoiceInstalled(QLatin1String(v.id))});
    } else if (e == QLatin1String("edge")) {
        for (const auto& v : kEdgeVoices) {
            QString name = QLatin1String(v.id);
            name.chop(6);   // "Neural"
            out.push_back({QLatin1String(v.id), name.section(QLatin1Char('-'), 2) + QStringLiteral(" (") + languageName(QLatin1String(v.language)) +
                               QLatin1Char(')'),
                           QLatin1String(v.language), QLatin1String(v.gender), e, 0, isInstalled(e)});
        }
    }
    return out;
}

QString TextToSpeech::defaultVoice(const QString& e, const QString& language)
{
    const auto list = voices(e);
    for (const Voice& v : list)       // exact language first (ar-EG before ar)
        if (!language.isEmpty() && v.language == language) return v.id;
    for (const Voice& v : list)
        if (sameLanguage(v.language, language)) return v.id;
    return list.isEmpty() || !language.isEmpty() ? QString() : list.first().id;
}

QString TextToSpeech::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/VideoEditor/tts");
}

bool TextToSpeech::install(const QString& e, const QString& voice, const Progress& progress, QString* error)
{
    if (e == QLatin1String("system")) {
        if (!engineAvailable(e) && error) *error = QObject::tr("Windows voices are only available on Windows.");
        return engineAvailable(e);
    }
    if (!ensureEnv(progress, error)) return false;
    if (e == QLatin1String("edge")) {
        return sitePackage(QStringLiteral("edge_tts")) ||
               pipInstall({QStringLiteral("edge-tts==7.0.2")}, QObject::tr("Edge voices"), progress, error);
    }
    if (e != QLatin1String("piper")) {
        if (error) *error = QObject::tr("Unknown speech engine '%1'").arg(e);
        return false;
    }
    if (!sitePackage(QStringLiteral("piper")) &&
        !pipInstall({QStringLiteral("piper-tts==1.3.0")}, QObject::tr("Piper"), progress, error))
        return false;
    if (voice.isEmpty() || piperVoiceInstalled(voice)) return true;
    const ScriptRun r = runScript(QStringLiteral("tts.py"), {QStringLiteral("--download-only"), QStringLiteral("--engine"), e,
                                                             QStringLiteral("--voice"), voice},
                                  QObject::tr("Downloading the voice %1...").arg(voice),
                                  [&](const QJsonObject& o, double* f) {
                                      *f = o.value("value").toDouble(-1);
                                      return QObject::tr("Downloading the voice %1... %2%").arg(voice).arg(int(*f * 100));
                                  },
                                  progress);
    if (!r.ok && error) *error = r.error;
    return r.ok && piperVoiceInstalled(voice);
}

TextToSpeech::Result TextToSpeech::synthesize(const QString& textIn, const QString& e, const QString& voiceIn, double rate,
                                              const Progress& progress)
{
    Result res;
    const QString text = textIn.trimmed();
    if (text.isEmpty()) {
        res.error = QObject::tr("There is no text to speak.");
        return res;
    }
    rate = std::clamp(rate, 0.5, 2.0);
    QString voice = voiceIn;
    if (voice.isEmpty()) voice = defaultVoice(e, {});
    if (voice.isEmpty() && e != QLatin1String("system")) {
        res.error = QObject::tr("No voice chosen.");
        return res;
    }
    QDir().mkpath(cacheDir());
    const QByteArray key = (e + QLatin1Char('|') + voice + QLatin1Char('|') + QString::number(rate, 'f', 2) + QLatin1Char('|') + text).toUtf8();
    const QString base = cacheDir() + QLatin1Char('/') +
                         QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(20));
    const QString out = base + (e == QLatin1String("edge") ? QStringLiteral(".mp3") : QStringLiteral(".wav"));
    if (QFileInfo(out).size() > 256) {
        res.ok = res.cached = true;
        res.path = out;
        return res;
    }

    if (e == QLatin1String("system")) {
#ifdef Q_OS_WIN
        // Parameters go through a JSON file: nothing from the text reaches the command line.
        QTemporaryDir tmp;
        const QString params = tmp.filePath(QStringLiteral("params.json"));
        QFile pf(params);
        if (!pf.open(QIODevice::WriteOnly)) {
            res.error = pf.errorString();
            return res;
        }
        const int sapiRate = std::clamp(int(std::lround(10.0 * std::log(rate) / std::log(3.0))), -10, 10);
        pf.write(QJsonDocument(QJsonObject{{"text", text}, {"voice", voice}, {"rate", sapiRate},
                                           {"out", QDir::toNativeSeparators(out)}}).toJson());
        pf.close();
        const QString script = QStringLiteral(
            "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Speech; "
            "$p = Get-Content -Raw -Encoding UTF8 -LiteralPath $env:VE_TTS_PARAMS | ConvertFrom-Json; "
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
            "if ($p.voice) { $s.SelectVoice($p.voice) }; $s.Rate = [int]$p.rate; "
            "$s.SetOutputToWaveFile($p.out); $s.Speak([string]$p.text); $s.Dispose()");
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("VE_TTS_PARAMS"), QDir::toNativeSeparators(params));
        proc.setProcessEnvironment(env);
        QString status = QObject::tr("Speaking...");
        double fraction = -1;
        if (progress) progress(status, fraction);
        proc.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                                                      QStringLiteral("-Command"), script});
        if (!proc.waitForFinished(120000) || proc.exitCode() != 0 || QFileInfo(out).size() <= 256) {
            QFile::remove(out);
            res.error = QObject::tr("Windows speech failed: %1").arg(QString::fromLocal8Bit(proc.readAllStandardError()).trimmed().left(400));
            return res;
        }
        res.ok = true;
        res.path = out;
        return res;
#else
        res.error = QObject::tr("Windows voices are only available on Windows.");
        return res;
#endif
    }

    if (!isInstalled(e, e == QLatin1String("piper") ? voice : QString())) {
        res.error = QObject::tr("%1 is not installed yet.").arg(engineName(e));
        return res;
    }
    QTemporaryDir tmp;
    const QString textFile = tmp.filePath(QStringLiteral("text.txt"));
    QFile tf(textFile);
    if (!tf.open(QIODevice::WriteOnly)) {
        res.error = tf.errorString();
        return res;
    }
    tf.write(text.toUtf8());
    tf.close();
    const ScriptRun r = runScript(QStringLiteral("tts.py"),
                                  {textFile, out, QStringLiteral("--engine"), e, QStringLiteral("--voice"), voice,
                                   QStringLiteral("--rate"), QString::number(rate, 'f', 3)},
                                  QObject::tr("Speaking..."), nullptr, progress);
    res.cancelled = r.cancelled;
    if (!r.ok || QFileInfo(out).size() <= 256) {
        QFile::remove(out);
        res.error = r.error.isEmpty() ? QObject::tr("No audio was produced.") : r.error;
        return res;
    }
    res.ok = true;
    res.path = out;
    return res;
}

// ---------------------------------------------------------------------------
// Translator (NLLB-200)
// ---------------------------------------------------------------------------
QString Translator::modelId() { return ai::selected(AiTask::Translate); }

QString Translator::hfModel(const QString& id)
{
    const QString m = id.isEmpty() ? modelId() : id;
    if (m == QLatin1String("nllb-1.3b")) return QStringLiteral("facebook/nllb-200-distilled-1.3B");
    return QStringLiteral("facebook/nllb-200-distilled-600M");
}

bool Translator::isInstalled() { return isInstalled(modelId()); }

bool Translator::isInstalled(const QString& id)
{
    return QFileInfo::exists(StemSeparator::pythonPath()) && sitePackage(QStringLiteral("transformers")) &&
           sitePackage(QStringLiteral("torch")) && QFileInfo::exists(marker(id));
}

double Translator::downloadGB()
{
    const AiModel* m = ai::find(modelId());
    return m ? m->downloadGB : 2.5;
}

bool Translator::supports(const QString& language)
{
    static const QStringList codes = {"en", "ar", "ar-EG", "es", "fr", "de", "it", "pt", "ru", "tr", "hi", "ur", "fa", "id",
                                      "nl", "pl", "uk", "ja", "ko", "zh", "he", "sv", "el", "vi", "th", "bn", "sw"};
    return codes.contains(language);
}

bool Translator::install(const Progress& progress, QString* error)
{
    // PyTorch comes with the voice/music separator environment.
    if (!sitePackage(QStringLiteral("torch")) && !StemSeparator::install(progress, error)) return false;
    if (!sitePackage(QStringLiteral("transformers")) &&
        !pipInstall({QStringLiteral("transformers==4.51.3"), QStringLiteral("sentencepiece==0.2.0"), QStringLiteral("protobuf")},
                    QObject::tr("the translator"), progress, error))
        return false;
    const QString id = modelId();
    const ScriptRun r = runScript(QStringLiteral("translate.py"), {QStringLiteral("--download-only"), QStringLiteral("--model"), hfModel(id)},
                                  QObject::tr("Downloading the translation model (about %1 GB)...").arg(downloadGB(), 0, 'f', 1),
                                  nullptr, progress);
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    writeMarker(id);
    return true;
}

Translator::Result Translator::translate(const QStringList& texts, const QString& source, const QString& target,
                                         const Progress& progress)
{
    Result res;
    if (!isInstalled()) {
        res.error = QObject::tr("Translation is not installed yet.");
        return res;
    }
    QTemporaryDir tmp;
    const QString in = tmp.filePath(QStringLiteral("in.json")), outPath = tmp.filePath(QStringLiteral("out.json"));
    QFile f(in);
    if (!f.open(QIODevice::WriteOnly)) {
        res.error = f.errorString();
        return res;
    }
    f.write(QJsonDocument(QJsonObject{{"texts", QJsonArray::fromStringList(texts)}}).toJson());
    f.close();
    const ScriptRun r = runScript(QStringLiteral("translate.py"),
                                  {in, outPath, QStringLiteral("--model"), hfModel(), QStringLiteral("--source"),
                                   source.isEmpty() ? QStringLiteral("auto") : source, QStringLiteral("--target"), target},
                                  QObject::tr("Loading the translation model..."),
                                  [](const QJsonObject& o, double* fr) {
                                      *fr = o.value("value").toDouble(-1);
                                      return o.value("stage").toString() == QLatin1String("loading model")
                                                 ? QObject::tr("Loading the translation model...")
                                                 : QObject::tr("Translating... %1%").arg(int(*fr * 100));
                                  },
                                  progress);
    res.cancelled = r.cancelled;
    if (!r.ok) {
        res.error = r.error;
        return res;
    }
    QFile of(outPath);
    if (!of.open(QIODevice::ReadOnly)) {
        res.error = QObject::tr("The translator produced no output");
        return res;
    }
    for (const auto& v : QJsonDocument::fromJson(of.readAll()).object().value("texts").toArray()) res.texts << v.toString();
    if (res.texts.size() != texts.size()) {
        res.error = QObject::tr("The translator returned %1 lines for %2").arg(res.texts.size()).arg(texts.size());
        return res;
    }
    res.ok = true;
    res.device = r.done.value("device").toString();
    res.seconds = r.done.value("seconds").toDouble();
    return res;
}

} // namespace ve
