#include "media/SystemProfile.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

#include <cmath>

namespace ve {

namespace {

double totalRamGB()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return double(m.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    return 0.0;
#elif defined(Q_OS_MACOS)
    int64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0) return double(bytes) / (1024.0 * 1024.0 * 1024.0);
    return 0.0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? double(pages) * double(page) / (1024.0 * 1024.0 * 1024.0) : 0.0;
#endif
}

QString nvidiaSmi()
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
#ifdef Q_OS_WIN
    if (exe.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Windows/System32/nvidia-smi.exe")))
        exe = QStringLiteral("C:/Windows/System32/nvidia-smi.exe");
#endif
    return exe;
}

QString runQuiet(const QString& exe, const QStringList& args)
{
    QProcess p;
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; });
#endif
    p.start(exe, args);
    if (!p.waitForFinished(8000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput());
}

// Drivers older than ~510 cannot report compute_cap: estimate it from the name.
double computeCapabilityFromName(const QString& name)
{
    const QString n = name.toUpper();
    static const QRegularExpression rtx(QStringLiteral("RTX\\s*(?:PRO\\s*)?(\\d)0\\d0"));
    const auto m = rtx.match(n);
    if (m.hasMatch()) {
        switch (m.captured(1).toInt()) {
        case 2: return 7.5;
        case 3: return 8.6;
        case 4: return 8.9;
        case 5: return 12.0;
        default: break;
        }
    }
    if (n.contains(QLatin1String("GTX 16")) || n.contains(QLatin1String("TITAN RTX")) || n.contains(QLatin1String("T4"))) return 7.5;
    if (n.contains(QLatin1String("A100")) || n.contains(QLatin1String("A30"))) return 8.0;
    if (n.contains(QLatin1String("H100")) || n.contains(QLatin1String("H200"))) return 9.0;
    if (n.contains(QLatin1String("L4")) || n.contains(QLatin1String("L40"))) return 8.9;
    if (n.contains(QRegularExpression(QStringLiteral("\\bA\\d{4}\\b")))) return 8.6;   // RTX A4000 etc.
    if (n.contains(QLatin1String("GTX 10")) || n.contains(QLatin1String("P100"))) return 6.1;
    return 6.0;
}

QVector<NvidiaGpu> detectNvidia()
{
    QVector<NvidiaGpu> gpus;
    const QString smi = nvidiaSmi();
    if (smi.isEmpty()) return gpus;
    QString out = runQuiet(smi, {QStringLiteral("--query-gpu=index,name,memory.total,compute_cap"),
                                 QStringLiteral("--format=csv,noheader,nounits")});
    bool withCc = !out.trimmed().isEmpty();
    if (!withCc)
        out = runQuiet(smi, {QStringLiteral("--query-gpu=index,name,memory.total"), QStringLiteral("--format=csv,noheader,nounits")});
    for (const QString& line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char(','));
        if (f.size() < 3) continue;
        NvidiaGpu g;
        g.index = f[0].trimmed().toInt();
        g.name = f[1].trimmed();
        g.vramGB = f[2].trimmed().toDouble() / 1024.0;
        bool ok = false;
        g.computeCapability = withCc && f.size() > 3 ? f[3].trimmed().toDouble(&ok) : 0.0;
        if (!ok) g.computeCapability = computeCapabilityFromName(g.name);
        gpus << g;
    }
    return gpus;
}

QString gb(double v)
{
    return v >= 10 || std::abs(v - std::round(v)) < 0.05 ? QString::number(std::lround(v)) : QString::number(v, 'f', 1);
}

} // namespace

const NvidiaGpu* SystemProfile::bestGpu() const
{
    const NvidiaGpu* best = nullptr;
    for (const NvidiaGpu& g : nvidia)
        if (!best || g.vramGB > best->vramGB) best = &g;
    return best;
}

QString SystemProfile::gpuText() const
{
    if (const NvidiaGpu* g = bestGpu()) return QStringLiteral("%1 (%2 GB)").arg(g->name, gb(g->vramGB));
    return appleSilicon ? QStringLiteral("Apple Silicon GPU (shared memory)") : QStringLiteral("No NVIDIA GPU");
}

QString SystemProfile::summary() const
{
    return QStringLiteral("%1 GB RAM \u00B7 %2 CPU threads \u00B7 %3%4")
        .arg(gb(ramGB)).arg(cpuThreads).arg(gpuText(), simulated ? QStringLiteral(" (simulated)") : QString());
}

QString SystemProfile::fingerprint() const
{
    QString s = QStringLiteral("ram=%1").arg(std::lround(ramGB));
    for (const NvidiaGpu& g : nvidia) s += QStringLiteral(";%1/%2").arg(g.name).arg(std::lround(g.vramGB));
    if (appleSilicon) s += QStringLiteral(";mps");
    return s;
}

QJsonObject SystemProfile::toJson() const
{
    QJsonArray gpus;
    for (const NvidiaGpu& g : nvidia)
        gpus.append(QJsonObject{{"index", g.index}, {"name", g.name}, {"vramGB", std::round(g.vramGB * 10) / 10},
                                {"computeCapability", g.computeCapability}});
    return {{"ramGB", std::round(ramGB * 10) / 10}, {"cpuThreads", cpuThreads}, {"nvidiaGpus", gpus},
            {"appleSilicon", appleSilicon}, {"diskFreeGB", std::round(diskFreeGB * 10) / 10},
            {"simulated", simulated}, {"summary", summary()}};
}

SystemProfile SystemProfile::detect()
{
    SystemProfile p;
    p.ramGB = totalRamGB();
    p.cpuThreads = std::max(1, QThread::idealThreadCount());
#ifdef Q_OS_MACOS
    p.appleSilicon = QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("arm"));
#else
    p.nvidia = detectNvidia();
#endif
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QDir().mkpath(data);
    p.diskFreeGB = double(QStorageInfo(data).bytesAvailable()) / (1024.0 * 1024.0 * 1024.0);

    // Testing: pretend to be another computer.
    const QString sim = qEnvironmentVariable("VE_SIMULATE_HARDWARE");
    if (!sim.isEmpty()) {
        p.simulated = true;
        NvidiaGpu g = p.bestGpu() ? *p.bestGpu() : NvidiaGpu{0, QStringLiteral("NVIDIA GPU"), 0.0, 8.6};
        bool gpu = p.hasNvidia();
        for (const QString& kv : sim.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString k = kv.section(QLatin1Char('='), 0, 0).trimmed().toLower();
            const QString v = kv.section(QLatin1Char('='), 1).trimmed();
            if (k == QLatin1String("ram")) p.ramGB = v.toDouble();
            else if (k == QLatin1String("threads")) p.cpuThreads = v.toInt();
            else if (k == QLatin1String("gpu") && v == QLatin1String("none")) gpu = false;
            else if (k == QLatin1String("vram")) { g.vramGB = v.toDouble(); gpu = g.vramGB > 0; }
            else if (k == QLatin1String("cc")) g.computeCapability = v.toDouble();
            else if (k == QLatin1String("name")) g.name = v;
            else if (k == QLatin1String("mps")) p.appleSilicon = v == QLatin1String("1");
        }
        p.nvidia.clear();
        if (gpu) p.nvidia << g;
    }
    return p;
}

const SystemProfile& SystemProfile::current()
{
    static const SystemProfile profile = detect();
    return profile;
}

} // namespace ve
