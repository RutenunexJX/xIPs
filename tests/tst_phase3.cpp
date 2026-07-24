#include "assetcore/JsonUtil.h"
#include "dependency/DependencyResolver.h"
#include "importer/ImportService.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
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
                        const QList<DependencySpec> &dependencies = {},
                        const QStringList &extraSources = {})
{
    const QString assetRoot = QDir(libraryRoot).absoluteFilePath(id);
    QDir().mkpath(QDir(assetRoot).absoluteFilePath(QStringLiteral("rtl/include")));

    Manifest manifest;
    manifest.id = id;
    manifest.type = AssetType::Module;
    manifest.name = id.toUpper();
    manifest.version = version;
    manifest.top = id;
    manifest.sources = {QStringLiteral("rtl/%1.sv").arg(id)};
    manifest.sources.append(extraSources);
    manifest.includeDirs = {QStringLiteral("rtl/include")};
    manifest.defines = {QStringLiteral("%1_ENABLED=1").arg(id.toUpper())};
    manifest.dependencies = dependencies;
    manifest.tags = {QStringLiteral("test")};
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), manifest.name},
    };

    const QString manifestPath =
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    QString error;
    if (!ManifestService().write(manifestPath, manifest, &error)) {
        qFatal("Cannot create test manifest: %s", qPrintable(error));
    }
    if (!writeFile(QDir(assetRoot).absoluteFilePath(manifest.sources.first()),
                   QStringLiteral("module %1; endmodule\n").arg(id).toUtf8())) {
        qFatal("Cannot create primary test source");
    }
    if (!writeFile(
            QDir(assetRoot).absoluteFilePath(QStringLiteral("rtl/include/%1.svh").arg(id)),
            QStringLiteral("`define %1_HEADER 1\n").arg(id.toUpper()).toUtf8())) {
        qFatal("Cannot create test include");
    }
    for (const QString &source : extraSources) {
        if (!writeFile(QDir(assetRoot).absoluteFilePath(source),
                       QByteArrayLiteral("extra\n"))) {
            qFatal("Cannot create additional test source");
        }
    }

    AssetRecord record;
    record.manifest = manifest;
    record.assetRoot = assetRoot;
    record.manifestPath = manifestPath;
    record.contentHash = QStringLiteral("sha256:content-") + id + u'-' + version;
    record.sourceRepository = libraryRoot;
    record.gitCommit = QStringLiteral("commit-") + id;
    return record;
}

DependencySpec dependency(const QString &id,
                          const QString &constraint = {},
                          const bool optional = false)
{
    return DependencySpec{
        .id = id,
        .versionConstraint = constraint,
        .optional = optional,
    };
}

bool hasIssue(const DependencyResolution &resolution, const DependencyIssueKind kind)
{
    return std::any_of(
        resolution.issues.cbegin(),
        resolution.issues.cend(),
        [kind](const DependencyIssue &issue) { return issue.kind == kind; });
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString planProblems(const ImportPlan &plan)
{
    QStringList messages;
    for (const DependencyIssue &issue : plan.dependencies.issues) {
        messages.append(issue.message);
    }
    for (const ImportIssue &issue : plan.issues) {
        messages.append(QStringLiteral("%1: %2").arg(issue.message, issue.path));
    }
    for (const PlannedFile &file : plan.files) {
        if (file.action == PlannedFileAction::Conflict) {
            messages.append(QStringLiteral("Conflict: %1").arg(file.destinationPath));
        }
    }
    return messages.join(QStringLiteral("\n"));
}

QList<AssetRecord> relocatedCatalog(const QList<AssetRecord> &catalog,
                                    const QString &oldRoot,
                                    const QString &newRoot)
{
    QList<AssetRecord> result = catalog;
    for (AssetRecord &record : result) {
        const QString relative = QDir(oldRoot).relativeFilePath(record.assetRoot);
        record.assetRoot = QDir(newRoot).absoluteFilePath(relative);
        record.manifestPath =
            QDir(record.assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
        record.sourceRepository = newRoot;
    }
    return result;
}

} // namespace

class Phase3Test final : public QObject {
    Q_OBJECT

private slots:
    void deterministicDependencyClosure();
    void detectsCycleVersionAndToolConflicts();
    void referenceDoesNotCopyAndCanRepairById();
    void referenceImportsMergeAndProtectConcurrentEdits();
    void referenceRepairProtectsConcurrentEdits();
    void vendorPlanAndLockfileAreDeterministic();
    void vendorRejectsUnsafeAndConcurrentLockfiles();
    void userModifiedVendorFileBecomesConflict();
    void vendorFailureRollsBackAllFiles();
    void vendorCancellationDoesNotMutateTarget();
    void vendorWorkspaceIsIsolated();
};

void Phase3Test::deterministicDependencyClosure()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetRecord d = createAsset(library, QStringLiteral("d"), QStringLiteral("1.0.0"));
    AssetRecord b = createAsset(library,
                                QStringLiteral("b"),
                                QStringLiteral("1.0.0"),
                                {dependency(QStringLiteral("d"), QStringLiteral("^1.0.0"))});
    AssetRecord c = createAsset(library,
                                QStringLiteral("c"),
                                QStringLiteral("1.0.0"),
                                {dependency(QStringLiteral("d"), QStringLiteral(">=1.0.0"))});
    AssetRecord a = createAsset(
        library,
        QStringLiteral("a"),
        QStringLiteral("2.0.0"),
        {dependency(QStringLiteral("c")), dependency(QStringLiteral("b"))});

    DependencyResolver resolver;
    const DependencyResolution first = resolver.resolve({a, c, d, b}, {QStringLiteral("a")});
    const DependencyResolution second = resolver.resolve({d, b, a, c}, {QStringLiteral("a")});
    QVERIFY(!first.hasErrors());
    QCOMPARE(first.orderedIds(),
             QStringList({QStringLiteral("d"),
                          QStringLiteral("b"),
                          QStringLiteral("c"),
                          QStringLiteral("a")}));
    QCOMPARE(second.orderedIds(), first.orderedIds());
    QCOMPARE(first.graph.value(QStringLiteral("a")),
             QStringList({QStringLiteral("b"), QStringLiteral("c")}));
}

void Phase3Test::detectsCycleVersionAndToolConflicts()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetRecord a = createAsset(
        library,
        QStringLiteral("a"),
        QStringLiteral("1.0.0"),
        {dependency(QStringLiteral("b"), QStringLiteral("^2.0.0")),
         dependency(QStringLiteral("missing_optional"), {}, true)});
    AssetRecord b = createAsset(library,
                                QStringLiteral("b"),
                                QStringLiteral("1.5.0"),
                                {dependency(QStringLiteral("a"))});
    a.manifest.tools.insert(QStringLiteral("vivado"), QStringLiteral(">=2022.2"));

    const DependencyResolution result = DependencyResolver().resolve(
        {a, b},
        {QStringLiteral("a")},
        QJsonObject{{QStringLiteral("vivado"), QStringLiteral("2022.1")}});
    QVERIFY(result.hasErrors());
    QVERIFY(hasIssue(result, DependencyIssueKind::Cycle));
    QVERIFY(hasIssue(result, DependencyIssueKind::VersionConflict));
    QVERIFY(hasIssue(result, DependencyIssueKind::ToolConflict));
    QVERIFY(hasIssue(result, DependencyIssueKind::Missing));

    const auto optional = std::find_if(
        result.issues.cbegin(),
        result.issues.cend(),
        [](const DependencyIssue &issue) {
            return issue.dependencyId == QStringLiteral("missing_optional");
        });
    QVERIFY(optional != result.issues.cend());
    QCOMPARE(optional->severity, Diagnostic::Severity::Warning);
}

void Phase3Test::referenceDoesNotCopyAndCanRepairById()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    AssetRecord base =
        createAsset(library, QStringLiteral("base"), QStringLiteral("1.0.0"));
    AssetRecord top = createAsset(library,
                                  QStringLiteral("top"),
                                  QStringLiteral("1.0.0"),
                                  {dependency(QStringLiteral("base"))});
    QList<AssetRecord> catalog{top, base};

    ImportService service;
    const ImportPlan plan =
        service.planReference(catalog, {QStringLiteral("top")}, target);
    QVERIFY(plan.canExecute());
    QCOMPARE(plan.dependencies.orderedIds(),
             QStringList({QStringLiteral("base"), QStringLiteral("top")}));
    QString error;
    QVERIFY2(service.writeReference(plan, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(plan.outputPath));
    QVERIFY(!QFileInfo::exists(QDir(target).absoluteFilePath(QStringLiteral("vendor"))));

    const QJsonObject document = QJsonDocument::fromJson(readFile(plan.outputPath)).object();
    QCOMPARE(document.value(QStringLiteral("mode")).toString(), QStringLiteral("reference"));
    QCOMPARE(document.value(QStringLiteral("assets")).toArray().size(), 2);

    const QString moved = temporary.filePath(QStringLiteral("moved-library"));
    QVERIFY(QDir(temporary.path()).rename(QStringLiteral("library"),
                                         QStringLiteral("moved-library")));
    QList<AssetRecord> movedCatalog = relocatedCatalog(catalog, library, moved);
    QList<ReferenceState> states =
        service.inspectReferences(document, movedCatalog, target);
    QCOMPARE(states.size(), 2);
    QVERIFY(std::all_of(states.cbegin(), states.cend(), [](const ReferenceState &state) {
        return !state.available && state.repairableById;
    }));

    QJsonObject repaired = document;
    QVERIFY2(service.repairReferences(repaired, movedCatalog, target, &error), qPrintable(error));
    QVERIFY2(service.writeReferenceDocument(plan.outputPath, repaired, &error),
             qPrintable(error));
    repaired = QJsonDocument::fromJson(readFile(plan.outputPath)).object();
    states = service.inspectReferences(repaired, movedCatalog, target);
    QVERIFY(std::all_of(states.cbegin(), states.cend(), [](const ReferenceState &state) {
        return state.available && state.hashMatches;
    }));
}

void Phase3Test::referenceImportsMergeAndProtectConcurrentEdits()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord first =
        createAsset(library, QStringLiteral("first"), QStringLiteral("1.0.0"));
    const AssetRecord second =
        createAsset(library, QStringLiteral("second"), QStringLiteral("2.0.0"));
    const QList<AssetRecord> catalog{first, second};

    ImportService service;
    QString error;
    const ImportPlan firstPlan =
        service.planReference(catalog, {QStringLiteral("first")}, target);
    QVERIFY2(firstPlan.canExecute(), qPrintable(planProblems(firstPlan)));
    QVERIFY2(service.writeReference(firstPlan, &error), qPrintable(error));

    QJsonObject customized =
        QJsonDocument::fromJson(readFile(firstPlan.outputPath)).object();
    customized.insert(QStringLiteral("consumerNote"), QStringLiteral("preserve"));
    QJsonArray customizedAssets =
        customized.value(QStringLiteral("assets")).toArray();
    QJsonObject customizedFirst = customizedAssets.at(0).toObject();
    customizedFirst.insert(QStringLiteral("alias"), QStringLiteral("first-local"));
    customizedAssets.replace(0, customizedFirst);
    customized.insert(QStringLiteral("assets"), customizedAssets);
    QVERIFY(writeFile(firstPlan.outputPath,
                      QJsonDocument(customized).toJson(QJsonDocument::Indented)));

    const ImportPlan mergedPlan =
        service.planReference(catalog, {QStringLiteral("second")}, target);
    QVERIFY2(mergedPlan.canExecute(), qPrintable(planProblems(mergedPlan)));
    QCOMPARE(mergedPlan.outputDocument.value(QStringLiteral("consumerNote")).toString(),
             QStringLiteral("preserve"));
    const QJsonArray mergedAssets =
        mergedPlan.outputDocument.value(QStringLiteral("assets")).toArray();
    QCOMPARE(mergedAssets.size(), 2);
    QCOMPARE(mergedAssets.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("first"));
    QCOMPARE(mergedAssets.at(0).toObject().value(QStringLiteral("alias")).toString(),
             QStringLiteral("first-local"));
    QCOMPARE(mergedAssets.at(1).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("second"));
    QVERIFY2(service.writeReference(mergedPlan, &error), qPrintable(error));

    const ImportPlan stalePlan =
        service.planReference(catalog, {QStringLiteral("first")}, target);
    QVERIFY(stalePlan.canExecute());
    QJsonObject externallyChanged =
        QJsonDocument::fromJson(readFile(stalePlan.outputPath)).object();
    externallyChanged.insert(QStringLiteral("changedAfterPlanning"), true);
    QVERIFY(writeFile(stalePlan.outputPath,
                      QJsonDocument(externallyChanged)
                          .toJson(QJsonDocument::Indented)));
    QVERIFY(!service.writeReference(stalePlan, &error));
    QVERIFY(error.contains(QStringLiteral("changed after planning")));

    ImportPlan escapedPlan = stalePlan;
    escapedPlan.outputPath =
        temporary.filePath(QStringLiteral("outside-reference.json"));
    escapedPlan.outputDocumentExisted = false;
    escapedPlan.expectedOutputHash.clear();
    QVERIFY(!service.writeReference(escapedPlan, &error));
    QVERIFY(!QFileInfo::exists(escapedPlan.outputPath));

    QVERIFY(writeFile(stalePlan.outputPath, QByteArrayLiteral("{invalid")));
    const ImportPlan invalidPlan =
        service.planReference(catalog, {QStringLiteral("second")}, target);
    QVERIFY(!invalidPlan.canExecute());
    QVERIFY(std::any_of(
        invalidPlan.issues.cbegin(),
        invalidPlan.issues.cend(),
        [](const ImportIssue &issue) {
            return issue.severity == Diagnostic::Severity::Error
                   && issue.message.contains(QStringLiteral("Invalid Reference"));
        }));
    QVERIFY(!service.writeReference(invalidPlan, &error));
    QCOMPARE(readFile(stalePlan.outputPath), QByteArrayLiteral("{invalid"));
}

void Phase3Test::referenceRepairProtectsConcurrentEdits()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord asset =
        createAsset(library, QStringLiteral("repairable"), QStringLiteral("1.0.0"));

    ImportService service;
    QString error;
    const ImportPlan plan =
        service.planReference({asset}, {QStringLiteral("repairable")}, target);
    QVERIFY2(plan.canExecute(), qPrintable(planProblems(plan)));
    QVERIFY2(service.writeReference(plan, &error), qPrintable(error));

    const QString inspectedHash = ImportService::fileHash(plan.outputPath);
    QJsonObject repaired =
        QJsonDocument::fromJson(readFile(plan.outputPath)).object();
    QVERIFY2(service.repairReferences(repaired, {asset}, target, &error),
             qPrintable(error));

    QJsonObject externallyEdited =
        QJsonDocument::fromJson(readFile(plan.outputPath)).object();
    externallyEdited.insert(QStringLiteral("externalEdit"),
                            QStringLiteral("must-survive"));
    const QByteArray externalBytes =
        QJsonDocument(externallyEdited).toJson(QJsonDocument::Indented);
    QVERIFY(writeFile(plan.outputPath, externalBytes));

    error.clear();
    QVERIFY(!service.writeReferenceDocumentIfUnchanged(plan.outputPath,
                                                       repaired,
                                                       inspectedHash,
                                                       &error));
    QVERIFY(error.contains(QStringLiteral("changed after inspection")));
    QCOMPARE(readFile(plan.outputPath), externalBytes);

    QJsonObject invalid = externallyEdited;
    QJsonArray duplicateAssets =
        invalid.value(QStringLiteral("assets")).toArray();
    duplicateAssets.append(duplicateAssets.first());
    invalid.insert(QStringLiteral("assets"), duplicateAssets);
    error.clear();
    QVERIFY(!service.writeReferenceDocument(plan.outputPath, invalid, &error));
    QVERIFY(error.contains(QStringLiteral("Duplicate Reference asset ID")));
    QCOMPARE(readFile(plan.outputPath), externalBytes);
}

void Phase3Test::vendorPlanAndLockfileAreDeterministic()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    AssetRecord base =
        createAsset(library, QStringLiteral("base"), QStringLiteral("1.0.0"));
    AssetRecord top = createAsset(library,
                                  QStringLiteral("top"),
                                  QStringLiteral("2.1.0"),
                                  {dependency(QStringLiteral("base"), QStringLiteral("^1.0.0"))});
    const QList<AssetRecord> catalog{top, base};

    ImportService service;
    const ImportPlan first =
        service.planVendor(catalog, {QStringLiteral("top")}, target);
    const ImportPlan second =
        service.planVendor({base, top}, {QStringLiteral("top")}, target);
    QVERIFY2(first.canExecute(), qPrintable(planProblems(first)));
    QCOMPARE(first.dependencies.orderedIds(),
             QStringList({QStringLiteral("base"), QStringLiteral("top")}));
    QCOMPARE(json::canonicalJson(first.outputDocument),
             json::canonicalJson(second.outputDocument));
    QVERIFY(std::all_of(first.files.cbegin(), first.files.cend(), [](const PlannedFile &file) {
        return file.action == PlannedFileAction::Add;
    }));

    const ImportExecutionResult executed = service.executeVendor(
        first,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY2(executed.success, qPrintable(executed.error));
    QVERIFY(QFileInfo::exists(first.outputPath));
    QVERIFY(QFileInfo::exists(
        QDir(target).absoluteFilePath(QStringLiteral("vendor/xips/top/rtl/top.sv"))));
    const QJsonObject lockfile =
        QJsonDocument::fromJson(readFile(first.outputPath)).object();
    QCOMPARE(lockfile.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(lockfile.value(QStringLiteral("assets")).toArray().size(), 2);

    const ImportPlan unchanged =
        service.planVendor(catalog, {QStringLiteral("top")}, target);
    QVERIFY2(unchanged.canExecute(), qPrintable(planProblems(unchanged)));
    QVERIFY(std::all_of(
        unchanged.files.cbegin(), unchanged.files.cend(), [](const PlannedFile &file) {
            return file.action == PlannedFileAction::Skip;
    }));
}

void Phase3Test::vendorRejectsUnsafeAndConcurrentLockfiles()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord asset =
        createAsset(library, QStringLiteral("safe"), QStringLiteral("1.0.0"));
    const QString outside = temporary.filePath(QStringLiteral("outside.sv"));
    QVERIFY(writeFile(outside, QByteArrayLiteral("keep\n")));
    const QByteArray outsideBefore = readFile(outside);
    const QString lockfile =
        QDir(target).absoluteFilePath(QStringLiteral("xips-lock.json"));

    const QJsonObject unsafeLock{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("mode"), QStringLiteral("vendor")},
        {QStringLiteral("assets"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("old")},
             {QStringLiteral("files"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("path"), QStringLiteral("../outside.sv")},
                  {QStringLiteral("hash"), ImportService::fileHash(outside)},
              }}},
         }}},
    };
    QVERIFY(writeFile(lockfile,
                      QJsonDocument(unsafeLock)
                          .toJson(QJsonDocument::Indented)));
    ImportService service;
    const ImportPlan unsafePlan =
        service.planVendor({asset}, {QStringLiteral("safe")}, target);
    QVERIFY(!unsafePlan.canExecute());
    QVERIFY(std::any_of(
        unsafePlan.issues.cbegin(),
        unsafePlan.issues.cend(),
        [](const ImportIssue &issue) {
            return issue.severity == Diagnostic::Severity::Error
                   && issue.message.contains(
                       QStringLiteral("Vendor-owned path"));
        }));
    QCOMPARE(readFile(outside), outsideBefore);

    QVERIFY(QFile::remove(lockfile));
    const ImportPlan stalePlan =
        service.planVendor({asset}, {QStringLiteral("safe")}, target);
    QVERIFY2(stalePlan.canExecute(), qPrintable(planProblems(stalePlan)));
    ImportPlan escapedPlan = stalePlan;
    QVERIFY(!escapedPlan.files.isEmpty());
    escapedPlan.files.first().destinationPath =
        QStringLiteral("../escaped.sv");
    const ImportExecutionResult escapedResult = service.executeVendor(
        escapedPlan,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY(!escapedResult.success);
    QVERIFY(escapedResult.error.contains(
        QStringLiteral("Unsafe Vendor destination")));
    QVERIFY(!QFileInfo::exists(
        temporary.filePath(QStringLiteral("escaped.sv"))));

    QVERIFY(writeFile(lockfile,
                      QJsonDocument(stalePlan.outputDocument)
                          .toJson(QJsonDocument::Indented)));
    const ImportExecutionResult staleResult = service.executeVendor(
        stalePlan,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY(!staleResult.success);
    QVERIFY(staleResult.error.contains(
        QStringLiteral("lockfile changed after planning")));
    QVERIFY(!QFileInfo::exists(QDir(target).absoluteFilePath(
        QStringLiteral("vendor/xips/safe/rtl/safe.sv"))));
    QCOMPARE(readFile(outside), outsideBefore);
}

void Phase3Test::userModifiedVendorFileBecomesConflict()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord asset =
        createAsset(library, QStringLiteral("owned"), QStringLiteral("1.0.0"));

    ImportService service;
    const ImportPlan initial =
        service.planVendor({asset}, {QStringLiteral("owned")}, target);
    QVERIFY2(initial.canExecute(), qPrintable(planProblems(initial)));
    const ImportExecutionResult initialResult =
        service.executeVendor(initial, ImportExecutionOptions{.confirmed = true});
    QVERIFY2(initialResult.success, qPrintable(initialResult.error));

    const QString vendored =
        QDir(target).absoluteFilePath(QStringLiteral("vendor/xips/owned/rtl/owned.sv"));
    QVERIFY(writeFile(vendored, QByteArrayLiteral("// user modification\n")));
    const ImportPlan upgrade =
        service.planVendor({asset}, {QStringLiteral("owned")}, target);
    const auto conflict = std::find_if(
        upgrade.files.cbegin(),
        upgrade.files.cend(),
        [](const PlannedFile &file) {
            return file.destinationPath.endsWith(QStringLiteral("owned.sv"));
        });
    QVERIFY(conflict != upgrade.files.cend());
    QCOMPARE(conflict->action, PlannedFileAction::Conflict);
    QVERIFY(!upgrade.canExecute());
    const ImportExecutionResult blocked = service.executeVendor(
        upgrade,
        ImportExecutionOptions{.confirmed = true});
    QVERIFY(!blocked.success);
    QCOMPARE(readFile(vendored), QByteArrayLiteral("// user modification\n"));
}

void Phase3Test::vendorFailureRollsBackAllFiles()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord asset = createAsset(library,
                                          QStringLiteral("rollback"),
                                          QStringLiteral("1.0.0"),
                                          {},
                                          {QStringLiteral("rtl/second.sv")});

    ImportService service;
    const ImportPlan plan =
        service.planVendor({asset}, {QStringLiteral("rollback")}, target);
    QVERIFY2(plan.canExecute(), qPrintable(planProblems(plan)));
    const ImportExecutionResult result = service.executeVendor(
        plan,
        ImportExecutionOptions{
            .confirmed = true,
            .failAfterFileOperations = 1,
        });
    QVERIFY(!result.success);
    QVERIFY(result.rolledBack);
    QVERIFY(!QFileInfo::exists(plan.outputPath));
    const QDir vendorRoot(
        QDir(target).absoluteFilePath(QStringLiteral("vendor/xips/rollback")));
    QVERIFY(!vendorRoot.exists());

    QStringList recovered;
    QString error;
    QVERIFY2(service.recoverVendorTransactions(target, &recovered, &error), qPrintable(error));
    QVERIFY(recovered.isEmpty());
}

void Phase3Test::vendorCancellationDoesNotMutateTarget()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString target = temporary.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(target));
    const AssetRecord asset =
        createAsset(library, QStringLiteral("cancelled"), QStringLiteral("1.0.0"));
    const ImportPlan plan =
        ImportService().planVendor({asset}, {QStringLiteral("cancelled")}, target);
    QVERIFY2(plan.canExecute(), qPrintable(planProblems(plan)));
    std::atomic_bool cancelled = true;
    const ImportExecutionResult result = ImportService().executeVendor(
        plan,
        ImportExecutionOptions{
            .confirmed = true,
            .cancelled = &cancelled,
        });
    QVERIFY(!result.success);
    QVERIFY(result.cancelled);
    QVERIFY(!QFileInfo::exists(plan.outputPath));
    QVERIFY(!QFileInfo::exists(
        QDir(target).absoluteFilePath(QStringLiteral("vendor"))));
}

void Phase3Test::vendorWorkspaceIsIsolated()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString targetA = temporary.filePath(QStringLiteral("project-a"));
    const QString targetB = temporary.filePath(QStringLiteral("project-b"));
    QVERIFY(QDir().mkpath(targetA));
    QVERIFY(QDir().mkpath(targetB));
    const QString sentinel = QDir(targetB).absoluteFilePath(QStringLiteral("sentinel.txt"));
    QVERIFY(writeFile(sentinel, QByteArrayLiteral("unchanged")));
    const AssetRecord asset =
        createAsset(library, QStringLiteral("isolated"), QStringLiteral("1.0.0"));

    ImportService service;
    const ImportPlan plan =
        service.planVendor({asset}, {QStringLiteral("isolated")}, targetA);
    QVERIFY2(plan.canExecute(), qPrintable(planProblems(plan)));
    const ImportExecutionResult executed =
        service.executeVendor(plan, ImportExecutionOptions{.confirmed = true});
    QVERIFY2(executed.success, qPrintable(executed.error));
    QCOMPARE(readFile(sentinel), QByteArrayLiteral("unchanged"));
    QVERIFY(!QFileInfo::exists(
        QDir(targetB).absoluteFilePath(QStringLiteral("xips-lock.json"))));
}

QTEST_GUILESS_MAIN(Phase3Test)

#include "tst_phase3.moc"
