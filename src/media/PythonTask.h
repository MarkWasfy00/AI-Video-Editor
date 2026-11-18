#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace ve {

// Runs a helper process (uv, the AI environments' Python) while keeping the Qt
// event loop alive. Each stdout line goes to onLine; `progress` is polled every
// 100 ms with *status / *fraction and returns false to cancel (kills the process).
struct PythonTaskResult {
    int exitCode = -1;
    bool cancelled = false;
    QStringList stderrTail;   // last lines of stderr, for error messages
};

using TaskProgress = std::function<bool(const QString& text, double fraction)>;

PythonTaskResult runPythonTask(const QString& program, const QStringList& args, const QProcessEnvironment& env,
                               const std::function<void(const QString&)>& onLine, const TaskProgress& progress,
                               QString* status, double* fraction);

} // namespace ve
