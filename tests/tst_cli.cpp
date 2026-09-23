#include "library/AssetLibraryService.h"
#include "library/SnapshotLibrary.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace xips;

namespace
{

struct CliResult
{
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

CliResult runCli(const QStringList &arguments)
{
    QProcess process;
    process.start(QString::fromUtf8(XIPS_CLI_PATH), arguments);
    if (!process.waitForFinished(15000))
    {
        process.kill();
        process.waitForFinished();
    }
    return {
        .exitStatus = process.exitStatus(),
        .exitCode = process.exitCode(),
        .standardOutput = process.readAllStandardOutput(),
        .standardError = process.readAllStandardError(),
    };
}

QJsonObject envelope(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

bool createAsset(const QString &root, const QString &id, const QString &name,
                 const QString &fileName)
{
    if (!writeFile(QDir(root).absoluteFilePath(fileName),
                   QByteArrayLiteral("module test; endmodule\n")))
    {
        return false;
    }
    Manifest manifest;
    manifest.id = id;
    manifest.name = name;
    QString error;
    return ManifestService().write(QDir(root).absoluteFilePath(QStringLiteral(".xips.json")),
                                   manifest, &error);
}

} // namespace

class CliTest final : public QObject
{
    Q_OBJECT

  private slots:
    void scanProblemsDoNotBlockHealthyAssets();
    void caseInsensitiveResolveRejectsAmbiguity();
    void savedVersionRequiresExplicitMaterializationForPaths();
    void snapshotsResolveAndExportPinnedContent();
};

void CliTest::snapshotsResolveAndExportPinnedContent()
{
    QTemporaryDir temporary;
    const auto library = temporary.filePath("library");
    const auto source = temporary.filePath("uart.sv");
    QVERIFY(QDir().mkpath(library));
    QVERIFY(writeFile(source, "version one"));
    const auto first = SnapshotLibrary::collect(library, {source}, "UART", "module");
    QVERIFY2(first.ok, qPrintable(first.error));
    QVERIFY(writeFile(source, "version two"));
    const auto second = SnapshotLibrary::update(first.asset, {source});
    QVERIFY2(second.ok, qPrintable(second.error));
    const auto listed = runCli({"--action", "list", "--library", library});
    QCOMPARE(listed.exitCode, 0);
    const auto assets =
        envelope(listed.standardOutput).value("data").toObject().value("assets").toArray();
    QCOMPARE(assets.size(), 1);
    QCOMPARE(assets.first().toObject().value("category").toString(), QString("module"));
    const QStringList resolve{"--action", "resolve", "--library",
                              library,    "--asset", first.asset.id};
    const auto latest = runCli(resolve);
    QCOMPARE(latest.exitCode, 0);
    const auto data = envelope(latest.standardOutput).value("data").toObject();
    QCOMPARE(data.value("resolvedVersion").toString(), QString("2"));
    QCOMPARE(data.value("access").toString(), QString("metadata-only"));
    QVERIFY(!data.contains("resolvedPath"));
    QVERIFY(!data.contains("path"));
    const auto target = temporary.filePath("project.sv");
    const auto exported =
        runCli(resolve + QStringList{"--asset-version", "1", "--destination", target});
    QCOMPARE(exported.exitCode, 0);
    QCOMPARE(readFile(target), QByteArray("version one"));
    const auto missing = runCli(resolve + QStringList{"--asset-version", "99"});
    QCOMPARE(missing.exitCode, 4);
    QVERIFY(writeFile(second.asset.root + "/.xips/revisions/2/uart.sv", "corrupt"));
    const auto corruptTarget = temporary.filePath("corrupt.sv");
    const auto rejected = runCli(resolve + QStringList{"--destination", corruptTarget});
    QCOMPARE(rejected.exitCode, 3);
    QVERIFY(!QFileInfo::exists(corruptTarget));
}

void CliTest::scanProblemsDoNotBlockHealthyAssets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString healthyRoot = QDir(library).absoluteFilePath(QStringLiteral("healthy"));
    QVERIFY(createAsset(healthyRoot, QStringLiteral("healthy_ip"), QStringLiteral("Healthy IP"),
                        QStringLiteral("rtl/healthy.sv")));
    QVERIFY(writeFile(QDir(library).absoluteFilePath(QStringLiteral("broken/.xips.json")),
                      QByteArrayLiteral("{broken")));

    const CliResult listed = runCli(
        {QStringLiteral("--action"), QStringLiteral("list"), QStringLiteral("--library"), library});
    QCOMPARE(listed.exitStatus, QProcess::NormalExit);
    QCOMPARE(listed.exitCode, 0);
    const QJsonObject listEnvelope = envelope(listed.standardOutput);
    QVERIFY(listEnvelope.value(QStringLiteral("ok")).toBool());
    const QJsonObject listData = listEnvelope.value(QStringLiteral("data")).toObject();
    QCOMPARE(listData.value(QStringLiteral("count")).toInt(), 1);
    QVERIFY(listData.value(QStringLiteral("problemCount")).toInt() >= 1);
    QVERIFY(!listData.value(QStringLiteral("problems")).toArray().isEmpty());

    const CliResult resolved =
        runCli({QStringLiteral("--action"), QStringLiteral("resolve"), QStringLiteral("--library"),
                library, QStringLiteral("--asset"), QStringLiteral("HEALTHY_IP")});
    QCOMPARE(resolved.exitStatus, QProcess::NormalExit);
    QCOMPARE(resolved.exitCode, 0);
    const QJsonObject resolveData =
        envelope(resolved.standardOutput).value(QStringLiteral("data")).toObject();
    QCOMPARE(resolveData.value(QStringLiteral("id")).toString(), QStringLiteral("healthy_ip"));
    QCOMPARE(resolveData.value(QStringLiteral("access")).toString(),
             QStringLiteral("working-copy"));
    QCOMPARE(resolveData.value(QStringLiteral("problemCount")).toInt(),
             listData.value(QStringLiteral("problemCount")).toInt());
    QCOMPARE(QDir::cleanPath(resolveData.value(QStringLiteral("resolvedFile")).toString()),
             QDir::cleanPath(QDir(healthyRoot).absoluteFilePath(QStringLiteral("rtl/healthy.sv"))));
}

void CliTest::caseInsensitiveResolveRejectsAmbiguity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(createAsset(QDir(library).absoluteFilePath(QStringLiteral("first")),
                        QStringLiteral("Shared_ID"), QStringLiteral("First"),
                        QStringLiteral("first.sv")));
    QVERIFY(createAsset(QDir(library).absoluteFilePath(QStringLiteral("second")),
                        QStringLiteral("shared_id"), QStringLiteral("Second"),
                        QStringLiteral("second.sv")));

    const CliResult listed = runCli(
        {QStringLiteral("--action"), QStringLiteral("list"), QStringLiteral("--library"), library});
    QCOMPARE(listed.exitStatus, QProcess::NormalExit);
    QCOMPARE(listed.exitCode, 0);
    const QJsonObject listData =
        envelope(listed.standardOutput).value(QStringLiteral("data")).toObject();
    QCOMPARE(listData.value(QStringLiteral("count")).toInt(), 0);
    QVERIFY(listData.value(QStringLiteral("problemCount")).toInt() >= 1);

    const CliResult resolved =
        runCli({QStringLiteral("--action"), QStringLiteral("resolve"), QStringLiteral("--library"),
                library, QStringLiteral("--asset"), QStringLiteral("SHARED_ID")});
    QCOMPARE(resolved.exitStatus, QProcess::NormalExit);
    QCOMPARE(resolved.exitCode, 3);
    const QJsonObject failure = envelope(resolved.standardError);
    QCOMPARE(failure.value(QStringLiteral("ok")).toBool(), false);
    QVERIFY(failure.value(QStringLiteral("error"))
                .toString()
                .contains(QStringLiteral("ambiguous"), Qt::CaseInsensitive));
    QVERIFY(!failure.value(QStringLiteral("problems")).toArray().isEmpty());
}

void CliTest::savedVersionRequiresExplicitMaterializationForPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source/top.sv"));
    const QByteArray sourceContents = QByteArrayLiteral("module saved_version; endmodule\n");
    QVERIFY(writeFile(source, sourceContents));
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));

    AssetLibraryService service;
    AssetMetadata metadata = service.suggestedMetadata(source);
    metadata.id = QStringLiteral("saved_ip");
    AssetRecord asset;
    QString error;
    QVERIFY2(
        service.importAsset({.libraryRoot = library, .sourcePath = source, .metadata = metadata},
                            &asset, &error),
        qPrintable(error));
    VersionInfo version;
    QVERIFY2(service.createVersion(asset, QStringLiteral("1.0.0"), &version, &error),
             qPrintable(error));

    const QStringList savedArguments{
        QStringLiteral("--action"),        QStringLiteral("resolve"),
        QStringLiteral("--library"),       library,
        QStringLiteral("--asset"),         QStringLiteral("SAVED_IP"),
        QStringLiteral("--asset-version"), QStringLiteral("1.0.0"),
    };
    const CliResult metadataOnly = runCli(savedArguments);
    QCOMPARE(metadataOnly.exitStatus, QProcess::NormalExit);
    QCOMPARE(metadataOnly.exitCode, 0);
    const QJsonObject metadataData =
        envelope(metadataOnly.standardOutput).value(QStringLiteral("data")).toObject();
    QCOMPARE(metadataData.value(QStringLiteral("access")).toString(),
             QStringLiteral("metadata-only"));
    QCOMPARE(metadataData.value(QStringLiteral("immutable")).toBool(), true);
    QCOMPARE(metadataData.value(QStringLiteral("editable")).toBool(), false);
    QVERIFY(!metadataData.contains(QStringLiteral("path")));
    QVERIFY(!metadataData.contains(QStringLiteral("resolvedPath")));
    QVERIFY(!metadataData.contains(QStringLiteral("resolvedFiles")));
    QVERIFY(!metadataData.contains(QStringLiteral("resolvedFile")));
    QCOMPARE(metadataData.value(QStringLiteral("resolvedRelativeFiles")).toArray().size(), 1);

    const QString exportParent = temporary.filePath(QStringLiteral("exports"));
    QVERIFY(QDir().mkpath(exportParent));
    const QString destination = QDir(exportParent).absoluteFilePath(QStringLiteral("saved_top.sv"));
    QStringList materializeArguments = savedArguments;
    materializeArguments.append({QStringLiteral("--destination"), destination});
    const CliResult materialized = runCli(materializeArguments);
    QCOMPARE(materialized.exitStatus, QProcess::NormalExit);
    QCOMPARE(materialized.exitCode, 0);
    const QJsonObject materializedData =
        envelope(materialized.standardOutput).value(QStringLiteral("data")).toObject();
    QCOMPARE(materializedData.value(QStringLiteral("access")).toString(),
             QStringLiteral("materialized-copy"));
    QCOMPARE(materializedData.value(QStringLiteral("sourceImmutable")).toBool(), true);
    QCOMPARE(materializedData.value(QStringLiteral("editable")).toBool(), true);
    QCOMPARE(QDir::cleanPath(materializedData.value(QStringLiteral("resolvedFile")).toString()),
             QDir::cleanPath(destination));
    QCOMPARE(readFile(destination), sourceContents);
    QVERIFY(!materializedData.value(QStringLiteral("resolvedFile"))
                 .toString()
                 .contains(QStringLiteral(".xips/versions"), Qt::CaseInsensitive));

    const QString unsafeDestination = QDir(library).absoluteFilePath(QStringLiteral("exported.sv"));
    QStringList unsafeArguments = savedArguments;
    unsafeArguments.append({QStringLiteral("--destination"), unsafeDestination});
    const CliResult rejected = runCli(unsafeArguments);
    QCOMPARE(rejected.exitStatus, QProcess::NormalExit);
    QCOMPARE(rejected.exitCode, 3);
    QVERIFY(!QFileInfo::exists(unsafeDestination));
}

QTEST_APPLESS_MAIN(CliTest)

#include "tst_cli.moc"
