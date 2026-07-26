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
    void scannerListsPayloadAndHashesOnDemand();
    void metadataKeepsStableId();
    void versionsAreImmutableAndExportable();
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
    const AssetMetadata metadata = service.suggestedMetadata(source);
    QCOMPARE(metadata.name, QStringLiteral("uart_rx.sv"));
    QCOMPARE(metadata.id, QStringLiteral("uart_rx_sv"));

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

void CoreTest::versionsAreImmutableAndExportable()
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

    const QString exportRoot = temporary.filePath(QStringLiteral("exported-1.0.0"));
    QVERIFY2(service.exportVersion(asset,
                                   QStringLiteral("1.0.0"),
                                   exportRoot,
                                   &error),
             qPrintable(error));
    QFile exported(QDir(exportRoot).absoluteFilePath(QStringLiteral("rtl/top.sv")));
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), QByteArrayLiteral("module top; endmodule\n"));
    QVERIFY(QFileInfo::exists(QDir(exportRoot).absoluteFilePath(
        QStringLiteral(".xips.json"))));
    QVERIFY(!QFileInfo::exists(QDir(exportRoot).absoluteFilePath(
        QStringLiteral(".snapshot.json"))));
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
