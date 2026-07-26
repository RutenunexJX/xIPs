#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"
#include "library/AssetScanner.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
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

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString createSourceIp(const QString &root)
{
    const QString source = QDir(root).absoluteFilePath(QStringLiteral("source_ip"));
    if (!writeFile(QDir(source).absoluteFilePath(QStringLiteral("rtl/top.sv")),
                   QByteArrayLiteral("module top; endmodule\n"))
        || !writeFile(QDir(source).absoluteFilePath(QStringLiteral("README.md")),
                      QByteArrayLiteral("Example IP\n"))
        || !writeFile(QDir(source).absoluteFilePath(QStringLiteral("build/cache.bin")),
                      QByteArrayLiteral("generated"))) {
        return {};
    }
    return source;
}

} // namespace

class CoreTest final : public QObject {
    Q_OBJECT

private slots:
    void manifestWritesMinimalSchemaAndPreservesUnknownFields();
    void importFolderCreatesPortableAsset();
    void importSingleFileCreatesAsset();
    void batchImportRecoversCollisionsAndAssignsGroups();
    void scannerListsPayloadAndHashesOnDemand();
    void metadataKeepsStableId();
    void updateReplacesWorkingCopyAndPreservesSavedVersions();
    void workingCopyUndoFailsClosedAtSecurityBoundaries();
    void workingCopyTransactionsRejectConcurrentMutation();
    void assetDeletionIsBoundedAndLeavesSourcesUntouched();
    void versionsAreImmutableAndCopyable();
    void versionStateCopyToAndDeletionFormASafeWorkflow();
    void groupChangesApplyAcrossAssets();
    void activationUrisParse();
    void cliListsAndResolvesAssets();
};

void CoreTest::manifestWritesMinimalSchemaAndPreservesUnknownFields()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray json = R"({
        "schemaVersion": 1,
        "id": "legacy_ip",
        "type": "module",
        "name": "Legacy IP",
        "sources": ["rtl/top.sv"],
        "tags": ["legacy"],
        "custom": {"keep": true}
    })";
    ManifestService service;
    ManifestLoadResult loaded = service.parse(json);
    QVERIFY(loaded.ok());
    loaded.manifest->name = QStringLiteral("Renamed legacy IP");
    const QString path = temporary.filePath(QStringLiteral(".xips.json"));
    QString error;
    QVERIFY2(service.write(path, *loaded.manifest, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(!object.contains(QStringLiteral("type")));
    QVERIFY(!object.contains(QStringLiteral("sources")));
    QVERIFY(object.value(QStringLiteral("custom")).toObject()
                .value(QStringLiteral("keep")).toBool());
    QCOMPARE(object.value(QStringLiteral("name")).toString(),
             QStringLiteral("Renamed legacy IP"));
}

void CoreTest::importFolderCreatesPortableAsset()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord created;
    QString error;
    QVERIFY2(service.importAsset(
                 ImportAssetRequest{
                     .libraryRoot = library,
                     .sourcePath = source,
                     .metadata = AssetMetadata{
                         .id = QStringLiteral("uart_ip"),
                         .name = QStringLiteral("UART IP"),
                         .description = QStringLiteral("Reusable UART"),
                         .tags = {QStringLiteral("serial"), QStringLiteral("uart")},
                     },
                 },
                 &created,
                 &error),
             qPrintable(error));
    QCOMPARE(created.manifest.id, QStringLiteral("uart_ip"));
    QVERIFY(QFileInfo::exists(QDir(created.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"))));
    QVERIFY(!QFileInfo::exists(QDir(created.assetRoot).absoluteFilePath(
        QStringLiteral("build/cache.bin"))));
    QVERIFY(QFileInfo::exists(QDir(source).absoluteFilePath(
        QStringLiteral("rtl/top.sv"))));
    QVERIFY(created.files.contains(QStringLiteral("rtl/top.sv")));
    QVERIFY(created.files.contains(QStringLiteral("README.md")));
    QFile manifestFile(created.manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifestObject = QJsonDocument::fromJson(
        manifestFile.readAll()).object();
    QVERIFY(!manifestObject.contains(QStringLiteral("type")));
    QVERIFY(!manifestObject.contains(QStringLiteral("sources")));
}

void CoreTest::importSingleFileCreatesAsset()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("uart_rx.sv"));
    const QByteArray contents = QByteArrayLiteral("module uart_rx; endmodule\n");
    QVERIFY(writeFile(source, contents));

    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetMetadata metadata = service.suggestedMetadata(source);
    QCOMPARE(metadata.name, QStringLiteral("uart_rx.sv"));
    QCOMPARE(metadata.id, QStringLiteral("uart_rx_sv"));
    metadata.tags = {QStringLiteral("AXI"),
                     QStringLiteral("axi"),
                     QStringLiteral("UART")};

    AssetRecord created;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = metadata},
                 &created,
                 &error),
             qPrintable(error));
    QCOMPARE(created.manifest.id, QStringLiteral("uart_rx_sv"));
    QCOMPARE(created.manifest.tags,
             QStringList({QStringLiteral("AXI"), QStringLiteral("UART")}));
    QCOMPARE(created.fileCount, 1);
    QCOMPARE(created.files, QStringList{QStringLiteral("uart_rx.sv")});
    QFile imported(QDir(created.assetRoot).absoluteFilePath(
        QStringLiteral("uart_rx.sv")));
    QVERIFY(imported.open(QIODevice::ReadOnly));
    QCOMPARE(imported.readAll(), contents);
    QVERIFY(QFileInfo::exists(source));

    VersionInfo saved;
    QVERIFY2(service.createVersion(created,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(
        QDir(saved.path).absoluteFilePath(QStringLiteral("uart_rx.sv"))));

    QProcess resolve;
    resolve.start(QString::fromUtf8(XIPS_CLI_PATH),
                  {QStringLiteral("--action"), QStringLiteral("resolve"),
                   QStringLiteral("--library"), library,
                   QStringLiteral("--asset"), created.manifest.id});
    QVERIFY(resolve.waitForFinished(10000));
    QCOMPARE(resolve.exitCode(), 0);
    const QJsonObject resolved = QJsonDocument::fromJson(
        resolve.readAllStandardOutput()).object().value(QStringLiteral("data")).toObject();
    const QString importedPath = QDir(created.assetRoot).absoluteFilePath(
        QStringLiteral("uart_rx.sv"));
    QCOMPARE(QDir::cleanPath(
                 resolved.value(QStringLiteral("resolvedFile")).toString()),
             QDir::cleanPath(importedPath));
    QCOMPARE(resolved.value(QStringLiteral("resolvedFiles")).toArray().size(), 1);
}

void CoreTest::batchImportRecoversCollisionsAndAssignsGroups()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString first = temporary.filePath(QStringLiteral("one/uart.sv"));
    const QString second = temporary.filePath(QStringLiteral("two/uart.sv"));
    QVERIFY(writeFile(first, QByteArrayLiteral("module uart_one; endmodule\n")));
    QVERIFY(writeFile(second, QByteArrayLiteral("module uart_two; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    Manifest existing;
    existing.id = QStringLiteral("uart_sv");
    existing.name = QStringLiteral("Existing UART");
    QString setupError;
    const QString existingRoot = QDir(library).absoluteFilePath(
        QStringLiteral("nested/existing"));
    QVERIFY(QDir().mkpath(existingRoot));
    QVERIFY2(ManifestService().write(
                 QDir(existingRoot).absoluteFilePath(
                     QStringLiteral(".xips.json")),
                 existing,
                 &setupError),
             qPrintable(setupError));

    AssetLibraryService service;
    const ImportBatchResult result = service.importAssets(
        library,
        {first, second, temporary.filePath(QStringLiteral("missing.sv"))},
        {QStringLiteral("UART"), QStringLiteral("uart")});
    QCOMPARE(result.created.size(), 2);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.created.at(0).manifest.id, QStringLiteral("uart_sv_2"));
    QCOMPARE(result.created.at(1).manifest.id, QStringLiteral("uart_sv_3"));
    QCOMPARE(result.created.at(1).manifest.name, QStringLiteral("uart.sv (3)"));
    QCOMPARE(result.created.at(0).manifest.tags,
             QStringList{QStringLiteral("UART")});
    QVERIFY(result.errors.first().contains(QStringLiteral("missing.sv")));
}

void CoreTest::scannerListsPayloadAndHashesOnDemand()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library/ip"));
    QVERIFY(writeFile(QDir(root).absoluteFilePath(QStringLiteral("rtl/top.sv")),
                      QByteArrayLiteral("module top; endmodule\n")));
    Manifest manifest;
    manifest.id = QStringLiteral("ip");
    manifest.name = QStringLiteral("IP");
    QString error;
    QVERIFY(ManifestService().write(
        QDir(root).absoluteFilePath(QStringLiteral(".xips.json")), manifest, &error));
    const ScanResult before = AssetScanner().scan(temporary.filePath(QStringLiteral("library")));
    QCOMPARE(before.assets.size(), 1);
    const QString beforeHash = AssetScanner::contentHash(
        before.assets.first().manifest,
        before.assets.first().assetRoot);
    QVERIFY(writeFile(QDir(root).absoluteFilePath(QStringLiteral("data/table.mem")),
                      QByteArrayLiteral("00112233\n")));
    const ScanResult after = AssetScanner().scan(temporary.filePath(QStringLiteral("library")));
    QCOMPARE(after.assets.size(), 1);
    const QString afterHash = AssetScanner::contentHash(
        after.assets.first().manifest,
        after.assets.first().assetRoot);
    QVERIFY(beforeHash != afterHash);
    QCOMPARE(after.assets.first().fileCount, 2);
    QVERIFY(after.assets.first().files.contains(QStringLiteral("data/table.mem")));
}

void CoreTest::metadataKeepsStableId()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY(service.importAsset(
        {.libraryRoot = library,
         .sourcePath = source,
         .metadata = {.id = QStringLiteral("stable_ip"),
                      .name = QStringLiteral("Stable IP"),
                      .description = {},
                      .tags = {}}},
        &asset,
        &error));
    QVERIFY(!service.updateMetadata(
        asset,
        {.id = QStringLiteral("renamed_id"),
         .name = QStringLiteral("Renamed"),
         .description = {},
         .tags = {}},
        &error));
    QVERIFY(error.contains(QStringLiteral("cannot be changed")));
    error.clear();
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   nullptr,
                                   &error),
             qPrintable(error));
    QVERIFY2(service.updateMetadata(
                 asset,
                 {.id = QStringLiteral("stable_ip"),
                  .name = QStringLiteral("Updated name"),
                  .description = QStringLiteral("Updated description"),
                  .tags = {QStringLiteral("updated")}},
                 &error),
             qPrintable(error));
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->id, QStringLiteral("stable_ip"));
    QCOMPARE(loaded.manifest->name, QStringLiteral("Updated name"));
    QCOMPARE(loaded.manifest->version, QStringLiteral("1.0.0"));
}

void CoreTest::updateReplacesWorkingCopyAndPreservesSavedVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString original = createSourceIp(temporary.path());
    QVERIFY(!original.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = original,
                  .metadata = {.id = QStringLiteral("update_ip"),
                               .name = QStringLiteral("Update IP"),
                               .description = QStringLiteral("Before update"),
                               .tags = {QStringLiteral("UART")}}},
                 &asset,
                 &error),
             qPrintable(error));
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   nullptr,
                                   &error),
             qPrintable(error));

    const QString replacement = temporary.filePath(
        QStringLiteral("project/replacement"));
    const QByteArray revised = QByteArrayLiteral(
        "module top; localparam REV = 2; endmodule\n");
    QVERIFY(writeFile(QDir(replacement).absoluteFilePath(
                          QStringLiteral("rtl/top.sv")),
                      revised));
    QVERIFY(writeFile(QDir(replacement).absoluteFilePath(
                          QStringLiteral("doc/guide.md")),
                      QByteArrayLiteral("Updated guide\n")));

    const UpdatePreview preview = service.previewUpdate(asset, replacement);
    QVERIFY2(preview.ok(), qPrintable(preview.error));
    QCOMPARE(preview.addedFiles,
             QStringList{QStringLiteral("doc/guide.md")});
    QCOMPARE(preview.replacedFiles,
             QStringList{QStringLiteral("rtl/top.sv")});
    QCOMPARE(preview.removedFiles,
             QStringList{QStringLiteral("README.md")});

    UpdateAssetResult updated;
    QVERIFY2(service.updateAsset(asset,
                                 replacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 &updated,
                                 &error),
             qPrintable(error));
    QVERIFY(updated.warning.isEmpty());
    QVERIFY(updated.undoToken.isValid());
    QCOMPARE(updated.undoToken.assetId, asset.manifest.id);
    QCOMPARE(updated.undoToken.assetRoot, asset.assetRoot);
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());
    QVERIFY(updated.retainedPaths.isEmpty());
    QString fingerprint;
    QString fingerprintError;
    QVERIFY2(AssetScanner::strictContentHash(updated.updated.manifest,
                                             updated.updated.assetRoot,
                                             &fingerprint,
                                             &fingerprintError),
             qPrintable(fingerprintError));
    QCOMPARE(fingerprint, updated.undoToken.publishedFingerprint);
    const ManifestLoadResult recoveryManifest = ManifestService().load(
        QDir(updated.undoToken.recoveryPath).absoluteFilePath(
            QStringLiteral(".xips.json")));
    QVERIFY(recoveryManifest.ok());
    fingerprint.clear();
    fingerprintError.clear();
    QVERIFY2(AssetScanner::strictContentHash(*recoveryManifest.manifest,
                                             updated.undoToken.recoveryPath,
                                             &fingerprint,
                                             &fingerprintError),
             qPrintable(fingerprintError));
    QCOMPARE(fingerprint, updated.undoToken.recoveryFingerprint);
    QCOMPARE(updated.updated.manifest.id, QStringLiteral("update_ip"));
    QCOMPARE(updated.updated.manifest.name, QStringLiteral("Update IP"));
    QCOMPARE(updated.updated.manifest.tags,
             QStringList{QStringLiteral("UART")});
    QCOMPARE(updated.updated.manifest.version, QStringLiteral("1.0.0"));
    QVERIFY(!QFileInfo(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("README.md"))).exists());
    QFile working(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv")));
    QVERIFY(working.open(QIODevice::ReadOnly));
    QCOMPARE(working.readAll(), revised);
    working.close();
    QVERIFY(QFileInfo(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("doc/guide.md"))).isFile());
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 1);
    const WorkingCopyState state = service.workingCopyState(updated.updated);
    QVERIFY(state.changed);
    QVERIFY(QFileInfo(QDir(original).absoluteFilePath(
        QStringLiteral("README.md"))).isFile());
    QVERIFY(!service.updateAsset(updated.updated,
                                 asset.assetRoot,
                                 WorkingCopyRecoveryMode::Permanent,
                                 nullptr,
                                 &error));
    QVERIFY(error.contains(QStringLiteral("cannot contain")));
    UpdateAssetResult undone;
    error.clear();
    QVERIFY2(service.undoWorkingCopyChange(updated.updated,
                                           updated.undoToken,
                                           &undone,
                                           &error),
             qPrintable(error));
    QVERIFY(undone.warning.isEmpty());
    QVERIFY(undone.retainedPaths.isEmpty());
    QVERIFY(!QFileInfo::exists(updated.undoToken.recoveryPath));
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(QFileInfo(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("README.md"))).isFile());
    QVERIFY(!QFileInfo(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("doc/guide.md"))).exists());
    QFile originalWorking(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv")));
    QVERIFY(originalWorking.open(QIODevice::ReadOnly));
    QCOMPARE(originalWorking.readAll(),
             QByteArrayLiteral("module top; endmodule\n"));
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 1);
    const WorkingCopyState undoneState = service.workingCopyState(undone.updated);
    QVERIFY(!undoneState.changed);
}

void CoreTest::workingCopyUndoFailsClosedAtSecurityBoundaries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString original = createSourceIp(temporary.path());
    QVERIFY(!original.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = original,
                  .metadata = {.id = QStringLiteral("undo_safety"),
                               .name = QStringLiteral("Undo Safety"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    QString failedFingerprint = QStringLiteral("stale fingerprint");
    error.clear();
    QVERIFY(!AssetScanner::strictContentHash(
        asset.manifest,
        temporary.filePath(QStringLiteral("missing-root")),
        &failedFingerprint,
        &error));
    QVERIFY(failedFingerprint.isEmpty());
    QVERIFY(!error.isEmpty());

    const QByteArray originalBytes = QByteArrayLiteral(
        "module top; endmodule\n");
    const QByteArray revisedBytes = QByteArrayLiteral(
        "module top; localparam REV = 2; endmodule\n");
    const QByteArray newerBytes = QByteArrayLiteral(
        "module top; localparam REV = 3; endmodule\n");
    const QByteArray boundaryBytes = QByteArrayLiteral(
        "module top; localparam REV = 4; endmodule\n");
    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QString originalSourceFile = QDir(original).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QString replacement = temporary.filePath(
        QStringLiteral("replacement"));
    const QString replacementFile = QDir(replacement).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QVERIFY(writeFile(replacementFile, revisedBytes));

    error.clear();
    QVERIFY(!service.updateAsset(asset,
                                 replacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 nullptr,
                                 &error));
    QVERIFY(error.contains(QStringLiteral("requires an Undo result token")));
    QCOMPARE(readFile(workingFile), originalBytes);
    QCOMPARE(readFile(originalSourceFile), originalBytes);
    QCOMPARE(readFile(replacementFile), revisedBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());

    UpdateAssetResult updated;
    error.clear();
    QVERIFY2(service.updateAsset(asset,
                                 replacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 &updated,
                                 &error),
             qPrintable(error));
    QVERIFY(updated.undoToken.isValid());
    QCOMPARE(readFile(workingFile), revisedBytes);
    QCOMPARE(readFile(replacementFile), revisedBytes);
    const QString neighbor = QDir(library).absoluteFilePath(
        QStringLiteral("neighbor.keep"));
    const QByteArray neighborBytes = QByteArrayLiteral("do not delete\n");
    QVERIFY(writeFile(neighbor, neighborBytes));

    QVERIFY(writeFile(workingFile, newerBytes));
    UpdateAssetResult rejectedUndo;
    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(updated.updated,
                                           updated.undoToken,
                                           &rejectedUndo,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("changed after the operation")));
    QCOMPARE(readFile(workingFile), newerBytes);
    QCOMPARE(readFile(QDir(updated.undoToken.recoveryPath).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());

    RecoveryDiscardResult explicitDiscard;
    error.clear();
    QVERIFY2(service.discardWorkingCopyRecovery(updated.updated,
                                                updated.undoToken,
                                                RemovalMode::Permanent,
                                                &explicitDiscard,
                                                &error),
             qPrintable(error));
    QVERIFY(explicitDiscard.warning.isEmpty());
    QVERIFY(explicitDiscard.retainedPath.isEmpty());
    QVERIFY(!QFileInfo::exists(updated.undoToken.recoveryPath));
    QCOMPARE(readFile(workingFile), newerBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);

    UpdateAssetResult tamperedRecovery;
    error.clear();
    QVERIFY2(service.updateAsset(updated.updated,
                                 replacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 &tamperedRecovery,
                                 &error),
             qPrintable(error));
    QVERIFY(tamperedRecovery.undoToken.isValid());
    QCOMPARE(readFile(workingFile), revisedBytes);
    const QString tamperedRecoveryFile =
        QDir(tamperedRecovery.undoToken.recoveryPath).absoluteFilePath(
            QStringLiteral("rtl/top.sv"));
    const QByteArray tamperedBytes = QByteArrayLiteral(
        "module top; localparam TAMPERED = 1; endmodule\n");
    QVERIFY(writeFile(tamperedRecoveryFile, tamperedBytes));

    UpdateAssetResult tamperedUndo;
    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(tamperedRecovery.updated,
                                           tamperedRecovery.undoToken,
                                           &tamperedUndo,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("retained recovery changed")));
    QCOMPARE(readFile(workingFile), revisedBytes);
    QCOMPARE(readFile(tamperedRecoveryFile), tamperedBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);

    RecoveryDiscardResult rejectedDiscard;
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(
        tamperedRecovery.updated,
        tamperedRecovery.undoToken,
        RemovalMode::Permanent,
        &rejectedDiscard,
        &error));
    QVERIFY(error.contains(QStringLiteral("not discarded automatically")));
    QVERIFY(rejectedDiscard.retainedPath.isEmpty());
    QVERIFY(rejectedDiscard.warning.isEmpty());
    QVERIFY(QFileInfo(tamperedRecovery.undoToken.recoveryPath).isDir());
    QCOMPARE(readFile(workingFile), revisedBytes);
    QCOMPARE(readFile(tamperedRecoveryFile), tamperedBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-discard-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());

    const QString boundaryReplacement = temporary.filePath(
        QStringLiteral("boundary-replacement"));
    const QString boundaryReplacementFile =
        QDir(boundaryReplacement).absoluteFilePath(
            QStringLiteral("rtl/top.sv"));
    QVERIFY(writeFile(boundaryReplacementFile, boundaryBytes));
    UpdateAssetResult bounded;
    error.clear();
    QVERIFY2(service.updateAsset(tamperedRecovery.updated,
                                 boundaryReplacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 &bounded,
                                 &error),
             qPrintable(error));
    QVERIFY(bounded.undoToken.isValid());
    const WorkingCopyUndoToken validToken = bounded.undoToken;
    const QString validRecoveryFile =
        QDir(validToken.recoveryPath).absoluteFilePath(
            QStringLiteral("rtl/top.sv"));
    QCOMPARE(readFile(validRecoveryFile), revisedBytes);
    QCOMPARE(readFile(workingFile), boundaryBytes);

    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(bounded.updated,
                                           validToken,
                                           nullptr,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("cleanup reporting")));
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(bounded.updated,
                                                validToken,
                                                RemovalMode::Permanent,
                                                nullptr,
                                                &error));
    QVERIFY(error.contains(QStringLiteral("cleanup reporting")));
    QVERIFY(QFileInfo(validToken.recoveryPath).isDir());
    QCOMPARE(readFile(validRecoveryFile), revisedBytes);
    QCOMPARE(readFile(workingFile), boundaryBytes);

    const QString unexpectedVersionRoot =
        QDir(validToken.recoveryPath).absoluteFilePath(
            QStringLiteral(".xips/versions/external"));
    const QString unexpectedVersionFile =
        QDir(unexpectedVersionRoot).absoluteFilePath(
            QStringLiteral("rtl/newer.sv"));
    QVERIFY(writeFile(unexpectedVersionFile,
                      QByteArrayLiteral("module newer; endmodule\n")));
    UpdateAssetResult protectedUndo;
    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(bounded.updated,
                                           validToken,
                                           &protectedUndo,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("saved-version data")));
    RecoveryDiscardResult protectedDiscard;
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(bounded.updated,
                                                validToken,
                                                RemovalMode::Permanent,
                                                &protectedDiscard,
                                                &error));
    QVERIFY(error.contains(QStringLiteral("saved-version data")));
    QVERIFY(QFileInfo(unexpectedVersionFile).isFile());
    QVERIFY(QFileInfo(validToken.recoveryPath).isDir());
    QVERIFY(QDir(QDir(validToken.recoveryPath).absoluteFilePath(
                     QStringLiteral(".xips")))
                .removeRecursively());

    const QString fakeRecovery = QDir(library).absoluteFilePath(
        QStringLiteral(".xips-create-recovery-not-a-uuid"));
    const QString fakeVictim = QDir(fakeRecovery).absoluteFilePath(
        QStringLiteral("victim.keep"));
    const QByteArray fakeVictimBytes = QByteArrayLiteral("fake victim\n");
    QVERIFY(writeFile(fakeVictim, fakeVictimBytes));
    QVERIFY2(ManifestService().write(
                 QDir(fakeRecovery).absoluteFilePath(
                     QStringLiteral(".xips.json")),
                 bounded.updated.manifest,
                 &error),
             qPrintable(error));
    const QString externalRecovery = temporary.filePath(
        QStringLiteral(
            "outside/.xips-create-recovery-11111111-1111-1111-1111-111111111111"));
    const QString externalVictim = QDir(externalRecovery).absoluteFilePath(
        QStringLiteral("victim.keep"));
    const QByteArray externalVictimBytes = QByteArrayLiteral(
        "external victim\n");
    QVERIFY(writeFile(externalVictim, externalVictimBytes));
    QVERIFY2(ManifestService().write(
                 QDir(externalRecovery).absoluteFilePath(
                     QStringLiteral(".xips.json")),
                 bounded.updated.manifest,
                 &error),
             qPrintable(error));

    QList<WorkingCopyUndoToken> invalidTokens;
    QStringList invalidLabels;
    const auto addInvalid = [&invalidTokens, &invalidLabels](
                                const QString &label,
                                const WorkingCopyUndoToken &token) {
        invalidLabels.append(label);
        invalidTokens.append(token);
    };
    WorkingCopyUndoToken invalid = validToken;
    invalid.assetId = QStringLiteral("other_asset");
    addInvalid(QStringLiteral("asset id"), invalid);
    invalid = validToken;
    invalid.assetRoot = temporary.filePath(QStringLiteral("outside/current"));
    addInvalid(QStringLiteral("asset root"), invalid);
    invalid = validToken;
    invalid.publishedFingerprint = QStringLiteral("sha256:not-a-hash");
    addInvalid(QStringLiteral("published fingerprint"), invalid);
    invalid = validToken;
    invalid.recoveryFingerprint = QStringLiteral("invalid");
    addInvalid(QStringLiteral("recovery fingerprint"), invalid);
    invalid = validToken;
    invalid.recoveryPath = fakeRecovery;
    addInvalid(QStringLiteral("fake recovery uuid"), invalid);
    invalid = validToken;
    invalid.recoveryPath = externalRecovery;
    addInvalid(QStringLiteral("external recovery"), invalid);

    QCOMPARE(invalidTokens.size(), invalidLabels.size());
    for (qsizetype index = 0; index < invalidTokens.size(); ++index) {
        UpdateAssetResult invalidUndo;
        error.clear();
        QVERIFY2(!service.undoWorkingCopyChange(bounded.updated,
                                                invalidTokens.at(index),
                                                &invalidUndo,
                                                &error),
                 qPrintable(QStringLiteral("Undo accepted invalid %1 token")
                                .arg(invalidLabels.at(index))));
        QVERIFY2(!error.isEmpty(), qPrintable(invalidLabels.at(index)));
        RecoveryDiscardResult invalidDiscard;
        error.clear();
        QVERIFY2(!service.discardWorkingCopyRecovery(
                     bounded.updated,
                     invalidTokens.at(index),
                     RemovalMode::Permanent,
                     &invalidDiscard,
                     &error),
                 qPrintable(QStringLiteral("Discard accepted invalid %1 token")
                                .arg(invalidLabels.at(index))));
        QVERIFY2(!error.isEmpty(), qPrintable(invalidLabels.at(index)));
        QVERIFY(QFileInfo(validToken.recoveryPath).isDir());
        QCOMPARE(readFile(validRecoveryFile), revisedBytes);
        QCOMPARE(readFile(workingFile), boundaryBytes);
        QCOMPARE(readFile(neighbor), neighborBytes);
        QCOMPARE(readFile(fakeVictim), fakeVictimBytes);
        QCOMPARE(readFile(externalVictim), externalVictimBytes);
        QVERIFY(QDir(library)
                    .entryList({QStringLiteral(".xips-create-discard-*")},
                               QDir::Dirs | QDir::Hidden
                                   | QDir::NoDotAndDotDot)
                    .isEmpty());
    }

    RecoveryDiscardResult validDiscard;
    error.clear();
    QVERIFY2(service.discardWorkingCopyRecovery(bounded.updated,
                                                validToken,
                                                RemovalMode::Permanent,
                                                &validDiscard,
                                                &error),
             qPrintable(error));
    QVERIFY(validDiscard.warning.isEmpty());
    QVERIFY(validDiscard.retainedPath.isEmpty());
    QVERIFY(!QFileInfo::exists(validToken.recoveryPath));
    QVERIFY(QFileInfo(tamperedRecovery.undoToken.recoveryPath).isDir());
    QCOMPARE(readFile(tamperedRecoveryFile), tamperedBytes);
    QCOMPARE(readFile(workingFile), boundaryBytes);
    QCOMPARE(readFile(boundaryReplacementFile), boundaryBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);
    QCOMPARE(readFile(fakeVictim), fakeVictimBytes);
    QCOMPARE(readFile(externalVictim), externalVictimBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-discard-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::workingCopyTransactionsRejectConcurrentMutation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray originalBytes = QByteArrayLiteral(
        "module top; endmodule\n");
    const QByteArray revisedBytes = QByteArrayLiteral(
        "module top; localparam REV = 2; endmodule\n");
    const QByteArray concurrentBytes = QByteArrayLiteral(
        "module top; localparam SYNC = 3; endmodule\n");

    const QList<WorkingCopyRecoveryMode> modes{
        WorkingCopyRecoveryMode::RetainForUndo,
        WorkingCopyRecoveryMode::Permanent,
    };
    for (qsizetype index = 0; index < modes.size(); ++index) {
        const QString caseRoot = temporary.filePath(
            QStringLiteral("publish-race-%1").arg(index));
        const QString source = createSourceIp(caseRoot);
        QVERIFY(!source.isEmpty());
        const QString library = QDir(caseRoot).absoluteFilePath(
            QStringLiteral("library"));
        const QString replacement = QDir(caseRoot).absoluteFilePath(
            QStringLiteral("replacement"));
        const QString replacementFile = QDir(replacement).absoluteFilePath(
            QStringLiteral("rtl/top.sv"));
        QVERIFY(writeFile(replacementFile, revisedBytes));

        AssetLibraryService service;
        AssetRecord asset;
        QString error;
        QVERIFY2(service.importAsset(
                     {.libraryRoot = library,
                      .sourcePath = source,
                      .metadata = {
                          .id = QStringLiteral("publish_race_%1").arg(index),
                          .name = QStringLiteral("Publish Race %1").arg(index),
                          .description = {},
                          .tags = {}}},
                     &asset,
                     &error),
                 qPrintable(error));

        int hookCount = 0;
        bool mutationSucceeded = false;
        service.setWorkingCopyTestHook(
            [&](const WorkingCopyTestPoint point, const QString &path) {
                ++hookCount;
                if (point
                    == WorkingCopyTestPoint::StagingVerifiedBeforePublish) {
                    mutationSucceeded = writeFile(
                        QDir(path).absoluteFilePath(
                            QStringLiteral("rtl/top.sv")),
                        concurrentBytes);
                }
            });

        UpdateAssetResult updated;
        error.clear();
        QVERIFY2(service.updateAsset(asset,
                                     replacement,
                                     modes.at(index),
                                     &updated,
                                     &error),
                 qPrintable(error));
        QCOMPARE(hookCount, 1);
        QVERIFY(mutationSucceeded);
        QVERIFY(!updated.publishedAsIntended);
        QVERIFY(!updated.undoToken.isValid());
        QCOMPARE(updated.retainedPaths.size(), 1);
        const QString recoveryPath = updated.retainedPaths.first();
        QVERIFY(QFileInfo(recoveryPath).isDir());
        QVERIFY(QFileInfo(recoveryPath).fileName().startsWith(
            QStringLiteral(".xips-create-recovery-")));
        QVERIFY(updated.warning.contains(recoveryPath));
        QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 concurrentBytes);
        QCOMPARE(readFile(replacementFile), revisedBytes);
        QCOMPARE(readFile(QDir(recoveryPath).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 originalBytes);
        QVERIFY(QFileInfo(QDir(recoveryPath).absoluteFilePath(
                              QStringLiteral("README.md")))
                    .isFile());
        const ManifestLoadResult recoveryManifest = ManifestService().load(
            QDir(recoveryPath).absoluteFilePath(
                QStringLiteral(".xips.json")));
        QVERIFY(recoveryManifest.ok());
        QCOMPARE(recoveryManifest.manifest->id, asset.manifest.id);
        QVERIFY(QDir(library)
                    .entryList({QStringLiteral(".xips-create-discard-*")},
                               QDir::Dirs | QDir::Hidden
                                   | QDir::NoDotAndDotDot)
                    .isEmpty());
    }

    const QString discardRoot = temporary.filePath(
        QStringLiteral("discard-race"));
    const QString source = createSourceIp(discardRoot);
    QVERIFY(!source.isEmpty());
    const QString library = QDir(discardRoot).absoluteFilePath(
        QStringLiteral("library"));
    const QString replacement = QDir(discardRoot).absoluteFilePath(
        QStringLiteral("replacement"));
    const QString replacementFile = QDir(replacement).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QVERIFY(writeFile(replacementFile, revisedBytes));

    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("discard_race"),
                               .name = QStringLiteral("Discard Race"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    UpdateAssetResult updated;
    QVERIFY2(service.updateAsset(asset,
                                 replacement,
                                 WorkingCopyRecoveryMode::RetainForUndo,
                                 &updated,
                                 &error),
             qPrintable(error));
    QVERIFY(updated.publishedAsIntended);
    QVERIFY(updated.undoToken.isValid());

    const QString recoveryManifestPath = QDir(updated.undoToken.recoveryPath)
                                             .absoluteFilePath(
                                                 QStringLiteral(".xips.json"));
    const ManifestLoadResult originalRecoveryManifest =
        ManifestService().load(recoveryManifestPath);
    QVERIFY(originalRecoveryManifest.ok());
    Manifest changedRecoveryManifest = *originalRecoveryManifest.manifest;
    changedRecoveryManifest.description = QStringLiteral(
        "manifest changed before Undo");
    QVERIFY2(ManifestService().write(recoveryManifestPath,
                                     changedRecoveryManifest,
                                     &error),
             qPrintable(error));
    UpdateAssetResult rejectedManifestUndo;
    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(updated.updated,
                                           updated.undoToken,
                                           &rejectedManifestUndo,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("retained recovery changed")));
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             revisedBytes);
    QVERIFY2(ManifestService().write(recoveryManifestPath,
                                     *originalRecoveryManifest.manifest,
                                     &error),
             qPrintable(error));

    const QString ignoredVictim = QDir(updated.undoToken.recoveryPath)
                                      .absoluteFilePath(
                                          QStringLiteral(".cache/victim.keep"));
    QVERIFY(writeFile(ignoredVictim, QByteArrayLiteral("keep synced data\n")));
    RecoveryDiscardResult blockedDiscard;
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(updated.updated,
                                                updated.undoToken,
                                                RemovalMode::Permanent,
                                                &blockedDiscard,
                                                &error));
    QVERIFY(error.contains(QStringLiteral("unrecognized data")));
    QVERIFY(QFileInfo(ignoredVictim).isFile());
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());
    QVERIFY(QDir(QDir(updated.undoToken.recoveryPath).absoluteFilePath(
                     QStringLiteral(".cache")))
                .removeRecursively());

    int hookCount = 0;
    bool manifestMutationSucceeded = false;
    const QString concurrentDescription = QStringLiteral(
        "manifest changed by concurrent sync");
    service.setWorkingCopyTestHook(
        [&](const WorkingCopyTestPoint point, const QString &path) {
            ++hookCount;
            if (point
                != WorkingCopyTestPoint::RecoveryVerifiedBeforeDiscardIsolation) {
                return;
            }
            const QString manifestPath = QDir(path).absoluteFilePath(
                QStringLiteral(".xips.json"));
            const ManifestLoadResult loaded = ManifestService().load(
                manifestPath);
            if (!loaded.ok()) {
                return;
            }
            Manifest changed = *loaded.manifest;
            changed.description = concurrentDescription;
            QString writeError;
            manifestMutationSucceeded = ManifestService().write(
                manifestPath,
                changed,
                &writeError);
        });

    RecoveryDiscardResult isolated;
    error.clear();
    QVERIFY2(service.discardWorkingCopyRecovery(updated.updated,
                                                updated.undoToken,
                                                RemovalMode::Permanent,
                                                &isolated,
                                                &error),
             qPrintable(error));
    QCOMPARE(hookCount, 1);
    QVERIFY(manifestMutationSucceeded);
    QVERIFY(!isolated.warning.isEmpty());
    QVERIFY(!isolated.retainedPath.isEmpty());
    QVERIFY(isolated.warning.contains(isolated.retainedPath));
    QVERIFY(!QFileInfo::exists(updated.undoToken.recoveryPath));
    QVERIFY(QFileInfo(isolated.retainedPath).isDir());
    const ManifestLoadResult retainedManifest = ManifestService().load(
        QDir(isolated.retainedPath).absoluteFilePath(
            QStringLiteral(".xips.json")));
    QVERIFY(retainedManifest.ok());
    QCOMPARE(retainedManifest.manifest->description,
             concurrentDescription);
    QCOMPARE(readFile(QDir(isolated.retainedPath).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             revisedBytes);
    QCOMPARE(readFile(replacementFile), revisedBytes);
}

void CoreTest::assetDeletionIsBoundedAndLeavesSourcesUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("delete_me.sv"));
    QVERIFY(writeFile(source, QByteArrayLiteral("module delete_me; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = service.suggestedMetadata(source)},
                 &asset,
                 &error),
             qPrintable(error));
    QVERIFY(!service.deleteAsset(asset.assetRoot,
                                 asset,
                                 RemovalMode::Permanent,
                                 nullptr,
                                 &error));
    QVERIFY(QFileInfo(asset.assetRoot).isDir());

    AssetRecord forged = asset;
    forged.assetRoot = library;
    forged.manifestPath = QDir(library).absoluteFilePath(
        QStringLiteral(".xips.json"));
    QVERIFY(!service.deleteAsset(library,
                                 forged,
                                 RemovalMode::Permanent,
                                 nullptr,
                                 &error));
    QVERIFY(QFileInfo(library).isDir());

    QString removedPath;
    QVERIFY2(service.deleteAsset(library,
                                 asset,
                                 RemovalMode::Permanent,
                                 &removedPath,
                                 &error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(asset.assetRoot));
    QVERIFY(QFileInfo(source).isFile());
    QVERIFY(QFileInfo(library).isDir());
    QVERIFY(removedPath.isEmpty());
}

void CoreTest::versionsAreImmutableAndCopyable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("versioned_ip"),
                               .name = QStringLiteral("Versioned IP"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    VersionInfo first;
    QVERIFY2(service.createVersion(asset, QStringLiteral("1.0.0"), &first, &error),
             qPrintable(error));
    const QString workingSource = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QVERIFY(writeFile(workingSource,
                      QByteArrayLiteral("module top; localparam V = 2; endmodule\n")));
    VersionInfo second;
    QVERIFY2(service.createVersion(asset, QStringLiteral("1.1.0"), &second, &error),
             qPrintable(error));
    QVERIFY(first.contentHash != second.contentHash);
    const QList<VersionInfo> versions = service.versions(asset.assetRoot, &error);
    QCOMPARE(versions.size(), 2);
    QVERIFY(!service.createVersion(asset, QStringLiteral("1.0.0"), nullptr, &error));

    const CopyPlan workingPlan = service.copyPlan(asset, QString());
    QVERIFY2(workingPlan.ok(), qPrintable(workingPlan.error));
    QCOMPARE(workingPlan.version, QString());
    QCOMPARE(workingPlan.sourceRoot, asset.assetRoot);
    QCOMPARE(workingPlan.suggestedName, QStringLiteral("Versioned IP"));
    const CopyPlan savedPlan = service.copyPlan(asset, QStringLiteral("1.0.0"));
    QVERIFY2(savedPlan.ok(), qPrintable(savedPlan.error));
    QCOMPARE(savedPlan.version, QStringLiteral("1.0.0"));
    QCOMPARE(savedPlan.sourceRoot, first.path);
    QCOMPARE(savedPlan.suggestedName, QStringLiteral("Versioned IP"));

    const QString copyDirectory = temporary.filePath(QStringLiteral("copy-to"));
    QVERIFY(QDir().mkpath(copyDirectory));
    const QString requestedTarget = QDir(copyDirectory).absoluteFilePath(
        QStringLiteral("versioned_ip"));
    QString copiedRoot;
    QVERIFY2(service.copyVersionPayload(asset,
                                        QStringLiteral("1.0.0"),
                                        requestedTarget,
                                        &copiedRoot,
                                        &error),
             qPrintable(error));
    QCOMPARE(QFileInfo(copiedRoot).fileName(), QStringLiteral("versioned_ip"));
    QFile copied(QDir(copiedRoot).absoluteFilePath(QStringLiteral("rtl/top.sv")));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArrayLiteral("module top; endmodule\n"));
    QVERIFY(!QFileInfo::exists(QDir(copiedRoot).absoluteFilePath(
        QStringLiteral(".xips.json"))));
}

void CoreTest::versionStateCopyToAndDeletionFormASafeWorkflow()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("uart_rx.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module uart_rx; localparam V = 1; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = service.suggestedMetadata(source)},
                 &asset,
                 &error),
             qPrintable(error));

    QCOMPARE(service.suggestedNextVersion(QString()), QStringLiteral("1.0.0"));
    QCOMPARE(service.suggestedNextVersion(QStringLiteral("1.2.9")),
             QStringLiteral("1.2.10"));
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   nullptr,
                                   &error),
             qPrintable(error));
    WorkingCopyState state = service.workingCopyState(asset);
    QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
    QVERIFY(state.hasSavedVersion);
    QVERIFY(!state.changed);
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   nullptr,
                                   &error));
    QVERIFY(error.contains(QStringLiteral("has not changed")));

    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("uart_rx.sv"));
    QVERIFY(writeFile(workingFile,
                      QByteArrayLiteral("module uart_rx; localparam V = 2; endmodule\n")));
    state = service.workingCopyState(asset);
    QVERIFY(state.changed);
    error.clear();
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   nullptr,
                                   &error),
             qPrintable(error));

    const CopyPlan singleFilePlan = service.copyPlan(
        asset,
        QStringLiteral("1.0.0"));
    QVERIFY2(singleFilePlan.ok(), qPrintable(singleFilePlan.error));
    QVERIFY(singleFilePlan.isSingleFile());
    QCOMPARE(singleFilePlan.suggestedName, QStringLiteral("uart_rx.sv"));

    const QString destination = temporary.filePath(QStringLiteral("copy-to"));
    QVERIFY(QDir().mkpath(destination));
    const QString requestedTarget = QDir(destination).absoluteFilePath(
        QStringLiteral("uart_rx.sv"));
    QString copiedPath;
    QVERIFY2(service.copyVersionPayload(asset,
                                        QStringLiteral("1.0.0"),
                                        requestedTarget,
                                        &copiedPath,
                                        &error),
             qPrintable(error));
    QCOMPARE(QFileInfo(copiedPath).fileName(), QStringLiteral("uart_rx.sv"));
    QVERIFY(!QFileInfo(QDir(destination).absoluteFilePath(
        QStringLiteral(".xips.json"))).exists());
    QFile copied(copiedPath);
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QVERIFY(copied.readAll().contains(QByteArrayLiteral("V = 1")));
    QVERIFY(!service.copyVersionPayload(asset,
                                        QStringLiteral("1.0.0"),
                                        requestedTarget,
                                        nullptr,
                                        &error));

    const QList<VersionInfo> versionsBeforeRestore = service.versions(
        asset.assetRoot, &error);
    QCOMPARE(versionsBeforeRestore.size(), 2);
    const ManifestLoadResult manifestBeforeRestore = ManifestService().load(
        asset.manifestPath);
    QVERIFY(manifestBeforeRestore.ok());
    QCOMPARE(manifestBeforeRestore.manifest->version,
             QStringLiteral("1.0.1"));
    const UpdatePreview restorePreview = service.previewRestore(
        asset, QStringLiteral("1.0.0"));
    QVERIFY2(restorePreview.ok(), qPrintable(restorePreview.error));
    QCOMPARE(restorePreview.addedFiles.size(), 0);
    QCOMPARE(restorePreview.replacedFiles,
             QStringList{QStringLiteral("uart_rx.sv")});
    QCOMPARE(restorePreview.removedFiles.size(), 0);
    UpdateAssetResult restored;
    error.clear();
    QVERIFY2(service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::RetainForUndo,
                                    &restored,
                                    &error),
             qPrintable(error));
    QCOMPARE(restored.preview.replacedFiles,
             QStringList{QStringLiteral("uart_rx.sv")});
    QVERIFY(restored.warning.isEmpty());
    QVERIFY(restored.undoToken.isValid());
    QVERIFY(QFileInfo(restored.undoToken.recoveryPath).isDir());
    QCOMPARE(restored.updated.manifest.version, QStringLiteral("1.0.1"));
    QFile restoredWorking(workingFile);
    QVERIFY(restoredWorking.open(QIODevice::ReadOnly));
    QVERIFY(restoredWorking.readAll().contains(QByteArrayLiteral("V = 1")));
    restoredWorking.close();
    const QList<VersionInfo> versionsAfterRestore = service.versions(
        asset.assetRoot, &error);
    QCOMPARE(versionsAfterRestore.size(), versionsBeforeRestore.size());
    for (const VersionInfo &before : versionsBeforeRestore) {
        const auto after = std::find_if(
            versionsAfterRestore.cbegin(),
            versionsAfterRestore.cend(),
            [&before](const VersionInfo &candidate) {
                return candidate.version == before.version;
            });
        QVERIFY(after != versionsAfterRestore.cend());
        QCOMPARE(after->contentHash, before.contentHash);
    }
    const ManifestLoadResult manifestAfterRestore = ManifestService().load(
        asset.manifestPath);
    QVERIFY(manifestAfterRestore.ok());
    QCOMPARE(manifestAfterRestore.manifest->id,
             manifestBeforeRestore.manifest->id);
    QCOMPARE(manifestAfterRestore.manifest->name,
             manifestBeforeRestore.manifest->name);
    QCOMPARE(manifestAfterRestore.manifest->description,
             manifestBeforeRestore.manifest->description);
    QCOMPARE(manifestAfterRestore.manifest->tags,
             manifestBeforeRestore.manifest->tags);
    QCOMPARE(manifestAfterRestore.manifest->version,
             manifestBeforeRestore.manifest->version);
    QVERIFY(writeFile(workingFile,
                      QByteArrayLiteral(
                          "module uart_rx; localparam V = 3; endmodule\n")));
    UpdateAssetResult undoneRestore;
    error.clear();
    QVERIFY(!service.undoWorkingCopyChange(asset,
                                           restored.undoToken,
                                           &undoneRestore,
                                           &error));
    QVERIFY(error.contains(QStringLiteral("changed after the operation")));
    QVERIFY(QFileInfo(restored.undoToken.recoveryPath).isDir());
    QVERIFY(writeFile(workingFile,
                      QByteArrayLiteral(
                          "module uart_rx; localparam V = 1; endmodule\n")));
    error.clear();
    QVERIFY2(service.undoWorkingCopyChange(asset,
                                           restored.undoToken,
                                           &undoneRestore,
                                           &error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(restored.undoToken.recoveryPath));
    QFile undoRestoredWorking(workingFile);
    QVERIFY(undoRestoredWorking.open(QIODevice::ReadOnly));
    QVERIFY(undoRestoredWorking.readAll().contains(QByteArrayLiteral("V = 2")));
    undoRestoredWorking.close();
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 2);
    UpdateAssetResult restoredAgain;
    QVERIFY2(service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::RetainForUndo,
                                    &restoredAgain,
                                    &error),
             qPrintable(error));
    QVERIFY(QFileInfo(restoredAgain.undoToken.recoveryPath).isDir());
    RecoveryDiscardResult discarded;
    QVERIFY2(service.discardWorkingCopyRecovery(asset,
                                                restoredAgain.undoToken,
                                                RemovalMode::Permanent,
                                                &discarded,
                                                &error),
             qPrintable(error));
    QVERIFY(discarded.warning.isEmpty());
    QVERIFY(!QFileInfo::exists(restoredAgain.undoToken.recoveryPath));
    state = service.workingCopyState(asset);
    QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
    QVERIFY(state.changed);
    QCOMPARE(state.latestVersion, QStringLiteral("1.0.1"));
    error.clear();
    QVERIFY(!service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::Permanent,
                                    nullptr,
                                    &error));
    QVERIFY(error.contains(QStringLiteral("already matches")));
    error.clear();
    QVERIFY(!service.restoreVersion(asset,
                                    QStringLiteral("missing"),
                                    WorkingCopyRecoveryMode::Permanent,
                                    nullptr,
                                    &error));
    QVERIFY(error.contains(QStringLiteral("Version not found")));
    const QStringList restoreStaging = QDir(QFileInfo(asset.assetRoot).absolutePath())
                                           .entryList(
                                               {QStringLiteral(".xips-create-restore-*")},
                                               QDir::Dirs | QDir::Hidden
                                                   | QDir::NoDotAndDotDot);
    QVERIFY(restoreStaging.isEmpty());

    QVERIFY2(service.deleteVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   RemovalMode::Permanent,
                                   &error),
             qPrintable(error));
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 1);
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->version, QStringLiteral("1.0.0"));
    QVERIFY(!service.deleteVersion(asset,
                                   QString(),
                                   RemovalMode::Permanent,
                                   &error));
}

void CoreTest::groupChangesApplyAcrossAssets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString first = temporary.filePath(QStringLiteral("one.sv"));
    const QString second = temporary.filePath(QStringLiteral("two.sv"));
    QVERIFY(writeFile(first, QByteArrayLiteral("module one; endmodule\n")));
    QVERIFY(writeFile(second, QByteArrayLiteral("module two; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    AssetLibraryService service;
    const ImportBatchResult imported = service.importAssets(library, {first, second});
    QCOMPARE(imported.created.size(), 2);

    int changed = 0;
    QString error;
    QVERIFY2(service.changeGroupMembership(imported.created,
                                           QString(),
                                           QStringLiteral("AXI"),
                                           &changed,
                                           &error),
             qPrintable(error));
    QCOMPARE(changed, 2);
    QVERIFY2(service.changeGroupMembership(imported.created,
                                           QStringLiteral("AXI"),
                                           QStringLiteral("AMBA"),
                                           &changed,
                                           &error),
             qPrintable(error));
    QCOMPARE(changed, 2);
    const ScanResult renamed = AssetScanner().scan(library);
    QCOMPARE(renamed.assets.size(), 2);
    for (const AssetRecord &asset : renamed.assets) {
        QCOMPARE(asset.manifest.tags, QStringList{QStringLiteral("AMBA")});
    }
    QVERIFY2(service.changeGroupMembership(renamed.assets,
                                           QStringLiteral("AMBA"),
                                           QString(),
                                           &changed,
                                           &error),
             qPrintable(error));
    QCOMPARE(changed, 2);
    const ScanResult removed = AssetScanner().scan(library);
    for (const AssetRecord &asset : removed.assets) {
        QVERIFY(asset.manifest.tags.isEmpty());
    }
}

void CoreTest::activationUrisParse()
{
    const QUrl assetUri = IntegrationService::assetUri(QStringLiteral("uart_ip"));
    QCOMPARE(assetUri.toString(QUrl::FullyEncoded),
             QStringLiteral("xips://asset/uart_ip"));
    const auto assetRequest = IntegrationService::parseUri(assetUri);
    QVERIFY(assetRequest.has_value());
    QCOMPARE(assetRequest->action, ActivationAction::OpenAsset);
    QCOMPARE(assetRequest->value, QStringLiteral("uart_ip"));

    const QUrl searchUri(QStringLiteral("xips://search?q=axi%20fifo"));
    const auto searchRequest = IntegrationService::parseUri(searchUri);
    QVERIFY(searchRequest.has_value());
    QCOMPARE(searchRequest->action, ActivationAction::Search);
    QCOMPARE(searchRequest->value, QStringLiteral("axi fifo"));
    QVERIFY(!IntegrationService::parseUri(QUrl(QStringLiteral("https://example.com"))));
}

void CoreTest::cliListsAndResolvesAssets()
{
    const QString cli = QString::fromUtf8(XIPS_CLI_PATH);
    const QString library = QString::fromUtf8(XIPS_EXAMPLE_LIBRARY);
    QProcess list;
    list.start(cli,
               {QStringLiteral("--action"), QStringLiteral("list"),
                QStringLiteral("--library"), library,
                QStringLiteral("--query"), QStringLiteral("reset")});
    QVERIFY(list.waitForFinished(10000));
    QCOMPARE(list.exitStatus(), QProcess::NormalExit);
    QCOMPARE(list.exitCode(), 0);
    const QJsonObject listEnvelope = QJsonDocument::fromJson(
        list.readAllStandardOutput()).object();
    QVERIFY(listEnvelope.value(QStringLiteral("ok")).toBool());
    const QJsonArray assets = listEnvelope.value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("assets"))
                                  .toArray();
    QVERIFY(!assets.isEmpty());
    bool foundResetGenerator = false;
    for (const QJsonValue &value : assets) {
        foundResetGenerator |= value.toObject().value(QStringLiteral("id")).toString()
                               == QStringLiteral("reset_gen");
    }
    QVERIFY(foundResetGenerator);

    QProcess fileSearch;
    fileSearch.start(cli,
                     {QStringLiteral("--action"), QStringLiteral("list"),
                      QStringLiteral("--library"), library,
                      QStringLiteral("--query"),
                      QStringLiteral("reset_config.svh")});
    QVERIFY(fileSearch.waitForFinished(10000));
    QCOMPARE(fileSearch.exitCode(), 0);
    const QJsonArray fileMatches = QJsonDocument::fromJson(
        fileSearch.readAllStandardOutput())
                                       .object()
                                       .value(QStringLiteral("data"))
                                       .toObject()
                                       .value(QStringLiteral("assets"))
                                       .toArray();
    QCOMPARE(fileMatches.size(), 1);
    QCOMPARE(fileMatches.first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("reset_gen"));

    QProcess resolve;
    resolve.start(cli,
                  {QStringLiteral("--action"), QStringLiteral("resolve"),
                   QStringLiteral("--library"), library,
                   QStringLiteral("--asset"), QStringLiteral("reset_gen")});
    QVERIFY(resolve.waitForFinished(10000));
    QCOMPARE(resolve.exitCode(), 0);
    const QJsonObject resolved = QJsonDocument::fromJson(
        resolve.readAllStandardOutput()).object();
    QCOMPARE(resolved.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("resolvedVersion"))
                 .toString(),
             QStringLiteral("working"));
}

QTEST_APPLESS_MAIN(CoreTest)

#include "tst_core.moc"
