#include "media/StemSeparator.h"

#include "media/AudioDecoder.h"
#include "media/PythonTask.h"
#include "media/AiModels.h"
#include "media/SystemProfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <vector>

namespace ve {

namespace {

constexpr int kRate = 44100;

using RunResult = PythonTaskResult;
const auto runProcess = &runPythonTask;

QProcessEnvironment separatorEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TORCH_HOME"), QDir::toNativeSeparators(StemSeparator::envDir() + QStringLiteral("/torch")));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("WHISPER_MODEL_DIR"), QDir::toNativeSeparators(StemSeparator::envDir() + QStringLiteral("/whisper")));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.remove(QStringLiteral("PYTHONHOME"));
    return env;
}

bool hasNvidiaGpu() { return SystemProfile::current().hasNvidia(); }

QString modelMarker(const QString& model) { return StemSeparator::envDir() + QStringLiteral("/models/") + model + QStringLiteral(".ok"); }

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

bool demucsPackageInstalled() { return QFileInfo::exists(StemSeparator::pythonPath()) && sitePackage(QStringLiteral("demucs")); }

} // namespace

QString StemSeparator::envDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/VideoEditor/separator");
}

QString StemSeparator::pythonPath()
{
#ifdef Q_OS_WIN
    return envDir() + QStringLiteral("/.venv/Scripts/python.exe");
#else
    return envDir() + QStringLiteral("/.venv/bin/python");
#endif
}

QString StemSeparator::findScript(const QString& fileName)
{
    QStringList candidates{QCoreApplication::applicationDirPath() + QStringLiteral("/separator/") + fileName};
#ifdef VE_SOURCE_DIR
    candidates << QStringLiteral(VE_SOURCE_DIR "/separator/") + fileName;
#endif
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) return c;
    return {};
}

QString StemSeparator::scriptPath()
{
    return findScript(QStringLiteral("separate.py"));
}

QString StemSeparator::uvPath()
{
    QString uv = QStandardPaths::findExecutable(QStringLiteral("uv"));
    if (!uv.isEmpty()) return uv;
    const QString home = QDir::homePath();
    for (const QString& c : {home + QStringLiteral("/.local/bin/uv.exe"), home + QStringLiteral("/.local/bin/uv"),
                             home + QStringLiteral("/.cargo/bin/uv.exe"), home + QStringLiteral("/.cargo/bin/uv")})
        if (QFileInfo::exists(c)) return c;
    return {};
}

QString StemSeparator::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/VideoEditor/separated");
}

QString StemSeparator::model()
{
    return ai::selected(AiTask::Separate);
}

bool StemSeparator::isInstalled()
{
    return isInstalled(model());
}

bool StemSeparator::isInstalled(const QString& model)
{
    if (!QFileInfo::exists(pythonPath())) return false;
    if (QFileInfo::exists(modelMarker(model))) return true;
    if (model != QLatin1String("htdemucs")) return false;
    // Installed before models were selectable: htdemucs is the only checkpoint there.
    QDirIterator it(envDir() + QStringLiteral("/torch/hub/checkpoints"), {QStringLiteral("*.th")}, QDir::Files);
    return it.hasNext();
}

bool StemSeparator::install(const Progress& progress, QString* error)
{
    const QString uv = uvPath();
    if (uv.isEmpty()) {
        if (error) *error = QObject::tr("The installer needs 'uv' (https://docs.astral.sh/uv/). Install it, then try again.");
        return false;
    }
    const QString script = scriptPath();
    if (script.isEmpty()) {
        if (error) *error = QObject::tr("separator/separate.py is missing next to the editor.");
        return false;
    }
    QDir().mkpath(envDir());
    const QProcessEnvironment env = separatorEnv();
    QString status;
    double fraction = -1;
    const auto lineToStatus = [&](const QString& l) { status = l.left(160); };
    const auto fail = [&](const RunResult& r, const QString& what) {
        if (error)
            *error = r.cancelled ? QObject::tr("Cancelled")
                                 : QObject::tr("%1 failed:\n%2").arg(what, r.stderrTail.join(QLatin1Char('\n')));
        return false;
    };

    const QString demucsModel = model();
    RunResult r;
    // The environment is already there when only another Demucs model is added.
    if (!demucsPackageInstalled()) {
    status = QObject::tr("Creating the Python environment...");
    fraction = 0.05;
    r = runProcess(uv, {QStringLiteral("venv"), QStringLiteral("--allow-existing"), QStringLiteral("--python"),
                                  QStringLiteral("3.12"), envDir() + QStringLiteral("/.venv")},
                             env, lineToStatus, progress, &status, &fraction);
    if (r.exitCode != 0) return fail(r, QObject::tr("Creating the environment"));

    const bool cuda = hasNvidiaGpu();
    status = cuda ? QObject::tr("Installing PyTorch (GPU) and Demucs...") : QObject::tr("Installing PyTorch (CPU) and Demucs...");
    fraction = 0.2;
    QStringList args{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), pythonPath()};
    if (cuda) {
        args << QStringLiteral("torch==2.7.1+cu128") << QStringLiteral("torchaudio==2.7.1+cu128")
             << QStringLiteral("--extra-index-url") << QStringLiteral("https://download.pytorch.org/whl/cu128");
    } else {
        args << QStringLiteral("torch==2.7.1") << QStringLiteral("torchaudio==2.7.1")
             << QStringLiteral("--extra-index-url") << QStringLiteral("https://download.pytorch.org/whl/cpu");
    }
    args << QStringLiteral("demucs==4.0.1") << QStringLiteral("soundfile") << QStringLiteral("numpy")
         << QStringLiteral("--index-strategy") << QStringLiteral("unsafe-best-match");
    r = runProcess(uv, args, env, lineToStatus, progress, &status, &fraction);
    if (r.exitCode != 0) return fail(r, QObject::tr("Installing PyTorch and Demucs"));
    }

    const AiModel* info = ai::find(demucsModel);
    status = QObject::tr("Downloading the separation model %1 (about %2 MB)...")
                 .arg(info ? info->name : demucsModel).arg(info ? int(info->downloadGB * 1000) : 80);
    fraction = 0.8;
    QString pyError;
    r = runProcess(pythonPath(), {QStringLiteral("-I"), script, QStringLiteral("--download-only"), QStringLiteral("--model"), demucsModel}, env,
                   [&](const QString& l) {
                       const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
                       if (o.value("type").toString() == QLatin1String("error")) pyError = o.value("message").toString();
                   },
                   progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (!pyError.isEmpty() && error) *error = pyError;
        return pyError.isEmpty() ? fail(r, QObject::tr("Downloading the model")) : false;
    }
    QDir().mkpath(envDir() + QStringLiteral("/models"));
    QFile marker(modelMarker(demucsModel));
    if (marker.open(QIODevice::WriteOnly)) marker.write("ok");
    if (progress) progress(QObject::tr("Ready"), 1.0);
    return isInstalled(demucsModel);
}

StemSeparator::Result StemSeparator::separate(const QString& input, const QString& outDir, const Progress& progress)
{
    Result res;
    if (!isInstalled()) {
        res.error = QObject::tr("Voice/music separation is not installed yet.");
        return res;
    }
    const QString script = scriptPath();
    if (script.isEmpty()) {
        res.error = QObject::tr("separator/separate.py is missing next to the editor.");
        return res;
    }
    QString status = QObject::tr("Starting the separation...");
    double fraction = 0.0;
    QString pyError;
    const RunResult r = runProcess(
        pythonPath(), {QStringLiteral("-I"), script, input, outDir, QStringLiteral("--model"), model()}, separatorEnv(),
        [&](const QString& l) {
            const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
            const QString type = o.value("type").toString();
            if (type == QLatin1String("progress")) {
                fraction = o.value("value").toDouble();
                status = o.value("stage").toString() == QLatin1String("loading model")
                             ? QObject::tr("Loading the AI model...")
                             : QObject::tr("Separating voice and music... %1%").arg(int(fraction * 100));
            } else if (type == QLatin1String("done")) {
                res.ok = true;
                res.voicePath = o.value("voice").toString();
                res.musicPath = o.value("music").toString();
                res.device = o.value("device").toString();
                res.seconds = o.value("seconds").toDouble();
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

bool StemSeparator::decodeToWav(const QString& mediaPath, TimeUs duration, const QString& wavPath,
                                const Progress& progress, QString* error)
{
    AudioDecoder dec;
    if (!dec.open(mediaPath, kRate, error)) return false;
    QFile f(wavPath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const std::int64_t total = std::int64_t(usToSeconds(duration) * kRate);
    const std::uint32_t dataBytes = std::uint32_t(std::min<std::int64_t>(total * 8, 0x7fffffff));
    // RIFF / WAVE header, IEEE float, 2 channels
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(kRate); u32(kRate * 8); u16(8); u16(32);
    f.write("data", 4); u32(dataBytes);

    std::vector<float> buf(size_t(kRate) * 2);
    for (std::int64_t pos = 0; pos < total; pos += kRate) {
        const int n = int(std::min<std::int64_t>(kRate, total - pos));
        dec.read(TimeUs(double(pos) * kUsPerSecond / kRate), n, buf.data());
        f.write(reinterpret_cast<const char*>(buf.data()), qint64(n) * 8);
        if (progress && !progress(QObject::tr("Reading the audio..."), double(pos) / double(std::max<std::int64_t>(1, total)) * 0.1)) {
            f.remove();
            if (error) *error = QObject::tr("Cancelled");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transcriber (Whisper)
// ---------------------------------------------------------------------------
QString Transcriber::modelId()
{
    return ai::selected(AiTask::Transcribe);
}

QString Transcriber::model()
{
    QString id = modelId();
    return id.startsWith(QLatin1String("whisper-")) ? id.mid(8) : QStringLiteral("turbo");
}

QString Transcriber::modelFile(const QString& id)
{
    const QString name = (id.isEmpty() ? modelId() : id).mid(8);   // "whisper-small" -> "small"
    const QString file = name == QLatin1String("turbo") ? QStringLiteral("large-v3-turbo") : name;
    return StemSeparator::envDir() + QStringLiteral("/whisper/") + file + QStringLiteral(".pt");
}

double Transcriber::downloadGB()
{
    const AiModel* m = ai::find(modelId());
    return m ? m->downloadGB : 1.6;
}

bool Transcriber::isInstalled()
{
    return isInstalled(modelId());
}

bool Transcriber::isInstalled(const QString& id)
{
    return QFileInfo::exists(StemSeparator::pythonPath()) && QFileInfo::exists(modelFile(id)) &&
           sitePackage(QStringLiteral("whisper"));
}

bool Transcriber::install(const Progress& progress, QString* error)
{
    const QString uv = StemSeparator::uvPath();
    const QString script = StemSeparator::findScript(QStringLiteral("transcribe.py"));
    if (uv.isEmpty()) {
        if (error) *error = QObject::tr("The installer needs 'uv' (https://docs.astral.sh/uv/). Install it, then try again.");
        return false;
    }
    if (script.isEmpty()) {
        if (error) *error = QObject::tr("separator/transcribe.py is missing next to the editor.");
        return false;
    }
    // Whisper shares PyTorch with the voice/music separator: set that up first if needed.
    if (!QFileInfo::exists(StemSeparator::pythonPath()) && !StemSeparator::install(progress, error)) return false;
    const QProcessEnvironment env = separatorEnv();
    QString status = QObject::tr("Installing Whisper...");
    double fraction = 0.1;
    RunResult r;
    if (sitePackage(QStringLiteral("whisper"))) r.exitCode = 0;   // only a new model to download
    else r = runProcess(uv, {QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"),
                                  StemSeparator::pythonPath(), QStringLiteral("openai-whisper==20250625")},
                             env, [&](const QString& l) { status = l.left(160); }, progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (error) *error = r.cancelled ? QObject::tr("Cancelled")
                                        : QObject::tr("Installing Whisper failed:\n%1").arg(r.stderrTail.join(QLatin1Char('\n')));
        return false;
    }
    status = QObject::tr("Downloading the Whisper model %1 (about %2 GB)...").arg(model()).arg(downloadGB(), 0, 'f', 1);
    fraction = -1;
    QString pyError;
    r = runProcess(StemSeparator::pythonPath(),
                   {QStringLiteral("-I"), script, QStringLiteral("--download-only"), QStringLiteral("--model"), model()}, env,
                   [&](const QString& l) {
                       const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
                       if (o.value("type").toString() == QLatin1String("error")) pyError = o.value("message").toString();
                   },
                   progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (error) *error = r.cancelled ? QObject::tr("Cancelled") : !pyError.isEmpty() ? pyError : r.stderrTail.join(QLatin1Char('\n'));
        return false;
    }
    return isInstalled();
}

Transcriber::Result Transcriber::transcribe(const QString& input, const QString& outJson, const QString& language,
                                            const Progress& progress, const QString& hints)
{
    Result res;
    const QString script = StemSeparator::findScript(QStringLiteral("transcribe.py"));
    if (!isInstalled() || script.isEmpty()) {
        res.error = QObject::tr("Transcription is not installed yet.");
        return res;
    }
    QString status = QObject::tr("Loading Whisper...");
    double fraction = 0.0;
    QString pyError;
    const RunResult r = runProcess(
        StemSeparator::pythonPath(),
        {QStringLiteral("-I"), script, input, outJson, QStringLiteral("--model"), model(), QStringLiteral("--language"),
         language.isEmpty() ? QStringLiteral("auto") : language, QStringLiteral("--prompt"), hints},
        separatorEnv(),
        [&](const QString& l) {
            const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
            const QString type = o.value("type").toString();
            if (type == QLatin1String("progress")) {
                fraction = o.value("value").toDouble();
                status = o.value("stage").toString() == QLatin1String("loading model")
                             ? QObject::tr("Loading Whisper...")
                             : QObject::tr("Transcribing... %1%").arg(int(fraction * 100));
            } else if (type == QLatin1String("done")) {
                res.ok = true;
                res.jsonPath = o.value("path").toString();
                res.language = o.value("language").toString();
                res.device = o.value("device").toString();
                res.segments = o.value("segments").toInt();
                res.seconds = o.value("seconds").toDouble();
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

} // namespace ve
