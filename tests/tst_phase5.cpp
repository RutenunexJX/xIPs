#include "assetindex/AssetScanner.h"
#include "integration/IntegrationService.h"
#include "managed/ManagedAssetService.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

#include <algorithm>

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

AssetRecord createModule(const QString &libraryRoot, const QString &id)
{
    const QString root = QDir(libraryRoot).absoluteFilePath(id);
    if (!QDir().mkpath(root)) {
        qFatal("Cannot create module root");
    }
    Manifest manifest;
    manifest.id = id;
    manifest.type = AssetType::Module;
    manifest.name = id;
    manifest.version = QStringLiteral("1.0.0");
    manifest.top = id;
    manifest.sources = {QStringLiteral("rtl/%1.sv").arg(id)};
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), id},
    };
    const QString manifestPath =
        QDir(root).absoluteFilePath(QStringLiteral(".xips.json"));
    QString error;
    if (!ManifestService().write(manifestPath, manifest, &error)
        || !writeFile(QDir(root).absoluteFilePath(manifest.sources.first()),
                      QStringLiteral("module %1; endmodule\n").arg(id).toUtf8())) {
        qFatal("Cannot create module fixture: %s", qPrintable(error));
    }
    AssetRecord record;
    record.manifest = manifest;
    record.assetRoot = root;
    record.manifestPath = manifestPath;
    record.contentHash = AssetScanner::contentHash(manifest, root);
    return record;
}

AssetRecord createCodeBlock(const QString &libraryRoot)
{
    const QString root =
        QDir(libraryRoot).absoluteFilePath(QStringLiteral("block"));
    if (!QDir().mkpath(root)) {
        qFatal("Cannot create Code Block root");
    }
    Manifest manifest;
    manifest.id = QStringLiteral("block");
    manifest.type = AssetType::CodeBlock;
    manifest.name = QStringLiteral("Block");
    manifest.version = QStringLiteral("1.0.0");
    manifest.templateText = QStringLiteral("assign ${output} = ${input};");
    manifest.scope = {QStringLiteral("module")};
    manifest.requiredSymbols = {QStringLiteral("logic")};
    manifest.allowWorkspaceOverride = true;
    manifest.exampleInput = QJsonObject{
        {QStringLiteral("output"), QStringLiteral("result_o")},
        {QStringLiteral("input"), QStringLiteral("value_i")},
    };
    manifest.exampleOutput = QStringLiteral("assign result_o = value_i;");
    manifest.slotDefinitions = {
        SlotDefinition{.name = QStringLiteral("output"), .type = QStringLiteral("signal")},
        SlotDefinition{.name = QStringLiteral("input"), .type = QStringLiteral("signal")},
    };
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), manifest.id},
        {QStringLiteral("type"), QStringLiteral("code-block")},
        {QStringLiteral("name"), manifest.name},
    };
    const QString manifestPath =
        QDir(root).absoluteFilePath(QStringLiteral(".xips.json"));
    QString error;
    if (!ManifestService().write(manifestPath, manifest, &error)) {
        qFatal("Cannot create Code Block fixture: %s", qPrintable(error));
    }
    AssetRecord record;
    record.manifest = manifest;
    record.assetRoot = root;
    record.manifestPath = manifestPath;
    record.contentHash = AssetScanner::contentHash(manifest, root);
    return record;
}

struct CliResult {
    int exitCode = -1;
    QJsonObject response;
    QString output;
    QString error;
};

CliResult runCli(const QStringList &arguments)
{
    CliResult result;
    QProcess process;
    process.start(QString::fromUtf8(XIPS_CLI_PATH), arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        result.error = process.errorString();
        return result;
    }
    result.exitCode = process.exitCode();
    const QByteArray standardOutput = process.readAllStandardOutput();
    result.output = QString::fromUtf8(standardOutput);
    const QJsonDocument document =
        QJsonDocument::fromJson(standardOutput);
    if (!document.isObject()) {
        result.error = QString::fromUtf8(process.readAllStandardError());
        return result;
    }
    result.response = document.object();
    return result;
}

} // namespace

class Phase5Test final : public QObject {
    Q_OBJECT

private slots:
    void openUriUsesVersionedExplicitFields();
    void codeBlockPayloadPreservesSlotOrderAndWritesAtomically();
    void moduleRegistrationPreviewsAndNeverCopiesSource();
    void moduleRegistrationRejectsForgedAndStalePlans();
    void cliCatalogAndOpenUriReturnStableJson();
    void cliImportRequiresExecuteFlag();
    void cliRegistersModuleAndHandsOffCodeBlock();
    void managedCreationIsPreviewedAndPublishedAtomically();
    void managedCreationFailureLeavesNoPartialDirectory();
    void managedCreationRejectsForgedAndStalePlans();
    void cliCreatesManagedAssetOnlyWithExecute();
    void creationAndRegistrationRejectDuplicateCatalogIds();
};

void Phase5Test::openUriUsesVersionedExplicitFields()
{
    QTemporaryDir temporary;
    AssetRecord asset =
        createModule(temporary.path(), QStringLiteral("open_me"));
    const QUrl uri = IntegrationService::zeroSlackOpenUri(asset);
    QCOMPARE(uri.scheme(), QStringLiteral("zeroslack"));
    QCOMPARE(uri.host(), QStringLiteral("open"));
    const QUrlQuery query(uri);
    QCOMPARE(query.queryItemValue(QStringLiteral("protocolVersion")),
             QStringLiteral("1"));
    QCOMPARE(query.queryItemValue(QStringLiteral("assetId")),
             QStringLiteral("open_me"));
    QCOMPARE(query.queryItemValue(QStringLiteral("top")),
             QStringLiteral("open_me"));
    QCOMPARE(query.queryItemValue(QStringLiteral("contentHash")),
             asset.contentHash);
    QVERIFY(QFileInfo(query.queryItemValue(QStringLiteral("source"))).isAbsolute());
}

void Phase5Test::codeBlockPayloadPreservesSlotOrderAndWritesAtomically()
{
    QTemporaryDir temporary;
    const AssetRecord block = createCodeBlock(temporary.path());
    const ManifestLoadResult loaded =
        ManifestService().load(block.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->exampleInput, block.manifest.exampleInput);
    QCOMPARE(loaded.manifest->exampleOutput, block.manifest.exampleOutput);
    const QJsonObject payload = IntegrationService::codeBlockPayload(block);
    QCOMPARE(payload.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(payload.value(QStringLiteral("operation")).toString(),
             QStringLiteral("insert-code-block"));
    const QJsonArray slotArray = payload.value(QStringLiteral("slots")).toArray();
    QCOMPARE(slotArray.size(), 2);
    QCOMPARE(slotArray.at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("output"));
    QCOMPARE(slotArray.at(1).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("input"));
    QCOMPARE(payload.value(QStringLiteral("exampleInput")),
             block.manifest.exampleInput);
    QCOMPARE(payload.value(QStringLiteral("exampleOutput")),
             block.manifest.exampleOutput);

    const QString handoff = temporary.filePath(QStringLiteral("handoff/block.json"));
    QString error;
    QVERIFY2(IntegrationService::writeCodeBlockHandoff(block, handoff, &error),
             qPrintable(error));
    QCOMPARE(QJsonDocument::fromJson(readFile(handoff)).object(), payload);
    const QUrl uri = IntegrationService::zeroSlackCodeBlockUri(block, handoff);
    QCOMPARE(uri.scheme(), QStringLiteral("zeroslack"));
    QCOMPARE(uri.host(), QStringLiteral("insert-code-block"));
    QCOMPARE(QUrlQuery(uri).queryItemValue(QStringLiteral("handoff")),
             QFileInfo(handoff).absoluteFilePath());
}

void Phase5Test::moduleRegistrationPreviewsAndNeverCopiesSource()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("asset"));
    QVERIFY(QDir().mkpath(QDir(root).absoluteFilePath(QStringLiteral("rtl"))));
    const QString source =
        QDir(root).absoluteFilePath(QStringLiteral("rtl/current.sv"));
    const QString packageSource =
        QDir(root).absoluteFilePath(QStringLiteral("rtl/current_pkg.sv"));
    const QByteArray original = QByteArrayLiteral("module current; endmodule\n");
    QVERIFY(writeFile(source, original));
    QVERIFY(writeFile(packageSource,
                      QByteArrayLiteral("package current_pkg; endpackage\n")));

    IntegrationService service;
    const ModuleRegistrationPlan plan = service.planModuleRegistration(
        ModuleRegistrationRequest{
            .assetRoot = root,
            .sourcePaths = {source, packageSource},
            .id = QStringLiteral("current"),
            .name = QStringLiteral("Current Module"),
            .top = QStringLiteral("current"),
            .version = QStringLiteral("1.0.0"),
            .defines = {QStringLiteral("CURRENT=1")},
        });
    QVERIFY(plan.canExecute());
    QCOMPARE(plan.manifest.sources,
             QStringList({QStringLiteral("rtl/current.sv"),
                          QStringLiteral("rtl/current_pkg.sv")}));
    QVERIFY(!QFileInfo::exists(plan.manifestPath));
    QString error;
    QVERIFY(!service.executeModuleRegistration(plan, false, &error));
    QVERIFY(!QFileInfo::exists(plan.manifestPath));
    QVERIFY2(service.executeModuleRegistration(plan, true, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(plan.manifestPath));
    QCOMPARE(readFile(source), original);
    QCOMPARE(readFile(packageSource),
             QByteArrayLiteral("package current_pkg; endpackage\n"));
    QVERIFY(!service.executeModuleRegistration(plan, true, &error));
}

void Phase5Test::moduleRegistrationRejectsForgedAndStalePlans()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("asset"));
    QVERIFY(QDir().mkpath(root));
    const QString source =
        QDir(root).absoluteFilePath(QStringLiteral("guarded.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module guarded; endmodule\n")));

    IntegrationService service;
    const ModuleRegistrationPlan plan = service.planModuleRegistration(
        ModuleRegistrationRequest{
            .assetRoot = root,
            .sourcePath = source,
            .id = QStringLiteral("guarded"),
            .name = QStringLiteral("Guarded"),
            .top = QStringLiteral("guarded"),
        });
    QVERIFY(plan.canExecute());

    ModuleRegistrationPlan escaped = plan;
    escaped.manifestPath =
        temporary.filePath(QStringLiteral("outside.json"));
    QString error;
    QVERIFY(!service.executeModuleRegistration(escaped, true, &error));
    QVERIFY(error.contains(QStringLiteral("Unsafe or invalid")));
    QVERIFY(!QFileInfo::exists(escaped.manifestPath));
    QVERIFY(!QFileInfo::exists(plan.manifestPath));

    QVERIFY(QFile::remove(source));
    error.clear();
    QVERIFY(!service.executeModuleRegistration(plan, true, &error));
    QVERIFY(error.contains(QStringLiteral("changed after planning")));
    QVERIFY(!QFileInfo::exists(plan.manifestPath));
}

void Phase5Test::cliCatalogAndOpenUriReturnStableJson()
{
    QProcess guiVersion;
    QProcessEnvironment guiEnvironment =
        QProcessEnvironment::systemEnvironment();
    guiEnvironment.insert(
        QStringLiteral("PATH"),
        QStringLiteral(XIPS_QT_BIN ";"
                       XIPS_COMPILER_BIN ";")
            + guiEnvironment.value(QStringLiteral("PATH")));
    guiEnvironment.insert(QStringLiteral("QT_PLUGIN_PATH"),
                          QStringLiteral(XIPS_QT_PLUGIN_PATH));
    guiEnvironment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                          QStringLiteral("offscreen"));
    guiVersion.setProcessEnvironment(guiEnvironment);
    guiVersion.start(QString::fromUtf8(XIPS_GUI_PATH),
                     {QStringLiteral("--version")});
    QVERIFY(guiVersion.waitForStarted(5000));
    QVERIFY(guiVersion.waitForFinished(5000));
    QCOMPARE(guiVersion.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(guiVersion.readAllStandardOutput()).trimmed(),
             QStringLiteral("xIPs 0.1.0"));

    const CliResult version = runCli({QStringLiteral("--version")});
    QVERIFY2(version.error.isEmpty(), qPrintable(version.error));
    QCOMPARE(version.exitCode, 0);
    QCOMPARE(version.output.trimmed(), QStringLiteral("xips-cli 0.1.0"));
    const CliResult help = runCli({QStringLiteral("--help")});
    QVERIFY2(help.error.isEmpty(), qPrintable(help.error));
    QCOMPARE(help.exitCode, 0);
    QVERIFY(help.output.contains(
        QStringLiteral("Stable JSON interface for xIPs")));

    QTemporaryDir temporary;
    createModule(temporary.path(), QStringLiteral("cli_asset"));
    const CliResult catalog = runCli({
        QStringLiteral("catalog"),
        QStringLiteral("--library"),
        temporary.path(),
    });
    QVERIFY2(catalog.error.isEmpty(), qPrintable(catalog.error));
    QCOMPARE(catalog.exitCode, 0);
    QCOMPARE(catalog.response.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(catalog.response.value(QStringLiteral("command")).toString(),
             QStringLiteral("catalog"));
    QVERIFY(catalog.response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(catalog.response.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("assets"))
                 .toArray()
                 .size(),
             1);

    const CliResult uri = runCli({
        QStringLiteral("open-uri"),
        QStringLiteral("--library"),
        temporary.path(),
        QStringLiteral("--asset"),
        QStringLiteral("cli_asset"),
    });
    QCOMPARE(uri.exitCode, 0);
    const QUrl parsed(uri.response.value(QStringLiteral("data"))
                          .toObject()
                          .value(QStringLiteral("uri"))
                          .toString());
    QCOMPARE(parsed.scheme(), QStringLiteral("zeroslack"));
    QCOMPARE(parsed.host(), QStringLiteral("open"));
}

void Phase5Test::cliImportRequiresExecuteFlag()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString vendorTarget = temporary.filePath(QStringLiteral("vendor-project"));
    const QString referenceTarget =
        temporary.filePath(QStringLiteral("reference-project"));
    QVERIFY(QDir().mkpath(vendorTarget));
    QVERIFY(QDir().mkpath(referenceTarget));
    AssetRecord imported =
        createModule(library, QStringLiteral("imported"));
    imported.manifest.tools.insert(QStringLiteral("vivado"),
                                   QStringLiteral(">=2022.2"));
    QString manifestError;
    QVERIFY2(ManifestService().write(imported.manifestPath,
                                     imported.manifest,
                                     &manifestError),
             qPrintable(manifestError));

    const QStringList vendorArguments{
        QStringLiteral("import"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--asset"),
        QStringLiteral("imported"),
        QStringLiteral("--target"),
        vendorTarget,
        QStringLiteral("--mode"),
        QStringLiteral("vendor"),
    };
    const CliResult preview = runCli(vendorArguments);
    QCOMPARE(preview.exitCode, 0);
    QVERIFY(preview.response.value(QStringLiteral("ok")).toBool());
    const QJsonArray importIssues =
        preview.response.value(QStringLiteral("data"))
            .toObject()
            .value(QStringLiteral("plan"))
            .toObject()
            .value(QStringLiteral("importIssues"))
            .toArray();
    QVERIFY(std::any_of(
        importIssues.cbegin(), importIssues.cend(), [](const QJsonValue &value) {
            return value.toObject()
                .value(QStringLiteral("message"))
                .toString()
                .contains(QStringLiteral("compatibility is unverified"));
        }));
    QVERIFY(!QFileInfo::exists(
        QDir(vendorTarget).absoluteFilePath(QStringLiteral("xips-lock.json"))));

    QStringList incompatibleArguments = vendorArguments;
    incompatibleArguments.append(
        {QStringLiteral("--target-tool"), QStringLiteral("vivado=2020.1")});
    const CliResult incompatible = runCli(incompatibleArguments);
    QCOMPARE(incompatible.exitCode, 3);
    QVERIFY(!incompatible.response.value(QStringLiteral("ok")).toBool());

    QStringList executeArguments = vendorArguments;
    executeArguments.append(
        {QStringLiteral("--target-tool"), QStringLiteral("vivado=2022.2")});
    executeArguments.append(QStringLiteral("--execute"));
    const CliResult executed = runCli(executeArguments);
    QCOMPARE(executed.exitCode, 0);
    QVERIFY(QFileInfo::exists(
        QDir(vendorTarget).absoluteFilePath(QStringLiteral("xips-lock.json"))));

    const CliResult reference = runCli({
        QStringLiteral("import"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--asset"),
        QStringLiteral("imported"),
        QStringLiteral("--target"),
        referenceTarget,
        QStringLiteral("--mode"),
        QStringLiteral("reference"),
        QStringLiteral("--execute"),
    });
    QCOMPARE(reference.exitCode, 0);
    QVERIFY(QFileInfo::exists(QDir(referenceTarget).absoluteFilePath(
        QStringLiteral(".xips/references.json"))));
}

void Phase5Test::cliRegistersModuleAndHandsOffCodeBlock()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString moduleRoot =
        QDir(library).absoluteFilePath(QStringLiteral("registered"));
    QVERIFY(QDir().mkpath(moduleRoot));
    const QString source =
        QDir(moduleRoot).absoluteFilePath(QStringLiteral("registered.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module registered; endmodule\n")));

    const QStringList registration{
        QStringLiteral("register-module"),
        QStringLiteral("--asset-root"),
        moduleRoot,
        QStringLiteral("--source"),
        source,
        QStringLiteral("--id"),
        QStringLiteral("registered"),
        QStringLiteral("--name"),
        QStringLiteral("Registered"),
        QStringLiteral("--top"),
        QStringLiteral("registered"),
    };
    const CliResult preview = runCli(registration);
    QCOMPARE(preview.exitCode, 0);
    QVERIFY(!QFileInfo::exists(
        QDir(moduleRoot).absoluteFilePath(QStringLiteral(".xips.json"))));
    QStringList execute = registration;
    execute.append(QStringLiteral("--execute"));
    const CliResult registered = runCli(execute);
    QCOMPARE(registered.exitCode, 0);
    QVERIFY(QFileInfo::exists(
        QDir(moduleRoot).absoluteFilePath(QStringLiteral(".xips.json"))));

    createCodeBlock(library);
    const QString handoff = temporary.filePath(QStringLiteral("handoff.json"));
    const QStringList handoffArguments{
        QStringLiteral("code-block"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--asset"),
        QStringLiteral("block"),
        QStringLiteral("--handoff"),
        handoff,
    };
    const CliResult handoffPreview = runCli(handoffArguments);
    QCOMPARE(handoffPreview.exitCode, 0);
    QVERIFY(!QFileInfo::exists(handoff));
    QStringList handoffExecute = handoffArguments;
    handoffExecute.append(QStringLiteral("--execute"));
    const CliResult handedOff = runCli(handoffExecute);
    QCOMPARE(handedOff.exitCode, 0);
    QVERIFY(QFileInfo::exists(handoff));
    QCOMPARE(QJsonDocument::fromJson(readFile(handoff))
                 .object()
                 .value(QStringLiteral("operation"))
                 .toString(),
             QStringLiteral("insert-code-block"));
}

void Phase5Test::managedCreationIsPreviewedAndPublishedAtomically()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    ManagedAssetService service;
    const ManagedAssetPlan plan = service.plan(ManagedAssetRequest{
        .libraryRoot = library,
        .id = QStringLiteral("generated"),
        .name = QStringLiteral("Generated"),
        .top = QStringLiteral("generated"),
        .version = QStringLiteral("0.1.0"),
        .seed = ManagedAssetSeed::EmptyModule,
    });
    QVERIFY(plan.canExecute());
    QVERIFY(!QFileInfo::exists(plan.targetRoot));
    const ManagedAssetExecutionResult rejected =
        service.execute(plan, ManagedAssetExecutionOptions{.confirmed = false});
    QVERIFY(!rejected.success);
    QVERIFY(!QFileInfo::exists(plan.targetRoot));

    const ManagedAssetExecutionResult created =
        service.execute(plan, ManagedAssetExecutionOptions{.confirmed = true});
    QVERIFY2(created.success, qPrintable(created.error));
    QVERIFY(QFileInfo::exists(plan.manifestPath));
    QVERIFY(QFileInfo::exists(QDir(plan.targetRoot).absoluteFilePath(
        QStringLiteral("rtl/generated.sv"))));
    const ManifestLoadResult loaded = ManifestService().load(plan.manifestPath);
    QVERIFY(loaded.ok());
    QCOMPARE(loaded.manifest->top, QStringLiteral("generated"));
}

void Phase5Test::managedCreationFailureLeavesNoPartialDirectory()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString seed = temporary.filePath(QStringLiteral("seed"));
    QVERIFY(QDir().mkpath(library));
    QVERIFY(writeFile(QDir(seed).absoluteFilePath(QStringLiteral("rtl/a.sv")),
                      QByteArrayLiteral("module a; endmodule\n")));
    QVERIFY(writeFile(QDir(seed).absoluteFilePath(QStringLiteral("rtl/b.sv")),
                      QByteArrayLiteral("module b; endmodule\n")));
    QVERIFY(writeFile(QDir(seed).absoluteFilePath(QStringLiteral("build/generated.tmp")),
                      QByteArrayLiteral("ignore\n")));

    ManagedAssetService service;
    const ManagedAssetPlan plan = service.plan(ManagedAssetRequest{
        .libraryRoot = library,
        .id = QStringLiteral("copied"),
        .name = QStringLiteral("Copied"),
        .top = QStringLiteral("a"),
        .seed = ManagedAssetSeed::ExistingDirectory,
        .existingDirectory = seed,
    });
    QVERIFY(plan.canExecute());
    QVERIFY(std::none_of(
        plan.files.cbegin(), plan.files.cend(), [](const ManagedPlannedFile &file) {
            return file.destinationPath.contains(QStringLiteral("build"));
        }));
    const ManagedAssetExecutionResult failed = service.execute(
        plan,
        ManagedAssetExecutionOptions{
            .confirmed = true,
            .failAfterFileOperations = 1,
        });
    QVERIFY(!failed.success);
    QVERIFY(failed.rolledBack);
    QVERIFY(!QFileInfo::exists(plan.targetRoot));
    const QStringList staging = QDir(library).entryList(
        {QStringLiteral(".xips-create-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(staging.isEmpty());

    std::atomic_bool cancelled{true};
    const ManagedAssetExecutionResult cancelledResult = service.execute(
        plan,
        ManagedAssetExecutionOptions{
            .confirmed = true,
            .cancelled = &cancelled,
        });
    QVERIFY(!cancelledResult.success);
    QVERIFY(cancelledResult.rolledBack);
    QVERIFY(!QFileInfo::exists(plan.targetRoot));
}

void Phase5Test::managedCreationRejectsForgedAndStalePlans()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));

    ManagedAssetService service;
    const ManagedAssetPlan valid = service.plan(ManagedAssetRequest{
        .libraryRoot = library,
        .id = QStringLiteral("guarded"),
        .name = QStringLiteral("Guarded"),
        .top = QStringLiteral("guarded"),
        .version = QStringLiteral("0.1.0"),
        .seed = ManagedAssetSeed::EmptyModule,
    });
    QVERIFY(valid.canExecute());

    ManagedAssetPlan escaped = valid;
    escaped.targetRoot =
        temporary.filePath(QStringLiteral("outside/escaped"));
    escaped.manifestPath =
        QDir(escaped.targetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    const ManagedAssetExecutionResult escapedResult = service.execute(
        escaped,
        ManagedAssetExecutionOptions{.confirmed = true});
    QVERIFY(!escapedResult.success);
    QVERIFY(escapedResult.error.contains(QStringLiteral("outside the library")));
    QVERIFY(!QFileInfo::exists(escaped.targetRoot));
    QVERIFY(!QFileInfo::exists(valid.targetRoot));

    ManagedAssetPlan reserved = valid;
    reserved.files.first().destinationPath = QStringLiteral(".xips.json");
    const ManagedAssetExecutionResult reservedResult = service.execute(
        reserved,
        ManagedAssetExecutionOptions{.confirmed = true});
    QVERIFY(!reservedResult.success);
    QVERIFY(reservedResult.error.contains(QStringLiteral("Unsafe or duplicate")));
    QVERIFY(!QFileInfo::exists(valid.targetRoot));

    const QString seed =
        temporary.filePath(QStringLiteral("seed/source.sv"));
    QVERIFY(writeFile(seed, QByteArrayLiteral("module seeded; endmodule\n")));
    const ManagedAssetPlan stale = service.plan(ManagedAssetRequest{
        .libraryRoot = library,
        .id = QStringLiteral("stale"),
        .name = QStringLiteral("Stale"),
        .top = QStringLiteral("seeded"),
        .seed = ManagedAssetSeed::SourceFile,
        .sourceFile = seed,
    });
    QVERIFY(stale.canExecute());
    QVERIFY(writeFile(seed,
                      QByteArrayLiteral("module seeded; wire changed; endmodule\n")));
    const ManagedAssetExecutionResult staleResult = service.execute(
        stale,
        ManagedAssetExecutionOptions{.confirmed = true});
    QVERIFY(!staleResult.success);
    QVERIFY(staleResult.rolledBack);
    QVERIFY(!QFileInfo::exists(stale.targetRoot));

    const QStringList staging = QDir(library).entryList(
        {QStringLiteral(".xips-create-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(staging.isEmpty());
}

void Phase5Test::cliCreatesManagedAssetOnlyWithExecute()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(library));
    const QStringList arguments{
        QStringLiteral("create-managed"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--id"),
        QStringLiteral("cli_managed"),
        QStringLiteral("--name"),
        QStringLiteral("CLI Managed"),
        QStringLiteral("--top"),
        QStringLiteral("cli_managed"),
        QStringLiteral("--seed"),
        QStringLiteral("empty"),
    };
    const CliResult preview = runCli(arguments);
    QCOMPARE(preview.exitCode, 0);
    QVERIFY(!QFileInfo::exists(
        QDir(library).absoluteFilePath(QStringLiteral("cli_managed"))));
    QStringList execute = arguments;
    execute.append(QStringLiteral("--execute"));
    const CliResult created = runCli(execute);
    QCOMPARE(created.exitCode, 0);
    QVERIFY(QFileInfo::exists(QDir(library).absoluteFilePath(
        QStringLiteral("cli_managed/.xips.json"))));
}

void Phase5Test::creationAndRegistrationRejectDuplicateCatalogIds()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString registrationRoot =
        temporary.filePath(QStringLiteral("registration"));
    QVERIFY(QDir().mkpath(library));
    QVERIFY(QDir().mkpath(registrationRoot));
    const QString source =
        QDir(registrationRoot).absoluteFilePath(QStringLiteral("duplicate.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module duplicate; endmodule\n")));

    const ManagedAssetPlan managed = ManagedAssetService().plan(
        ManagedAssetRequest{
            .libraryRoot = library,
            .id = QStringLiteral("duplicate"),
            .name = QStringLiteral("Duplicate"),
            .top = QStringLiteral("duplicate"),
            .existingAssetIds = {QStringLiteral("duplicate")},
        });
    QVERIFY(!managed.canExecute());

    const ModuleRegistrationPlan registration =
        IntegrationService().planModuleRegistration(
            ModuleRegistrationRequest{
                .assetRoot = registrationRoot,
                .sourcePath = source,
                .id = QStringLiteral("duplicate"),
                .name = QStringLiteral("Duplicate"),
                .top = QStringLiteral("duplicate"),
                .existingAssetIds = {QStringLiteral("duplicate")},
            });
    QVERIFY(!registration.canExecute());
    QVERIFY(!QFileInfo::exists(registration.manifestPath));
}

QTEST_GUILESS_MAIN(Phase5Test)

#include "tst_phase5.moc"
