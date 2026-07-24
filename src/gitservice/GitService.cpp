#include "gitservice/GitService.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

namespace xips {
namespace {

constexpr qsizetype MaximumGitOutput = 4 * 1024 * 1024;

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

ProcessResult runGit(const QString &executable,
                     const QStringList &arguments,
                     const int timeoutMs,
                     const std::atomic_bool *cancelled)
{
    ProcessResult result;
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    result.started = process.waitForStarted(qMin(timeoutMs, 1000));
    if (!result.started) {
        result.standardError = process.errorString().toUtf8();
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(25);
        result.standardOutput.append(process.readAllStandardOutput());
        result.standardError.append(process.readAllStandardError());
        if (result.standardOutput.size() + result.standardError.size()
            > MaximumGitOutput) {
            result.standardError =
                QByteArrayLiteral("Git output exceeded the 4 MiB safety limit");
            process.kill();
            process.waitForFinished(1000);
            break;
        }
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
        if (timer.elapsed() >= timeoutMs) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
    }
    result.standardOutput.append(process.readAllStandardOutput());
    if (!result.timedOut && !result.cancelled) {
        result.standardError.append(process.readAllStandardError());
    }
    result.exitCode = process.exitCode();
    return result;
}

QString firstLine(const QByteArray &output)
{
    return QString::fromUtf8(output).split(u'\n').value(0).trimmed();
}

} // namespace

GitService::GitService(QString executable)
    : m_executable(std::move(executable))
{
    if (m_executable.isEmpty()) {
        m_executable = qEnvironmentVariable("XIPS_GIT", QStringLiteral("git"));
    }
}

GitInfo GitService::query(const QString &path,
                          const int timeoutMs,
                          const std::atomic_bool *cancelled) const
{
    GitInfo info;
    const QFileInfo supplied(path);
    const QString workingPath =
        supplied.isDir() ? supplied.absoluteFilePath() : supplied.absolutePath();
    if (!QFileInfo(workingPath).isDir()) {
        info.error = QStringLiteral("Git query path is not a directory: %1").arg(workingPath);
        return info;
    }

    const ProcessResult root = runGit(
        m_executable,
        {QStringLiteral("-C"),
         workingPath,
         QStringLiteral("rev-parse"),
         QStringLiteral("--show-toplevel")},
        timeoutMs,
        cancelled);
    info.timedOut = root.timedOut;
    info.cancelled = root.cancelled;
    if (!root.started || root.timedOut || root.cancelled || root.exitCode != 0) {
        if (!root.cancelled) {
            info.error = root.timedOut
                             ? QStringLiteral("Git query timed out")
                             : QString::fromUtf8(root.standardError).trimmed();
            if (info.error.isEmpty()) {
                info.error = QStringLiteral("Path is not inside a Git work tree");
            }
        }
        return info;
    }

    info.repositoryRoot = QDir::cleanPath(firstLine(root.standardOutput));
    const int remainingTimeout = qMax(250, timeoutMs);
    const ProcessResult revision = runGit(
        m_executable,
        {QStringLiteral("-C"),
         info.repositoryRoot,
         QStringLiteral("rev-parse"),
         QStringLiteral("HEAD"),
         QStringLiteral("--abbrev-ref"),
         QStringLiteral("HEAD")},
        remainingTimeout,
        cancelled);
    if (!revision.started || revision.timedOut || revision.cancelled
        || revision.exitCode != 0) {
        info.timedOut = revision.timedOut;
        info.cancelled = revision.cancelled;
        info.error = revision.timedOut
                         ? QStringLiteral("Git revision query timed out")
                         : QString::fromUtf8(revision.standardError).trimmed();
        return info;
    }
    const QStringList revisionLines =
        QString::fromUtf8(revision.standardOutput).split(u'\n', Qt::SkipEmptyParts);
    info.commit = revisionLines.value(0).trimmed();
    info.branch = revisionLines.value(1).trimmed();

    const ProcessResult tags = runGit(
        m_executable,
        {QStringLiteral("-C"),
         info.repositoryRoot,
         QStringLiteral("tag"),
         QStringLiteral("--points-at"),
         QStringLiteral("HEAD"),
         QStringLiteral("--sort=-version:refname")},
        remainingTimeout,
        cancelled);
    if (tags.started && !tags.timedOut && !tags.cancelled && tags.exitCode == 0) {
        info.tag = firstLine(tags.standardOutput);
    }

    QString relative = QDir(info.repositoryRoot).relativeFilePath(workingPath);
    relative = QDir::cleanPath(relative);
    QStringList statusArguments{
        QStringLiteral("-C"),
        info.repositoryRoot,
        QStringLiteral("status"),
        QStringLiteral("--porcelain=v1"),
        QStringLiteral("--untracked-files=normal"),
    };
    if (relative != QStringLiteral(".") && !relative.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(relative)) {
        statusArguments.append(QStringLiteral("--"));
        statusArguments.append(relative);
    }
    const ProcessResult status =
        runGit(m_executable, statusArguments, remainingTimeout, cancelled);
    info.timedOut = status.timedOut;
    info.cancelled = status.cancelled;
    if (!status.started || status.timedOut || status.cancelled || status.exitCode != 0) {
        info.error = status.timedOut
                         ? QStringLiteral("Git status query timed out")
                         : QString::fromUtf8(status.standardError).trimmed();
        return info;
    }
    info.dirty = !status.standardOutput.trimmed().isEmpty();
    info.available = true;
    return info;
}

} // namespace xips
