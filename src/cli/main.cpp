#include "assetcore/JsonUtil.h"
#include "library/AssetScanner.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

namespace {

void writeJson(const QJsonObject &object, QTextStream &stream)
{
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact) << u'\n';
    stream.flush();
}

int fail(const QString &action,
         const QString &message,
         const int exitCode = 2)
{
    QTextStream stream(stderr);
    writeJson(QJsonObject{
                  {QStringLiteral("schemaVersion"), 1},
                  {QStringLiteral("ok"), false},
                  {QStringLiteral("action"), action},
                  {QStringLiteral("error"), message},
              },
              stream);
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
        {QStringLiteral("description"), asset.manifest.description},
        {QStringLiteral("tags"), xips::json::toArray(asset.manifest.tags)},
        {QStringLiteral("files"), xips::json::toArray(asset.files)},
        {QStringLiteral("path"), asset.assetRoot},
        {QStringLiteral("fileCount"), static_cast<qint64>(asset.fileCount)},
        {QStringLiteral("uri"),
         xips::IntegrationService::assetUri(asset.manifest.id)
             .toString(QUrl::FullyEncoded)},
    };
}

QJsonArray resolvedFiles(const QString &root,
                         const QStringList &relativeFiles)
{
    QJsonArray result;
    for (const QString &relative : relativeFiles) {
        result.append(QDir(root).absoluteFilePath(relative));
    }
    return result;
}

QJsonArray resolvedFiles(const QString &root)
{
    return resolvedFiles(root, xips::AssetScanner::assetFiles(root));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xips-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(XIPS_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Read-only bridge for the xIPs FPGA asset library"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption actionOption(
        QStringLiteral("action"),
        QStringLiteral("Action: list or resolve."),
        QStringLiteral("name"),
        QStringLiteral("list"));
    const QCommandLineOption libraryOption(
        {QStringLiteral("l"), QStringLiteral("library")},
        QStringLiteral("Asset library directory."),
        QStringLiteral("directory"));
    const QCommandLineOption assetOption(
        QStringLiteral("asset"),
        QStringLiteral("Stable asset id."),
        QStringLiteral("id"));
    const QCommandLineOption versionOption(
        QStringLiteral("asset-version"),
        QStringLiteral("Saved asset version."),
        QStringLiteral("version"));
    const QCommandLineOption queryOption(
        QStringLiteral("query"),
        QStringLiteral("Search text."),
        QStringLiteral("text"));
    parser.addOption(actionOption);
    parser.addOption(libraryOption);
    parser.addOption(assetOption);
    parser.addOption(versionOption);
    parser.addOption(queryOption);
    if (!parser.parse(application.arguments())) {
        QTextStream(stderr) << parser.errorText() << u'\n';
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QTextStream(stdout) << QCoreApplication::applicationName() << u' '
                            << QCoreApplication::applicationVersion() << u'\n';
        return 0;
    }

    const QString action = parser.value(actionOption).trimmed().toLower();
    QTextStream output(stdout);
    if (action != QStringLiteral("list")
        && action != QStringLiteral("resolve")) {
        return fail(action, QStringLiteral("Unknown action"));
    }

    QString libraryValue = parser.value(libraryOption);
    if (libraryValue.isEmpty()) {
        libraryValue = qEnvironmentVariable("XIPS_LIBRARY");
    }
    if (libraryValue.isEmpty()) {
        return fail(action, QStringLiteral("--library or XIPS_LIBRARY is required"));
    }
    const QString library = QFileInfo(libraryValue).absoluteFilePath();
    if (!QFileInfo(library).isDir()) {
        return fail(action,
                    QStringLiteral("--library must name an existing directory"),
                    3);
    }
    const xips::ScanResult scan = xips::AssetScanner().scan(library);
    if (!scan.errors.isEmpty()) {
        return fail(action,
                    QStringLiteral("Library scan failed: %1")
                        .arg(scan.errors.first()),
                    3);
    }

    if (action == QStringLiteral("list")) {
        const QString query = xips::json::normalizeSearchText(
            parser.value(queryOption));
        const QStringList terms = query.split(u' ', Qt::SkipEmptyParts);
        QJsonArray assets;
        for (const xips::AssetRecord &asset : scan.assets) {
            const QString searchable = xips::json::normalizeSearchText(
                QStringList{
                    asset.manifest.id,
                    asset.manifest.name,
                    asset.manifest.version,
                    asset.manifest.description,
                    asset.manifest.tags.join(u' '),
                    asset.files.join(u' '),
                }.join(u' '));
            bool matches = true;
            for (const QString &term : terms) {
                if (!searchable.contains(term)) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                assets.append(assetJson(asset));
            }
        }
        writeJson(success(action,
                          QJsonObject{
                              {QStringLiteral("count"), assets.size()},
                              {QStringLiteral("assets"), assets},
                          }),
                  output);
        return 0;
    }

    const QString assetId = parser.value(assetOption).trimmed();
    if (assetId.isEmpty()) {
        return fail(action, QStringLiteral("resolve requires --asset"));
    }
    const auto found = std::find_if(
        scan.assets.cbegin(), scan.assets.cend(), [&assetId](const xips::AssetRecord &asset) {
            return asset.manifest.id == assetId;
        });
    if (found == scan.assets.cend()) {
        return fail(action, QStringLiteral("Asset not found: %1").arg(assetId), 4);
    }

    QJsonObject data = assetJson(*found);
    const QString requestedVersion = parser.value(versionOption).trimmed();
    QString resolvedPath;
    xips::CopyPlan savedPlan;
    if (!requestedVersion.isEmpty()) {
        savedPlan = xips::AssetLibraryService().copyPlan(*found,
                                                         requestedVersion);
        if (!savedPlan.ok()) {
            return fail(action,
                        savedPlan.error,
                        savedPlan.error.startsWith(
                            QStringLiteral("Version not found:"))
                            ? 4
                            : 3);
        }
        resolvedPath = savedPlan.sourceRoot;
        data.insert(QStringLiteral("resolvedVersion"), savedPlan.version);
        data.insert(QStringLiteral("resolvedPath"), resolvedPath);
        data.insert(QStringLiteral("resolvedContentHash"), savedPlan.contentHash);
    } else {
        resolvedPath = found->assetRoot;
        data.insert(QStringLiteral("resolvedVersion"), QStringLiteral("working"));
        data.insert(QStringLiteral("resolvedPath"), resolvedPath);
        data.insert(QStringLiteral("resolvedContentHash"),
                    xips::AssetScanner::contentHash(found->manifest,
                                                    found->assetRoot));
    }
    const QJsonArray files = requestedVersion.isEmpty()
                                 ? resolvedFiles(resolvedPath)
                                 : resolvedFiles(resolvedPath, savedPlan.files);
    data.insert(QStringLiteral("resolvedFiles"), files);
    if (files.size() == 1) {
        data.insert(QStringLiteral("resolvedFile"), files.at(0));
    }
    if (!requestedVersion.isEmpty()) {
        const xips::CopyPlan confirmed = xips::AssetLibraryService().copyPlan(
            *found,
            requestedVersion);
        if (!confirmed.ok()
            || confirmed.sourceRoot != savedPlan.sourceRoot
            || confirmed.files != savedPlan.files
            || confirmed.contentHash != savedPlan.contentHash
            || confirmed.strictContentHash != savedPlan.strictContentHash
            || confirmed.payloadFingerprint != savedPlan.payloadFingerprint
            || confirmed.proofFingerprint != savedPlan.proofFingerprint) {
            return fail(action,
                        confirmed.ok()
                            ? QStringLiteral(
                                  "Saved version changed while resolve output was prepared")
                            : confirmed.error,
                        3);
        }
    }
    writeJson(success(action, data), output);
    return 0;
}
