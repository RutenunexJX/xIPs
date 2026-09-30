#include "assetcore/JsonUtil.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"
#include "library/AssetScanner.h"
#include "library/FileSystemUtil.h"
#include "library/SnapshotLibrary.h"
#include "library/CatalogIndex.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace
{

void writeJson(const QJsonObject &object, QTextStream &stream)
{
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact) << u'\n';
    stream.flush();
}

int fail(const QString &action, const QString &message, const int exitCode = 2,
         const QStringList &problems = {})
{
    QTextStream stream(stderr);
    QJsonObject envelope{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("ok"), false},
        {QStringLiteral("action"), action},
        {QStringLiteral("error"), message},
    };
    if (!problems.isEmpty())
    {
        envelope.insert(QStringLiteral("problems"), xips::json::toArray(problems));
    }
    writeJson(envelope, stream);
    return exitCode;
}

QJsonObject success(const QString &action, const QJsonObject &data)
{
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("ok"), true},
        {QStringLiteral("action"), action},
        {QStringLiteral("data"), data},
    };
}

QJsonObject assetJson(const xips::AssetRecord &asset)
{
    return QJsonObject{
        {QStringLiteral("id"), asset.manifest.id},
        {QStringLiteral("name"), asset.manifest.name},
        {QStringLiteral("version"), asset.manifest.version},
        {QStringLiteral("category"), asset.manifest.rawObject.value("category").toString("other")},
        {QStringLiteral("description"), asset.manifest.description},
        {QStringLiteral("tags"), xips::json::toArray(asset.manifest.tags)},
        {QStringLiteral("files"), xips::json::toArray(asset.files)},
        {QStringLiteral("path"), asset.assetRoot},
        {QStringLiteral("fileCount"), static_cast<qint64>(asset.fileCount)},
        {QStringLiteral("uri"),
         xips::IntegrationService::assetUri(asset.manifest.id).toString(QUrl::FullyEncoded)},
    };
}

QJsonObject assetMetadataJson(const xips::AssetRecord &asset)
{
    QJsonObject result = assetJson(asset);
    result.remove(QStringLiteral("path"));
    return result;
}

QJsonArray resolvedFiles(const QString &root, const QStringList &relativeFiles)
{
    QJsonArray result;
    for (const QString &relative : relativeFiles)
    {
        result.append(QDir(root).absoluteFilePath(relative));
    }
    return result;
}

QString duplicateProblemId(const QString &problem)
{
    static const QRegularExpression expression(
        QStringLiteral("(?:^|:\\s)Duplicate asset id '([^']+)'(?:$|:)"));
    const QRegularExpressionMatch match = expression.match(problem);
    return match.hasMatch() ? match.captured(1) : QString();
}

struct AssetIndex
{
    QHash<QString, QList<const xips::AssetRecord *>> assetsByIdentity;
    QSet<QString> ambiguousIdentities;
    QStringList problems;
};

AssetIndex indexAssets(const xips::ScanResult &scan)
{
    AssetIndex index;
    index.problems = scan.errors;
    for (const QString &problem : scan.errors)
    {
        const QString duplicateId = duplicateProblemId(problem);
        if (!duplicateId.isEmpty())
        {
            index.ambiguousIdentities.insert(duplicateId.toCaseFolded());
        }
    }
    for (const xips::AssetRecord &asset : scan.assets)
    {
        index.assetsByIdentity[asset.manifest.id.toCaseFolded()].append(&asset);
    }
    for (auto it = index.assetsByIdentity.cbegin(); it != index.assetsByIdentity.cend(); ++it)
    {
        if (it.value().size() < 2)
        {
            continue;
        }
        index.ambiguousIdentities.insert(it.key());
        QStringList paths;
        for (const xips::AssetRecord *asset : it.value())
        {
            paths.append(asset->manifestPath);
        }
        index.problems.append(
            QStringLiteral("Ambiguous case-insensitive asset id '%1': %2")
                .arg(it.value().first()->manifest.id, paths.join(QStringLiteral(", "))));
    }
    index.problems.removeDuplicates();
    return index;
}

bool sameCopyPlan(const xips::CopyPlan &left, const xips::CopyPlan &right)
{
    return left.ok() && right.ok() && left.version == right.version &&
           left.sourceRoot == right.sourceRoot && left.files == right.files &&
           left.contentHash == right.contentHash &&
           left.strictContentHash == right.strictContentHash &&
           left.payloadFingerprint == right.payloadFingerprint &&
           left.proofFingerprint == right.proofFingerprint;
}

void attachProblems(QJsonObject &data, const QStringList &problems)
{
    data.insert(QStringLiteral("problemCount"), problems.size());
    data.insert(QStringLiteral("problems"), xips::json::toArray(problems));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xips-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(XIPS_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Safe query and explicit export bridge for the xIPs FPGA asset library"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption actionOption(QStringLiteral("action"),
                                          QStringLiteral("Action: list or resolve."),
                                          QStringLiteral("name"), QStringLiteral("list"));
    const QCommandLineOption libraryOption({QStringLiteral("l"), QStringLiteral("library")},
                                           QStringLiteral("Asset library directory."),
                                           QStringLiteral("directory"));
    const QCommandLineOption assetOption(QStringLiteral("asset"),
                                         QStringLiteral("Stable asset id."), QStringLiteral("id"));
    const QCommandLineOption versionOption(QStringLiteral("asset-version"),
                                           QStringLiteral("Saved asset version."),
                                           QStringLiteral("version"));
    const QCommandLineOption queryOption(QStringLiteral("query"), QStringLiteral("Search text."),
                                         QStringLiteral("text"));
    const QCommandLineOption destinationOption(
        QStringLiteral("destination"),
        QStringLiteral("Materialize a saved version at this explicit path."),
        QStringLiteral("path"));
    parser.addOption(actionOption);
    parser.addOption(libraryOption);
    parser.addOption(assetOption);
    parser.addOption(versionOption);
    parser.addOption(queryOption);
    parser.addOption(destinationOption);
    if (!parser.parse(application.arguments()))
    {
        QTextStream(stderr) << parser.errorText() << u'\n';
        return 2;
    }
    if (parser.isSet(QStringLiteral("help")))
    {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version")))
    {
        QTextStream(stdout) << QCoreApplication::applicationName() << u' '
                            << QCoreApplication::applicationVersion() << u'\n';
        return 0;
    }

    const QString action = parser.value(actionOption).trimmed().toLower();
    QTextStream output(stdout);
    if (action != QStringLiteral("list") && action != QStringLiteral("resolve"))
    {
        return fail(action, QStringLiteral("Unknown action"));
    }

    QString libraryValue = parser.value(libraryOption);
    if (libraryValue.isEmpty())
    {
        libraryValue = qEnvironmentVariable("XIPS_LIBRARY");
    }
    if (libraryValue.isEmpty())
    {
        return fail(action, QStringLiteral("--library or XIPS_LIBRARY is required"));
    }
    const QString library = QFileInfo(libraryValue).absoluteFilePath();
    if (!QFileInfo(library).isDir())
    {
        return fail(action, QStringLiteral("--library must name an existing directory"), 3);
    }
    xips::ScanResult scan = xips::AssetScanner().scan(library);
    const auto catalog = xips::SnapshotLibrary::scan(library);
    QHash<QString, xips::CatalogAsset> modern;
    QSet<QString> healthyIds;
    for (const auto &entry : catalog.assets)
        healthyIds.insert(entry.id.toCaseFolded());
    scan.assets.removeIf([&](const auto &entry)
                         { return !healthyIds.contains(entry.manifest.id.toCaseFolded()); });
    for (const auto &entry : catalog.assets)
    {
        if (entry.legacy)
            continue;
        scan.errors.removeIf([&](const QString &error)
                             { return error.contains(entry.root + "/.xips.json"); });
        xips::AssetRecord record;
        record.manifest.id = entry.id;
        record.manifest.name = entry.name;
        record.manifest.description = entry.description;
        record.manifest.tags = entry.tags;
        record.manifest.version = entry.snapshots.isEmpty() ? QString() : entry.snapshots.last().id;
        record.manifest.rawObject.insert("category", entry.category);
        record.assetRoot = entry.root;
        record.manifestPath = entry.root + "/.xips.json";
        record.files = entry.snapshots.isEmpty() ? QStringList{} : entry.snapshots.last().files;
        record.fileCount = record.files.size();
        scan.assets.append(record);
        modern.insert(entry.id.toCaseFolded(), entry);
    }
    for (const auto &problem : catalog.problems)
    {
        if (!scan.errors.contains(problem))
            scan.errors.append(problem);
    }
    const AssetIndex index = indexAssets(scan);

    if (action == QStringLiteral("list"))
    {
        QString queryError;
        const QStringList terms = xips::CatalogIndex::queryTerms(parser.value(queryOption), &queryError);
        if (!queryError.isEmpty()) return fail(action, queryError);
        QJsonArray assets;
        for (const xips::AssetRecord &asset : scan.assets)
        {
            if (index.ambiguousIdentities.contains(asset.manifest.id.toCaseFolded()))
            {
                continue;
            }
            const QString searchable = xips::json::normalizeSearchText(QStringList{
                asset.manifest.id,
                asset.manifest.name,
                asset.manifest.version,
                asset.manifest.description,
                asset.manifest.tags.join(u' '),
                asset.files.join(u' '),
            }
                                                                           .join(u' '));
            bool matches = true;
            for (const QString &term : terms)
            {
                if (!searchable.contains(term))
                {
                    matches = false;
                    break;
                }
            }
            if (modern.contains(asset.manifest.id.toCaseFolded()))
                matches = xips::CatalogIndex::matches(modern.value(asset.manifest.id.toCaseFolded()), terms);
            if (matches)
            {
                auto item = assetJson(asset);
                if (modern.contains(asset.manifest.id.toCaseFolded()))
                {
                    const auto entry = modern.value(asset.manifest.id.toCaseFolded());
                    item.insert("indexes", entry.document.value("indexes"));
                    item.insert("reference", !entry.referencePath.isEmpty());
                }
                assets.append(item);
            }
        }
        QJsonObject data{
            {QStringLiteral("count"), assets.size()},
            {QStringLiteral("assets"), assets},
        };
        attachProblems(data, index.problems);
        writeJson(success(action, data), output);
        return 0;
    }

    const QString assetId = parser.value(assetOption).trimmed();
    if (assetId.isEmpty())
    {
        return fail(action, QStringLiteral("resolve requires --asset"));
    }
    const QString identity = assetId.toCaseFolded();
    const QList<const xips::AssetRecord *> matches = index.assetsByIdentity.value(identity);
    if (index.ambiguousIdentities.contains(identity) || matches.size() > 1)
    {
        return fail(action, QStringLiteral("Asset id is ambiguous: %1").arg(assetId), 3,
                    index.problems);
    }
    if (matches.isEmpty())
    {
        return fail(action, QStringLiteral("Asset not found: %1").arg(assetId), 4, index.problems);
    }
    const xips::AssetRecord &found = *matches.first();

    if (modern.contains(identity))
    {
        const auto entry = modern.value(identity);
        if (entry.snapshots.isEmpty()) return fail(action, QStringLiteral("No healthy revisions are available."), 3, index.problems);
        const QString revision = parser.value(versionOption).trimmed().isEmpty()
                                     ? entry.snapshots.last().id
                                     : parser.value(versionOption).trimmed();
        const auto verified = xips::SnapshotLibrary::verifySnapshot(entry, revision);
        if (!verified.ok)
            return fail(action, verified.error,
                        verified.error.startsWith("Revision not found:") ? 4 : 3, index.problems);
        QJsonObject data = assetMetadataJson(found);
        data.insert("category", entry.category);
        data.insert("discovered", entry.discovered);
        data.insert("indexes", entry.document.value("indexes"));
        data.insert("reference", !entry.referencePath.isEmpty());
        data.insert("resolvedVersion", verified.snapshot.id);
        data.insert("resolvedContentHash", verified.snapshot.hash);
        data.insert("resolvedStrictContentHash", verified.snapshot.hash);
        data.insert("resolvedRelativeFiles", xips::json::toArray(verified.snapshot.files));
        data.insert("files", xips::json::toArray(verified.snapshot.files));
        data.insert("fileCount", verified.snapshot.files.size());
        const QString destination = parser.value(destinationOption).trimmed();
        if (destination.isEmpty())
        {
            data.insert("access", "metadata-only");
            data.insert("immutable", verified.snapshot.id != "current");
            data.insert("editable", false);
            data.insert("materialized", false);
        }
        else
        {
            const auto exported =
                xips::SnapshotLibrary::exportSnapshot(entry, revision, destination);
            if (!exported.ok)
                return fail(action, exported.error, 3, index.problems);
            data.insert("access", "materialized-copy");
            data.insert("immutable", false);
            data.insert("sourceImmutable", exported.snapshot.id != "current");
            data.insert("resolvedContentHash", exported.snapshot.hash);
            data.insert("resolvedStrictContentHash", exported.snapshot.hash);
            data.insert("resolvedRelativeFiles", xips::json::toArray(exported.snapshot.files));
            data.insert("files", xips::json::toArray(exported.snapshot.files));
            data.insert("fileCount", exported.snapshot.files.size());
            data.insert("editable", true);
            data.insert("materialized", true);
            data.insert("materializedPath", exported.exportedPath);
            data.insert("resolvedPath", exported.exportedPath);
            const auto paths = exported.snapshot.files.size() == 1
                                   ? QJsonArray{exported.exportedPath}
                                   : resolvedFiles(exported.exportedPath, exported.snapshot.files);
            data.insert("resolvedFiles", paths);
            if (paths.size() == 1)
                data.insert("resolvedFile", paths.first());
        }
        attachProblems(data, index.problems);
        writeJson(success(action, data), output);
        return 0;
    }
    const QString requestedVersion = parser.value(versionOption).trimmed();
    const QString destinationValue = parser.value(destinationOption).trimmed();
    if (!destinationValue.isEmpty() && requestedVersion.isEmpty())
    {
        return fail(action, QStringLiteral("--destination requires --asset-version"), 2,
                    index.problems);
    }
    QJsonObject data = requestedVersion.isEmpty() ? assetJson(found) : assetMetadataJson(found);

    xips::AssetLibraryService libraryService;
    const xips::CopyPlan plan = libraryService.copyPlan(found, requestedVersion);
    if (!plan.ok())
    {
        return fail(action, plan.error,
                    plan.error.startsWith(QStringLiteral("Version not found:")) ? 4 : 3,
                    index.problems);
    }

    data.insert(QStringLiteral("files"), xips::json::toArray(plan.files));
    data.insert(QStringLiteral("fileCount"), plan.files.size());
    data.insert(QStringLiteral("resolvedVersion"),
                requestedVersion.isEmpty() ? QStringLiteral("working") : plan.version);
    data.insert(QStringLiteral("resolvedContentHash"), plan.contentHash);
    data.insert(QStringLiteral("resolvedStrictContentHash"), plan.strictContentHash);
    data.insert(QStringLiteral("resolvedRelativeFiles"), xips::json::toArray(plan.files));

    QString copiedPath;
    if (requestedVersion.isEmpty())
    {
        data.insert(QStringLiteral("access"), QStringLiteral("working-copy"));
        data.insert(QStringLiteral("immutable"), false);
        data.insert(QStringLiteral("editable"), true);
        data.insert(QStringLiteral("resolvedPath"), plan.sourceRoot);
        const QJsonArray files = resolvedFiles(plan.sourceRoot, plan.files);
        data.insert(QStringLiteral("resolvedFiles"), files);
        if (files.size() == 1)
        {
            data.insert(QStringLiteral("resolvedFile"), files.at(0));
        }
    }
    else if (destinationValue.isEmpty())
    {
        data.insert(QStringLiteral("access"), QStringLiteral("metadata-only"));
        data.insert(QStringLiteral("immutable"), true);
        data.insert(QStringLiteral("editable"), false);
        data.insert(QStringLiteral("materialized"), false);
    }
    else
    {
        const QString destination = xips::files::normalizedAbsolute(destinationValue);
        const QFileInfo destinationInfo(destination);
        const QFileInfo parentInfo(destinationInfo.absolutePath());
        if (destinationInfo.fileName().isEmpty() || !parentInfo.isDir() ||
            xips::files::isLinkLike(parentInfo))
        {
            return fail(
                action,
                QStringLiteral("--destination must have an existing, non-linked parent directory"),
                3, index.problems);
        }
        const QString canonicalParent = parentInfo.canonicalFilePath();
        const QString effectiveDestination =
            QDir(canonicalParent).absoluteFilePath(destinationInfo.fileName());
        const QString canonicalLibrary = QFileInfo(library).canonicalFilePath();
        if (xips::files::isWithin(effectiveDestination, canonicalLibrary))
        {
            return fail(action, QStringLiteral("--destination must be outside the asset library"),
                        3, index.problems);
        }
        QString copyError;
        if (!libraryService.copyVersionPayload(found, requestedVersion, effectiveDestination,
                                               &copiedPath, &copyError))
        {
            return fail(action, copyError, 3, index.problems);
        }
        data.insert(QStringLiteral("access"), QStringLiteral("materialized-copy"));
        data.insert(QStringLiteral("immutable"), false);
        data.insert(QStringLiteral("sourceImmutable"), true);
        data.insert(QStringLiteral("editable"), true);
        data.insert(QStringLiteral("materialized"), true);
        data.insert(QStringLiteral("materializedPath"), copiedPath);
        const QJsonArray files =
            plan.isSingleFile() ? QJsonArray{copiedPath} : resolvedFiles(copiedPath, plan.files);
        data.insert(QStringLiteral("resolvedPath"),
                    plan.isSingleFile() ? QFileInfo(copiedPath).absolutePath() : copiedPath);
        data.insert(QStringLiteral("resolvedFiles"), files);
        if (files.size() == 1)
        {
            data.insert(QStringLiteral("resolvedFile"), files.at(0));
        }
    }

    const xips::CopyPlan confirmed = libraryService.copyPlan(found, requestedVersion);
    if (!sameCopyPlan(plan, confirmed))
    {
        const QString retained =
            copiedPath.isEmpty()
                ? QString()
                : QStringLiteral("; materialized copy remains at %1").arg(copiedPath);
        return fail(
            action,
            confirmed.ok()
                ? QStringLiteral("Asset content changed while resolve output was prepared%1")
                      .arg(retained)
                : confirmed.error + retained,
            3, index.problems);
    }
    attachProblems(data, index.problems);
    writeJson(success(action, data), output);
    return 0;
}
