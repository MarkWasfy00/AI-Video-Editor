#include "media/PythonTask.h"

#include <QEventLoop>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace ve {

PythonTaskResult runPythonTask(const QString& program, const QStringList& args, const QProcessEnvironment& env,
                               const std::function<void(const QString&)>& onLine, const TaskProgress& progress,
                               QString* status, double* fraction)
{
    PythonTaskResult r;
    QProcess proc;
    proc.setProcessEnvironment(env);
    QEventLoop loop;
    QByteArray outBuf, errBuf;
    auto drainLines = [](QByteArray& buf, const std::function<void(const QString&)>& fn) {
        int nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(buf.left(nl)).trimmed();
            buf.remove(0, nl + 1);
            if (!line.isEmpty() && fn) fn(line);
        }
    };
    QObject::connect(&proc, &QProcess::readyReadStandardOutput, &loop, [&] {
        outBuf += proc.readAllStandardOutput();
        drainLines(outBuf, onLine);
    });
    QObject::connect(&proc, &QProcess::readyReadStandardError, &loop, [&] {
        errBuf += proc.readAllStandardError().replace('\r', '\n');
        drainLines(errBuf, [&](const QString& l) {
            r.stderrTail << l;
            while (r.stderrTail.size() > 15) r.stderrTail.removeFirst();
        });
    });
    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            r.stderrTail << QStringLiteral("Could not start %1: %2").arg(program, proc.errorString());
            loop.quit();
        }
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (progress && !progress(*status, *fraction)) {
            r.cancelled = true;
            proc.kill();
        }
    });
    poll.start(100);
#ifdef Q_OS_WIN
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; }); // no console
#endif
    proc.start(program, args);
    if (!(proc.state() == QProcess::NotRunning && proc.error() == QProcess::FailedToStart)) loop.exec();
    if (proc.state() != QProcess::NotRunning) proc.waitForFinished(5000);
    outBuf += proc.readAllStandardOutput();
    outBuf += '\n';
    drainLines(outBuf, onLine);
    r.exitCode = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
    return r;
}

} // namespace ve
