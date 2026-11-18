#include "media/VideoUpscaler.h"

#include "media/StemSeparator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>

namespace ve {

namespace {

QString scriptPath() { return StemSeparator::findScript(QStringLiteral("upscale.py")); }

QString envMarker(const QString& flavor) { return VideoUpscaler::envDir() + QStringLiteral("/env-") + flavor + QStringLiteral(".ok"); }

// Bump when the package list changes so existing environments are updated.
constexpr int kEnvVersion = 1;

QProcessEnvironment upscalerEnv(int gpuIndex = -1)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("VE_UPSCALER_HOME"), QDir::toNativeSeparators(VideoUpscaler::envDir()));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_DISABLE_SYMLINKS_WARNING"), QStringLiteral("1"));
    env.insert(QStringLiteral("TRITON_CACHE_DIR"), QDir::toNativeSeparators(VideoUpscaler::envDir() + QStringLiteral("/triton-cache")));
    env.insert(QStringLiteral("CUDA_DEVICE_ORDER"), QStringLiteral("PCI_BUS_ID"));   // same numbering as nvidia-smi
    if (gpuIndex >= 0) env.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"), QString::number(gpuIndex));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.remove(QStringLiteral("PYTHONHOME"));
    return env;
}

QString modelName(const QString& id)
{
    const AiModel* m = ai::find(id);
    return m ? m->name : id;
}

// Status text for one JSON progress line of upscale.py.
QString stageText(const QJsonObject& o, const QString& model)
{
    const QString stage = o.value("stage").toString();
    if (stage == QLatin1String("loading model")) return QObject::tr("Loading %1...").arg(modelName(model));
    if (stage == QLatin1String("upscaling")) {
        const int frame = o.value("frame").toInt(), frames = o.value("frames").toInt();
        return frames > 0 ? QObject::tr("Upscaling with %1... frame %2 of %3").arg(modelName(model)).arg(frame).arg(frames)
                          : QObject::tr("Upscaling with %1...").arg(modelName(model));
    }
    if (stage == QLatin1String("adding sound")) return QObject::tr("Adding the sound...");
    if (stage.startsWith(QLatin1String("downloading"))) {
        const double done = o.value("bytes").toDouble() / 1e9, total = o.value("total").toDouble() / 1e9;
        const QString what = stage.mid(12);
        return total > 0 ? QObject::tr("Downloading %1 (%2 of %3 GB)...").arg(what).arg(done, 0, 'f', 2).arg(total, 0, 'f', 2)
                         : QObject::tr("Downloading %1...").arg(what);
    }
    return stage;
}

} // namespace

QString VideoUpscaler::envDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/VideoEditor/upscaler");
}

QString VideoUpscaler::pythonPath()
{
#ifdef Q_OS_WIN
    return envDir() + QStringLiteral("/.venv/Scripts/python.exe");
#else
    return envDir() + QStringLiteral("/.venv/bin/python");
#endif
}

QString VideoUpscaler::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/VideoEditor/upscaled");
}

QString VideoUpscaler::torchFlavor(const SystemProfile& hw)
{
    if (!hw.hasNvidia()) return QStringLiteral("cpu");
    const double cc = hw.computeCapability();
    if (cc >= 8.0) return QStringLiteral("cu128");
    if (cc >= 7.5) return QStringLiteral("cu124");   // Turing: Triton < 3.3 -> PyTorch 2.6
    return QStringLiteral("cu126");                  // older GPUs: no FlashVSR, Real-ESRGAN only
}

bool VideoUpscaler::isEnvInstalled()
{
    QFile f(envMarker(torchFlavor()));
    return QFileInfo::exists(pythonPath()) && f.open(QIODevice::ReadOnly) && f.readAll().trimmed().toInt() >= kEnvVersion;
}

bool VideoUpscaler::isInstalled(const QString& modelId)
{
    return isEnvInstalled() && QFileInfo::exists(envDir() + QStringLiteral("/models/") + modelId + QStringLiteral(".ok"));
}

bool VideoUpscaler::install(const QString& modelId, const Progress& progress, QString* error)
{
    const QString uv = StemSeparator::uvPath();
    const QString script = scriptPath();
    if (uv.isEmpty()) {
        if (error) *error = QObject::tr("The installer needs 'uv' (https://docs.astral.sh/uv/). Install it, then try again.");
        return false;
    }
    if (script.isEmpty()) {
        if (error) *error = QObject::tr("separator/upscale.py is missing next to the editor.");
        return false;
    }
    QDir().mkpath(envDir());
    const QProcessEnvironment env = upscalerEnv();
    QString status;
    double fraction = -1;
    const auto lineToStatus = [&](const QString& l) { status = l.left(160); };
    const auto fail = [&](const PythonTaskResult& r, const QString& what) {
        if (error)
            *error = r.cancelled ? QObject::tr("Cancelled")
                                 : QObject::tr("%1 failed:\n%2").arg(what, r.stderrTail.join(QLatin1Char('\n')));
        return false;
    };

    const QString flavor = torchFlavor();
    if (!isEnvInstalled()) {
        status = QObject::tr("Creating the Python environment...");
        fraction = 0.02;
        PythonTaskResult r = runPythonTask(uv, {QStringLiteral("venv"), QStringLiteral("--allow-existing"), QStringLiteral("--python"),
                                                QStringLiteral("3.12"), envDir() + QStringLiteral("/.venv")},
                                           env, lineToStatus, progress, &status, &fraction);
        if (r.exitCode != 0) return fail(r, QObject::tr("Creating the environment"));

        QStringList args{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), pythonPath()};
        QString index;
        if (flavor == QLatin1String("cu128")) {
            args << QStringLiteral("torch==2.7.1+cu128") << QStringLiteral("torchvision==0.22.1+cu128");
#ifdef Q_OS_WIN
            args << QStringLiteral("triton-windows>=3.3,<3.4");
#endif
            index = QStringLiteral("https://download.pytorch.org/whl/cu128");
        } else if (flavor == QLatin1String("cu124")) {
            args << QStringLiteral("torch==2.6.0+cu124") << QStringLiteral("torchvision==0.21.0+cu124");
#ifdef Q_OS_WIN
            args << QStringLiteral("triton-windows>=3.2,<3.3");
#endif
            index = QStringLiteral("https://download.pytorch.org/whl/cu124");
        } else if (flavor == QLatin1String("cu126")) {
            args << QStringLiteral("torch==2.7.1+cu126") << QStringLiteral("torchvision==0.22.1+cu126");
            index = QStringLiteral("https://download.pytorch.org/whl/cu126");
        } else {
            args << QStringLiteral("torch==2.7.1") << QStringLiteral("torchvision==0.22.1");
#ifndef Q_OS_MACOS
            index = QStringLiteral("https://download.pytorch.org/whl/cpu");
#endif
        }
        args << QStringLiteral("numpy<2.3") << QStringLiteral("einops") << QStringLiteral("safetensors")
             << QStringLiteral("tqdm") << QStringLiteral("pillow") << QStringLiteral("huggingface_hub>=0.23")
             << QStringLiteral("imageio>=2.34") << QStringLiteral("imageio-ffmpeg>=0.5") << QStringLiteral("spandrel==0.4.1")
             << QStringLiteral("typing_extensions");
        if (!index.isEmpty())
            args << QStringLiteral("--extra-index-url") << index << QStringLiteral("--index-strategy") << QStringLiteral("unsafe-best-match");
        status = flavor == QLatin1String("cpu") ? QObject::tr("Installing PyTorch (CPU) for upscaling...")
                                                : QObject::tr("Installing PyTorch (GPU) for upscaling...");
        fraction = 0.05;
        r = runPythonTask(uv, args, env, lineToStatus, progress, &status, &fraction);
        if (r.exitCode != 0) return fail(r, QObject::tr("Installing PyTorch"));
        QFile marker(envMarker(flavor));
        if (marker.open(QIODevice::WriteOnly)) marker.write(QByteArray::number(kEnvVersion));
    }

    status = QObject::tr("Downloading %1...").arg(modelName(modelId));
    fraction = 0.4;
    QString pyError;
    const PythonTaskResult r = runPythonTask(
        pythonPath(), {QStringLiteral("-I"), script, QStringLiteral("--download-only"), QStringLiteral("--model"), modelId}, env,
        [&](const QString& l) {
            const QJsonObject o = QJsonDocument::fromJson(l.toUtf8()).object();
            const QString type = o.value("type").toString();
            if (type == QLatin1String("progress")) {
                fraction = 0.4 + 0.6 * o.value("value").toDouble();
                status = stageText(o, modelId);
            } else if (type == QLatin1String("error")) {
                pyError = o.value("message").toString();
            }
        },
        progress, &status, &fraction);
    if (r.exitCode != 0) {
        if (error) *error = r.cancelled ? QObject::tr("Cancelled") : !pyError.isEmpty() ? pyError : r.stderrTail.join(QLatin1Char('\n'));
        return false;
    }
    if (progress) progress(QObject::tr("Ready"), 1.0);
    return isInstalled(modelId);
}

VideoUpscaler::Result VideoUpscaler::upscale(const QString& input, const QString& output, double start, double duration,
                                             const UpscaleOptions& o, const Progress& progress)
{
    Result res;
    const QString script = scriptPath();
    if (!isInstalled(o.model) || script.isEmpty()) {
        res.error = QObject::tr("%1 is not installed yet.").arg(modelName(o.model));
        return res;
    }
    QStringList args{QStringLiteral("-I"), script, input, output, QStringLiteral("--model"), o.model,
                     QStringLiteral("--scale"), QString::number(o.scale), QStringLiteral("--tile-size"), QString::number(o.tileSize)};
    if (start > 0) args << QStringLiteral("--start") << QString::number(start, 'f', 6);
    if (duration > 0) args << QStringLiteral("--duration") << QString::number(duration, 'f', 6);
    if (o.tileDit) args << QStringLiteral("--tile-dit");
    if (o.tileVae) args << QStringLiteral("--tile-vae");
    if (o.unloadDit) args << QStringLiteral("--unload-dit");

    QString status = QObject::tr("Starting %1...").arg(modelName(o.model));
    double fraction = -1;
    QString pyError;
    const PythonTaskResult r = runPythonTask(
        pythonPath(), args, upscalerEnv(o.gpuIndex),
        [&](const QString& l) {
            const QJsonObject j = QJsonDocument::fromJson(l.toUtf8()).object();
            const QString type = j.value("type").toString();
            if (type == QLatin1String("progress")) {
                fraction = j.value("stage").toString() == QLatin1String("loading model") ? -1 : j.value("value").toDouble();
                status = stageText(j, o.model);
            } else if (type == QLatin1String("done")) {
                res.ok = true;
                res.path = j.value("path").toString();
                res.width = j.value("width").toInt();
                res.height = j.value("height").toInt();
                res.frames = j.value("frames").toInt();
                res.device = j.value("device").toString();
                res.seconds = j.value("seconds").toDouble();
            } else if (type == QLatin1String("error")) {
                pyError = j.value("message").toString();
            }
        },
        progress, &status, &fraction);
    res.cancelled = r.cancelled;
    if (r.cancelled) {
        res.ok = false;
        res.error = QObject::tr("Cancelled");
    } else if (!res.ok) {
        res.error = !pyError.isEmpty() ? pyError : r.stderrTail.join(QLatin1Char('\n'));
        if (res.error.contains(QLatin1String("out of memory"), Qt::CaseInsensitive))
            res.error += QObject::tr("\n\nThe GPU ran out of memory. Try 2x instead of 4x, a lighter model "
                                     "(see Tools > AI Models), or close other GPU programs.");
    }
    return res;
}

} // namespace ve
