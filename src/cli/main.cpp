#include "assetcore/JsonUtil.h"
#include "assetindex/AssetScanner.h"
#include "importer/ImportService.h"
#include "integration/IntegrationService.h"
#include "managed/ManagedAssetService.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>

using namespace xips;

namespace {

constexpr int ExitUsage = 2;
constexpr int ExitBlocked = 3;
constexpr int ExitIo = 4;

QJsonObject errorObject(const QString &code, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
    };
}

int writeResponse(const QString &command,
                  const bool ok,
                  const QJsonObject &data,
                  const QJsonArray &errors,
                  const bool pretty,
                  const int exitCode)
{
    const QJsonObject response{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("command"), command},
        {QStringLiteral("ok"), ok},
        {QStringLiteral("data"), data},
        {QStringLiteral("errors"), errors},
    };
    QTextStream output(stdout);
    output.setEncoding(QStringConverter::Utf8);
    output << QJsonDocument(response).toJson(
        pretty ? QJsonDocument::Indented : QJsonDocument::Compact);
    output.flush();
    return exitCode;
}

QJsonObject scanIssueJson(const ScanIssue &issue)
{
    return QJsonObject{
        {QStringLiteral("severity"), diagnosticSeverityToString(issue.severity)},
        {QStringLiteral("assetId"), issue.assetId},
        {QStringLiteral("path"), issue.path},
        {QStringLiteral("message"), issue.message},
    };
}

QJsonObject assetJson(const AssetRecord &asset)
{
    return QJsonObject{
        {QStringLiteral("id"), asset.manifest.id},
        {QStringLiteral("name"), asset.manifest.name},
        {QStringLiteral("type"), assetTypeToString(asset.manifest.type)},
        {QStringLiteral("version"), asset.manifest.version},
        {QStringLiteral("top"), asset.manifest.top},
        {QStringLiteral("language"), asset.manifest.language},
        {QStringLiteral("assetRoot"), asset.assetRoot},
        {QStringLiteral("manifestPath"), asset.manifestPath},
        {QStringLiteral("contentHash"), asset.contentHash},
        {QStringLiteral("origin"), assetOriginToString(asset.origin)},
        {QStringLiteral("sources"), json::toArray(asset.manifest.sources)},
        {QStringLiteral("tags"), json::toArray(asset.manifest.tags)},
    };
}

struct CatalogResult {
    QList<AssetRecord> assets;
    QList<ScanIssue> issues;
    QString error;
};

CatalogResult scanCatalog(const QCommandLineParser &parser)
{
    CatalogResult result;
    QList<LibraryRoot> roots;
    for (const QString &library : parser.values(QStringLiteral("library"))) {
        roots.append(LibraryRoot{
            .path = QFileInfo(library).absoluteFilePath(),
            .origin = AssetOrigin::Managed,
        });
    }
    for (const QString &library : parser.values(QStringLiteral("external"))) {
        roots.append(LibraryRoot{
            .path = QFileInfo(library).absoluteFilePath(),
            .origin = AssetOrigin::External,
        });
    }
    if (roots.isEmpty()) {
        result.error = QStringLiteral("At least one --library or --external root is required");
        return result;
    }
    const ScanResult scan = AssetScanner().scan(roots);
    result.assets = scan.assets;
    result.issues = scan.issues;
    return result;
}

const AssetRecord *findAsset(const QList<AssetRecord> &assets, const QString &id)
{
    const auto iterator = std::find_if(
        assets.cbegin(),
        assets.cend(),
        [&id](const AssetRecord &asset) {
            return asset.manifest.id == id;
        });
    return iterator == assets.cend() ? nullptr : &*iterator;
}

bool hasScanErrors(const QList<ScanIssue> &issues)
{
    return std::any_of(issues.cbegin(), issues.cend(), [](const ScanIssue &issue) {
        return issue.severity == Diagnostic::Severity::Error;
    });
}

QJsonArray scanIssuesJson(const QList<ScanIssue> &issues)
{
    QJsonArray result;
    for (const ScanIssue &issue : issues) {
        result.append(scanIssueJson(issue));
    }
    return result;
}

QJsonObject importPlanJson(const ImportPlan &plan)
{
    QJsonArray dependencyIssues;
    for (const DependencyIssue &issue : plan.dependencies.issues) {
        dependencyIssues.append(QJsonObject{
            {QStringLiteral("severity"), diagnosticSeverityToString(issue.severity)},
            {QStringLiteral("assetId"), issue.assetId},
            {QStringLiteral("dependencyId"), issue.dependencyId},
            {QStringLiteral("message"), issue.message},
            {QStringLiteral("path"), json::toArray(issue.path)},
        });
    }
    QJsonArray importIssues;
    for (const ImportIssue &issue : plan.issues) {
        importIssues.append(QJsonObject{
            {QStringLiteral("severity"), diagnosticSeverityToString(issue.severity)},
            {QStringLiteral("message"), issue.message},
            {QStringLiteral("path"), issue.path},
        });
    }
    QJsonArray files;
    for (const PlannedFile &file : plan.files) {
        files.append(QJsonObject{
            {QStringLiteral("assetId"), file.assetId},
            {QStringLiteral("source"), file.sourcePath},
            {QStringLiteral("destination"), file.destinationPath},
            {QStringLiteral("sourceHash"), file.sourceHash},
            {QStringLiteral("existingHash"), file.existingHash},
            {QStringLiteral("ownedHash"), file.previouslyOwnedHash},
            {QStringLiteral("action"), plannedFileActionToString(file.action)},
        });
    }
    QJsonObject counts;
    const QMap<PlannedFileAction, int> actionCounts = plan.actionCounts();
    for (auto iterator = actionCounts.cbegin(); iterator != actionCounts.cend(); ++iterator) {
        counts.insert(plannedFileActionToString(iterator.key()), iterator.value());
    }
    return QJsonObject{
        {QStringLiteral("mode"),
         plan.mode == ImportMode::Reference ? QStringLiteral("reference")
                                            : QStringLiteral("vendor")},
        {QStringLiteral("targetRoot"), plan.targetRoot},
        {QStringLiteral("outputPath"), plan.outputPath},
        {QStringLiteral("canExecute"), plan.canExecute()},
        {QStringLiteral("dependencyOrder"),
         json::toArray(plan.dependencies.orderedIds())},
        {QStringLiteral("dependencyIssues"), dependencyIssues},
        {QStringLiteral("importIssues"), importIssues},
        {QStringLiteral("actionCounts"), counts},
        {QStringLiteral("files"), files},
        {QStringLiteral("outputDocument"), plan.outputDocument},
    };
}

QString upgradeKind(const AssetUpgradeKind kind)
{
    switch (kind) {
    case AssetUpgradeKind::Added:
        return QStringLiteral("added");
    case AssetUpgradeKind::Removed:
        return QStringLiteral("removed");
    case AssetUpgradeKind::Changed:
        return QStringLiteral("changed");
    case AssetUpgradeKind::Unchanged:
        return QStringLiteral("unchanged");
    }
    return QStringLiteral("unchanged");
}

QJsonArray upgradesJson(const QList<AssetUpgrade> &upgrades)
{
    QJsonArray result;
    for (const AssetUpgrade &upgrade : upgrades) {
        result.append(QJsonObject{
            {QStringLiteral("assetId"), upgrade.assetId},
            {QStringLiteral("kind"), upgradeKind(upgrade.kind)},
            {QStringLiteral("beforeVersion"), upgrade.beforeVersion},
            {QStringLiteral("afterVersion"), upgrade.afterVersion},
            {QStringLiteral("beforeContentHash"), upgrade.beforeContentHash},
            {QStringLiteral("afterContentHash"), upgrade.afterContentHash},
        });
    }
    return result;
}

QList<DependencySpec> parseDependencies(const QStringList &values)
{
    QList<DependencySpec> result;
    for (QString value : values) {
        DependencySpec dependency;
        if (value.startsWith(u'?')) {
            dependency.optional = true;
            value.remove(0, 1);
        }
        const qsizetype separator = value.indexOf(u'@');
        dependency.id = separator < 0 ? value : value.first(separator);
        dependency.versionConstraint =
            separator < 0 ? QString() : value.mid(separator + 1);
        result.append(dependency);
    }
    return result;
}

QJsonObject parseTargetTools(const QStringList &values, QString *error)
{
    QJsonObject result;
    for (const QString &value : values) {
        const qsizetype separator = value.indexOf(u'=');
        const QString name =
            separator < 1 ? QString() : value.first(separator).trimmed();
        const QString version =
            separator < 0 ? QString() : value.mid(separator + 1).trimmed();
        if (name.isEmpty() || version.isEmpty()) {
            if (error) {
                *error =
                    QStringLiteral("Target tools must use name=version: %1")
                        .arg(value);
            }
            return {};
        }
        result.insert(name, version);
    }
    return result;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xips-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(XIPS_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Stable JSON interface for xIPs asset integration"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral("catalog | open-uri | register-module | create-managed | import | code-block"));
    parser.addOption({QStringLiteral("library"),
                      QStringLiteral("Managed asset-library root; repeatable."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("external"),
                      QStringLiteral("External registered root; repeatable."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("asset"),
                      QStringLiteral("Asset ID; repeatable for import."),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("target"),
                      QStringLiteral("Target project directory."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("mode"),
                      QStringLiteral("Import mode: reference or vendor."),
                      QStringLiteral("mode"),
                      QStringLiteral("reference")});
    parser.addOption({QStringLiteral("execute"),
                      QStringLiteral("Execute a previously previewable write operation.")});
    parser.addOption({QStringLiteral("asset-root"),
                      QStringLiteral("Existing module asset root."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("source"),
                      QStringLiteral("Module source file."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("id"),
                      QStringLiteral("Stable ID for module registration."),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("name"),
                      QStringLiteral("Display name for module registration."),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("top"),
                      QStringLiteral("Top unit for module registration."),
                      QStringLiteral("unit")});
    parser.addOption({QStringLiteral("semantic-version"),
                      QStringLiteral("Optional manifest semantic version."),
                      QStringLiteral("version")});
    parser.addOption({QStringLiteral("include-dir"),
                      QStringLiteral("Include directory; repeatable."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("define"),
                      QStringLiteral("Preprocessor definition; repeatable."),
                      QStringLiteral("define")});
    parser.addOption({QStringLiteral("dependency"),
                      QStringLiteral("Dependency id[@constraint], prefix ? for optional; repeatable."),
                      QStringLiteral("dependency")});
    parser.addOption({QStringLiteral("target-tool"),
                      QStringLiteral("Target tool name=version; repeatable for import."),
                      QStringLiteral("tool=version")});
    parser.addOption({QStringLiteral("handoff"),
                      QStringLiteral("Code Block JSON handoff path."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("seed"),
                      QStringLiteral("Managed seed: empty, source, or directory."),
                      QStringLiteral("kind"),
                      QStringLiteral("empty")});
    parser.addOption({QStringLiteral("existing-directory"),
                      QStringLiteral("Existing directory used as a managed seed."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("pretty"),
                      QStringLiteral("Indent JSON output.")});

    QString parseError;
    if (!parser.parse(application.arguments())) {
        parseError = parser.errorText();
        return writeResponse(
            QString(),
            false,
            {},
            QJsonArray{errorObject(QStringLiteral("usage"), parseError)},
            application.arguments().contains(QStringLiteral("--pretty")),
            ExitUsage);
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream output(stdout);
        output << parser.helpText();
        output.flush();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QTextStream output(stdout);
        output << QCoreApplication::applicationName() << u' '
               << QCoreApplication::applicationVersion() << u'\n';
        output.flush();
        return 0;
    }
    const QStringList positional = parser.positionalArguments();
    const QString command = positional.value(0);
    const bool pretty = parser.isSet(QStringLiteral("pretty"));
    if (command.isEmpty()) {
        return writeResponse(
            command,
            false,
            {},
            QJsonArray{errorObject(QStringLiteral("usage"), QStringLiteral("Missing command"))},
            pretty,
            ExitUsage);
    }

    if (command == QStringLiteral("catalog")) {
        const CatalogResult catalog = scanCatalog(parser);
        if (!catalog.error.isEmpty()) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(QStringLiteral("usage"), catalog.error)},
                pretty,
                ExitUsage);
        }
        QJsonArray assets;
        for (const AssetRecord &asset : catalog.assets) {
            assets.append(assetJson(asset));
        }
        const bool ok = !hasScanErrors(catalog.issues);
        return writeResponse(
            command,
            ok,
            QJsonObject{
                {QStringLiteral("assets"), assets},
                {QStringLiteral("scanIssues"), scanIssuesJson(catalog.issues)},
            },
            {},
            pretty,
            ok ? 0 : ExitBlocked);
    }

    if (command == QStringLiteral("register-module")) {
        QStringList existingAssetIds;
        if (!parser.values(QStringLiteral("library")).isEmpty()
            || !parser.values(QStringLiteral("external")).isEmpty()) {
            const CatalogResult existingCatalog = scanCatalog(parser);
            for (const AssetRecord &asset : existingCatalog.assets) {
                existingAssetIds.append(asset.manifest.id);
            }
        }
        IntegrationService service;
        const ModuleRegistrationPlan plan = service.planModuleRegistration(
            ModuleRegistrationRequest{
                .assetRoot = parser.value(QStringLiteral("asset-root")),
                .sourcePaths = parser.values(QStringLiteral("source")),
                .id = parser.value(QStringLiteral("id")),
                .name = parser.value(QStringLiteral("name")),
                .top = parser.value(QStringLiteral("top")),
                .version = parser.value(QStringLiteral("semantic-version")),
                .includeDirs = parser.values(QStringLiteral("include-dir")),
                .defines = parser.values(QStringLiteral("define")),
                .dependencies =
                    parseDependencies(parser.values(QStringLiteral("dependency"))),
                .existingAssetIds = existingAssetIds,
            });
        const bool duplicate = existingAssetIds.contains(plan.manifest.id);
        QJsonObject data{
            {QStringLiteral("plan"), plan.toJson()},
            {QStringLiteral("executed"), false},
        };
        if (duplicate) {
            return writeResponse(
                command,
                false,
                data,
                QJsonArray{errorObject(
                    QStringLiteral("duplicate-id"),
                    QStringLiteral("Asset ID already exists in the supplied catalog"))},
                pretty,
                ExitBlocked);
        }
        if (!plan.canExecute()) {
            return writeResponse(command, false, data, {}, pretty, ExitBlocked);
        }
        if (!parser.isSet(QStringLiteral("execute"))) {
            return writeResponse(command, true, data, {}, pretty, 0);
        }
        QString error;
        const bool executed =
            service.executeModuleRegistration(plan, true, &error);
        data.insert(QStringLiteral("executed"), executed);
        return writeResponse(
            command,
            executed,
            data,
            error.isEmpty()
                ? QJsonArray{}
                : QJsonArray{errorObject(QStringLiteral("write"), error)},
            pretty,
            executed ? 0 : ExitIo);
    }

    if (command == QStringLiteral("create-managed")) {
        const QStringList libraries = parser.values(QStringLiteral("library"));
        const QString seed = parser.value(QStringLiteral("seed")).toLower();
        if (libraries.size() != 1
            || (seed != QStringLiteral("empty")
                && seed != QStringLiteral("source")
                && seed != QStringLiteral("directory"))) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(
                    QStringLiteral("usage"),
                    QStringLiteral("create-managed requires exactly one --library and a valid --seed"))},
                pretty,
                ExitUsage);
        }
        ManagedAssetSeed seedKind = ManagedAssetSeed::EmptyModule;
        if (seed == QStringLiteral("source")) {
            seedKind = ManagedAssetSeed::SourceFile;
        } else if (seed == QStringLiteral("directory")) {
            seedKind = ManagedAssetSeed::ExistingDirectory;
        }
        QStringList existingAssetIds;
        const ScanResult existing = AssetScanner().scan({
            LibraryRoot{
                .path = libraries.first(),
                .origin = AssetOrigin::Managed,
            },
        });
        for (const AssetRecord &asset : existing.assets) {
            existingAssetIds.append(asset.manifest.id);
        }
        ManagedAssetService service;
        const ManagedAssetPlan plan = service.plan(ManagedAssetRequest{
            .libraryRoot = libraries.first(),
            .id = parser.value(QStringLiteral("id")),
            .name = parser.value(QStringLiteral("name")),
            .top = parser.value(QStringLiteral("top")),
            .version = parser.value(QStringLiteral("semantic-version")),
            .seed = seedKind,
            .sourceFile = parser.value(QStringLiteral("source")),
            .existingDirectory =
                parser.value(QStringLiteral("existing-directory")),
            .existingAssetIds = existingAssetIds,
        });
        QJsonObject data{
            {QStringLiteral("plan"), plan.toJson()},
            {QStringLiteral("executed"), false},
        };
        if (!plan.canExecute()) {
            return writeResponse(command, false, data, {}, pretty, ExitBlocked);
        }
        if (!parser.isSet(QStringLiteral("execute"))) {
            return writeResponse(command, true, data, {}, pretty, 0);
        }
        const ManagedAssetExecutionResult result = service.execute(
            plan,
            ManagedAssetExecutionOptions{.confirmed = true});
        data.insert(QStringLiteral("executed"), result.success);
        data.insert(QStringLiteral("result"),
                    QJsonObject{
                        {QStringLiteral("success"), result.success},
                        {QStringLiteral("rolledBack"), result.rolledBack},
                        {QStringLiteral("filesCreated"), result.filesCreated},
                        {QStringLiteral("targetRoot"), result.targetRoot},
                        {QStringLiteral("error"), result.error},
                    });
        return writeResponse(
            command,
            result.success,
            data,
            result.error.isEmpty()
                ? QJsonArray{}
                : QJsonArray{errorObject(QStringLiteral("managed-create"),
                                         result.error)},
            pretty,
            result.success ? 0 : ExitIo);
    }

    const CatalogResult catalog = scanCatalog(parser);
    if (!catalog.error.isEmpty()) {
        return writeResponse(
            command,
            false,
            {},
            QJsonArray{errorObject(QStringLiteral("usage"), catalog.error)},
            pretty,
            ExitUsage);
    }
    if (hasScanErrors(catalog.issues)) {
        return writeResponse(
            command,
            false,
            QJsonObject{{QStringLiteral("scanIssues"), scanIssuesJson(catalog.issues)}},
            QJsonArray{errorObject(
                QStringLiteral("catalog-invalid"),
                QStringLiteral("Catalog contains scan errors"))},
            pretty,
            ExitBlocked);
    }

    if (command == QStringLiteral("open-uri")) {
        const QString id = parser.value(QStringLiteral("asset"));
        const AssetRecord *asset = findAsset(catalog.assets, id);
        if (!asset) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(
                    QStringLiteral("missing-asset"),
                    QStringLiteral("Asset ID was not found"))},
                pretty,
                ExitBlocked);
        }
        return writeResponse(
            command,
            true,
            QJsonObject{
                {QStringLiteral("asset"), assetJson(*asset)},
                {QStringLiteral("uri"),
                 IntegrationService::zeroSlackOpenUri(*asset)
                     .toString(QUrl::FullyEncoded)},
            },
            {},
            pretty,
            0);
    }

    if (command == QStringLiteral("code-block")) {
        const QString id = parser.value(QStringLiteral("asset"));
        const AssetRecord *asset = findAsset(catalog.assets, id);
        if (!asset || asset->manifest.type != AssetType::CodeBlock) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(
                    QStringLiteral("not-code-block"),
                    QStringLiteral("A Code Block asset ID is required"))},
                pretty,
                ExitBlocked);
        }
        const QString handoff = parser.value(QStringLiteral("handoff"));
        QJsonObject data{
            {QStringLiteral("payload"), IntegrationService::codeBlockPayload(*asset)},
            {QStringLiteral("executed"), false},
        };
        if (!handoff.isEmpty()) {
            data.insert(QStringLiteral("handoffPath"),
                        QFileInfo(handoff).absoluteFilePath());
            data.insert(QStringLiteral("uri"),
                        IntegrationService::zeroSlackCodeBlockUri(*asset, handoff)
                            .toString(QUrl::FullyEncoded));
        }
        if (!parser.isSet(QStringLiteral("execute"))) {
            return writeResponse(command, true, data, {}, pretty, 0);
        }
        if (handoff.isEmpty()) {
            return writeResponse(
                command,
                false,
                data,
                QJsonArray{errorObject(
                    QStringLiteral("usage"),
                    QStringLiteral("--handoff is required with --execute"))},
                pretty,
                ExitUsage);
        }
        QString error;
        const bool executed =
            IntegrationService::writeCodeBlockHandoff(*asset, handoff, &error);
        data.insert(QStringLiteral("executed"), executed);
        return writeResponse(
            command,
            executed,
            data,
            error.isEmpty()
                ? QJsonArray{}
                : QJsonArray{errorObject(QStringLiteral("write"), error)},
            pretty,
            executed ? 0 : ExitIo);
    }

    if (command == QStringLiteral("import")) {
        const QString target = parser.value(QStringLiteral("target"));
        const QStringList assetIds = parser.values(QStringLiteral("asset"));
        const QString mode = parser.value(QStringLiteral("mode")).toLower();
        if (target.isEmpty() || assetIds.isEmpty()
            || (mode != QStringLiteral("reference") && mode != QStringLiteral("vendor"))) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(
                    QStringLiteral("usage"),
                    QStringLiteral("import requires --target, --asset, and a valid --mode"))},
                pretty,
                ExitUsage);
        }
        ImportService service;
        ImportPlan plan;
        QJsonArray upgrades;
        QString targetToolsError;
        const QJsonObject targetTools =
            parseTargetTools(parser.values(QStringLiteral("target-tool")),
                             &targetToolsError);
        if (!targetToolsError.isEmpty()) {
            return writeResponse(
                command,
                false,
                {},
                QJsonArray{errorObject(QStringLiteral("usage"),
                                       targetToolsError)},
                pretty,
                ExitUsage);
        }
        if (mode == QStringLiteral("reference")) {
            plan =
                service.planReference(catalog.assets, assetIds, target, targetTools);
        } else {
            const VendorUpgradePlan upgrade =
                service.planVendorUpgrade(catalog.assets,
                                          assetIds,
                                          target,
                                          targetTools);
            plan = upgrade.importPlan;
            upgrades = upgradesJson(upgrade.assets);
        }
        QJsonObject data{
            {QStringLiteral("plan"), importPlanJson(plan)},
            {QStringLiteral("upgrades"), upgrades},
            {QStringLiteral("executed"), false},
        };
        if (!plan.canExecute()) {
            return writeResponse(command, false, data, {}, pretty, ExitBlocked);
        }
        if (!parser.isSet(QStringLiteral("execute"))) {
            return writeResponse(command, true, data, {}, pretty, 0);
        }

        if (mode == QStringLiteral("reference")) {
            QString error;
            const bool executed = service.writeReference(plan, &error);
            data.insert(QStringLiteral("executed"), executed);
            return writeResponse(
                command,
                executed,
                data,
                error.isEmpty()
                    ? QJsonArray{}
                    : QJsonArray{errorObject(QStringLiteral("write"), error)},
                pretty,
                executed ? 0 : ExitIo);
        }
        const ImportExecutionResult result = service.executeVendor(
            plan,
            ImportExecutionOptions{.confirmed = true});
        data.insert(QStringLiteral("executed"), result.success);
        data.insert(
            QStringLiteral("result"),
            QJsonObject{
                {QStringLiteral("success"), result.success},
                {QStringLiteral("rolledBack"), result.rolledBack},
                {QStringLiteral("cancelled"), result.cancelled},
                {QStringLiteral("added"), result.added},
                {QStringLiteral("overwritten"), result.overwritten},
                {QStringLiteral("removed"), result.removed},
                {QStringLiteral("skipped"), result.skipped},
                {QStringLiteral("error"), result.error},
            });
        return writeResponse(
            command,
            result.success,
            data,
            result.error.isEmpty()
                ? QJsonArray{}
                : QJsonArray{errorObject(QStringLiteral("vendor"), result.error)},
            pretty,
            result.success ? 0 : ExitIo);
    }

    return writeResponse(
        command,
        false,
        {},
        QJsonArray{errorObject(
            QStringLiteral("usage"),
            QStringLiteral("Unknown command"))},
        pretty,
        ExitUsage);
}
