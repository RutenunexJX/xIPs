#include "assetcore/JsonUtil.h"
#include "assetindex/AssetIndex.h"
#include "assetindex/AssetScanner.h"
#include "diff/DiffService.h"
#include "gitservice/GitService.h"
#include "importer/ImportService.h"
#include "manifest/ManifestService.h"
#include "testrunner/TestRunner.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace xips;

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

AssetRecord createAsset(const QString &libraryRoot,
                        const QString &id,
                        const QString &version,
                        const QStringList &extraSources = {})
{
    const QString assetRoot = QDir(libraryRoot).absoluteFilePath(id);
    if (!QDir().mkpath(assetRoot)) {
        qFatal("Cannot create test asset directory");
    }
    Manifest manifest;
    manifest.id = id;
    manifest.type = AssetType::Module;
    manifest.name = id;
    manifest.version = version;
    manifest.top = id;
    manifest.sources = {QStringLiteral("rtl/%1.sv").arg(id)};
    manifest.sources.append(extraSources);
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), id},
    };
    const QString manifestPath =
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    QString error;
    if (!ManifestService().write(manifestPath, manifest, &error)) {
        qFatal("Cannot write test manifest: %s", qPrintable(error));
    }
    if (!writeFile(QDir(assetRoot).absoluteFilePath(manifest.sources.first()),
                   QStringLiteral("module %1; endmodule\n").arg(id).toUtf8())) {
        qFatal("Cannot write primary source");
    }
    for (const QString &source : extraSources) {
        if (!writeFile(QDir(assetRoot).absoluteFilePath(source),
                       QByteArrayLiteral("legacy\n"))) {
            qFatal("Cannot write extra source");
        }
    }
    AssetRecord record;
    record.manifest = manifest;
    record.assetRoot = assetRoot;
    record.manifestPath = manifestPath;
    record.contentHash = AssetScanner::contentHash(manifest, assetRoot);
    record.gitCommit = QStringLiteral("commit-") + version;
    return record;
}

bool runGit(const QString &workingDirectory,
            const QStringList &arguments,
            QByteArray *output = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForStarted(3000) || !process.waitForFinished(10000)) {
        return false;
    }
    if (output) {
        *output = process.readAllStandardOutput();
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool containsDifference(const AssetDifference &difference,
                        const DifferenceCategory category,
                        const QString &keyPart)
{
    return std::any_of(
        difference.entries.cbegin(),
        difference.entries.cend(),
        [category, &keyPart](const DifferenceEntry &entry) {
            return entry.category == category && entry.key.contains(keyPart);
        });
}

} // namespace

class Phase4Test final : public QObject {
    Q_OBJECT

private slots:
    void manifestParsesStructuredTestCommands();
    void gitQueriesCommitTagAndWorkingState();
    void diffSeparatesFourCategories();
    void testRunnerRequiresConfirmationAndRecordsProvenance();
    void testRunnerCanCancel();
    void testResultPublicationRejectsStaleGeneration();
    void vendorUpgradeIsPreviewedBeforeMutation();
};

void Phase4Test::manifestParsesStructuredTestCommands()
{
    const QByteArray source = R"json({
        "schemaVersion": 1,
        "id": "tested",
        "type": "module",
        "name": "Tested",
        "sources": [],
        "testCommands": [{
            "name": "smoke",
            "program": "simulator",
            "arguments": ["--batch", "tb/top.sv"],
            "workingDirectory": "tb",
            "extension": 7
        }]
    })json";
    const ManifestLoadResult loaded = ManifestService().parse(source);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->testCommands.size(), 1);
    const TestCommandSpec command = loaded.manifest->testCommands.first();
    QCOMPARE(command.name, QStringLiteral("smoke"));
    QCOMPARE(command.program, QStringLiteral("simulator"));
    QCOMPARE(command.arguments,
             QStringList({QStringLiteral("--batch"), QStringLiteral("tb/top.sv")}));
    QCOMPARE(command.extraFields.value(QStringLiteral("extension")).toInt(), 7);
    const QJsonObject rewritten = ManifestService().toJson(*loaded.manifest);
    QCOMPARE(rewritten.value(QStringLiteral("testCommands"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("extension"))
                 .toInt(),
             7);
}

void Phase4Test::gitQueriesCommitTagAndWorkingState()
{
    QTemporaryDir repository;
    QVERIFY(repository.isValid());
    QVERIFY(writeFile(repository.filePath(QStringLiteral("asset/file.sv")),
                      QByteArrayLiteral("module file; endmodule\n")));
    QVERIFY(runGit(repository.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(runGit(repository.path(),
                   {QStringLiteral("config"),
                    QStringLiteral("user.email"),
                    QStringLiteral("xips-tests@example.invalid")}));
    QVERIFY(runGit(repository.path(),
                   {QStringLiteral("config"),
                    QStringLiteral("user.name"),
                    QStringLiteral("xIPs Tests")}));
    QVERIFY(runGit(repository.path(), {QStringLiteral("add"), QStringLiteral(".")}));
    QVERIFY(runGit(repository.path(),
                   {QStringLiteral("commit"),
                    QStringLiteral("-q"),
                    QStringLiteral("-m"),
                    QStringLiteral("initial")}));
    QVERIFY(runGit(repository.path(),
                   {QStringLiteral("tag"), QStringLiteral("v1.2.3")}));

    const GitInfo clean =
        GitService().query(repository.filePath(QStringLiteral("asset")), 5000);
    QVERIFY2(clean.available, qPrintable(clean.error));
    QVERIFY(!clean.commit.isEmpty());
    QCOMPARE(clean.tag, QStringLiteral("v1.2.3"));
    QVERIFY(!clean.dirty);

    QVERIFY(writeFile(repository.filePath(QStringLiteral("asset/file.sv")),
                      QByteArrayLiteral("module changed; endmodule\n")));
    const GitInfo dirty =
        GitService().query(repository.filePath(QStringLiteral("asset")), 5000);
    QVERIFY2(dirty.available, qPrintable(dirty.error));
    QVERIFY(dirty.dirty);
    QCOMPARE(dirty.commit, clean.commit);
}

void Phase4Test::diffSeparatesFourCategories()
{
    QTemporaryDir temporary;
    const QString beforeRoot = temporary.filePath(QStringLiteral("before"));
    const QString afterRoot = temporary.filePath(QStringLiteral("after"));
    AssetRecord before = createAsset(beforeRoot,
                                     QStringLiteral("core"),
                                     QStringLiteral("1.0.0"));
    AssetRecord after = createAsset(afterRoot,
                                    QStringLiteral("core"),
                                    QStringLiteral("2.0.0"),
                                    {QStringLiteral("rtl/new.sv")});
    QVERIFY(writeFile(QDir(after.assetRoot).absoluteFilePath(QStringLiteral("rtl/core.sv")),
                      QByteArrayLiteral("module core(input logic clk); endmodule\n")));
    after.manifest.description = QStringLiteral("changed");
    after.manifest.dependencies.append(DependencySpec{
        .id = QStringLiteral("support"),
        .versionConstraint = QStringLiteral("^2.0.0"),
    });
    after.contentHash = AssetScanner::contentHash(after.manifest, after.assetRoot);

    before.semantic.available = true;
    before.semantic.units.append(SemanticUnit{
        .name = QStringLiteral("core"),
        .kind = QStringLiteral("module"),
        .ports = {SemanticPort{
            .name = QStringLiteral("data"),
            .direction = QStringLiteral("input"),
            .type = QStringLiteral("logic"),
        }},
    });
    after.semantic = before.semantic;
    after.semantic.units.first().ports.first().type = QStringLiteral("logic [7:0]");
    after.semantic.units.first().parameters.append(SemanticParameter{
        .name = QStringLiteral("WIDTH"),
        .type = QStringLiteral("int"),
        .value = QStringLiteral("8"),
    });

    const AssetDifference difference = DiffService().compare(before, after);
    QVERIFY(!difference.identical());
    QVERIFY(containsDifference(difference,
                               DifferenceCategory::File,
                               QStringLiteral("rtl/core.sv")));
    QVERIFY(containsDifference(difference,
                               DifferenceCategory::Manifest,
                               QStringLiteral("description")));
    QVERIFY(containsDifference(difference,
                               DifferenceCategory::Semantic,
                               QStringLiteral("port/core/data")));
    QVERIFY(containsDifference(difference,
                               DifferenceCategory::Dependency,
                               QStringLiteral("support")));
}

void Phase4Test::testRunnerRequiresConfirmationAndRecordsProvenance()
{
    QTemporaryDir temporary;
    AssetRecord asset =
        createAsset(temporary.path(), QStringLiteral("runner"), QStringLiteral("1.0.0"));
    TestCommandSpec command{
        .name = QStringLiteral("fixture"),
        .program = QString::fromUtf8(FAKE_TEST_TOOL_PATH),
        .arguments = {QStringLiteral("--stdout"),
                      QStringLiteral("stream-out"),
                      QStringLiteral("--stderr"),
                      QStringLiteral("stream-err")},
    };

    TestRunner runner;
    QString error;
    QVERIFY(!runner.start(TestRunRequest{
                              .asset = asset,
                              .command = command,
                              .confirmed = false,
                          },
                          &error));
    QVERIFY(error.contains(QStringLiteral("confirmation")));

    QSignalSpy outputSpy(&runner, &TestRunner::outputReceived);
    QSignalSpy finishedSpy(&runner, &TestRunner::finished);
    QVERIFY(runner.start(TestRunRequest{
        .asset = asset,
        .command = command,
        .confirmed = true,
    }));
    QVERIFY(finishedSpy.wait(5000));
    QCOMPARE(finishedSpy.size(), 1);
    const TestResult result =
        qvariant_cast<TestResult>(finishedSpy.first().first());
    QCOMPARE(result.status, QStringLiteral("passed"));
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardOutput.contains(QStringLiteral("stream-out")));
    QVERIFY(result.standardError.contains(QStringLiteral("stream-err")));
    QCOMPARE(result.contentHash, asset.contentHash);
    QCOMPARE(result.gitCommit, asset.gitCommit);
    QCOMPARE(result.workingDirectory, QFileInfo(asset.assetRoot).absoluteFilePath());
    QVERIFY(result.durationMs >= 0);
    QVERIFY(outputSpy.size() >= 2);
    QVERIFY(!result.isStale(asset.contentHash, asset.gitCommit));
    QVERIFY(result.isStale(QStringLiteral("sha256:changed"), asset.gitCommit));
}

void Phase4Test::testRunnerCanCancel()
{
    QTemporaryDir temporary;
    AssetRecord asset =
        createAsset(temporary.path(), QStringLiteral("cancel"), QStringLiteral("1.0.0"));
    TestRunner runner;
    QSignalSpy finishedSpy(&runner, &TestRunner::finished);
    QVERIFY(runner.start(TestRunRequest{
        .asset = asset,
        .command = TestCommandSpec{
            .name = QStringLiteral("slow"),
            .program = QString::fromUtf8(FAKE_TEST_TOOL_PATH),
            .arguments = {QStringLiteral("--sleep-ms"), QStringLiteral("5000")},
        },
        .confirmed = true,
    }));
    QTimer::singleShot(100, &runner, &TestRunner::cancel);
    QVERIFY(finishedSpy.wait(4000));
    const TestResult result =
        qvariant_cast<TestResult>(finishedSpy.first().first());
    QCOMPARE(result.status, QStringLiteral("cancelled"));
    QVERIFY(result.cancelled);
}

void Phase4Test::testResultPublicationRejectsStaleGeneration()
{
    QTemporaryDir temporary;
    AssetRecord asset =
        createAsset(temporary.filePath(QStringLiteral("library")),
                    QStringLiteral("indexed"),
                    QStringLiteral("1.0.0"));
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    QString error;
    const qint64 firstGeneration = index.reserveGeneration(&error);
    QVERIFY2(firstGeneration > 0, qPrintable(error));
    QVERIFY2(index.rebuild({asset}, firstGeneration, &error), qPrintable(error));

    TestResult result{
        .assetId = asset.manifest.id,
        .commandName = QStringLiteral("smoke"),
        .program = QStringLiteral("fixture"),
        .status = QStringLiteral("passed"),
        .exitCode = 0,
        .exitStatus = QStringLiteral("normal"),
        .startedAt = QDateTime::currentDateTimeUtc(),
        .gitCommit = asset.gitCommit,
        .contentHash = asset.contentHash,
    };
    QCOMPARE(index.publishTestResult(asset.manifest.id,
                                    asset.contentHash,
                                    firstGeneration,
                                    result,
                                    &error),
             TestPublishStatus::Published);
    QCOMPARE(index.testResults(asset.manifest.id, &error).size(), 1);

    AssetRecord changed = asset;
    QVERIFY(writeFile(QDir(asset.assetRoot).absoluteFilePath(QStringLiteral("rtl/indexed.sv")),
                      QByteArrayLiteral("module indexed; logic changed; endmodule\n")));
    changed.contentHash =
        AssetScanner::contentHash(changed.manifest, changed.assetRoot);
    const qint64 secondGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.rebuild({changed}, secondGeneration, &error), qPrintable(error));
    QCOMPARE(index.publishTestResult(asset.manifest.id,
                                    asset.contentHash,
                                    firstGeneration,
                                    result,
                                    &error),
             TestPublishStatus::Stale);
    const QList<AssetRecord> records = index.allAssets(&error);
    QCOMPARE(records.first().testStatus, QStringLiteral("stale"));
    const QList<TestResult> cached = index.testResults(asset.manifest.id, &error);
    QCOMPARE(cached.size(), 1);
    QVERIFY(cached.first().isStale(changed.contentHash, changed.gitCommit));
}

void Phase4Test::vendorUpgradeIsPreviewedBeforeMutation()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    AssetRecord first = createAsset(library,
                                    QStringLiteral("upgradable"),
                                    QStringLiteral("1.0.0"),
                                    {QStringLiteral("rtl/legacy.sv")});
    ImportService importer;
    const ImportPlan initial =
        importer.planVendor({first}, {QStringLiteral("upgradable")}, target);
    const ImportExecutionResult installed = importer.executeVendor(
        initial,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY2(installed.success, qPrintable(installed.error));

    AssetRecord second =
        createAsset(library, QStringLiteral("upgradable"), QStringLiteral("2.0.0"));
    const QString vendoredLegacy = QDir(target).absoluteFilePath(
        QStringLiteral("vendor/xips/upgradable/rtl/legacy.sv"));
    QVERIFY(writeFile(vendoredLegacy, QByteArrayLiteral("user change\n")));
    VendorUpgradePlan blocked = importer.planVendorUpgrade(
        {second},
        {QStringLiteral("upgradable")},
        target);
    QVERIFY(blocked.hasChanges());
    QCOMPARE(blocked.assets.size(), 1);
    QCOMPARE(blocked.assets.first().kind, AssetUpgradeKind::Changed);
    QVERIFY(!blocked.canExecute());
    QVERIFY(QFileInfo::exists(vendoredLegacy));

    QVERIFY(writeFile(vendoredLegacy, QByteArrayLiteral("legacy\n")));
    const VendorUpgradePlan safe = importer.planVendorUpgrade(
        {second},
        {QStringLiteral("upgradable")},
        target);
    QVERIFY(safe.canExecute());
    QVERIFY(std::any_of(
        safe.importPlan.files.cbegin(),
        safe.importPlan.files.cend(),
        [](const PlannedFile &file) {
            return file.action == PlannedFileAction::Remove
                   && file.destinationPath.endsWith(QStringLiteral("legacy.sv"));
        }));
    const ImportExecutionResult upgraded = importer.executeVendor(
        safe.importPlan,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY2(upgraded.success, qPrintable(upgraded.error));
    QCOMPARE(upgraded.removed, 1);
    QVERIFY(!QFileInfo::exists(vendoredLegacy));
}

QTEST_GUILESS_MAIN(Phase4Test)

#include "tst_phase4.moc"
