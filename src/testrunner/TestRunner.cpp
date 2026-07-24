#include "testrunner/TestRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace xips {
namespace {

constexpr qsizetype MaximumLogBytes = 4 * 1024 * 1024;

bool directoryIsWithin(const QString &directory, const QString &root)
{
    QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    QString canonicalDirectory = QFileInfo(directory).canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        canonicalRoot = QFileInfo(root).absoluteFilePath();
    }
    if (canonicalDirectory.isEmpty()) {
        canonicalDirectory = QFileInfo(directory).absoluteFilePath();
    }
    canonicalRoot = QDir::cleanPath(canonicalRoot);
    canonicalDirectory = QDir::cleanPath(canonicalDirectory);
    canonicalRoot = QDir::fromNativeSeparators(canonicalRoot);
    canonicalDirectory = QDir::fromNativeSeparators(canonicalDirectory);
    if (canonicalDirectory.compare(canonicalRoot, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString prefix = canonicalRoot + u'/';
    return canonicalDirectory.startsWith(prefix, Qt::CaseInsensitive);
}

} // namespace

TestRunner::TestRunner(QObject *parent)
    : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process,
            &QProcess::readyReadStandardOutput,
            this,
            &TestRunner::readStandardOutput);
    connect(&m_process,
            &QProcess::readyReadStandardError,
            this,
            &TestRunner::readStandardError);
    connect(
        &m_process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
            finish(exitCode, exitStatus);
        });
    connect(&m_process, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(-1, QProcess::CrashExit, m_process.errorString());
        }
    });
}

TestRunner::~TestRunner()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_finishing = true;
        m_process.kill();
        m_process.waitForFinished(3000);
    }
}

bool TestRunner::start(const TestRunRequest &request, QString *error)
{
    if (isRunning()) {
        if (error) {
            *error = QStringLiteral("A test is already running");
        }
        return false;
    }
    if (!request.confirmed) {
        if (error) {
            *error = QStringLiteral("Test execution requires explicit confirmation");
        }
        return false;
    }
    if (request.command.program.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("Test command has no program");
        }
        return false;
    }

    const QString workingDirectory =
        request.command.workingDirectory.isEmpty()
            ? QFileInfo(request.asset.assetRoot).absoluteFilePath()
            : (QDir::isAbsolutePath(request.command.workingDirectory)
                   ? QFileInfo(request.command.workingDirectory).absoluteFilePath()
                   : QDir(request.asset.assetRoot)
                         .absoluteFilePath(request.command.workingDirectory));
    if (!QFileInfo(workingDirectory).isDir()) {
        if (error) {
            *error = QStringLiteral("Test working directory does not exist: %1")
                         .arg(workingDirectory);
        }
        return false;
    }
    if (!directoryIsWithin(workingDirectory, request.asset.assetRoot)) {
        if (error) {
            *error = QStringLiteral("Test working directory must stay inside the asset root");
        }
        return false;
    }

    m_result = TestResult{
        .assetId = request.asset.manifest.id,
        .commandName = request.command.name.isEmpty() ? request.command.program
                                                      : request.command.name,
        .program = request.command.program,
        .arguments = request.command.arguments,
        .workingDirectory = workingDirectory,
        .status = QStringLiteral("running"),
        .startedAt = QDateTime::currentDateTimeUtc(),
        .gitCommit = request.asset.gitCommit,
        .contentHash = request.asset.contentHash,
    };
    m_cancelRequested = false;
    m_finishing = false;
    m_logBytes = 0;
    m_process.setWorkingDirectory(workingDirectory);

    QString program = request.command.program;
    if ((program.contains(u'/') || program.contains(u'\\'))
        && !QDir::isAbsolutePath(program)) {
        program = QDir(workingDirectory).absoluteFilePath(program);
    }
    m_process.setProgram(program);
    m_process.setArguments(request.command.arguments);
    m_timer.start();
    m_process.start(QIODevice::ReadOnly);
    return true;
}

void TestRunner::cancel()
{
    if (!isRunning()) {
        return;
    }
    m_cancelRequested = true;
    const qint64 processId = m_process.processId();
    m_process.terminate();
    QTimer::singleShot(1500, this, [this, processId] {
        if (m_process.state() != QProcess::NotRunning
            && m_process.processId() == processId) {
            m_process.kill();
        }
    });
}

bool TestRunner::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void TestRunner::readStandardOutput()
{
    appendOutput(m_process.readAllStandardOutput(), false);
}

void TestRunner::readStandardError()
{
    appendOutput(m_process.readAllStandardError(), true);
}

void TestRunner::appendOutput(const QByteArray &bytes, const bool standardError)
{
    if (bytes.isEmpty()) {
        return;
    }
    const qsizetype remaining = qMax<qsizetype>(0, MaximumLogBytes - m_logBytes);
    const QByteArray accepted = bytes.first(qMin(bytes.size(), remaining));
    m_logBytes += accepted.size();
    if (accepted.size() != bytes.size()) {
        m_result.logTruncated = true;
    }
    if (accepted.isEmpty()) {
        return;
    }
    const QString text = QString::fromUtf8(accepted);
    if (standardError) {
        m_result.standardError += text;
    } else {
        m_result.standardOutput += text;
    }
    emit outputReceived(text, standardError);
}

void TestRunner::finish(const int exitCode,
                        const QProcess::ExitStatus exitStatus,
                        const QString &startError)
{
    if (m_finishing || m_result.status != QStringLiteral("running")) {
        return;
    }
    m_finishing = true;
    readStandardOutput();
    readStandardError();
    m_result.exitCode = exitCode;
    m_result.exitStatus =
        exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                           : QStringLiteral("crash");
    m_result.durationMs = m_timer.isValid() ? m_timer.elapsed() : 0;
    m_result.cancelled = m_cancelRequested;
    if (!startError.isEmpty()) {
        m_result.status = QStringLiteral("start-error");
        m_result.standardError += startError;
    } else if (m_cancelRequested) {
        m_result.status = QStringLiteral("cancelled");
    } else if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        m_result.status = QStringLiteral("passed");
    } else {
        m_result.status = QStringLiteral("failed");
    }
    if (m_result.logTruncated) {
        const QString marker = QStringLiteral("\n[xIPs: log truncated at 4 MiB]\n");
        m_result.standardError += marker;
    }
    const TestResult completed = m_result;
    m_finishing = false;
    emit finished(completed);
}

} // namespace xips
