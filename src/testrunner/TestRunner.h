#pragma once

#include "assetcore/Asset.h"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>

namespace xips {

struct TestRunRequest {
    AssetRecord asset;
    TestCommandSpec command;
    bool confirmed = false;
};

class TestRunner final : public QObject {
    Q_OBJECT

public:
    explicit TestRunner(QObject *parent = nullptr);
    ~TestRunner() override;

    bool start(const TestRunRequest &request, QString *error = nullptr);
    void cancel();
    [[nodiscard]] bool isRunning() const;

signals:
    void outputReceived(const QString &text, bool standardError);
    void finished(const xips::TestResult &result);

private:
    void readStandardOutput();
    void readStandardError();
    void appendOutput(const QByteArray &bytes, bool standardError);
    void finish(int exitCode, QProcess::ExitStatus exitStatus, const QString &startError = {});

    QProcess m_process;
    QElapsedTimer m_timer;
    TestResult m_result;
    bool m_cancelRequested = false;
    bool m_finishing = false;
    qsizetype m_logBytes = 0;
};

} // namespace xips
