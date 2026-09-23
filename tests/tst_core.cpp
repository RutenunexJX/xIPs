#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"
#include "library/AssetScanner.h"
#include "library/FileSystemUtil.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

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

struct SimulatedManifestCrash final {
};

bool leaveMetadataTransaction(AssetLibraryService &service,
                              const AssetRecord &asset,
                              const QString &replacementName,
                              const WorkingCopyTestPoint crashPoint,
                              QString *error)
{
    service.setWorkingCopyTestHook(
        crashPoint,
        [crashPoint](const WorkingCopyTestPoint point, const QString &) {
            if (point == crashPoint) {
                throw SimulatedManifestCrash{};
            }
        });
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            asset,
            {.id = asset.manifest.id,
             .name = replacementName,
             .description = asset.manifest.description,
             .tags = asset.manifest.tags},
            &ignored,
            error);
    } catch (const SimulatedManifestCrash &) {
        return true;
    }
    return false;
}

} // namespace

class CoreTest final : public QObject {
    Q_OBJECT

private slots:
    void manifestWritesMinimalSchemaAndPreservesUnknownFields();
    void importRejectsUnavailableLibraryRoot();
    void importFolderCreatesPortableAsset();
    void importSingleFileCreatesAsset();
    void externalSourceCopyIsBoundToOneVerifiedMoment();
    void copyPayloadRejectsLinkedStagingBeforeWrite();
    void batchImportRecoversCollisionsAndAssignsGroups();
    void batchImportAvoidsAmbiguousManifestIds();
    void scannerListsPayloadAndHashesOnDemand();
    void linkedLibraryRootIsRejected();
    void staleAssetRecordRejectsLinkedAncestor();
    void scannerRejectsCaseOnlyDuplicateIds();
    void metadataKeepsStableId();
    void metadataMergesConcurrentManifestChangesAndRejectsConflicts();
    void metadataNoOpRechecksLiveManifest();
    void manifestTransactionsRecoverCrashWindows();
    void manifestTransactionsRecoverNestedAssets();
    void manifestTransactionChainsRestoreLatestOriginal();
    void corruptManifestTransactionBlocksAssetRecovery();
    void manifestTransactionChainsRetireAllAncestors();
    void corruptAncestorTransactionDoesNotHideNestedRecovery();
    void manifestTransactionRetirementPreservesDataWhenLiveChanges();
    void manifestCasRetirementRejectsThirdPartyLive();
    void manifestRecoveryPathIdentityIsCaseInsensitiveOnWindows();
    void updateReplacesWorkingCopyAndPreservesSavedVersions();
    void workingCopyUndoFailsClosedAtSecurityBoundaries();
    void workingCopyTransactionsRejectConcurrentMutation();
    void workingCopyDiscardRequiresValidLiveAsset_data();
    void workingCopyDiscardRequiresValidLiveAsset();
    void recoveryIsolationRacePreservesUndoData_data();
    void recoveryIsolationRacePreservesUndoData();
    void assetDeletionIsBoundedAndLeavesSourcesUntouched();
    void assetDeletionProofRejectsStaleAndBoundaryChanges();
    void versionsAreImmutableAndCopyable();
    void versionStateCopyToAndDeletionFormASafeWorkflow();
    void corruptSavedVersionIsRejectedEverywhere_data();
    void corruptSavedVersionIsRejectedEverywhere();
    void versionInventoryKeepsValidVersionsWhenOneIsCorrupt();
    void metadataChangesDoNotCreateCodeVersions();
    void corruptVersionDoesNotBlockHealthyVersionWorkflows();
    void payloadTransactionsRestoreVisibleDataAfterIsolationCrash();
    void payloadRecoveryIgnoresForgedControlDataInsideAssets();
    void payloadRecoveryDoesNotFollowLinkedInternalDirectory();
    void payloadRecoveryRetainsRecordWithConcurrentEvidence();
    void versionInventorySkipsLinkedVersionDirectories();
    void createVersionRejectsConcurrentWorkingCopyMutation_data();
    void createVersionRejectsConcurrentWorkingCopyMutation();
    void savedVersionConsumersRejectPostCopyMutation_data();
    void savedVersionConsumersRejectPostCopyMutation();
    void restoreBindsVerifiedStagingToUpdate_data();
    void restoreBindsVerifiedStagingToUpdate();
    void unverifiedOperationStagingIsRetained_data();
    void unverifiedOperationStagingIsRetained();
    void deleteVersionRejectsConcurrentBoundaryMutation_data();
    void deleteVersionRejectsConcurrentBoundaryMutation();
    void groupChangesApplyAcrossAssets();
    void groupChangesMergeConcurrencyAndKeepPartialSuccess();
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

void CoreTest::importRejectsUnavailableLibraryRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("offline.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module offline; endmodule\n")));
    const QString missingLibrary = temporary.filePath(
        QStringLiteral("Jianguoyun/offline-xIPs"));
    AssetLibraryService service;
    AssetRecord ignored;
    QString error;
    QVERIFY(!service.importAsset(
        {.libraryRoot = missingLibrary,
         .sourcePath = source,
         .metadata = {.id = QStringLiteral("offline"),
                      .name = QStringLiteral("Offline"),
                      .description = {},
                      .tags = {}}},
        &ignored,
        &error));
    QVERIFY(error.contains(QStringLiteral("must already exist")));
    QVERIFY(!QFileInfo::exists(missingLibrary));
}

void CoreTest::importFolderCreatesPortableAsset()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
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
    QVERIFY(QDir().mkpath(library));
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

void CoreTest::externalSourceCopyIsBoundToOneVerifiedMoment()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source.sv"));
    const QByteArray original = QByteArrayLiteral("module source; endmodule\n");
    const QByteArray changed = QByteArrayLiteral(
        "module source; localparam CHANGED = 1; endmodule\n");
    QVERIFY(writeFile(source, original));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));

    AssetLibraryService service;
    int hookCount = 0;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ImportSourceCopiedBeforeVerification,
        [&](const WorkingCopyTestPoint point, const QString &) {
            if (point
                == WorkingCopyTestPoint::ImportSourceCopiedBeforeVerification) {
                ++hookCount;
                QVERIFY(writeFile(source, changed));
            }
        });
    AssetRecord rejected;
    QString error;
    QVERIFY(!service.importAsset(
        {.libraryRoot = library,
         .sourcePath = source,
         .metadata = {.id = QStringLiteral("source"),
                      .name = QStringLiteral("Source"),
                      .description = {},
                      .tags = {}}},
        &rejected,
        &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(error.contains(QStringLiteral("changed while it was copied")));
    QVERIFY(!QFileInfo::exists(QDir(library).absoluteFilePath(
        QStringLiteral("source"))));

    QVERIFY(writeFile(source, original));
    AssetRecord asset;
    error.clear();
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("source"),
                               .name = QStringLiteral("Source"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    const QString updateSource = temporary.filePath(
        QStringLiteral("update.sv"));
    const QByteArray replacement = QByteArrayLiteral(
        "module source; localparam REV = 2; endmodule\n");
    QVERIFY(writeFile(updateSource, replacement));
    hookCount = 0;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::UpdateSourceCopiedBeforeVerification,
        [&](const WorkingCopyTestPoint point, const QString &) {
            if (point
                == WorkingCopyTestPoint::UpdateSourceCopiedBeforeVerification) {
                ++hookCount;
                QVERIFY(writeFile(updateSource, changed));
            }
        });
    UpdateAssetResult update;
    error.clear();
    QVERIFY(!service.updateAsset(asset,
                                 updateSource,
                                 WorkingCopyRecoveryMode::Permanent,
                                 &update,
                                 &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(error.contains(QStringLiteral("changed while it was copied")));
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("source.sv"))),
             original);
}

void CoreTest::copyPayloadRejectsLinkedStagingBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module source; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("copy_source"),
                               .name = QStringLiteral("Copy source"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    const QString destinationParent = temporary.filePath(
        QStringLiteral("copy-destination"));
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkpath(destinationParent));
    QVERIFY(QDir().mkpath(outside));
    const QString sentinel = QDir(outside).absoluteFilePath(
        QStringLiteral("sentinel.bin"));
    const QByteArray sentinelBytes = QByteArrayLiteral("outside-unchanged");
    QVERIFY(writeFile(sentinel, sentinelBytes));

    QString linkedStaging;
    bool linkCreated = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::CopyStagingCreatedBeforePayloadCopy,
        [&](const WorkingCopyTestPoint, const QString &stagingRoot) {
            linkedStaging = stagingRoot;
            if (!QDir().rmdir(stagingRoot)) {
                return;
            }
#ifdef Q_OS_WIN
            QProcess junction;
            junction.start(QStringLiteral("cmd.exe"),
                           {QStringLiteral("/d"),
                            QStringLiteral("/c"),
                            QStringLiteral("mklink"),
                            QStringLiteral("/J"),
                            QDir::toNativeSeparators(stagingRoot),
                            QDir::toNativeSeparators(outside)});
            linkCreated = junction.waitForFinished(10000)
                          && junction.exitStatus() == QProcess::NormalExit
                          && junction.exitCode() == 0;
#else
            linkCreated = QFile::link(outside, stagingRoot);
#endif
        });

    const QString requestedTarget = QDir(destinationParent).absoluteFilePath(
        QStringLiteral("copied.sv"));
    QVERIFY(!service.copyVersionPayload(asset,
                                        QString(),
                                        requestedTarget,
                                        nullptr,
                                        &error));
    QVERIFY(linkCreated);
    QVERIFY(!QFileInfo::exists(requestedTarget));
    QCOMPARE(readFile(sentinel), sentinelBytes);
    QVERIFY(!QFileInfo::exists(QDir(outside).absoluteFilePath(
        QStringLiteral("source.sv"))));
#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedStaging));
#else
    QVERIFY(QFile::remove(linkedStaging));
#endif
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
    QVERIFY(QDir().mkpath(library));
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

void CoreTest::batchImportAvoidsAmbiguousManifestIds()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString firstRoot = QDir(library).absoluteFilePath(
        QStringLiteral("nested/first"));
    const QString secondRoot = QDir(library).absoluteFilePath(
        QStringLiteral("nested/second"));
    QVERIFY(writeFile(QDir(firstRoot).absoluteFilePath(
                          QStringLiteral("first.sv")),
                      QByteArrayLiteral("module first; endmodule\n")));
    QVERIFY(writeFile(QDir(secondRoot).absoluteFilePath(
                          QStringLiteral("second.sv")),
                      QByteArrayLiteral("module second; endmodule\n")));

    QString error;
    Manifest first;
    first.id = QStringLiteral("uart_sv");
    first.name = QStringLiteral("First UART");
    QVERIFY2(ManifestService().write(
                 QDir(firstRoot).absoluteFilePath(
                     QStringLiteral(".xips.json")),
                 first,
                 &error),
             qPrintable(error));
    Manifest second;
    second.id = QStringLiteral("UART_SV");
    second.name = QStringLiteral("Second UART");
    QVERIFY2(ManifestService().write(
                 QDir(secondRoot).absoluteFilePath(
                     QStringLiteral(".xips.json")),
                 second,
                 &error),
             qPrintable(error));

    const ScanResult ambiguous = AssetScanner().scan(library);
    QVERIFY(ambiguous.assets.isEmpty());
    QCOMPARE(ambiguous.discoveredAssetIds.size(), 2);

    const QString source = temporary.filePath(
        QStringLiteral("incoming/uart.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module imported_uart; endmodule\n")));
    const ImportBatchResult imported = AssetLibraryService().importAssets(
        library, {source});
    QCOMPARE(imported.errors.size(), 0);
    QCOMPARE(imported.created.size(), 1);
    QCOMPARE(imported.created.first().manifest.id,
             QStringLiteral("uart_sv_2"));

    const ScanResult refreshed = AssetScanner().scan(library);
    QCOMPARE(refreshed.assets.size(), 1);
    QCOMPARE(refreshed.assets.first().manifest.id,
             QStringLiteral("uart_sv_2"));
    QCOMPARE(refreshed.discoveredAssetIds.size(), 3);
    QCOMPARE(refreshed.errors.size(), 2);
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

void CoreTest::linkedLibraryRootIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source.sv"));
    const QByteArray sourceBytes = QByteArrayLiteral(
        "module source; endmodule\n");
    QVERIFY(writeFile(source, sourceBytes));
    const QString realLibrary = temporary.filePath(
        QStringLiteral("real-library"));
    QVERIFY(QDir().mkpath(realLibrary));
    AssetLibraryService service;
    AssetRecord realAsset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = realLibrary,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("linked_root_asset"),
                               .name = QStringLiteral("Linked root asset"),
                               .description = {},
                               .tags = {}}},
                 &realAsset,
                 &error),
             qPrintable(error));
    const QString linkedLibrary = temporary.filePath(
        QStringLiteral("linked-library"));
#ifdef Q_OS_WIN
    QProcess junction;
    junction.start(QStringLiteral("cmd.exe"),
                   {QStringLiteral("/d"),
                    QStringLiteral("/c"),
                    QStringLiteral("mklink"),
                    QStringLiteral("/J"),
                    QDir::toNativeSeparators(linkedLibrary),
                    QDir::toNativeSeparators(realLibrary)});
    QVERIFY(junction.waitForFinished(10000));
    QCOMPARE(junction.exitStatus(), QProcess::NormalExit);
    QCOMPARE(junction.exitCode(), 0);
#else
    QVERIFY(QFile::link(realLibrary, linkedLibrary));
#endif

    const ScanResult scan = AssetScanner().scan(linkedLibrary);
    QVERIFY(scan.assets.isEmpty());
    QVERIFY(!scan.errors.isEmpty());

    AssetRecord linkedAsset = realAsset;
    linkedAsset.assetRoot = QDir(linkedLibrary).absoluteFilePath(
        realAsset.manifest.id);
    linkedAsset.manifestPath = QDir(linkedAsset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    AssetDeletionProof proof = service.assetDeletionProof(realAsset);
    QVERIFY(proof.ok());
    proof.assetRoot = linkedAsset.assetRoot;
    error.clear();
    QVERIFY(!service.deleteAsset(linkedLibrary,
                                 linkedAsset,
                                 proof,
                                 RemovalMode::Permanent,
                                 nullptr,
                                 &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo(realAsset.assetRoot).isDir());
    QCOMPARE(readFile(QDir(realAsset.assetRoot).absoluteFilePath(
                          QStringLiteral("source.sv"))),
             sourceBytes);
#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedLibrary));
#else
    QVERIFY(QFile::remove(linkedLibrary));
#endif
}

void CoreTest::staleAssetRecordRejectsLinkedAncestor()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString linkedParent = QDir(library).absoluteFilePath(
        QStringLiteral("nested/container"));
    QVERIFY(QDir().mkpath(linkedParent));
    const QString source = temporary.filePath(
        QStringLiteral("incoming/stale.sv"));
    const QByteArray firstRevision = QByteArrayLiteral(
        "module stale; localparam REV = 1; endmodule\n");
    const QByteArray secondRevision = QByteArrayLiteral(
        "module stale; localparam REV = 2; endmodule\n");
    const QByteArray thirdRevision = QByteArrayLiteral(
        "module stale; localparam REV = 3; endmodule\n");
    QVERIFY(writeFile(source, firstRevision));

    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = linkedParent,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("stale_asset"),
                               .name = QStringLiteral("Stale asset"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo first;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &first,
                                   &error),
             qPrintable(error));
    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("stale.sv"));
    QVERIFY(writeFile(workingFile, secondRevision));
    VersionInfo second;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("2.0.0"),
                                   &second,
                                   &error),
             qPrintable(error));
    QVERIFY(writeFile(QDir(second.path).absoluteFilePath(
                          QStringLiteral("stale.sv")),
                      QByteArrayLiteral("corrupt saved version\n")));
    const VersionInventoryResult inventory = service.versionInventory(
        asset.assetRoot);
    QCOMPARE(inventory.invalidVersions.size(), 1);
    const auto invalidVersion = inventory.invalidVersions.first();
    QCOMPARE(invalidVersion.version, QStringLiteral("2.0.0"));
    QVERIFY(writeFile(workingFile, thirdRevision));

    const QString relocatedParent = temporary.filePath(
        QStringLiteral("relocated-container"));
    QVERIFY(QDir().rename(linkedParent, relocatedParent));
    bool linkCreated = false;
#ifdef Q_OS_WIN
    QProcess junction;
    junction.start(QStringLiteral("cmd.exe"),
                   {QStringLiteral("/d"),
                    QStringLiteral("/c"),
                    QStringLiteral("mklink"),
                    QStringLiteral("/J"),
                    QDir::toNativeSeparators(linkedParent),
                    QDir::toNativeSeparators(relocatedParent)});
    linkCreated = junction.waitForFinished(10000)
                  && junction.exitStatus() == QProcess::NormalExit
                  && junction.exitCode() == 0;
#else
    linkCreated = QFile::link(relocatedParent, linkedParent);
#endif
    QVERIFY(linkCreated);
    QVERIFY(!files::isLinkLike(QFileInfo(asset.assetRoot)));
    QVERIFY(files::isLinkLike(QFileInfo(linkedParent)));

    const QString replacement = temporary.filePath(
        QStringLiteral("incoming/replacement.sv"));
    QVERIFY(writeFile(replacement,
                      QByteArrayLiteral(
                          "module stale; localparam REV = 4; endmodule\n")));
    UpdateAssetResult updated;
    error.clear();
    QVERIFY(!service.updateAsset(asset,
                                 replacement,
                                 WorkingCopyRecoveryMode::Permanent,
                                 &updated,
                                 &error));
    QVERIFY(!error.isEmpty());

    UpdateAssetResult restored;
    error.clear();
    QVERIFY(!service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::Permanent,
                                    &restored,
                                    &error));
    QVERIFY(!error.isEmpty());

    DeleteVersionResult deleted;
    error.clear();
    QVERIFY(!service.deleteVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   RemovalMode::Permanent,
                                   &deleted,
                                   &error));
    QVERIFY(!error.isEmpty());

    CorruptVersionIsolationResult isolated;
    error.clear();
    QVERIFY(!service.isolateCorruptVersion(asset,
                                           invalidVersion.version,
                                           invalidVersion.fingerprint,
                                           &isolated,
                                           &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!service.copyPlan(asset, QString()).ok());
    QVERIFY(!service.copyPlan(asset, QStringLiteral("1.0.0")).ok());
    QCOMPARE(readFile(QDir(relocatedParent).absoluteFilePath(
                          QStringLiteral("stale_asset/stale.sv"))),
             thirdRevision);
    QVERIFY(QFileInfo(QDir(relocatedParent).absoluteFilePath(
                         QStringLiteral(
                             "stale_asset/.xips/versions/1.0.0")))
                .isDir());
    const QString relocatedVersions = QDir(relocatedParent).absoluteFilePath(
        QStringLiteral("stale_asset/.xips/versions"));
    QVERIFY(QFileInfo(QDir(relocatedVersions).absoluteFilePath(
                         QStringLiteral("2.0.0")))
                .isDir());
    QVERIFY(QDir(relocatedVersions)
                .entryList({QStringLiteral(".staging-*")},
                           QDir::Dirs | QDir::Hidden
                               | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(QDir(relocatedParent)
                .entryList({QStringLiteral(".xips-create-*")},
                           QDir::Dirs | QDir::Hidden
                               | QDir::NoDotAndDotDot)
                .isEmpty());

#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedParent));
#else
    QVERIFY(QFile::remove(linkedParent));
#endif
}

void CoreTest::scannerRejectsCaseOnlyDuplicateIds()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString firstRoot = QDir(library).absoluteFilePath(
        QStringLiteral("first"));
    const QString secondRoot = QDir(library).absoluteFilePath(
        QStringLiteral("second"));
    QVERIFY(writeFile(QDir(firstRoot).absoluteFilePath(QStringLiteral("a.sv")),
                      QByteArrayLiteral("module a; endmodule\n")));
    QVERIFY(writeFile(QDir(secondRoot).absoluteFilePath(QStringLiteral("b.sv")),
                      QByteArrayLiteral("module b; endmodule\n")));

    QString error;
    Manifest first;
    first.id = QStringLiteral("Shared_ID");
    first.name = QStringLiteral("First");
    QVERIFY2(ManifestService().write(
                 QDir(firstRoot).absoluteFilePath(QStringLiteral(".xips.json")),
                 first,
                 &error),
             qPrintable(error));
    Manifest second;
    second.id = QStringLiteral("shared_id");
    second.name = QStringLiteral("Second");
    QVERIFY2(ManifestService().write(
                 QDir(secondRoot).absoluteFilePath(QStringLiteral(".xips.json")),
                 second,
                 &error),
             qPrintable(error));

    const ScanResult scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 0);
    QCOMPARE(scan.errors.size(), 2);
    const QString firstManifest = QFileInfo(
        QDir(firstRoot).absoluteFilePath(QStringLiteral(".xips.json")))
                                      .absoluteFilePath();
    const QString secondManifest = QFileInfo(
        QDir(secondRoot).absoluteFilePath(QStringLiteral(".xips.json")))
                                       .absoluteFilePath();
    bool firstReported = false;
    bool secondReported = false;
    for (const QString &problem : scan.errors) {
        QVERIFY(problem.contains(QStringLiteral("Duplicate asset id")));
        firstReported = firstReported || problem.contains(firstManifest);
        secondReported = secondReported || problem.contains(secondManifest);
    }
    QVERIFY(firstReported);
    QVERIFY(secondReported);
}

void CoreTest::metadataKeepsStableId()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
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
    VersionInfo savedVersion;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &savedVersion,
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

void CoreTest::metadataMergesConcurrentManifestChangesAndRejectsConflicts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("metadata.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module metadata; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord baseline;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("metadata_merge"),
                               .name = QStringLiteral("Baseline name"),
                               .description = QString(),
                               .tags = {}}},
                 &baseline,
                 &error),
             qPrintable(error));

    ManifestLoadResult loaded = ManifestService().load(baseline.manifestPath);
    QVERIFY(loaded.ok());
    Manifest remote = *loaded.manifest;
    remote.version = QStringLiteral("2.0.0");
    remote.description = QStringLiteral("Remote description");
    remote.tags = {QStringLiteral("remote-group")};
    remote.rawObject.insert(QStringLiteral("remoteCustom"),
                            QStringLiteral("preserved"));
    QVERIFY2(ManifestService().write(baseline.manifestPath, remote, &error),
             qPrintable(error));

    MetadataUpdateResult merged;
    error.clear();
    QVERIFY2(service.updateMetadata(
                 baseline,
                 {.id = baseline.manifest.id,
                  .name = QStringLiteral("User name"),
                  .description = baseline.manifest.description,
                  .tags = {QStringLiteral("user-group")}},
                 &merged,
                 &error),
             qPrintable(error));
    QVERIFY(merged.published);
    QVERIFY(merged.changed);
    QVERIFY(merged.conflictingFields.isEmpty());
    loaded = ManifestService().load(baseline.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->name, QStringLiteral("User name"));
    QCOMPARE(loaded.manifest->description,
             QStringLiteral("Remote description"));
    QCOMPARE(loaded.manifest->version, QStringLiteral("2.0.0"));
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("remote-group")));
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("user-group")));
    QCOMPARE(loaded.manifest->rawObject.value(QStringLiteral("remoteCustom"))
                 .toString(),
             QStringLiteral("preserved"));

    ScanResult scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 1);
    const AssetRecord retryBaseline = scan.assets.first();
    bool unrelatedMutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::MetadataMergedBeforeManifestCas,
        [&](const WorkingCopyTestPoint point, const QString &manifestPath) {
            if (point
                != WorkingCopyTestPoint::MetadataMergedBeforeManifestCas) {
                return;
            }
            const ManifestLoadResult concurrent = ManifestService().load(
                manifestPath);
            if (!concurrent.ok()) {
                return;
            }
            Manifest changed = *concurrent.manifest;
            changed.version = QStringLiteral("3.0.0");
            changed.rawObject.insert(QStringLiteral("hookCustom"), 3);
            QString writeError;
            unrelatedMutationSucceeded = ManifestService().write(
                manifestPath,
                changed,
                &writeError);
        });
    MetadataUpdateResult retried;
    error.clear();
    QVERIFY2(service.updateMetadata(
                 retryBaseline,
                 {.id = retryBaseline.manifest.id,
                  .name = retryBaseline.manifest.name,
                  .description = QStringLiteral("User description"),
                  .tags = retryBaseline.manifest.tags},
                 &retried,
                 &error),
             qPrintable(error));
    QVERIFY(unrelatedMutationSucceeded);
    QVERIFY(retried.published);
    loaded = ManifestService().load(retryBaseline.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->description,
             QStringLiteral("User description"));
    QCOMPARE(loaded.manifest->version, QStringLiteral("3.0.0"));
    QCOMPARE(loaded.manifest->rawObject.value(QStringLiteral("hookCustom"))
                 .toInt(),
             3);

    scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 1);
    const AssetRecord conflictBaseline = scan.assets.first();
    bool conflictingMutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::MetadataMergedBeforeManifestCas,
        [&](const WorkingCopyTestPoint point, const QString &manifestPath) {
            if (point
                != WorkingCopyTestPoint::MetadataMergedBeforeManifestCas) {
                return;
            }
            const ManifestLoadResult concurrent = ManifestService().load(
                manifestPath);
            if (!concurrent.ok()) {
                return;
            }
            Manifest changed = *concurrent.manifest;
            changed.name = QStringLiteral("Remote conflicting name");
            QString writeError;
            conflictingMutationSucceeded = ManifestService().write(
                manifestPath,
                changed,
                &writeError);
        });
    MetadataUpdateResult conflict;
    error.clear();
    QVERIFY(!service.updateMetadata(
        conflictBaseline,
        {.id = conflictBaseline.manifest.id,
         .name = QStringLiteral("Local conflicting name"),
         .description = conflictBaseline.manifest.description,
         .tags = conflictBaseline.manifest.tags},
        &conflict,
        &error));
    QVERIFY(conflictingMutationSucceeded);
    QCOMPARE(conflict.conflictingFields,
             QStringList{QStringLiteral("name")});
    QVERIFY(error.contains(QStringLiteral("name")));
    loaded = ManifestService().load(conflictBaseline.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->name,
             QStringLiteral("Remote conflicting name"));
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-metadata-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::metadataNoOpRechecksLiveManifest()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("noop.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module noop; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("metadata_noop"),
                               .name = QStringLiteral("No-op"),
                               .description = QStringLiteral("Baseline"),
                               .tags = {QStringLiteral("BASE")}}},
                 &asset,
                 &error),
             qPrintable(error));

    bool concurrentWriteSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::MetadataMergedBeforeManifestCas,
        [&](const WorkingCopyTestPoint, const QString &manifestPath) {
            const ManifestLoadResult loaded = ManifestService().load(
                manifestPath);
            if (!loaded.ok()) {
                return;
            }
            Manifest concurrent = *loaded.manifest;
            concurrent.version = QStringLiteral("7.0.0");
            concurrent.rawObject.insert(QStringLiteral("remoteNoOp"), true);
            QString writeError;
            concurrentWriteSucceeded = ManifestService().write(
                manifestPath,
                concurrent,
                &writeError);
        });

    MetadataUpdateResult update;
    QVERIFY2(service.updateMetadata(
                 asset,
                 {.id = asset.manifest.id,
                  .name = asset.manifest.name,
                  .description = asset.manifest.description,
                  .tags = asset.manifest.tags},
                 &update,
                 &error),
             qPrintable(error));
    QVERIFY(concurrentWriteSucceeded);
    QVERIFY(update.published);
    QVERIFY(!update.changed);
    QCOMPARE(update.manifest.version, QStringLiteral("7.0.0"));
    QVERIFY(update.manifest.rawObject.value(QStringLiteral("remoteNoOp"))
                .toBool());
    const ManifestLoadResult live = ManifestService().load(
        asset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->version, QStringLiteral("7.0.0"));
    QVERIFY(live.manifest->rawObject.value(QStringLiteral("remoteNoOp"))
                .toBool());
}

void CoreTest::manifestTransactionsRecoverCrashWindows()
{
    struct SimulatedCrash final {
    };

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    QString error;
    const auto transactionDirectories = [&]() {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };

    const QString restoreSource = temporary.filePath(
        QStringLiteral("restore_original.sv"));
    QVERIFY(writeFile(
        restoreSource,
        QByteArrayLiteral("module restore_original; endmodule\n")));
    AssetRecord restoreAsset;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = restoreSource,
                  .metadata = {.id = QStringLiteral("restore_original"),
                               .name = QStringLiteral("Original"),
                               .description = {},
                               .tags = {}}},
                 &restoreAsset,
                 &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    bool crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            restoreAsset,
            {.id = restoreAsset.manifest.id,
             .name = QStringLiteral("Replacement"),
             .description = restoreAsset.manifest.description,
             .tags = restoreAsset.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    QVERIFY(!QFileInfo::exists(restoreAsset.manifestPath));
    QCOMPARE(transactionDirectories().size(), 1);
    ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QVERIFY2(recovery.fatalError.isEmpty(),
             qPrintable(recovery.fatalError));
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.retained, 0);
    QCOMPARE(recovery.items.size(), 1);
    QCOMPARE(recovery.items.first().outcome,
             ManifestTransactionRecoveryOutcome::RestoredOriginal);
    ManifestLoadResult live = ManifestService().load(
        restoreAsset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("Original"));
    QVERIFY(transactionDirectories().isEmpty());

    const QString publishedSource = temporary.filePath(
        QStringLiteral("keep_replacement.sv"));
    QVERIFY(writeFile(
        publishedSource,
        QByteArrayLiteral("module keep_replacement; endmodule\n")));
    AssetRecord publishedAsset;
    error.clear();
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = publishedSource,
                  .metadata = {.id = QStringLiteral("keep_replacement"),
                               .name = QStringLiteral("Before publish"),
                               .description = {},
                               .tags = {}}},
                 &publishedAsset,
                 &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            publishedAsset,
            {.id = publishedAsset.manifest.id,
             .name = QStringLiteral("Published replacement"),
             .description = publishedAsset.manifest.description,
             .tags = publishedAsset.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    live = ManifestService().load(publishedAsset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name,
             QStringLiteral("Published replacement"));
    QCOMPARE(transactionDirectories().size(), 1);
    recovery = service.recoverManifestTransactions(library);
    QVERIFY2(recovery.fatalError.isEmpty(),
             qPrintable(recovery.fatalError));
    QCOMPARE(recovery.cleanedReplacement, 1);
    QCOMPARE(recovery.retained, 0);
    QCOMPARE(recovery.items.size(), 1);
    QCOMPARE(recovery.items.first().outcome,
             ManifestTransactionRecoveryOutcome::KeptReplacement);
    live = ManifestService().load(publishedAsset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name,
             QStringLiteral("Published replacement"));
    QVERIFY(transactionDirectories().isEmpty());

    const QString concurrentSource = temporary.filePath(
        QStringLiteral("keep_concurrent.sv"));
    QVERIFY(writeFile(
        concurrentSource,
        QByteArrayLiteral("module keep_concurrent; endmodule\n")));
    AssetRecord concurrentAsset;
    error.clear();
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = concurrentSource,
                  .metadata = {.id = QStringLiteral("keep_concurrent"),
                               .name = QStringLiteral("Concurrent baseline"),
                               .description = {},
                               .tags = {}}},
                 &concurrentAsset,
                 &error),
             qPrintable(error));
    bool concurrentManifestWritten = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [&](const WorkingCopyTestPoint, const QString &) {
            Manifest concurrent = concurrentAsset.manifest;
            concurrent.name = QStringLiteral("Third-party live manifest");
            concurrent.rawObject.insert(QStringLiteral("thirdParty"), 1);
            QString writeError;
            concurrentManifestWritten = ManifestService().write(
                concurrentAsset.manifestPath,
                concurrent,
                &writeError);
            throw SimulatedCrash{};
        });
    crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            concurrentAsset,
            {.id = concurrentAsset.manifest.id,
             .name = QStringLiteral("Transaction replacement"),
             .description = concurrentAsset.manifest.description,
             .tags = concurrentAsset.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    QVERIFY(concurrentManifestWritten);
    const QStringList retainedTransactions = transactionDirectories();
    QCOMPARE(retainedTransactions.size(), 1);
    const QString retainedTransactionPath = QDir(library).absoluteFilePath(
        retainedTransactions.first());
    recovery = service.recoverManifestTransactions(library);
    QVERIFY2(recovery.fatalError.isEmpty(),
             qPrintable(recovery.fatalError));
    QCOMPARE(recovery.retained, 1);
    QCOMPARE(recovery.items.size(), 1);
    QCOMPARE(recovery.items.first().outcome,
             ManifestTransactionRecoveryOutcome::Retained);
    QVERIFY(QFileInfo(retainedTransactionPath).isDir());
    live = ManifestService().load(concurrentAsset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name,
             QStringLiteral("Third-party live manifest"));
    QCOMPARE(live.manifest->rawObject.value(QStringLiteral("thirdParty"))
                 .toInt(),
             1);
}

void CoreTest::manifestTransactionsRecoverNestedAssets()
{
    struct SimulatedCrash final {
    };

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString nestedLibrary = QDir(library).absoluteFilePath(
        QStringLiteral("protocols/uart"));
    QVERIFY(QDir().mkpath(nestedLibrary));
    const QString source = temporary.filePath(QStringLiteral("nested.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module nested; endmodule\n")));

    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = nestedLibrary,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("nested_asset"),
                               .name = QStringLiteral("Nested original"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    bool crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            asset,
            {.id = asset.manifest.id,
             .name = QStringLiteral("Nested replacement"),
             .description = asset.manifest.description,
             .tags = asset.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    QVERIFY(!QFileInfo::exists(asset.manifestPath));
    QCOMPARE(QDir(nestedLibrary)
                 .entryList({QStringLiteral(".xips-create-metadata-*")},
                            QDir::Dirs | QDir::Hidden
                                | QDir::NoDotAndDotDot)
                 .size(),
             1);

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QVERIFY2(recovery.fatalError.isEmpty(),
             qPrintable(recovery.fatalError));
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.retained, 0);
    const ManifestLoadResult live = ManifestService().load(
        asset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("Nested original"));
    QVERIFY(QDir(nestedLibrary)
                .entryList({QStringLiteral(".xips-create-metadata-*")},
                           QDir::Dirs | QDir::Hidden
                               | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::manifestTransactionChainsRestoreLatestOriginal()
{
    struct SimulatedCrash final {
    };

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("chain.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module chain; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord stateA;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("chain_asset"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &stateA,
                 &error),
             qPrintable(error));
    const auto transactions = [&]() {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };

    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    bool crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            stateA,
            {.id = stateA.manifest.id,
             .name = QStringLiteral("State B"),
             .description = stateA.manifest.description,
             .tags = stateA.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    QStringList transactionNames = transactions();
    QCOMPARE(transactionNames.size(), 1);
    const QString firstTransaction = transactionNames.first();

    const ScanResult scanB = AssetScanner().scan(library);
    QCOMPARE(scanB.assets.size(), 1);
    const AssetRecord stateB = scanB.assets.first();
    QCOMPARE(stateB.manifest.name, QStringLiteral("State B"));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    crashed = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            stateB,
            {.id = stateB.manifest.id,
             .name = QStringLiteral("State C"),
             .description = stateB.manifest.description,
             .tags = stateB.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    QVERIFY(!QFileInfo::exists(stateB.manifestPath));
    transactionNames = transactions();
    QCOMPARE(transactionNames.size(), 2);
    QString secondTransaction;
    for (const QString &name : std::as_const(transactionNames)) {
        if (name != firstTransaction) {
            secondTransaction = name;
        }
    }
    QVERIFY(!secondTransaction.isEmpty());

    const auto renameTransaction = [&](const QString &oldName,
                                       const QString &newName,
                                       const QString &newId) {
        if (!QDir(library).rename(oldName, newName)) {
            return false;
        }
        const QString recordPath = QDir(library).absoluteFilePath(
            QStringLiteral("%1/transaction.json").arg(newName));
        QJsonParseError parseError;
        QJsonDocument document = QJsonDocument::fromJson(
            readFile(recordPath),
            &parseError);
        if (parseError.error != QJsonParseError::NoError
            || !document.isObject()) {
            return false;
        }
        QJsonObject object = document.object();
        object.insert(QStringLiteral("transactionId"), newId);
        return writeFile(recordPath,
                         QJsonDocument(object).toJson(
                             QJsonDocument::Indented));
    };
    const QString earlyId = QStringLiteral(
        "00000000-0000-4000-8000-000000000001");
    const QString lateId = QStringLiteral(
        "ffffffff-ffff-4fff-8fff-ffffffffffff");
    const QString earlyName = QStringLiteral(
        ".xips-create-metadata-%1").arg(earlyId);
    const QString lateName = QStringLiteral(
        ".xips-create-metadata-%1").arg(lateId);
    QVERIFY(renameTransaction(firstTransaction, earlyName, earlyId));
    QVERIFY(renameTransaction(secondTransaction, lateName, lateId));
    transactionNames = transactions();
    QCOMPARE(transactionNames,
             QStringList({earlyName, lateName}));

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QVERIFY2(recovery.fatalError.isEmpty(),
             qPrintable(recovery.fatalError));
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.cleanedReplacement, 1);
    QCOMPARE(recovery.retained, 0);
    QCOMPARE(recovery.items.size(), 2);
    const ManifestLoadResult live = ManifestService().load(
        stateB.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("State B"));
    QVERIFY(transactions().isEmpty());
}

void CoreTest::corruptManifestTransactionBlocksAssetRecovery()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("blocked.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module blocked; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord stateA;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("blocked_asset"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &stateA,
                 &error),
             qPrintable(error));
    const auto transactions = [&]() {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };
    QVERIFY(leaveMetadataTransaction(
        service,
        stateA,
        QStringLiteral("State B"),
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        &error));
    const QString firstTransaction = transactions().first();
    const ScanResult scanB = AssetScanner().scan(library);
    QCOMPARE(scanB.assets.size(), 1);
    const AssetRecord stateB = scanB.assets.first();
    QVERIFY(leaveMetadataTransaction(
        service,
        stateB,
        QStringLiteral("State C"),
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        &error));
    const QStringList allTransactions = transactions();
    QCOMPARE(allTransactions.size(), 2);
    QString secondTransaction;
    for (const QString &name : allTransactions) {
        if (name != firstTransaction) {
            secondTransaction = name;
        }
    }
    QVERIFY(!secondTransaction.isEmpty());
    const QString secondRoot = QDir(library).absoluteFilePath(
        secondTransaction);
    QVERIFY(writeFile(QDir(secondRoot).absoluteFilePath(
                          QStringLiteral("next.xips.json")),
                      QByteArrayLiteral("{corrupt next")));
    const QString secondOriginal = QDir(secondRoot).absoluteFilePath(
        QStringLiteral("original.xips.json"));
    ManifestLoadResult original = ManifestService().load(secondOriginal);
    QVERIFY(original.ok());
    QCOMPARE(original.manifest->name, QStringLiteral("State B"));

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QCOMPARE(recovery.retained, 2);
    QCOMPARE(recovery.items.size(), 2);
    QVERIFY(!QFileInfo::exists(stateB.manifestPath));
    QCOMPARE(transactions().size(), 2);
    original = ManifestService().load(secondOriginal);
    QVERIFY(original.ok());
    QCOMPARE(original.manifest->name, QStringLiteral("State B"));
}

void CoreTest::manifestTransactionChainsRetireAllAncestors()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("long_chain.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module long_chain; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord current;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("long_chain"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &current,
                 &error),
             qPrintable(error));
    const auto transactions = [&]() {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };
    QVERIFY(leaveMetadataTransaction(
        service,
        current,
        QStringLiteral("State B"),
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        &error));
    ScanResult scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 1);
    current = scan.assets.first();
    QVERIFY(leaveMetadataTransaction(
        service,
        current,
        QStringLiteral("State C"),
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        &error));
    scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 1);
    current = scan.assets.first();
    QCOMPARE(current.manifest.name, QStringLiteral("State C"));
    QVERIFY(leaveMetadataTransaction(
        service,
        current,
        QStringLiteral("State D"),
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        &error));
    QCOMPARE(transactions().size(), 3);
    QVERIFY(!QFileInfo::exists(current.manifestPath));

    ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.cleanedReplacement, 2);
    QCOMPARE(recovery.retained, 0);
    QVERIFY(transactions().isEmpty());
    ManifestLoadResult live = ManifestService().load(current.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("State C"));

    scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 1);
    current = scan.assets.first();
    QVERIFY(leaveMetadataTransaction(
        service,
        current,
        QStringLiteral("State E"),
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        &error));
    QCOMPARE(transactions().size(), 1);
    recovery = service.recoverManifestTransactions(library);
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.retained, 0);
    QVERIFY(transactions().isEmpty());
    live = ManifestService().load(current.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("State C"));
}

void CoreTest::corruptAncestorTransactionDoesNotHideNestedRecovery()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString protocols = QDir(library).absoluteFilePath(
        QStringLiteral("Protocols"));
    QVERIFY(QDir().mkpath(protocols));
    const QString source = temporary.filePath(QStringLiteral("nested_valid.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module nested_valid; endmodule\n")));
    AssetLibraryService service;
    AssetRecord nested;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = protocols,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("nested_valid"),
                               .name = QStringLiteral("Nested original"),
                               .description = {},
                               .tags = {}}},
                 &nested,
                 &error),
             qPrintable(error));
    QVERIFY(leaveMetadataTransaction(
        service,
        nested,
        QStringLiteral("Nested replacement"),
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        &error));
    QVERIFY(!QFileInfo::exists(nested.manifestPath));

    const QString damagedId = QStringLiteral(
        "11111111-1111-4111-8111-111111111111");
    const QString damagedName = QStringLiteral(
        ".xips-create-metadata-%1").arg(damagedId);
    const QString damagedRoot = QDir(library).absoluteFilePath(damagedName);
    QVERIFY(QDir().mkpath(damagedRoot));
    const QJsonObject record{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("owner"), QStringLiteral("xips-manifest-cas")},
        {QStringLiteral("operation"), QStringLiteral("metadata")},
        {QStringLiteral("transactionId"), damagedId},
        {QStringLiteral("assetDirectory"), QStringLiteral("Protocols")},
        {QStringLiteral("assetId"), QStringLiteral("protocols_container")},
        {QStringLiteral("expectedCanonicalSha256"), QString(64, u'0')},
        {QStringLiteral("replacementCanonicalSha256"), QString(64, u'1')},
    };
    QVERIFY(writeFile(QDir(damagedRoot).absoluteFilePath(
                          QStringLiteral("transaction.json")),
                      QJsonDocument(record).toJson(
                          QJsonDocument::Indented)));
    QVERIFY(writeFile(QDir(damagedRoot).absoluteFilePath(
                          QStringLiteral("next.xips.json")),
                      QByteArrayLiteral("{damaged ancestor")));

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QCOMPARE(recovery.restoredOriginal, 1);
    QCOMPARE(recovery.retained, 1);
    const ManifestLoadResult live = ManifestService().load(
        nested.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("Nested original"));
    QVERIFY(QFileInfo(damagedRoot).isDir());
}

void CoreTest::manifestTransactionRetirementPreservesDataWhenLiveChanges()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("retire_race.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module retire_race; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord stateA;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("retire_race"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &stateA,
                 &error),
             qPrintable(error));
    QVERIFY(leaveMetadataTransaction(
        service,
        stateA,
        QStringLiteral("State B"),
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        &error));
    const ScanResult scanB = AssetScanner().scan(library);
    QCOMPARE(scanB.assets.size(), 1);
    const AssetRecord stateB = scanB.assets.first();

    bool liveChanged = false;
    bool originalPathBlocked = false;
    QString isolatedPath;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestTransactionIsolatedBeforeRetireLiveVerification,
        [&](const WorkingCopyTestPoint, const QString &path) {
            isolatedPath = path;
            const QJsonObject record = QJsonDocument::fromJson(
                                           readFile(QDir(path).absoluteFilePath(
                                               QStringLiteral("transaction.json"))))
                                           .object();
            const QString originalName = QStringLiteral(
                ".xips-create-%1-%2")
                                             .arg(
                                                 record.value(QStringLiteral("operation"))
                                                     .toString(),
                                                 record.value(QStringLiteral("transactionId"))
                                                     .toString());
            originalPathBlocked = QDir().mkpath(
                QDir(QFileInfo(path).absolutePath()).absoluteFilePath(
                    originalName));
            const ManifestLoadResult loaded = ManifestService().load(
                stateB.manifestPath);
            if (!loaded.ok()) {
                return;
            }
            Manifest concurrent = *loaded.manifest;
            concurrent.name = QStringLiteral("Concurrent live state");
            concurrent.rawObject.insert(QStringLiteral("retireRace"), true);
            QString writeError;
            liveChanged = ManifestService().write(stateB.manifestPath,
                                                  concurrent,
                                                  &writeError);
        });

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QVERIFY(liveChanged);
    QVERIFY(originalPathBlocked);
    QVERIFY(!isolatedPath.isEmpty());
    QCOMPARE(recovery.retained, 1);
    QCOMPARE(recovery.items.size(), 1);
    QCOMPARE(recovery.items.first().transactionPath, isolatedPath);
    QVERIFY(QFileInfo(isolatedPath).isDir());
    const QStringList unfinished =
        AssetScanner::unfinishedOperationPaths(library);
    QVERIFY(unfinished.contains(isolatedPath));
    const ManifestLoadResult live = ManifestService().load(
        stateB.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name,
             QStringLiteral("Concurrent live state"));
    QVERIFY(live.manifest->rawObject.value(QStringLiteral("retireRace"))
                .toBool());
    QVERIFY(QFileInfo(QDir(isolatedPath).absoluteFilePath(
                          QStringLiteral("original.xips.json")))
                .isFile());
}

void CoreTest::manifestCasRetirementRejectsThirdPartyLive()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("cas_retire.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module cas_retire; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("cas_retire"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    bool concurrentWriteSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        [&](const WorkingCopyTestPoint, const QString &) {
            const ManifestLoadResult loaded = ManifestService().load(
                asset.manifestPath);
            if (!loaded.ok()) {
                return;
            }
            Manifest concurrent = *loaded.manifest;
            concurrent.name = QStringLiteral("Third-party state");
            concurrent.rawObject.insert(QStringLiteral("casRetireRace"), 1);
            QString writeError;
            concurrentWriteSucceeded = ManifestService().write(
                asset.manifestPath,
                concurrent,
                &writeError);
        });
    MetadataUpdateResult update;
    QVERIFY2(service.updateMetadata(
                 asset,
                 {.id = asset.manifest.id,
                  .name = QStringLiteral("State B"),
                  .description = asset.manifest.description,
                  .tags = asset.manifest.tags},
                 &update,
                 &error),
             qPrintable(error));
    QVERIFY(concurrentWriteSucceeded);
    QVERIFY(!update.retainedPath.isEmpty());
    QVERIFY(QFileInfo(update.retainedPath).isDir());
    QVERIFY(AssetScanner::unfinishedOperationPaths(library)
                .contains(update.retainedPath));
    const ManifestLoadResult live = ManifestService().load(
        asset.manifestPath);
    QVERIFY(live.ok());
    QCOMPARE(live.manifest->name, QStringLiteral("Third-party state"));
    QCOMPARE(live.manifest->rawObject.value(QStringLiteral("casRetireRace"))
                 .toInt(),
             1);
    QVERIFY(QFileInfo(QDir(update.retainedPath).absoluteFilePath(
                          QStringLiteral("original.xips.json")))
                .isFile());
}

void CoreTest::manifestRecoveryPathIdentityIsCaseInsensitiveOnWindows()
{
#ifndef Q_OS_WIN
    QSKIP("Windows path identity regression");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("case_path.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module case_path; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord stateA;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("CaseAsset"),
                               .name = QStringLiteral("State A"),
                               .description = {},
                               .tags = {}}},
                 &stateA,
                 &error),
             qPrintable(error));
    const auto transactions = [&]() {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };
    QVERIFY(leaveMetadataTransaction(
        service,
        stateA,
        QStringLiteral("State B"),
        WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
        &error));
    const QString firstTransaction = transactions().first();
    QDir libraryDirectory(library);
    QVERIFY(libraryDirectory.rename(QStringLiteral("CaseAsset"),
                                    QStringLiteral("CaseAsset_rename")));
    QVERIFY(libraryDirectory.rename(QStringLiteral("CaseAsset_rename"),
                                    QStringLiteral("caseasset")));
    const ScanResult scanB = AssetScanner().scan(library);
    QCOMPARE(scanB.assets.size(), 1);
    const AssetRecord stateB = scanB.assets.first();
    QCOMPARE(QFileInfo(stateB.assetRoot).fileName(),
             QStringLiteral("caseasset"));
    QVERIFY(leaveMetadataTransaction(
        service,
        stateB,
        QStringLiteral("State C"),
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        &error));
    const QStringList allTransactions = transactions();
    QCOMPARE(allTransactions.size(), 2);
    QString secondTransaction;
    for (const QString &name : allTransactions) {
        if (name != firstTransaction) {
            secondTransaction = name;
        }
    }
    QVERIFY(!secondTransaction.isEmpty());
    const QString secondRoot = QDir(library).absoluteFilePath(
        secondTransaction);
    QVERIFY(writeFile(QDir(secondRoot).absoluteFilePath(
                          QStringLiteral("next.xips.json")),
                      QByteArrayLiteral("{corrupt case transaction")));

    const ManifestTransactionRecoveryResult recovery =
        service.recoverManifestTransactions(library);
    QCOMPARE(recovery.retained, 2);
    QVERIFY(!QFileInfo::exists(stateB.manifestPath));
    QCOMPARE(transactions().size(), 2);
    const ManifestLoadResult preservedOriginal = ManifestService().load(
        QDir(secondRoot).absoluteFilePath(
            QStringLiteral("original.xips.json")));
    QVERIFY(preservedOriginal.ok());
    QCOMPARE(preservedOriginal.manifest->name,
             QStringLiteral("State B"));
#endif
}

void CoreTest::updateReplacesWorkingCopyAndPreservesSavedVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString original = createSourceIp(temporary.path());
    QVERIFY(!original.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
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
    VersionInfo savedVersion;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &savedVersion,
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
                                           &error,
                                           RemovalMode::Permanent),
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
    QVERIFY(QDir().mkpath(library));
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
                                           &error,
                                           RemovalMode::Permanent));
    QVERIFY(error.contains(QStringLiteral("changed after the operation")));
    QCOMPARE(readFile(workingFile), newerBytes);
    QCOMPARE(readFile(QDir(updated.undoToken.recoveryPath).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());

    RecoveryDiscardResult explicitDiscard;
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(updated.updated,
                                                updated.undoToken,
                                                RemovalMode::Permanent,
                                                &explicitDiscard,
                                                &error));
    QVERIFY(error.contains(QStringLiteral("changed after the operation")));
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());
    error.clear();
    QVERIFY2(service.discardWorkingCopyRecovery(updated.updated,
                                                updated.undoToken,
                                                RemovalMode::Permanent,
                                                &explicitDiscard,
                                                &error,
                                                WorkingCopyDiscardPolicy::AllowVerifiedCurrentCopy),
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
                                           &error,
                                           RemovalMode::Permanent));
    QVERIFY(error.contains(QStringLiteral("retained recovery changed")));
    QCOMPARE(readFile(workingFile), revisedBytes);
    QCOMPARE(readFile(tamperedRecoveryFile), tamperedBytes);
    QCOMPARE(readFile(neighbor), neighborBytes);

    RecoveryDiscardResult rejectedDiscard;
    error.clear();
    QVERIFY(service.discardWorkingCopyRecovery(
        tamperedRecovery.updated,
        tamperedRecovery.undoToken,
        RemovalMode::Permanent,
        &rejectedDiscard,
        &error));
    QCOMPARE(rejectedDiscard.outcome,
             RecoveryDiscardOutcome::TokenRetired);
    QCOMPARE(rejectedDiscard.retainedPath,
             tamperedRecovery.undoToken.recoveryPath);
    QVERIFY(!rejectedDiscard.warning.isEmpty());
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
                                           &error,
                                           RemovalMode::Permanent));
    QVERIFY(error.contains(QStringLiteral("saved-version data")));
    RecoveryDiscardResult protectedDiscard;
    error.clear();
    QVERIFY(service.discardWorkingCopyRecovery(bounded.updated,
                                               validToken,
                                               RemovalMode::Permanent,
                                               &protectedDiscard,
                                               &error));
    QCOMPARE(protectedDiscard.outcome,
             RecoveryDiscardOutcome::TokenRetired);
    QCOMPARE(protectedDiscard.retainedPath, validToken.recoveryPath);
    QVERIFY(protectedDiscard.warning.contains(
        QStringLiteral("saved-version data")));
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
                                                &error,
                                                RemovalMode::Permanent),
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
    QCOMPARE(validDiscard.outcome,
             RecoveryDiscardOutcome::TokenRetired);
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
        QVERIFY(QDir().mkpath(library));
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
            WorkingCopyTestPoint::StagingVerifiedBeforePublish,
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
        QVERIFY(!service.updateAsset(asset,
                                     replacement,
                                     modes.at(index),
                                     &updated,
                                     &error));
        QCOMPARE(hookCount, 1);
        QVERIFY(mutationSucceeded);
        QVERIFY(!updated.publishedAsIntended);
        QVERIFY(!updated.undoToken.isValid());
        QVERIFY(updated.retainedPaths.isEmpty());
        QVERIFY(error.contains(QStringLiteral("update staging remains")));
        const QStringList updateStaging = QDir(library).entryList(
            {QStringLiteral(".xips-create-update-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(updateStaging.size(), 1);
        const QString stagingPath = QDir(library).absoluteFilePath(
            updateStaging.first());
        QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 originalBytes);
        QCOMPARE(readFile(replacementFile), revisedBytes);
        QCOMPARE(readFile(QDir(stagingPath).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 concurrentBytes);
        const ManifestLoadResult stagingManifest = ManifestService().load(
            QDir(stagingPath).absoluteFilePath(
                QStringLiteral(".xips.json")));
        QVERIFY(stagingManifest.ok());
        QCOMPARE(stagingManifest.manifest->id, asset.manifest.id);
        QVERIFY(QDir(library)
                    .entryList({QStringLiteral(".xips-create-recovery-*")},
                               QDir::Dirs | QDir::Hidden
                                   | QDir::NoDotAndDotDot)
                    .isEmpty());
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
    QVERIFY(QDir().mkpath(library));
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
                                           &error,
                                           RemovalMode::Permanent));
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
    QVERIFY(service.discardWorkingCopyRecovery(updated.updated,
                                               updated.undoToken,
                                               RemovalMode::Permanent,
                                               &blockedDiscard,
                                               &error));
    QCOMPARE(blockedDiscard.outcome,
             RecoveryDiscardOutcome::TokenRetired);
    QCOMPARE(blockedDiscard.retainedPath,
             updated.undoToken.recoveryPath);
    QVERIFY(blockedDiscard.warning.contains(
        QStringLiteral("unrecognized data")));
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
        WorkingCopyTestPoint::RecoveryVerifiedBeforeDiscardIsolation,
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
    QVERIFY(service.discardWorkingCopyRecovery(updated.updated,
                                               updated.undoToken,
                                               RemovalMode::Permanent,
                                               &isolated,
                                               &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(manifestMutationSucceeded);
    QCOMPARE(isolated.outcome,
             RecoveryDiscardOutcome::TokenRetired);
    QVERIFY(isolated.warning.contains(
        QStringLiteral("could not be reverified")));
    QCOMPARE(isolated.retainedPath, updated.undoToken.recoveryPath);
    QVERIFY(QFileInfo(updated.undoToken.recoveryPath).isDir());
    const ManifestLoadResult retainedManifest = ManifestService().load(
        QDir(updated.undoToken.recoveryPath).absoluteFilePath(
            QStringLiteral(".xips.json")));
    QVERIFY(retainedManifest.ok());
    QCOMPARE(retainedManifest.manifest->description,
             concurrentDescription);
    QCOMPARE(readFile(QDir(updated.undoToken.recoveryPath).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             revisedBytes);
    QCOMPARE(readFile(replacementFile), revisedBytes);
}

void CoreTest::workingCopyDiscardRequiresValidLiveAsset_data()
{
    QTest::addColumn<bool>("removeLiveAsset");

    QTest::newRow("missing-live-asset") << true;
    QTest::newRow("corrupt-live-manifest") << false;
}

void CoreTest::workingCopyDiscardRequiresValidLiveAsset()
{
    QFETCH(bool, removeLiveAsset);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString replacement = temporary.filePath(
        QStringLiteral("replacement"));
    const QByteArray originalBytes = QByteArrayLiteral(
        "module top; endmodule\n");
    const QByteArray revisedBytes = QByteArrayLiteral(
        "module top; localparam REV = 2; endmodule\n");
    QVERIFY(writeFile(QDir(replacement).absoluteFilePath(
                          QStringLiteral("rtl/top.sv")),
                      revisedBytes));

    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("discard_live_safety"),
                               .name = QStringLiteral("Discard Live Safety"),
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
    QVERIFY(updated.undoToken.isValid());
    const QString recoveryPath = updated.undoToken.recoveryPath;
    const QString recoveryFile = QDir(recoveryPath).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QCOMPARE(readFile(recoveryFile), originalBytes);

    if (removeLiveAsset) {
        QVERIFY(QDir(asset.assetRoot).removeRecursively());
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
    } else {
        QVERIFY(writeFile(asset.manifestPath,
                          QByteArrayLiteral("{ invalid manifest")));
        QVERIFY(QFileInfo(asset.assetRoot).isDir());
    }

    RecoveryDiscardResult discarded;
    error.clear();
    QVERIFY(!service.discardWorkingCopyRecovery(
        updated.updated,
        updated.undoToken,
        RemovalMode::Permanent,
        &discarded,
        &error,
        WorkingCopyDiscardPolicy::AllowVerifiedCurrentCopy));
    QVERIFY(!error.isEmpty());
    QCOMPARE(discarded.outcome,
             RecoveryDiscardOutcome::PendingUndoPreserved);
    QCOMPARE(discarded.retainedPath, recoveryPath);
    QVERIFY(discarded.warning.isEmpty());
    QVERIFY(QFileInfo(recoveryPath).isDir());
    QCOMPARE(readFile(recoveryFile), originalBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-discard-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::recoveryIsolationRacePreservesUndoData_data()
{
    QTest::addColumn<bool>("automaticCleanup");

    QTest::newRow("automatic-update-cleanup") << true;
    QTest::newRow("explicit-discard") << false;
}

void CoreTest::recoveryIsolationRacePreservesUndoData()
{
    QFETCH(bool, automaticCleanup);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString replacement = temporary.filePath(
        QStringLiteral("replacement"));
    const QByteArray originalBytes = QByteArrayLiteral(
        "module top; endmodule\n");
    const QByteArray revisedBytes = QByteArrayLiteral(
        "module top; localparam REV = 2; endmodule\n");
    const QByteArray concurrentBytes = QByteArrayLiteral(
        "module top; localparam REV = 3; endmodule\n");
    QVERIFY(writeFile(QDir(replacement).absoluteFilePath(
                          QStringLiteral("rtl/top.sv")),
                      revisedBytes));

    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("discard_isolation_race"),
                               .name = QStringLiteral("Discard Isolation Race"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    UpdateAssetResult retainedUpdate;
    if (!automaticCleanup) {
        QVERIFY2(service.updateAsset(asset,
                                     replacement,
                                     WorkingCopyRecoveryMode::RetainForUndo,
                                     &retainedUpdate,
                                     &error),
                 qPrintable(error));
        QVERIFY(retainedUpdate.undoToken.isValid());
    }

    int hookCount = 0;
    bool mutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::RecoveryIsolatedBeforeLiveReverification,
        [&](const WorkingCopyTestPoint point, const QString &path) {
            if (point
                != WorkingCopyTestPoint::RecoveryIsolatedBeforeLiveReverification) {
                return;
            }
            ++hookCount;
            mutationSucceeded = writeFile(
                QDir(path).absoluteFilePath(QStringLiteral("rtl/top.sv")),
                concurrentBytes);
        });

    QString recoveryPath;
    if (automaticCleanup) {
        UpdateAssetResult updated;
        error.clear();
        QVERIFY2(service.updateAsset(asset,
                                     replacement,
                                     WorkingCopyRecoveryMode::Permanent,
                                     &updated,
                                     &error),
                 qPrintable(error));
        QVERIFY(!updated.warning.isEmpty());
        for (const QString &path : updated.retainedPaths) {
            if (QFileInfo(path).fileName().startsWith(
                    QStringLiteral(".xips-create-recovery-"))) {
                recoveryPath = path;
                break;
            }
        }
    } else {
        recoveryPath = retainedUpdate.undoToken.recoveryPath;
        RecoveryDiscardResult discarded;
        error.clear();
        QVERIFY(!service.discardWorkingCopyRecovery(
            retainedUpdate.updated,
            retainedUpdate.undoToken,
            RemovalMode::Permanent,
            &discarded,
            &error,
            WorkingCopyDiscardPolicy::AllowVerifiedCurrentCopy));
        QVERIFY(!error.isEmpty());
        QCOMPARE(discarded.retainedPath, recoveryPath);
        QVERIFY(discarded.warning.isEmpty());
    }

    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!recoveryPath.isEmpty());
    QVERIFY(QFileInfo(recoveryPath).isDir());
    QVERIFY(QFileInfo(recoveryPath).fileName().startsWith(
        QStringLiteral(".xips-create-recovery-")));
    QCOMPARE(readFile(QDir(recoveryPath).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             concurrentBytes);
    QCOMPARE(readFile(QDir(source).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-discard-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::assetDeletionIsBoundedAndLeavesSourcesUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("delete_me.sv"));
    QVERIFY(writeFile(source, QByteArrayLiteral("module delete_me; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
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

void CoreTest::assetDeletionProofRejectsStaleAndBoundaryChanges()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("delete_me.sv"));
    const QByteArray importedBytes = QByteArrayLiteral(
        "module delete_me; localparam REV = 1; endmodule\n");
    QVERIFY(writeFile(source, importedBytes));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    const ImportBatchResult imported = service.importAssets(library, {source});
    QCOMPARE(imported.errors.size(), 0);
    QCOMPARE(imported.created.size(), 1);
    QCOMPARE(imported.createdProofs.size(), 1);
    const AssetRecord asset = imported.created.first();
    const AssetDeletionProof importUndoProof = imported.createdProofs.first();
    QVERIFY2(importUndoProof.ok(), qPrintable(importUndoProof.error));
    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("delete_me.sv"));

    const QByteArray editedAfterImport = QByteArrayLiteral(
        "module delete_me; localparam REV = 2; endmodule\n");
    QVERIFY(writeFile(workingFile, editedAfterImport));
    QString removedPath;
    QString error;
    QVERIFY(!service.deleteAsset(library,
                                 asset,
                                 importUndoProof,
                                 RemovalMode::Permanent,
                                 &removedPath,
                                 &error));
    QVERIFY(error.contains(QStringLiteral("changed after deletion was confirmed")));
    QVERIFY(removedPath.isEmpty());
    QCOMPARE(readFile(workingFile), editedAfterImport);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-delete-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());

    const AssetDeletionProof confirmedProof = service.assetDeletionProof(asset);
    QVERIFY2(confirmedProof.ok(), qPrintable(confirmedProof.error));
    const QByteArray editedAtBoundary = QByteArrayLiteral(
        "module delete_me; localparam REV = 3; endmodule\n");
    int hookCount = 0;
    bool mutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteAssetIsolatedBeforeRemoval,
        [&](const WorkingCopyTestPoint point, const QString &isolatedRoot) {
            if (point != WorkingCopyTestPoint::DeleteAssetIsolatedBeforeRemoval) {
                return;
            }
            ++hookCount;
            mutationSucceeded = writeFile(
                QDir(isolatedRoot).absoluteFilePath(
                    QStringLiteral("delete_me.sv")),
                editedAtBoundary);
        });
    error.clear();
    QVERIFY(!service.deleteAsset(library,
                                 asset,
                                 confirmedProof,
                                 RemovalMode::Permanent,
                                 &removedPath,
                                 &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(error.contains(QStringLiteral("changed at the deletion boundary")));
    QVERIFY(removedPath.isEmpty());
    QCOMPARE(readFile(workingFile), editedAtBoundary);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-delete-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());

    const AssetDeletionProof beforeVersionProof = service.assetDeletionProof(asset);
    QVERIFY2(beforeVersionProof.ok(), qPrintable(beforeVersionProof.error));
    VersionInfo saved;
    error.clear();
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));
    error.clear();
    QVERIFY(!service.deleteAsset(library,
                                 asset,
                                 beforeVersionProof,
                                 RemovalMode::Permanent,
                                 &removedPath,
                                 &error));
    QVERIFY(error.contains(QStringLiteral("changed after deletion was confirmed")));
    QVERIFY(QFileInfo(saved.path).isDir());

    const AssetDeletionProof finalProof = service.assetDeletionProof(asset);
    QVERIFY2(finalProof.ok(), qPrintable(finalProof.error));
    error.clear();
    QVERIFY2(service.deleteAsset(library,
                                 asset,
                                 finalProof,
                                 RemovalMode::Permanent,
                                 &removedPath,
                                 &error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(asset.assetRoot));
    QCOMPARE(readFile(source), importedBytes);
}

void CoreTest::versionsAreImmutableAndCopyable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
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
    QCOMPARE(first.schemaVersion, 2);
    QVERIFY(first.strictContentHash.startsWith(QStringLiteral("sha256:")));
    QCOMPARE(first.strictContentHash.size(), 71);
    const QString firstMetadataPath = QDir(first.path).absoluteFilePath(
        QStringLiteral(".snapshot.json"));
    QFile firstMetadataFile(firstMetadataPath);
    QVERIFY(firstMetadataFile.open(QIODevice::ReadOnly));
    QJsonObject firstMetadata = QJsonDocument::fromJson(
        firstMetadataFile.readAll()).object();
    firstMetadataFile.close();
    QCOMPARE(firstMetadata.value(QStringLiteral("schemaVersion")).toInt(), 2);
    QCOMPARE(firstMetadata.value(QStringLiteral("strictContentHash")).toString(),
             first.strictContentHash);
    const QString workingSource = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QVERIFY(writeFile(workingSource,
                      QByteArrayLiteral("module top; localparam V = 2; endmodule\n")));
    VersionInfo second;
    QVERIFY2(service.createVersion(asset, QStringLiteral("1.1.0"), &second, &error),
             qPrintable(error));
    QVERIFY(first.contentHash != second.contentHash);
    firstMetadata.insert(QStringLiteral("schemaVersion"), 1);
    firstMetadata.remove(QStringLiteral("strictContentHash"));
    QVERIFY(writeFile(firstMetadataPath,
                      QJsonDocument(firstMetadata).toJson(
                          QJsonDocument::Indented)));
    const QList<VersionInfo> versions = service.versions(asset.assetRoot, &error);
    QCOMPARE(versions.size(), 2);
    const auto legacy = std::find_if(
        versions.cbegin(), versions.cend(), [](const VersionInfo &entry) {
            return entry.version == QStringLiteral("1.0.0");
        });
    QVERIFY(legacy != versions.cend());
    QCOMPARE(legacy->schemaVersion, 1);
    QCOMPARE(legacy->strictContentHash, first.strictContentHash);
    VersionInfo duplicateVersion;
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &duplicateVersion,
                                   &error));

    const CopyPlan workingPlan = service.copyPlan(asset, QString());
    QVERIFY2(workingPlan.ok(), qPrintable(workingPlan.error));
    QCOMPARE(workingPlan.version, QString());
    QCOMPARE(workingPlan.sourceRoot, asset.assetRoot);
    QCOMPARE(workingPlan.suggestedName, QStringLiteral("Versioned IP"));
    const CopyPlan savedPlan = service.copyPlan(asset, QStringLiteral("1.0.0"));
    QVERIFY2(savedPlan.ok(), qPrintable(savedPlan.error));
    QCOMPARE(savedPlan.version, QStringLiteral("1.0.0"));
    QCOMPARE(savedPlan.sourceRoot, first.path);
    QCOMPARE(savedPlan.strictContentHash, first.strictContentHash);
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
    QVERIFY(QDir().mkpath(library));
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
    VersionInfo savedVersion;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &savedVersion,
                                   &error),
             qPrintable(error));
    WorkingCopyState state = service.workingCopyState(asset);
    QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
    QVERIFY(state.hasSavedVersion);
    QVERIFY(!state.changed);
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   &savedVersion,
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
                                   &savedVersion,
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
                                           &error,
                                           RemovalMode::Permanent));
    QVERIFY(error.contains(QStringLiteral("changed after the operation")));
    QVERIFY(QFileInfo(restored.undoToken.recoveryPath).isDir());
    QVERIFY(writeFile(workingFile,
                      QByteArrayLiteral(
                          "module uart_rx; localparam V = 1; endmodule\n")));
    error.clear();
    QVERIFY2(service.undoWorkingCopyChange(asset,
                                           restored.undoToken,
                                           &undoneRestore,
                                           &error,
                                           RemovalMode::Permanent),
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

    DeleteVersionResult deleted;
    QVERIFY2(service.deleteVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   RemovalMode::Permanent,
                                   &deleted,
                                   &error),
             qPrintable(error));
    QVERIFY(deleted.snapshotRemoved);
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 1);
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->version, QStringLiteral("1.0.0"));
    QVERIFY(!service.deleteVersion(asset,
                                   QString(),
                                   RemovalMode::Permanent,
                                   &deleted,
                                   &error));
}

void CoreTest::corruptSavedVersionIsRejectedEverywhere_data()
{
    QTest::addColumn<bool>("removePayload");

    QTest::newRow("tampered-payload") << false;
    QTest::newRow("missing-payload") << true;
}

void CoreTest::corruptSavedVersionIsRejectedEverywhere()
{
    QFETCH(bool, removePayload);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("corrupt_snapshot"),
                               .name = QStringLiteral("Corrupt Snapshot"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));

    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QString liveReadme = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("README.md"));
    const QByteArray liveManifestBytes = readFile(asset.manifestPath);
    const QByteArray liveTopBytes = readFile(liveTop);
    const QByteArray liveReadmeBytes = readFile(liveReadme);
    QVERIFY(!liveManifestBytes.isEmpty());
    QVERIFY(!liveTopBytes.isEmpty());
    QVERIFY(!liveReadmeBytes.isEmpty());

    const QString snapshotPayload = QDir(saved.path).absoluteFilePath(
        removePayload ? QStringLiteral("README.md")
                      : QStringLiteral("rtl/top.sv"));
    const QByteArray tamperedBytes = QByteArrayLiteral(
        "module top; localparam TAMPERED = 1; endmodule\n");
    if (removePayload) {
        QVERIFY(QFile::remove(snapshotPayload));
        QVERIFY(!QFileInfo::exists(snapshotPayload));
    } else {
        QVERIFY(writeFile(snapshotPayload, tamperedBytes));
        QCOMPARE(readFile(snapshotPayload), tamperedBytes);
    }

    error.clear();
    const QList<VersionInfo> rejectedVersions = service.versions(
        asset.assetRoot, &error);
    QVERIFY(rejectedVersions.isEmpty());
    QVERIFY(error.isEmpty());

    const CopyPlan rejectedPlan = service.copyPlan(
        asset, QStringLiteral("1.0.0"));
    QVERIFY(!rejectedPlan.ok());
    QVERIFY(!rejectedPlan.error.isEmpty());

    const QString copyParent = temporary.filePath(QStringLiteral("copy-to"));
    QVERIFY(QDir().mkpath(copyParent));
    const QString copyTarget = QDir(copyParent).absoluteFilePath(
        QStringLiteral("corrupt_snapshot"));
    QString copiedPath;
    error.clear();
    QVERIFY(!service.copyVersionPayload(asset,
                                        QStringLiteral("1.0.0"),
                                        copyTarget,
                                        &copiedPath,
                                        &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(copiedPath.isEmpty());
    QVERIFY(!QFileInfo::exists(copyTarget));

    const UpdatePreview rejectedPreview = service.previewRestore(
        asset, QStringLiteral("1.0.0"));
    QVERIFY(!rejectedPreview.ok());
    QVERIFY(!rejectedPreview.error.isEmpty());
    UpdateAssetResult restored;
    error.clear();
    QVERIFY(!service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::Permanent,
                                    &restored,
                                    &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!restored.undoToken.isValid());

    QProcess resolve;
    resolve.start(QString::fromUtf8(XIPS_CLI_PATH),
                  {QStringLiteral("--action"), QStringLiteral("resolve"),
                   QStringLiteral("--library"), library,
                   QStringLiteral("--asset"), asset.manifest.id,
                   QStringLiteral("--asset-version"),
                   QStringLiteral("1.0.0")});
    QVERIFY(resolve.waitForFinished(10000));
    QCOMPARE(resolve.exitStatus(), QProcess::NormalExit);
    QCOMPARE(resolve.exitCode(), 3);
    const QJsonDocument cliFailure = QJsonDocument::fromJson(
        resolve.readAllStandardError());
    QVERIFY(cliFailure.isObject());
    QCOMPARE(cliFailure.object().value(QStringLiteral("ok")).toBool(), false);
    QVERIFY(!cliFailure.object().value(QStringLiteral("error"))
                 .toString().isEmpty());

    QCOMPARE(readFile(asset.manifestPath), liveManifestBytes);
    QCOMPARE(readFile(liveTop), liveTopBytes);
    QCOMPARE(readFile(liveReadme), liveReadmeBytes);
    if (removePayload) {
        QVERIFY(!QFileInfo::exists(snapshotPayload));
    } else {
        QCOMPARE(readFile(snapshotPayload), tamperedBytes);
    }
    QVERIFY(QDir(copyParent)
                .entryList({QStringLiteral(".xips-copy-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::versionInventoryKeepsValidVersionsWhenOneIsCorrupt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("counter.v"));
    QVERIFY(writeFile(
        source,
        QByteArrayLiteral("module counter; localparam REV = 1; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));

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

    VersionInfo first;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &first,
                                   &error),
             qPrintable(error));
    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("counter.v"));
    QVERIFY(writeFile(
        workingFile,
        QByteArrayLiteral("module counter; localparam REV = 2; endmodule\n")));
    VersionInfo second;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   &second,
                                   &error),
             qPrintable(error));

    const QString corruptPayload = QDir(first.path).absoluteFilePath(
        QStringLiteral("counter.v"));
    QVERIFY(writeFile(
        corruptPayload,
        QByteArrayLiteral("module counter; localparam CORRUPT = 1; endmodule\n")));

    const VersionInventoryResult inventory = service.versionInventory(
        asset.assetRoot);
    QVERIFY2(inventory.fatalError.isEmpty(),
             qPrintable(inventory.fatalError));
    QCOMPARE(inventory.validVersions.size(), 1);
    QCOMPARE(inventory.validVersions.first().version, QStringLiteral("1.0.1"));
    QCOMPARE(inventory.problems.size(), 1);
    QVERIFY(inventory.problems.first().contains(first.path));

    error.clear();
    const QList<VersionInfo> strictVersions = service.versions(asset.assetRoot,
                                                               &error);
    QCOMPARE(strictVersions.size(), 1);
    QCOMPARE(strictVersions.first().version, QStringLiteral("1.0.1"));
    QVERIFY(error.isEmpty());
}

void CoreTest::metadataChangesDoNotCreateCodeVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("metadata.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module metadata; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("metadata"),
                               .name = QStringLiteral("Metadata"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo first;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &first,
                                   &error),
             qPrintable(error));

    MetadataUpdateResult metadataResult;
    QVERIFY2(service.updateMetadata(
                 asset,
                 {.id = asset.manifest.id,
                  .name = QStringLiteral("Renamed metadata asset"),
                  .description = QStringLiteral("Description only"),
                  .tags = {QStringLiteral("Protocol")}},
                 &metadataResult,
                 &error),
             qPrintable(error));
    QVERIFY(metadataResult.changed);
    const WorkingCopyState state = service.workingCopyState(asset);
    QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
    QVERIFY(state.hasSavedVersion);
    QVERIFY(!state.changed);
    VersionInfo rejected;
    error.clear();
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("1.0.1"),
                                   &rejected,
                                   &error));
    QVERIFY(error.contains(QStringLiteral("has not changed")));
    QCOMPARE(readFile(QDir(first.path).absoluteFilePath(
                          QStringLiteral("metadata.sv"))),
             QByteArrayLiteral("module metadata; endmodule\n"));
}

void CoreTest::corruptVersionDoesNotBlockHealthyVersionWorkflows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("healthy.sv"));
    const QByteArray firstBytes = QByteArrayLiteral(
        "module healthy; localparam REV = 1; endmodule\n");
    const QByteArray secondBytes = QByteArrayLiteral(
        "module healthy; localparam REV = 2; endmodule\n");
    const QByteArray corruptBytes = QByteArrayLiteral("corrupt snapshot\n");
    QVERIFY(writeFile(source, firstBytes));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("healthy"),
                               .name = QStringLiteral("Healthy"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo first;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &first,
                                   &error),
             qPrintable(error));
    const QString workingFile = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("healthy.sv"));
    QVERIFY(writeFile(workingFile, secondBytes));
    VersionInfo corrupt;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("2.0.0"),
                                   &corrupt,
                                   &error),
             qPrintable(error));
    const QString corruptFile = QDir(corrupt.path).absoluteFilePath(
        QStringLiteral("healthy.sv"));
    QVERIFY(writeFile(corruptFile, corruptBytes));

    VersionInventoryResult inventory = service.versionInventory(
        asset.assetRoot);
    QCOMPARE(inventory.validVersions.size(), 1);
    QCOMPARE(inventory.invalidVersions.size(), 1);
    QCOMPARE(inventory.invalidVersions.first().version,
             QStringLiteral("2.0.0"));
    const WorkingCopyState state = service.workingCopyState(asset, inventory);
    QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
    QCOMPARE(state.latestVersion, QStringLiteral("1.0.0"));
    QVERIFY(state.changed);

    VersionInfo rejected;
    error.clear();
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("2.0.0"),
                                   &rejected,
                                   &error));
    QCOMPARE(readFile(corruptFile), corruptBytes);
    VersionInfo third;
    error.clear();
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("3.0.0"),
                                   &third,
                                   &error),
             qPrintable(error));
    DeleteVersionResult deleted;
    error.clear();
    QVERIFY2(service.deleteVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   RemovalMode::Permanent,
                                   &deleted,
                                   &error),
             qPrintable(error));
    QVERIFY(deleted.snapshotRemoved);
    QVERIFY(QFileInfo(corrupt.path).isDir());
    QVERIFY(QFileInfo(third.path).isDir());

    inventory = service.versionInventory(asset.assetRoot);
    QCOMPARE(inventory.invalidVersions.size(), 1);
    const auto problem = inventory.invalidVersions.first();
    CorruptVersionIsolationResult isolated;
    error.clear();
    QVERIFY(!service.isolateCorruptVersion(
        asset,
        problem.version,
        QStringLiteral("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
        &isolated,
        &error));
    QVERIFY(QFileInfo(problem.path).isDir());
    error.clear();
    QVERIFY2(service.isolateCorruptVersion(asset,
                                           problem.version,
                                           problem.fingerprint,
                                           &isolated,
                                           &error),
             qPrintable(error));
    QVERIFY(isolated.isolated);
    QVERIFY(!QFileInfo::exists(problem.path));
    QVERIFY(QFileInfo(isolated.retainedPath).isDir());
    QVERIFY(isolated.retainedPath.contains(
        QStringLiteral(".staging-quarantine-corrupt-")));
    QCOMPARE(readFile(QDir(isolated.retainedPath).absoluteFilePath(
                          QStringLiteral("healthy.sv"))),
             corruptBytes);
}

void CoreTest::payloadTransactionsRestoreVisibleDataAfterIsolationCrash()
{
    const auto expectCrash = [](auto operation) {
        bool crashed = false;
        try {
            operation();
        } catch (const SimulatedManifestCrash &) {
            crashed = true;
        }
        return crashed;
    };

    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath(QStringLiteral("update.sv"));
        const QString replacement = temporary.filePath(
            QStringLiteral("replacement.sv"));
        const QByteArray original = QByteArrayLiteral("module update; endmodule\n");
        QVERIFY(writeFile(source, original));
        QVERIFY(writeFile(replacement,
                          QByteArrayLiteral("module update; wire changed; endmodule\n")));
        const QString library = temporary.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library));
        AssetLibraryService service;
        AssetRecord asset;
        QString error;
        QVERIFY2(service.importAsset(
                     {.libraryRoot = library,
                      .sourcePath = source,
                      .metadata = {.id = QStringLiteral("update"),
                                   .name = QStringLiteral("Update"),
                                   .description = {},
                                   .tags = {}}},
                     &asset,
                     &error),
                 qPrintable(error));
        service.setWorkingCopyTestHook(
            WorkingCopyTestPoint::StagingVerifiedBeforePublish,
            [](const WorkingCopyTestPoint, const QString &) {
                throw SimulatedManifestCrash{};
            });
        QVERIFY(expectCrash([&] {
            UpdateAssetResult ignored;
            service.updateAsset(asset,
                                replacement,
                                WorkingCopyRecoveryMode::Permanent,
                                &ignored,
                                &error);
        }));
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        const QStringList transactionNames = QDir(library).entryList(
            {QStringLiteral(".xips-create-transaction-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(transactionNames.size(), 1);
        const QString transactionRecord = QDir(library).absoluteFilePath(
            transactionNames.first() + QStringLiteral("/transaction.json"));
        const QByteArray originalRecord = readFile(transactionRecord);
        QVERIFY(!originalRecord.isEmpty());
        bool changedRecord = false;
        service.setWorkingCopyTestHook(
            WorkingCopyTestPoint::PayloadTransactionReadBeforeRecovery,
            [&](const WorkingCopyTestPoint, const QString &) {
                QJsonObject changedRecordObject = QJsonDocument::fromJson(
                    originalRecord).object();
                changedRecordObject.insert(QStringLiteral("concurrent"), true);
                changedRecord = writeFile(
                    transactionRecord,
                    QJsonDocument(changedRecordObject).toJson(
                        QJsonDocument::Indented));
            });
        const PayloadTransactionRecoveryResult rejectedRecovery =
            service.recoverPayloadTransactions(library);
        QVERIFY(changedRecord);
        QCOMPARE(rejectedRecovery.retained, 1);
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        const auto recordWithPhase = [&](const QString &phase) {
            QJsonObject object = QJsonDocument::fromJson(originalRecord)
                                     .object();
            object.insert(QStringLiteral("phase"), phase);
            return QJsonDocument(object).toJson(QJsonDocument::Indented);
        };
        QVERIFY(writeFile(transactionRecord,
                          recordWithPhase(QStringLiteral("published"))));
        const PayloadTransactionRecoveryResult publishedRecovery =
            service.recoverPayloadTransactions(library);
        QCOMPARE(publishedRecovery.retained, 1);
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        QVERIFY(writeFile(transactionRecord,
                          recordWithPhase(QStringLiteral("prepared"))));
        const PayloadTransactionRecoveryResult preparedRecovery =
            service.recoverPayloadTransactions(library);
        QCOMPARE(preparedRecovery.retained, 1);
        QVERIFY(!QFileInfo::exists(asset.assetRoot));

        QVERIFY(writeFile(transactionRecord, originalRecord));
        const QString incompleteTransaction = QDir(library).absoluteFilePath(
            QStringLiteral(
                ".xips-create-delete-11111111-1111-1111-1111-111111111111"));
        QVERIFY(QDir().mkpath(incompleteTransaction));
        const PayloadTransactionRecoveryResult incompleteRecovery =
            service.recoverPayloadTransactions(library);
        QCOMPARE(incompleteRecovery.retained, 2);
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        QVERIFY(QDir().rmdir(incompleteTransaction));
        const PayloadTransactionRecoveryResult recovered =
            service.recoverPayloadTransactions(library);
        QCOMPARE(recovered.restoredVisible, 1);
        QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                              QStringLiteral("update.sv"))),
                 original);
    }

    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath(QStringLiteral("delete.sv"));
        QVERIFY(writeFile(source,
                          QByteArrayLiteral("module delete_me; endmodule\n")));
        const QString library = temporary.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library));
        AssetLibraryService service;
        AssetRecord asset;
        QString error;
        QVERIFY2(service.importAsset(
                     {.libraryRoot = library,
                      .sourcePath = source,
                      .metadata = {.id = QStringLiteral("delete_me"),
                                   .name = QStringLiteral("Delete"),
                                   .description = {},
                                   .tags = {}}},
                     &asset,
                     &error),
                 qPrintable(error));
        const AssetDeletionProof proof = service.assetDeletionProof(asset);
        QVERIFY(proof.ok());
        service.setWorkingCopyTestHook(
            WorkingCopyTestPoint::DeleteAssetIsolatedBeforeRemoval,
            [](const WorkingCopyTestPoint, const QString &) {
                throw SimulatedManifestCrash{};
            });
        QVERIFY(expectCrash([&] {
            QString ignored;
            service.deleteAsset(library,
                                asset,
                                proof,
                                RemovalMode::Permanent,
                                &ignored,
                                &error);
        }));
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        const QStringList deleteTransactions = QDir(library).entryList(
            {QStringLiteral(".xips-create-delete-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(deleteTransactions.size(), 1);
        const QString deleteRecord = QDir(library).absoluteFilePath(
            deleteTransactions.first() + QStringLiteral("/transaction.json"));
        const QByteArray isolatedDeleteRecord = readFile(deleteRecord);
        QJsonObject preparedDelete = QJsonDocument::fromJson(
            isolatedDeleteRecord).object();
        preparedDelete.insert(QStringLiteral("phase"),
                              QStringLiteral("prepared"));
        QVERIFY(writeFile(deleteRecord,
                          QJsonDocument(preparedDelete).toJson(
                              QJsonDocument::Indented)));
        const PayloadTransactionRecoveryResult preparedRecovery =
            service.recoverPayloadTransactions(library);
        QCOMPARE(preparedRecovery.retained, 1);
        QVERIFY(!QFileInfo::exists(asset.assetRoot));
        QVERIFY(writeFile(deleteRecord, isolatedDeleteRecord));
        const PayloadTransactionRecoveryResult recovered =
            service.recoverPayloadTransactions(library);
        QCOMPARE(recovered.restoredVisible, 1);
        QVERIFY(QFileInfo(asset.assetRoot).isDir());
    }

    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath(QStringLiteral("version.sv"));
        QVERIFY(writeFile(source,
                          QByteArrayLiteral("module version; endmodule\n")));
        const QString library = temporary.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(library));
        AssetLibraryService service;
        AssetRecord asset;
        QString error;
        QVERIFY2(service.importAsset(
                     {.libraryRoot = library,
                      .sourcePath = source,
                      .metadata = {.id = QStringLiteral("version"),
                                   .name = QStringLiteral("Version"),
                                   .description = {},
                                   .tags = {}}},
                     &asset,
                     &error),
                 qPrintable(error));
        VersionInfo saved;
        QVERIFY2(service.createVersion(asset,
                                       QStringLiteral("1.0.0"),
                                       &saved,
                                       &error),
                 qPrintable(error));
        service.setWorkingCopyTestHook(
            WorkingCopyTestPoint::DeleteVersionIsolatedBeforeMarkerCas,
            [](const WorkingCopyTestPoint, const QString &) {
                throw SimulatedManifestCrash{};
            });
        QVERIFY(expectCrash([&] {
            DeleteVersionResult ignored;
            service.deleteVersion(asset,
                                  QStringLiteral("1.0.0"),
                                  RemovalMode::Permanent,
                                  &ignored,
                                  &error);
        }));
        QVERIFY(!QFileInfo::exists(saved.path));
        const QString versionsRoot = QFileInfo(saved.path).absolutePath();
        const QStringList deleteTransactions = QDir(versionsRoot).entryList(
            {QStringLiteral(".staging-transaction-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(deleteTransactions.size(), 1);
        const QString deleteRecord = QDir(versionsRoot).absoluteFilePath(
            deleteTransactions.first() + QStringLiteral("/transaction.json"));
        const QByteArray isolatedDeleteRecord = readFile(deleteRecord);
        QJsonObject preparedDelete = QJsonDocument::fromJson(
            isolatedDeleteRecord).object();
        preparedDelete.insert(QStringLiteral("phase"),
                              QStringLiteral("prepared"));
        QVERIFY(writeFile(deleteRecord,
                          QJsonDocument(preparedDelete).toJson(
                              QJsonDocument::Indented)));
        const PayloadTransactionRecoveryResult preparedRecovery =
            service.recoverPayloadTransactions(library);
        QCOMPARE(preparedRecovery.retained, 1);
        QVERIFY(!QFileInfo::exists(saved.path));
        QVERIFY(writeFile(deleteRecord, isolatedDeleteRecord));
        const PayloadTransactionRecoveryResult recovered =
            service.recoverPayloadTransactions(library);
        QCOMPARE(recovered.restoredVisible, 1);
        QVERIFY(QFileInfo(saved.path).isDir());
    }
}

void CoreTest::payloadRecoveryIgnoresForgedControlDataInsideAssets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("payload.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module payload; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("payload"),
                               .name = QStringLiteral("Payload"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    const QString forgedRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(
            ".xips-create-transaction-11111111-1111-1111-1111-111111111111"));
    const QString forgedRecord = QDir(forgedRoot).absoluteFilePath(
        QStringLiteral("transaction.json"));
    const QByteArray forgedBytes = R"({
        "schemaVersion": 1,
        "owner": "xips-payload-transaction",
        "operation": "update",
        "transactionId": "11111111-1111-1111-1111-111111111111",
        "phase": "published",
        "assetName": "payload",
        "assetId": "payload",
        "stagingName": ".xips-create-update-11111111-1111-1111-1111-111111111111",
        "isolatedName": ".xips-create-recovery-11111111-1111-1111-1111-111111111111",
        "version": "",
        "expectedFingerprint": "sha256:0000000000000000000000000000000000000000000000000000000000000000"
    })";
    QVERIFY(writeFile(forgedRecord, forgedBytes));

    const PayloadTransactionRecoveryResult recovered =
        service.recoverPayloadTransactions(library);
    QVERIFY2(recovered.fatalError.isEmpty(),
             qPrintable(recovered.fatalError));
    QVERIFY(recovered.items.isEmpty());
    QVERIFY(QFileInfo(forgedRoot).isDir());
    QCOMPARE(readFile(forgedRecord), forgedBytes);
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("payload.sv"))),
             QByteArrayLiteral("module payload; endmodule\n"));
}

void CoreTest::payloadRecoveryDoesNotFollowLinkedInternalDirectory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QString assetRoot = QDir(library).absoluteFilePath(
        QStringLiteral("linked_asset"));
    QVERIFY(writeFile(QDir(assetRoot).absoluteFilePath(
                          QStringLiteral(".xips.json")),
                      QByteArrayLiteral(
                          "{\"schemaVersion\":1,\"id\":\"linked_asset\",\"name\":\"Linked\"}")));
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    const QString transactionRoot = QDir(outside).absoluteFilePath(
        QStringLiteral(
            "versions/.staging-transaction-11111111-1111-1111-1111-111111111111"));
    const QString recordPath = QDir(transactionRoot).absoluteFilePath(
        QStringLiteral("transaction.json"));
    const QByteArray recordBytes = R"({
        "schemaVersion": 1,
        "owner": "xips-payload-transaction",
        "operation": "delete-version",
        "transactionId": "11111111-1111-1111-1111-111111111111",
        "phase": "removed",
        "assetName": "linked_asset",
        "assetId": "linked_asset",
        "stagingName": "",
        "isolatedName": ".staging-delete-11111111-1111-1111-1111-111111111111",
        "version": "1.0.0",
        "expectedFingerprint": "sha256:0000000000000000000000000000000000000000000000000000000000000000"
    })";
    QVERIFY(writeFile(recordPath, recordBytes));
    const QString outsideVersion = QDir(outside).absoluteFilePath(
        QStringLiteral("versions/1.0.0"));
    const QString outsidePayload = QDir(outsideVersion).absoluteFilePath(
        QStringLiteral("outside.sv"));
    const QByteArray outsideBytes = QByteArrayLiteral(
        "module outside; endmodule\n");
    QVERIFY(writeFile(outsidePayload, outsideBytes));
    const QString linkedInternal = QDir(assetRoot).absoluteFilePath(
        QStringLiteral(".xips"));
#ifdef Q_OS_WIN
    QProcess junction;
    junction.start(QStringLiteral("cmd.exe"),
                   {QStringLiteral("/d"),
                    QStringLiteral("/c"),
                    QStringLiteral("mklink"),
                    QStringLiteral("/J"),
                    QDir::toNativeSeparators(linkedInternal),
                    QDir::toNativeSeparators(outside)});
    QVERIFY(junction.waitForFinished(10000));
    QCOMPARE(junction.exitStatus(), QProcess::NormalExit);
    QCOMPARE(junction.exitCode(), 0);
#else
    QVERIFY2(QFile::link(outside, linkedInternal),
             qPrintable(QStringLiteral("Cannot create directory link: %1")
                            .arg(linkedInternal)));
#endif

    AssetLibraryService service;
    const PayloadTransactionRecoveryResult recovered =
        service.recoverPayloadTransactions(library);
    QVERIFY2(recovered.fatalError.isEmpty(),
             qPrintable(recovered.fatalError));
    QVERIFY(recovered.items.isEmpty());
    QVERIFY(QFileInfo(transactionRoot).isDir());
    QCOMPARE(readFile(recordPath), recordBytes);

    const VersionInventoryResult inventory = service.versionInventory(
        assetRoot);
    QVERIFY(!inventory.fatalError.isEmpty());
    QVERIFY(inventory.invalidVersions.isEmpty());
    const ManifestLoadResult loaded = ManifestService().load(
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json")));
    QVERIFY(loaded.ok());
    AssetRecord linkedAsset{
        .manifest = *loaded.manifest,
        .assetRoot = assetRoot,
        .manifestPath = QDir(assetRoot).absoluteFilePath(
            QStringLiteral(".xips.json")),
        .files = {},
        .lastModified = {},
    };
    CorruptVersionIsolationResult isolation;
    QString isolationError;
    QVERIFY(!service.isolateCorruptVersion(
        linkedAsset,
        QStringLiteral("1.0.0"),
        QStringLiteral(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
        &isolation,
        &isolationError));
    QVERIFY(QFileInfo(outsideVersion).isDir());
    QCOMPARE(readFile(outsidePayload), outsideBytes);
    const QStringList quarantineEntries = QDir(
        QDir(outside).absoluteFilePath(QStringLiteral("versions")))
                                              .entryList(
        {QStringLiteral(".staging-quarantine-corrupt-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(quarantineEntries.isEmpty());

#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedInternal));
#else
    QVERIFY(QFile::remove(linkedInternal));
#endif
    QVERIFY(QFileInfo(transactionRoot).isDir());
    QCOMPARE(readFile(recordPath), recordBytes);
}

void CoreTest::payloadRecoveryRetainsRecordWithConcurrentEvidence()
{
    {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("original.sv"));
    const QString replacement = temporary.filePath(
        QStringLiteral("replacement.sv"));
    const QByteArray original = QByteArrayLiteral(
        "module original; endmodule\n");
    QVERIFY(writeFile(source, original));
    QVERIFY(writeFile(replacement,
                      QByteArrayLiteral(
                          "module original; wire changed; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("concurrent_evidence"),
                               .name = QStringLiteral("Concurrent evidence"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::StagingVerifiedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedManifestCrash{};
        });
    bool crashed = false;
    try {
        UpdateAssetResult ignored;
        service.updateAsset(asset,
                            replacement,
                            WorkingCopyRecoveryMode::Permanent,
                            &ignored,
                            &error);
    } catch (const SimulatedManifestCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);
    const QStringList transactionNames = QDir(library).entryList(
        {QStringLiteral(".xips-create-transaction-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(transactionNames.size(), 1);
    const QString transactionRoot = QDir(library).absoluteFilePath(
        transactionNames.first());
    const QString recordPath = QDir(transactionRoot).absoluteFilePath(
        QStringLiteral("transaction.json"));
    QVERIFY(QFileInfo(recordPath).isFile());
    const QString concurrentEvidence = QDir(transactionRoot).absoluteFilePath(
        QStringLiteral("concurrent.keep"));
    const QByteArray evidenceBytes = QByteArrayLiteral("synchronizer evidence");
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::PayloadTransactionReadBeforeRecovery,
        [&](const WorkingCopyTestPoint, const QString &path) {
            if (path == transactionRoot) {
                QVERIFY(writeFile(concurrentEvidence, evidenceBytes));
            }
        });
    const PayloadTransactionRecoveryResult recovered =
        service.recoverPayloadTransactions(library);
    QCOMPARE(recovered.retained, 1);
    QVERIFY(QFileInfo(recordPath).isFile());
    QCOMPARE(readFile(concurrentEvidence), evidenceBytes);
    QVERIFY(QFileInfo(asset.assetRoot).isDir());
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("original.sv"))),
             original);
    }

    {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("linked.sv"));
    const QString replacement = temporary.filePath(
        QStringLiteral("linked-replacement.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module linked; endmodule\n")));
    QVERIFY(writeFile(replacement,
                      QByteArrayLiteral(
                          "module linked; wire changed; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("linked_recovery"),
                               .name = QStringLiteral("Linked recovery"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo version;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &version,
                                   &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::StagingVerifiedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedManifestCrash{};
        });
    bool crashed = false;
    try {
        UpdateAssetResult ignored;
        service.updateAsset(asset,
                            replacement,
                            WorkingCopyRecoveryMode::Permanent,
                            &ignored,
                            &error);
    } catch (const SimulatedManifestCrash &) {
        crashed = true;
    }
    QVERIFY(crashed);

    const QString outside = temporary.filePath(QStringLiteral("outside"));
    const QString outsideSentinel = QDir(outside).absoluteFilePath(
        QStringLiteral(".xips/sentinel.bin"));
    const QByteArray outsideBytes = QByteArrayLiteral("outside-internal");
    QVERIFY(writeFile(outsideSentinel, outsideBytes));
    QString linkedStaging;
    QString preservedStaging;
    bool linkCreated = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::PayloadRecoveryBeforeInternalReattach,
        [&](const WorkingCopyTestPoint, const QString &stagingRoot) {
            linkedStaging = stagingRoot;
            preservedStaging = stagingRoot + QStringLiteral(".preserved");
            QDir parent(QFileInfo(stagingRoot).absolutePath());
            if (!parent.rename(QFileInfo(stagingRoot).fileName(),
                               QFileInfo(preservedStaging).fileName())) {
                return;
            }
#ifdef Q_OS_WIN
            QProcess junction;
            junction.start(QStringLiteral("cmd.exe"),
                           {QStringLiteral("/d"),
                            QStringLiteral("/c"),
                            QStringLiteral("mklink"),
                            QStringLiteral("/J"),
                            QDir::toNativeSeparators(stagingRoot),
                            QDir::toNativeSeparators(outside)});
            linkCreated = junction.waitForFinished(10000)
                          && junction.exitStatus() == QProcess::NormalExit
                          && junction.exitCode() == 0;
#else
            linkCreated = QFile::link(outside, stagingRoot);
#endif
        });
    const PayloadTransactionRecoveryResult recovered =
        service.recoverPayloadTransactions(library);
    QCOMPARE(recovered.retained, 1);
    QVERIFY(linkCreated);
    QCOMPARE(readFile(outsideSentinel), outsideBytes);
    QVERIFY(QFileInfo(QDir(outside).absoluteFilePath(
                         QStringLiteral(".xips"))).isDir());
    QVERIFY(QFileInfo(preservedStaging).isDir());
#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedStaging));
#else
    QVERIFY(QFile::remove(linkedStaging));
#endif
    }
}

void CoreTest::versionInventorySkipsLinkedVersionDirectories()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("safe_link.v"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module safe_link; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));

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
    VersionInfo saved;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));

    const QString linkedTarget = temporary.filePath(
        QStringLiteral("outside-version-target"));
    const QString sentinel = QDir(linkedTarget).absoluteFilePath(
        QStringLiteral("must-not-be-traversed.txt"));
    QVERIFY(writeFile(sentinel, QByteArrayLiteral("outside\n")));
    const QString linkedVersion = QDir(QFileInfo(saved.path).absolutePath())
                                      .absoluteFilePath(
                                          QStringLiteral("9.9.9"));
#ifdef Q_OS_WIN
    QProcess junction;
    junction.start(QStringLiteral("cmd.exe"),
                   {QStringLiteral("/d"),
                    QStringLiteral("/c"),
                    QStringLiteral("mklink"),
                    QStringLiteral("/J"),
                    QDir::toNativeSeparators(linkedVersion),
                    QDir::toNativeSeparators(linkedTarget)});
    QVERIFY(junction.waitForFinished(10000));
    QCOMPARE(junction.exitStatus(), QProcess::NormalExit);
    QCOMPARE(junction.exitCode(), 0);
#else
    QVERIFY2(QFile::link(linkedTarget, linkedVersion),
             qPrintable(QStringLiteral("Cannot create directory link: %1")
                            .arg(linkedVersion)));
#endif

    const VersionInventoryResult inventory = service.versionInventory(
        asset.assetRoot);
    QVERIFY2(inventory.fatalError.isEmpty(),
             qPrintable(inventory.fatalError));
    QCOMPARE(inventory.validVersions.size(), 1);
    QCOMPARE(inventory.validVersions.first().version, QStringLiteral("1.0.0"));
    QCOMPARE(inventory.problems.size(), 1);
    QVERIFY(inventory.problems.first().contains(linkedVersion));
    QVERIFY(QFileInfo(sentinel).isFile());

    error.clear();
    QCOMPARE(service.versions(asset.assetRoot, &error).size(), 1);
    QVERIFY(error.isEmpty());

#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedVersion));
#else
    QVERIFY(QFile::remove(linkedVersion));
#endif
    QVERIFY(QFileInfo(sentinel).isFile());
}

void CoreTest::createVersionRejectsConcurrentWorkingCopyMutation_data()
{
    QTest::addColumn<QString>("mutation");

    QTest::newRow("manifest-change") << QStringLiteral("manifest");
    QTest::newRow("payload-change") << QStringLiteral("payload");
    QTest::newRow("internal-junction") << QStringLiteral("internal-junction");
    QTest::newRow("asset-root-junction")
        << QStringLiteral("asset-root-junction");
}

void CoreTest::createVersionRejectsConcurrentWorkingCopyMutation()
{
    QFETCH(QString, mutation);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("version_race"),
                               .name = QStringLiteral("Version Race"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QByteArray originalBytes = readFile(liveTop);
    const QByteArray concurrentBytes = QByteArrayLiteral(
        "module top; localparam SYNC_REV = 2; endmodule\n");
    const QString concurrentDescription = QStringLiteral(
        "description written by concurrent sync");
    int hookCount = 0;
    bool mutationSucceeded = false;
    const QString internalRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips"));
    const QString outside = temporary.filePath(
        QStringLiteral("outside-version-root"));
    const QString outsideSentinel = QDir(outside).absoluteFilePath(
        QStringLiteral("sentinel.bin"));
    const QByteArray outsideBytes = QByteArrayLiteral("outside-unchanged");
    QVERIFY(writeFile(outsideSentinel, outsideBytes));
    const QString preservedAssetRoot = asset.assetRoot
                                       + QStringLiteral(".preserved");
    const WorkingCopyTestPoint testPoint =
        mutation.endsWith(QStringLiteral("junction"))
            ? WorkingCopyTestPoint::VersionHierarchyCheckedBeforeCreate
            : WorkingCopyTestPoint::VersionStagingVerifiedBeforePublish;
    service.setWorkingCopyTestHook(
        testPoint,
        [&](const WorkingCopyTestPoint point, const QString &) {
            if (point != testPoint) {
                return;
            }
            ++hookCount;
            if (mutation == QStringLiteral("asset-root-junction")) {
                QDir assetParent(QFileInfo(asset.assetRoot).absolutePath());
                if (!assetParent.rename(QFileInfo(asset.assetRoot).fileName(),
                                        QFileInfo(preservedAssetRoot)
                                            .fileName())) {
                    return;
                }
#ifdef Q_OS_WIN
                QProcess junction;
                junction.start(QStringLiteral("cmd.exe"),
                               {QStringLiteral("/d"),
                                QStringLiteral("/c"),
                                QStringLiteral("mklink"),
                                QStringLiteral("/J"),
                                QDir::toNativeSeparators(asset.assetRoot),
                                QDir::toNativeSeparators(outside)});
                mutationSucceeded = junction.waitForFinished(10000)
                                    && junction.exitStatus()
                                           == QProcess::NormalExit
                                    && junction.exitCode() == 0;
#else
                mutationSucceeded = QFile::link(outside, asset.assetRoot);
#endif
                return;
            }
            if (mutation == QStringLiteral("internal-junction")) {
#ifdef Q_OS_WIN
                QProcess junction;
                junction.start(QStringLiteral("cmd.exe"),
                               {QStringLiteral("/d"),
                                QStringLiteral("/c"),
                                QStringLiteral("mklink"),
                                QStringLiteral("/J"),
                                QDir::toNativeSeparators(internalRoot),
                                QDir::toNativeSeparators(outside)});
                mutationSucceeded = junction.waitForFinished(10000)
                                    && junction.exitStatus()
                                           == QProcess::NormalExit
                                    && junction.exitCode() == 0;
#else
                mutationSucceeded = QFile::link(outside, internalRoot);
#endif
                return;
            }
            if (mutation == QStringLiteral("payload")) {
                mutationSucceeded = writeFile(liveTop, concurrentBytes);
                return;
            }
            const ManifestLoadResult loaded = ManifestService().load(
                asset.manifestPath);
            if (!loaded.ok()) {
                return;
            }
            Manifest changed = *loaded.manifest;
            changed.description = concurrentDescription;
            QString writeError;
            mutationSucceeded = ManifestService().write(asset.manifestPath,
                                                         changed,
                                                         &writeError);
        });

    VersionInfo created;
    error.clear();
    QVERIFY(!service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &created,
                                   &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!error.isEmpty());
    QVERIFY(created.version.isEmpty());
    QVERIFY(created.path.isEmpty());
    QVERIFY(created.strictContentHash.isEmpty());

    if (mutation == QStringLiteral("asset-root-junction")) {
        QCOMPARE(readFile(outsideSentinel), outsideBytes);
        QVERIFY(!QFileInfo::exists(QDir(outside).absoluteFilePath(
            QStringLiteral(".xips"))));
#ifdef Q_OS_WIN
        QVERIFY(QDir().rmdir(asset.assetRoot));
#else
        QVERIFY(QFile::remove(asset.assetRoot));
#endif
        QDir assetParent(QFileInfo(asset.assetRoot).absolutePath());
        QVERIFY(assetParent.rename(QFileInfo(preservedAssetRoot).fileName(),
                                   QFileInfo(asset.assetRoot).fileName()));
    } else if (mutation == QStringLiteral("internal-junction")) {
        QCOMPARE(readFile(outsideSentinel), outsideBytes);
        QVERIFY(!QFileInfo::exists(QDir(outside).absoluteFilePath(
            QStringLiteral("versions"))));
#ifdef Q_OS_WIN
        QVERIFY(QDir().rmdir(internalRoot));
#else
        QVERIFY(QFile::remove(internalRoot));
#endif
    }

    const QString versionsRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    QVERIFY(!QFileInfo::exists(QDir(versionsRoot).absoluteFilePath(
        QStringLiteral("1.0.0"))));
    QVERIFY(QDir(versionsRoot)
                .entryList({QStringLiteral(".staging-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    error.clear();
    const QList<VersionInfo> versions = service.versions(asset.assetRoot,
                                                         &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(versions.isEmpty());

    const ManifestLoadResult liveManifest = ManifestService().load(
        asset.manifestPath);
    QVERIFY(liveManifest.ok());
    QVERIFY(liveManifest.manifest->version.isEmpty());
    if (mutation == QStringLiteral("manifest")) {
        QCOMPARE(liveManifest.manifest->description, concurrentDescription);
        QCOMPARE(readFile(liveTop), originalBytes);
    } else if (mutation == QStringLiteral("payload")) {
        QVERIFY(liveManifest.manifest->description.isEmpty());
        QCOMPARE(readFile(liveTop), concurrentBytes);
    } else {
        QVERIFY(liveManifest.manifest->description.isEmpty());
        QCOMPARE(readFile(liveTop), originalBytes);
    }
    QCOMPARE(readFile(QDir(source).absoluteFilePath(
                          QStringLiteral("rtl/top.sv"))),
             originalBytes);
}

void CoreTest::savedVersionConsumersRejectPostCopyMutation_data()
{
    QTest::addColumn<QString>("consumer");

    QTest::newRow("copy") << QStringLiteral("copy");
    QTest::newRow("restore") << QStringLiteral("restore");
}

void CoreTest::savedVersionConsumersRejectPostCopyMutation()
{
    QFETCH(QString, consumer);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("snapshot_copy_race"),
                               .name = QStringLiteral("Snapshot Copy Race"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));

    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QByteArray newerLiveBytes = QByteArrayLiteral(
        "module top; localparam LIVE_REV = 2; endmodule\n");
    QVERIFY(writeFile(liveTop, newerLiveBytes));
    const QByteArray liveManifestBytes = readFile(asset.manifestPath);
    const QString snapshotTop = QDir(saved.path).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QByteArray tamperedSnapshotBytes = QByteArrayLiteral(
        "module top; localparam SNAPSHOT_TAMPER = 1; endmodule\n");

    int hookCount = 0;
    bool mutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::SavedVersionCopiedBeforeVerification,
        [&](const WorkingCopyTestPoint point, const QString &path) {
            if (point
                != WorkingCopyTestPoint::SavedVersionCopiedBeforeVerification) {
                return;
            }
            ++hookCount;
            mutationSucceeded = writeFile(
                QDir(path).absoluteFilePath(QStringLiteral("rtl/top.sv")),
                tamperedSnapshotBytes);
        });

    const QString copyParent = temporary.filePath(QStringLiteral("copy-to"));
    QVERIFY(QDir().mkpath(copyParent));
    const QString copyTarget = QDir(copyParent).absoluteFilePath(
        QStringLiteral("snapshot_copy_race"));
    error.clear();
    if (consumer == QStringLiteral("copy")) {
        QString copiedPath;
        QVERIFY(!service.copyVersionPayload(asset,
                                            QStringLiteral("1.0.0"),
                                            copyTarget,
                                            &copiedPath,
                                            &error));
        QVERIFY(copiedPath.isEmpty());
        QVERIFY(!QFileInfo::exists(copyTarget));
    } else {
        UpdateAssetResult restored;
        QVERIFY(!service.restoreVersion(asset,
                                        QStringLiteral("1.0.0"),
                                        WorkingCopyRecoveryMode::Permanent,
                                        &restored,
                                        &error));
        QVERIFY(!restored.undoToken.isValid());
    }
    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(snapshotTop), tamperedSnapshotBytes);
    QCOMPARE(readFile(liveTop), newerLiveBytes);
    QCOMPARE(readFile(asset.manifestPath), liveManifestBytes);
    QVERIFY(!QFileInfo::exists(copyTarget));
    QVERIFY(QDir(copyParent)
                .entryList({QStringLiteral(".xips-copy-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::restoreBindsVerifiedStagingToUpdate_data()
{
    QTest::addColumn<bool>("addUnexpectedFile");

    QTest::newRow("payload-tampered") << false;
    QTest::newRow("unexpected-file-added") << true;
}

void CoreTest::restoreBindsVerifiedStagingToUpdate()
{
    QFETCH(bool, addUnexpectedFile);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("restore_proof_binding"),
                               .name = QStringLiteral("Restore Proof Binding"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));

    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QByteArray newerLiveBytes = QByteArrayLiteral(
        "module top; localparam LIVE_REV = 2; endmodule\n");
    QVERIFY(writeFile(liveTop, newerLiveBytes));
    const QByteArray liveManifestBytes = readFile(asset.manifestPath);
    const QByteArray liveReadmeBytes = readFile(
        QDir(asset.assetRoot).absoluteFilePath(QStringLiteral("README.md")));
    const QByteArray tamperedBytes = QByteArrayLiteral(
        "module top; localparam RESTORE_STAGING_TAMPER = 1; endmodule\n");
    const QByteArray foreignBytes = QByteArrayLiteral(
        "concurrent restore staging data\n");
    int hookCount = 0;
    bool mutationSucceeded = false;
    QString restoreStaging;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::RestoreStagingVerifiedBeforeUpdateAsset,
        [&](const WorkingCopyTestPoint point, const QString &path) {
            if (point
                != WorkingCopyTestPoint::RestoreStagingVerifiedBeforeUpdateAsset) {
                return;
            }
            ++hookCount;
            restoreStaging = path;
            mutationSucceeded = writeFile(
                QDir(path).absoluteFilePath(
                    addUnexpectedFile ? QStringLiteral("foreign.keep")
                                      : QStringLiteral("rtl/top.sv")),
                addUnexpectedFile ? foreignBytes : tamperedBytes);
        });

    UpdateAssetResult restored;
    error.clear();
    QVERIFY(!service.restoreVersion(asset,
                                    QStringLiteral("1.0.0"),
                                    WorkingCopyRecoveryMode::Permanent,
                                    &restored,
                                    &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!restoreStaging.isEmpty());
    QVERIFY(QFileInfo(restoreStaging).isDir());
    QVERIFY(restored.retainedPaths.contains(restoreStaging));
    QVERIFY(restored.warning.contains(restoreStaging));
    QVERIFY(error.contains(restoreStaging));
    QVERIFY(!restored.publishedAsIntended);
    if (addUnexpectedFile) {
        QCOMPARE(readFile(QDir(restoreStaging).absoluteFilePath(
                              QStringLiteral("foreign.keep"))),
                 foreignBytes);
    } else {
        QCOMPARE(readFile(QDir(restoreStaging).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 tamperedBytes);
    }
    QCOMPARE(readFile(liveTop), newerLiveBytes);
    QCOMPARE(readFile(asset.manifestPath), liveManifestBytes);
    QCOMPARE(readFile(QDir(asset.assetRoot).absoluteFilePath(
                          QStringLiteral("README.md"))),
             liveReadmeBytes);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-update-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-recovery-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
}

void CoreTest::unverifiedOperationStagingIsRetained_data()
{
    QTest::addColumn<QString>("operation");

    QTest::newRow("create-version") << QStringLiteral("version");
    QTest::newRow("copy-version") << QStringLiteral("copy");
    QTest::newRow("restore-version") << QStringLiteral("restore");
}

void CoreTest::unverifiedOperationStagingIsRetained()
{
    QFETCH(QString, operation);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("unverified_staging"),
                               .name = QStringLiteral("Unverified Staging"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));

    VersionInfo saved;
    if (operation != QStringLiteral("version")) {
        QVERIFY2(service.createVersion(asset,
                                       QStringLiteral("1.0.0"),
                                       &saved,
                                       &error),
                 qPrintable(error));
    }
    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    if (operation == QStringLiteral("restore")) {
        QVERIFY(writeFile(
            liveTop,
            QByteArrayLiteral(
                "module top; localparam LIVE_REV = 2; endmodule\n")));
    }
    const QByteArray liveTopBytes = readFile(liveTop);
    const QByteArray liveManifestBytes = readFile(asset.manifestPath);
    const QByteArray foreignBytes = QByteArrayLiteral(
        "foreign staging data must survive\n");

    WorkingCopyTestPoint point =
        WorkingCopyTestPoint::VersionStagingPreparedBeforeInitialVerification;
    if (operation == QStringLiteral("copy")) {
        point = WorkingCopyTestPoint::CopyStagingPreparedBeforeInitialVerification;
    } else if (operation == QStringLiteral("restore")) {
        point = WorkingCopyTestPoint::RestoreStagingPreparedBeforeInitialVerification;
    }
    int hookCount = 0;
    bool mutationSucceeded = false;
    QString stagingPath;
    service.setWorkingCopyTestHook(
        point,
        [&](const WorkingCopyTestPoint observed, const QString &path) {
            if (observed != point) {
                return;
            }
            ++hookCount;
            stagingPath = path;
            mutationSucceeded = writeFile(
                QDir(path).absoluteFilePath(QStringLiteral("foreign.keep")),
                foreignBytes);
        });

    const QString copyParent = temporary.filePath(QStringLiteral("copy-to"));
    QVERIFY(QDir().mkpath(copyParent));
    const QString copyTarget = QDir(copyParent).absoluteFilePath(
        QStringLiteral("unverified_staging"));
    UpdateAssetResult restored;
    error.clear();
    if (operation == QStringLiteral("version")) {
        VersionInfo created;
        QVERIFY(!service.createVersion(asset,
                                       QStringLiteral("1.0.0"),
                                       &created,
                                       &error));
        QVERIFY(created.path.isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(asset.assetRoot).absoluteFilePath(
            QStringLiteral(".xips/versions/1.0.0"))));
    } else if (operation == QStringLiteral("copy")) {
        QString copiedPath;
        QVERIFY(!service.copyVersionPayload(asset,
                                            QStringLiteral("1.0.0"),
                                            copyTarget,
                                            &copiedPath,
                                            &error));
        QVERIFY(copiedPath.isEmpty());
        QVERIFY(!QFileInfo::exists(copyTarget));
    } else {
        QVERIFY(!service.restoreVersion(asset,
                                        QStringLiteral("1.0.0"),
                                        WorkingCopyRecoveryMode::Permanent,
                                        &restored,
                                        &error));
        QVERIFY(restored.retainedPaths.contains(stagingPath));
        QVERIFY(restored.warning.contains(stagingPath));
    }

    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!stagingPath.isEmpty());
    QVERIFY(error.contains(stagingPath));
    QVERIFY(QFileInfo(stagingPath).isDir());
    QCOMPARE(readFile(QDir(stagingPath).absoluteFilePath(
                          QStringLiteral("foreign.keep"))),
             foreignBytes);
    QCOMPARE(readFile(liveTop), liveTopBytes);
    QCOMPARE(readFile(asset.manifestPath), liveManifestBytes);
    QVERIFY(!QFileInfo::exists(copyTarget));
}

void CoreTest::deleteVersionRejectsConcurrentBoundaryMutation_data()
{
    QTest::addColumn<QString>("boundary");

    QTest::newRow("verified-before-isolation")
        << QStringLiteral("before-isolation");
    QTest::newRow("isolated-before-marker-cas")
        << QStringLiteral("before-marker");
    QTest::newRow("marker-published-before-final-proof")
        << QStringLiteral("after-marker");
    QTest::newRow("verified-before-removal")
        << QStringLiteral("before-removal");
}

void CoreTest::deleteVersionRejectsConcurrentBoundaryMutation()
{
    QFETCH(QString, boundary);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = createSourceIp(temporary.path());
    QVERIFY(!source.isEmpty());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    AssetRecord asset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("delete_version_race"),
                               .name = QStringLiteral("Delete Version Race"),
                               .description = {},
                               .tags = {}}},
                 &asset,
                 &error),
             qPrintable(error));
    VersionInfo first;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   &first,
                                   &error),
             qPrintable(error));
    const QString liveTop = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    const QByteArray secondBytes = QByteArrayLiteral(
        "module top; localparam VERSION_REV = 2; endmodule\n");
    QVERIFY(writeFile(liveTop, secondBytes));
    VersionInfo second;
    QVERIFY2(service.createVersion(asset,
                                   QStringLiteral("1.1.0"),
                                   &second,
                                   &error),
             qPrintable(error));
    const QString secondSnapshotTop = QDir(second.path).absoluteFilePath(
        QStringLiteral("rtl/top.sv"));
    QCOMPARE(readFile(secondSnapshotTop), secondBytes);
    const QByteArray foreignBytes = QByteArrayLiteral(
        "concurrent delete boundary data\n");
    const QByteArray tamperedBytes = QByteArrayLiteral(
        "module top; localparam SNAPSHOT_TAMPER = 1; endmodule\n");
    const QString concurrentDescription = QStringLiteral(
        "metadata written concurrently after marker publish");
    const QString concurrentMarker = QStringLiteral("external-sync");

    WorkingCopyTestPoint point =
        WorkingCopyTestPoint::DeleteVersionVerifiedBeforeIsolation;
    if (boundary == QStringLiteral("before-marker")) {
        point = WorkingCopyTestPoint::DeleteVersionIsolatedBeforeMarkerCas;
    } else if (boundary == QStringLiteral("after-marker")) {
        point =
            WorkingCopyTestPoint::DeleteVersionMarkerPublishedBeforeFinalProof;
    } else if (boundary == QStringLiteral("before-removal")) {
        point = WorkingCopyTestPoint::DeleteVersionVerifiedBeforeRemoval;
    }
    int hookCount = 0;
    bool mutationSucceeded = false;
    service.setWorkingCopyTestHook(
        point,
        [&](const WorkingCopyTestPoint observed, const QString &path) {
            if (observed != point) {
                return;
            }
            ++hookCount;
            if (boundary == QStringLiteral("before-isolation")) {
                mutationSucceeded = writeFile(
                    QDir(path).absoluteFilePath(
                        QStringLiteral("rtl/top.sv")),
                    tamperedBytes);
                return;
            }
            if (boundary == QStringLiteral("before-marker")) {
                mutationSucceeded = writeFile(
                    QDir(path).absoluteFilePath(
                        QStringLiteral("foreign.keep")),
                    foreignBytes);
                return;
            }
            if (boundary == QStringLiteral("after-marker")) {
                const bool foreignWritten = writeFile(
                    QDir(path).absoluteFilePath(
                        QStringLiteral("foreign.keep")),
                    foreignBytes);
                const ManifestLoadResult loaded = ManifestService().load(
                    asset.manifestPath);
                if (!loaded.ok()) {
                    return;
                }
                Manifest changed = *loaded.manifest;
                changed.description = concurrentDescription;
                changed.version = concurrentMarker;
                QString writeError;
                mutationSucceeded = foreignWritten
                                    && ManifestService().write(
                                        asset.manifestPath,
                                        changed,
                                        &writeError);
                return;
            }
            const QString versionsRoot = QFileInfo(path).absolutePath();
            mutationSucceeded = writeFile(
                QDir(versionsRoot).absoluteFilePath(
                    QStringLiteral("1.1.0/blocker.keep")),
                foreignBytes);
        });

    DeleteVersionResult deleted;
    error.clear();
    QVERIFY(!service.deleteVersion(asset,
                                   QStringLiteral("1.1.0"),
                                   RemovalMode::Permanent,
                                   &deleted,
                                   &error));
    QCOMPARE(hookCount, 1);
    QVERIFY(mutationSucceeded);
    QVERIFY(!deleted.snapshotRemoved);
    QVERIFY(deleted.removedPath.isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(liveTop), secondBytes);
    QVERIFY(QFileInfo(first.path).isDir());

    const QString versionsRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    const QStringList isolatedNames = QDir(versionsRoot).entryList(
        {QStringLiteral(".staging-delete-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    const ManifestLoadResult liveManifest = ManifestService().load(
        asset.manifestPath);
    QVERIFY(liveManifest.ok());
    if (boundary == QStringLiteral("before-isolation")) {
        QVERIFY(QFileInfo(second.path).isDir());
        QCOMPARE(readFile(secondSnapshotTop), tamperedBytes);
        QVERIFY(isolatedNames.isEmpty());
        QVERIFY(deleted.retainedPaths.isEmpty());
        QVERIFY(!deleted.markerUpdated);
        QCOMPARE(liveManifest.manifest->version, QStringLiteral("1.1.0"));
        QVERIFY(error.contains(second.path));
    } else if (boundary == QStringLiteral("before-marker")) {
        QVERIFY(QFileInfo(second.path).isDir());
        QCOMPARE(readFile(QDir(second.path).absoluteFilePath(
                              QStringLiteral("foreign.keep"))),
                 foreignBytes);
        QVERIFY(isolatedNames.isEmpty());
        QVERIFY(deleted.retainedPaths.isEmpty());
        QVERIFY(!deleted.markerUpdated);
        QCOMPARE(liveManifest.manifest->version, QStringLiteral("1.1.0"));
        QVERIFY(error.contains(second.path));
    } else if (boundary == QStringLiteral("after-marker")) {
        QVERIFY(QFileInfo(second.path).isDir());
        QCOMPARE(readFile(QDir(second.path).absoluteFilePath(
                              QStringLiteral("foreign.keep"))),
                 foreignBytes);
        QVERIFY(isolatedNames.isEmpty());
        QVERIFY(deleted.markerUpdated);
        QCOMPARE(liveManifest.manifest->description,
                 concurrentDescription);
        QCOMPARE(liveManifest.manifest->version, concurrentMarker);
        QVERIFY(deleted.warning.contains(
            QStringLiteral("concurrent version-marker change")));
        QVERIFY(error.contains(second.path));
    } else {
        QCOMPARE(isolatedNames.size(), 1);
        const QString retainedSnapshot = QDir(versionsRoot).absoluteFilePath(
            isolatedNames.first());
        QVERIFY(deleted.retainedPaths.contains(retainedSnapshot));
        QVERIFY(deleted.warning.contains(retainedSnapshot));
        QVERIFY(error.contains(retainedSnapshot));
        QCOMPARE(readFile(QDir(retainedSnapshot).absoluteFilePath(
                              QStringLiteral("rtl/top.sv"))),
                 secondBytes);
        QCOMPARE(readFile(QDir(second.path).absoluteFilePath(
                              QStringLiteral("blocker.keep"))),
                 foreignBytes);
        QVERIFY(deleted.markerUpdated);
        QCOMPARE(liveManifest.manifest->version, QStringLiteral("1.0.0"));
    }
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
    QVERIFY(QDir().mkpath(library));
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

void CoreTest::groupChangesMergeConcurrencyAndKeepPartialSuccess()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstSource = temporary.filePath(QStringLiteral("first.sv"));
    const QString secondSource = temporary.filePath(QStringLiteral("second.sv"));
    QVERIFY(writeFile(firstSource,
                      QByteArrayLiteral("module first; endmodule\n")));
    QVERIFY(writeFile(secondSource,
                      QByteArrayLiteral("module second; endmodule\n")));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    AssetLibraryService service;
    const ImportBatchResult imported = service.importAssets(
        library,
        {firstSource, secondSource});
    QCOMPARE(imported.created.size(), 2);
    const AssetRecord first = imported.created.at(0);
    const AssetRecord second = imported.created.at(1);

    bool orthogonalMutationSucceeded = false;
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::GroupMembershipMergedBeforeManifestCas,
        [&](const WorkingCopyTestPoint point, const QString &manifestPath) {
            if (point
                != WorkingCopyTestPoint::GroupMembershipMergedBeforeManifestCas) {
                return;
            }
            const ManifestLoadResult concurrent = ManifestService().load(
                manifestPath);
            if (!concurrent.ok()) {
                return;
            }
            Manifest changed = *concurrent.manifest;
            changed.tags.append(QStringLiteral("REMOTE"));
            changed.version = QStringLiteral("9.0.0");
            changed.rawObject.insert(QStringLiteral("groupCustom"), true);
            QString writeError;
            orthogonalMutationSucceeded = ManifestService().write(
                manifestPath,
                changed,
                &writeError);
        });
    GroupChangeResult merged;
    QString error;
    QVERIFY2(service.changeGroupMembership({first},
                                           QString(),
                                           QStringLiteral("LOCAL"),
                                           &merged,
                                           &error),
             qPrintable(error));
    QVERIFY(orthogonalMutationSucceeded);
    QVERIFY(merged.complete());
    QCOMPARE(merged.updated, 1);
    QCOMPARE(merged.failed, 0);
    QCOMPARE(merged.items.size(), 1);
    QCOMPARE(merged.items.first().outcome, GroupChangeOutcome::Updated);
    ManifestLoadResult loaded = ManifestService().load(first.manifestPath);
    QVERIFY(loaded.ok());
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("LOCAL")));
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("REMOTE")));
    QCOMPARE(loaded.manifest->version, QStringLiteral("9.0.0"));
    QCOMPARE(loaded.manifest->rawObject.value(QStringLiteral("groupCustom"))
                 .toBool(),
             true);

    const QByteArray malformedManifest = QByteArrayLiteral("{broken sync");
    QVERIFY(writeFile(second.manifestPath, malformedManifest));
    GroupChangeResult partial;
    error.clear();
    QVERIFY(!service.changeGroupMembership({first, second},
                                           QString(),
                                           QStringLiteral("BATCH"),
                                           &partial,
                                           &error));
    QCOMPARE(partial.updated, 1);
    QCOMPARE(partial.failed, 1);
    QCOMPARE(partial.conflicts, 0);
    QCOMPARE(partial.items.size(), 2);
    QCOMPARE(partial.items.at(0).outcome, GroupChangeOutcome::Updated);
    QCOMPARE(partial.items.at(1).outcome, GroupChangeOutcome::Failed);
    QVERIFY(error.contains(QStringLiteral("partially applied")));

    loaded = ManifestService().load(first.manifestPath);
    QVERIFY(loaded.ok());
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("LOCAL")));
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("REMOTE")));
    QVERIFY(loaded.manifest->tags.contains(QStringLiteral("BATCH")));
    QCOMPARE(loaded.manifest->version, QStringLiteral("9.0.0"));
    QCOMPARE(loaded.manifest->rawObject.value(QStringLiteral("groupCustom"))
                 .toBool(),
             true);
    QCOMPARE(readFile(second.manifestPath), malformedManifest);
    QVERIFY(QDir(library)
                .entryList({QStringLiteral(".xips-create-group-membership-*")},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
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
