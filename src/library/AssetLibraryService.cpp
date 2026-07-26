#include "library/AssetLibraryService.h"

#include "library/AssetScanner.h"
#include "library/FileSystemUtil.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace xips {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool copyPayload(const QString &sourceRoot,
                 const QString &destinationRoot,
                 QString *error)
{
    QStringList payloadFiles;
    if (!files::collectPayloadFiles(sourceRoot,
                                    payloadFiles,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    for (const QString &relative : payloadFiles) {
        const QString source = QDir(sourceRoot).absoluteFilePath(relative);
        const QString destination = QDir(destinationRoot).absoluteFilePath(relative);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            return fail(error,
                        QStringLiteral("Cannot create destination directory for %1")
                            .arg(destination));
        }
        if (!QFile::copy(source, destination)) {
            return fail(error,
                        QStringLiteral("Cannot copy %1 to %2")
                            .arg(source, destination));
        }
    }
    return true;
}

QStringList cleanedTags(QStringList tags)
{
    for (QString &tag : tags) {
        tag = tag.trimmed();
    }
    tags.removeAll(QString());
    tags.removeDuplicates();
    std::sort(tags.begin(), tags.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return tags;
}

Manifest makeManifest(const IpMetadata &metadata)
{
    Manifest manifest;
    manifest.id = metadata.id.trimmed();
    manifest.name = metadata.name.trimmed();
    manifest.description = metadata.description.trimmed();
    manifest.tags = cleanedTags(metadata.tags);
    return manifest;
}

bool validVersion(const QString &version)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]*$"));
    return pattern.match(version.trimmed()).hasMatch();
}

bool writeSnapshotMetadata(const QString &path,
                           const VersionInfo &version,
                           QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error,
                    QStringLiteral("Cannot create version metadata: %1")
                        .arg(file.errorString()));
    }
    const QJsonObject object{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("version"), version.version},
        {QStringLiteral("createdAt"), version.createdAt.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("contentHash"), version.contentHash},
    };
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        return fail(error,
                    QStringLiteral("Cannot publish version metadata: %1")
                        .arg(file.errorString()));
    }
    return true;
}

} // namespace

QString AssetLibraryService::suggestedId(const QString &text)
{
    QString result;
    bool previousSeparator = false;
    for (const QChar character : text.toLower()) {
        if ((character >= u'a' && character <= u'z')
            || (character >= u'0' && character <= u'9')) {
            result.append(character);
            previousSeparator = false;
        } else if (!result.isEmpty() && !previousSeparator) {
            result.append(u'_');
            previousSeparator = true;
        }
    }
    while (result.endsWith(u'_')) {
        result.chop(1);
    }
    return result.isEmpty() ? QStringLiteral("ip") : result;
}

IpMetadata AssetLibraryService::suggestedMetadata(const QString &sourceDirectory)
{
    const QFileInfo directory(sourceDirectory);
    IpMetadata metadata;
    metadata.name = directory.fileName();
    metadata.id = suggestedId(metadata.name);

    const QString manifestPath = QDir(sourceDirectory).absoluteFilePath(
        QStringLiteral(".xips.json"));
    if (QFileInfo(manifestPath).isFile()) {
        const ManifestLoadResult loaded = ManifestService().load(manifestPath);
        if (loaded.ok()) {
            metadata.id = loaded.manifest->id;
            metadata.name = loaded.manifest->name;
            metadata.description = loaded.manifest->description;
            metadata.tags = loaded.manifest->tags;
        }
    }
    return metadata;
}

bool AssetLibraryService::importIp(const ImportIpRequest &request,
                                   AssetRecord *created,
                                   QString *error) const
{
    const QString sourceRoot = files::normalizedAbsolute(request.sourceDirectory);
    const QString libraryRoot = files::normalizedAbsolute(request.libraryRoot);
    if (!QFileInfo(sourceRoot).isDir()) {
        return fail(error, QStringLiteral("Source directory does not exist"));
    }
    if (!QDir().mkpath(libraryRoot)) {
        return fail(error, QStringLiteral("Cannot create the IP library directory"));
    }
    if (files::isWithin(libraryRoot, sourceRoot)) {
        return fail(error,
                    QStringLiteral("The IP library cannot be inside the imported directory"));
    }

    Manifest manifest = makeManifest(request.metadata);
    const QStringList validationErrors = ManifestService().validate(manifest);
    if (!validationErrors.isEmpty()) {
        return fail(error, validationErrors.first());
    }

    const QString targetRoot = QDir(libraryRoot).absoluteFilePath(manifest.id);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("An IP directory already exists: %1").arg(targetRoot));
    }
    const QString stagingName = QStringLiteral(".xips-create-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingRoot = QDir(libraryRoot).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the import staging directory"));
    }

    QString operationError;
    if (!copyPayload(sourceRoot, stagingRoot, &operationError)
        || !ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }
    if (!QDir(libraryRoot).rename(stagingName, manifest.id)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, QStringLiteral("Cannot publish the imported IP directory"));
    }

    if (created) {
        const ScanResult scan = AssetScanner().scan(targetRoot);
        if (!scan.assets.isEmpty()) {
            *created = scan.assets.first();
        }
    }
    return true;
}

bool AssetLibraryService::updateMetadata(const AssetRecord &asset,
                                         const IpMetadata &metadata,
                                         QString *error) const
{
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    if (!loaded.ok()) {
        return fail(error, QStringLiteral("Cannot reload the selected IP manifest"));
    }
    if (!metadata.id.trimmed().isEmpty()
        && metadata.id.trimmed() != loaded.manifest->id) {
        return fail(error, QStringLiteral("IP id is stable and cannot be changed"));
    }

    Manifest manifest = *loaded.manifest;
    manifest.name = metadata.name.trimmed();
    manifest.description = metadata.description.trimmed();
    manifest.tags = cleanedTags(metadata.tags);
    return ManifestService().write(asset.manifestPath, manifest, error);
}

QList<VersionInfo> AssetLibraryService::versions(const QString &assetRoot,
                                                 QString *error) const
{
    QList<VersionInfo> result;
    const QString root = QDir(assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    if (!QFileInfo(root).isDir()) {
        return result;
    }
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().startsWith(u'.')) {
            continue;
        }
        QFile metadata(QDir(entry.absoluteFilePath()).absoluteFilePath(
            QStringLiteral(".snapshot.json")));
        if (!metadata.open(QIODevice::ReadOnly)) {
            fail(error,
                 QStringLiteral("Cannot read version metadata: %1")
                     .arg(metadata.fileName()));
            return {};
        }
        const QJsonDocument document = QJsonDocument::fromJson(metadata.readAll());
        if (!document.isObject()) {
            fail(error,
                 QStringLiteral("Invalid version metadata: %1")
                     .arg(metadata.fileName()));
            return {};
        }
        const QJsonObject object = document.object();
        VersionInfo version;
        version.version = object.value(QStringLiteral("version")).toString();
        version.createdAt = QDateTime::fromString(
            object.value(QStringLiteral("createdAt")).toString(),
            Qt::ISODateWithMs);
        version.contentHash = object.value(QStringLiteral("contentHash")).toString();
        version.path = entry.absoluteFilePath();
        if (version.version.isEmpty() || !version.createdAt.isValid()
            || version.contentHash.isEmpty()) {
            fail(error,
                 QStringLiteral("Incomplete version metadata: %1")
                     .arg(metadata.fileName()));
            return {};
        }
        result.append(version);
    }
    std::sort(result.begin(), result.end(), [](const VersionInfo &left,
                                                const VersionInfo &right) {
        if (left.createdAt != right.createdAt) {
            return left.createdAt > right.createdAt;
        }
        return left.version > right.version;
    });
    return result;
}

bool AssetLibraryService::createVersion(const AssetRecord &asset,
                                        const QString &version,
                                        VersionInfo *created,
                                        QString *error) const
{
    const QString cleanVersion = version.trimmed();
    if (!validVersion(cleanVersion)) {
        return fail(error,
                    QStringLiteral("Version must use letters, digits, '.', '_', '+', or '-'"));
    }
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    if (!loaded.ok()) {
        return fail(error, QStringLiteral("Cannot reload the selected IP manifest"));
    }

    const QString versionsRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    if (!QDir().mkpath(versionsRoot)) {
        return fail(error, QStringLiteral("Cannot create the versions directory"));
    }
    const QString targetRoot = QDir(versionsRoot).absoluteFilePath(cleanVersion);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("Version already exists: %1").arg(cleanVersion));
    }
    const QString stagingName = QStringLiteral(".staging-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingRoot = QDir(versionsRoot).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the version staging directory"));
    }

    QString operationError;
    if (!copyPayload(asset.assetRoot, stagingRoot, &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }
    Manifest snapshotManifest = *loaded.manifest;
    snapshotManifest.version = cleanVersion;
    const QString snapshotManifestPath = QDir(stagingRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    if (!ManifestService().write(snapshotManifestPath,
                                 snapshotManifest,
                                 &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }

    VersionInfo versionInfo;
    versionInfo.version = cleanVersion;
    versionInfo.createdAt = QDateTime::currentDateTimeUtc();
    versionInfo.contentHash = AssetScanner::contentHash(snapshotManifest, stagingRoot);
    versionInfo.path = targetRoot;
    if (!writeSnapshotMetadata(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".snapshot.json")),
            versionInfo,
            &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }

    Manifest currentManifest = *loaded.manifest;
    currentManifest.version = cleanVersion;
    if (!ManifestService().write(asset.manifestPath,
                                 currentManifest,
                                 &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }
    if (!QDir(versionsRoot).rename(stagingName, cleanVersion)) {
        QString rollbackError;
        ManifestService().write(asset.manifestPath,
                                *loaded.manifest,
                                &rollbackError);
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    rollbackError.isEmpty()
                        ? QStringLiteral("Cannot publish the version snapshot")
                        : QStringLiteral("Cannot publish the version snapshot; manifest rollback also failed: %1")
                              .arg(rollbackError));
    }
    if (created) {
        *created = versionInfo;
    }
    return true;
}

bool AssetLibraryService::exportVersion(const AssetRecord &asset,
                                        const QString &version,
                                        const QString &destination,
                                        QString *error) const
{
    QString sourceRoot = asset.assetRoot;
    if (!version.trimmed().isEmpty()) {
        QString versionsError;
        const QList<VersionInfo> available = versions(asset.assetRoot, &versionsError);
        if (!versionsError.isEmpty()) {
            return fail(error, versionsError);
        }
        const auto found = std::find_if(
            available.cbegin(), available.cend(), [&version](const VersionInfo &entry) {
                return entry.version == version.trimmed();
            });
        if (found == available.cend()) {
            return fail(error,
                        QStringLiteral("Version not found: %1").arg(version.trimmed()));
        }
        sourceRoot = found->path;
    }

    const QString targetRoot = files::normalizedAbsolute(destination);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("Export destination already exists: %1").arg(targetRoot));
    }
    if (files::isWithin(targetRoot, asset.assetRoot)) {
        return fail(error,
                    QStringLiteral("Export destination cannot be inside the source IP"));
    }
    const QString parent = QFileInfo(targetRoot).absolutePath();
    const QString targetName = QFileInfo(targetRoot).fileName();
    if (targetName.isEmpty() || !QDir().mkpath(parent)) {
        return fail(error, QStringLiteral("Cannot create the export parent directory"));
    }
    const QString stagingName = QStringLiteral(".xips-export-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingRoot = QDir(parent).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the export staging directory"));
    }

    QString operationError;
    if (!copyPayload(sourceRoot, stagingRoot, &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }
    const QString sourceManifest = QDir(sourceRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    const ManifestLoadResult loaded = ManifestService().load(sourceManifest);
    if (!loaded.ok()
        || !ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            loaded.ok() ? *loaded.manifest : asset.manifest,
            &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    operationError.isEmpty()
                        ? QStringLiteral("Cannot read the version manifest")
                        : operationError);
    }
    if (!QDir(parent).rename(stagingName, targetName)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, QStringLiteral("Cannot publish the exported IP"));
    }
    return true;
}

} // namespace xips
