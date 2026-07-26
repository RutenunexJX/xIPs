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
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <limits>

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
    QStringList unique;
    QSet<QString> seen;
    for (QString &tag : tags) {
        tag = tag.trimmed();
        const QString key = tag.toCaseFolded();
        if (!tag.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            unique.append(tag);
        }
    }
    std::sort(unique.begin(), unique.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return unique;
}

Manifest makeManifest(const AssetMetadata &metadata)
{
    Manifest manifest;
    manifest.id = metadata.id.trimmed();
    manifest.name = metadata.name.trimmed();
    manifest.description = metadata.description.trimmed();
    manifest.tags = cleanedTags(metadata.tags);
    return manifest;
}

AssetMetadata withAvailableIdentity(AssetMetadata metadata,
                                    const QString &libraryRoot,
                                    const QSet<QString> &existingIds)
{
    const QString baseId = metadata.id;
    const QString baseName = metadata.name;
    int suffix = 2;
    while (existingIds.contains(metadata.id.toCaseFolded())
           || QFileInfo::exists(
               QDir(libraryRoot).absoluteFilePath(metadata.id))) {
        metadata.id = QStringLiteral("%1_%2").arg(baseId).arg(suffix);
        metadata.name = QStringLiteral("%1 (%2)").arg(baseName).arg(suffix);
        ++suffix;
    }
    return metadata;
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

AssetMetadata AssetLibraryService::suggestedMetadata(const QString &sourcePath)
{
    const QFileInfo source(sourcePath);
    AssetMetadata metadata;
    metadata.name = source.fileName();
    metadata.id = suggestedId(metadata.name);

    if (!source.isDir()) {
        return metadata;
    }
    const QString manifestPath = QDir(sourcePath).absoluteFilePath(
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

QString AssetLibraryService::suggestedNextVersion(const QString &currentVersion)
{
    static const QRegularExpression semanticVersion(
        QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)$"));
    const QRegularExpressionMatch match = semanticVersion.match(
        currentVersion.trimmed());
    if (!match.hasMatch()) {
        return QStringLiteral("1.0.0");
    }
    bool ok = false;
    const qulonglong patch = match.captured(3).toULongLong(&ok);
    if (!ok || patch == std::numeric_limits<qulonglong>::max()) {
        return currentVersion.trimmed() + QStringLiteral(".1");
    }
    return QStringLiteral("%1.%2.%3")
        .arg(match.captured(1), match.captured(2), QString::number(patch + 1));
}

bool AssetLibraryService::importAsset(const ImportAssetRequest &request,
                                      AssetRecord *created,
                                      QString *error) const
{
    const QString sourcePath = files::normalizedAbsolute(request.sourcePath);
    const QString libraryRoot = files::normalizedAbsolute(request.libraryRoot);
    const QFileInfo sourceInfo(sourcePath);
    if ((!sourceInfo.isDir() && !sourceInfo.isFile())
        || files::isLinkLike(sourceInfo)) {
        return fail(error,
                    QStringLiteral("Source must be a regular file or directory"));
    }
    if (sourceInfo.isFile()
        && (sourceInfo.fileName() == QStringLiteral(".xips.json")
            || sourceInfo.fileName() == QStringLiteral(".snapshot.json"))) {
        return fail(error,
                    QStringLiteral("xIPs metadata files cannot be imported as payload"));
    }
    if (!QDir().mkpath(libraryRoot)) {
        return fail(error, QStringLiteral("Cannot create the asset library directory"));
    }
    if (sourceInfo.isDir() && files::isWithin(libraryRoot, sourcePath)) {
        return fail(error,
                    QStringLiteral("The asset library cannot be inside the imported directory"));
    }

    Manifest manifest = makeManifest(request.metadata);
    const QStringList validationErrors = ManifestService().validate(manifest);
    if (!validationErrors.isEmpty()) {
        return fail(error, validationErrors.first());
    }

    const QString targetRoot = QDir(libraryRoot).absoluteFilePath(manifest.id);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("An asset already exists: %1").arg(targetRoot));
    }
    const QString stagingName = QStringLiteral(".xips-create-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stagingRoot = QDir(libraryRoot).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the import staging directory"));
    }

    QString operationError;
    const bool copied = sourceInfo.isDir()
                            ? copyPayload(sourcePath, stagingRoot, &operationError)
                            : QFile::copy(
                                  sourcePath,
                                  QDir(stagingRoot).absoluteFilePath(
                                      sourceInfo.fileName()));
    if (!copied && operationError.isEmpty()) {
        operationError = QStringLiteral("Cannot copy source file into the asset");
    }
    if (!copied
        || !ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }
    if (!QDir(libraryRoot).rename(stagingName, manifest.id)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, QStringLiteral("Cannot publish the imported asset"));
    }

    if (created) {
        const ScanResult scan = AssetScanner().scan(targetRoot);
        if (!scan.assets.isEmpty()) {
            *created = scan.assets.first();
        }
    }
    return true;
}

ImportBatchResult AssetLibraryService::importAssets(
    const QString &libraryRoot,
    const QStringList &sourcePaths,
    const QStringList &groups) const
{
    ImportBatchResult result;
    QSet<QString> existingIds;
    if (QFileInfo(libraryRoot).isDir()) {
        const ScanResult scan = AssetScanner().scan(libraryRoot);
        for (const AssetRecord &asset : scan.assets) {
            existingIds.insert(asset.manifest.id.toCaseFolded());
        }
    }
    for (const QString &sourcePath : sourcePaths) {
        AssetMetadata metadata = withAvailableIdentity(
            suggestedMetadata(sourcePath), libraryRoot, existingIds);
        metadata.tags.append(groups);
        AssetRecord created;
        QString error;
        if (importAsset({.libraryRoot = libraryRoot,
                         .sourcePath = sourcePath,
                         .metadata = metadata},
                        &created,
                        &error)) {
            result.created.append(created);
            existingIds.insert(created.manifest.id.toCaseFolded());
        } else {
            result.errors.append(
                QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(sourcePath),
                                              error));
        }
    }
    return result;
}

bool AssetLibraryService::updateMetadata(const AssetRecord &asset,
                                         const AssetMetadata &metadata,
                                         QString *error) const
{
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    if (!loaded.ok()) {
        return fail(error, QStringLiteral("Cannot reload the selected asset manifest"));
    }
    if (!metadata.id.trimmed().isEmpty()
        && metadata.id.trimmed() != loaded.manifest->id) {
        return fail(error, QStringLiteral("Asset id is stable and cannot be changed"));
    }

    Manifest manifest = *loaded.manifest;
    manifest.name = metadata.name.trimmed();
    manifest.description = metadata.description.trimmed();
    manifest.tags = cleanedTags(metadata.tags);
    return ManifestService().write(asset.manifestPath, manifest, error);
}

bool AssetLibraryService::changeGroupMembership(
    const QList<AssetRecord> &assets,
    const QString &oldGroup,
    const QString &newGroup,
    int *changed,
    QString *error) const
{
    const QString oldName = oldGroup.trimmed();
    const QString newName = newGroup.trimmed();
    if (oldName.isEmpty() && newName.isEmpty()) {
        return fail(error, QStringLiteral("A group name is required"));
    }

    struct PendingWrite {
        QString path;
        Manifest before;
        Manifest after;
    };
    QList<PendingWrite> writes;
    for (const AssetRecord &asset : assets) {
        const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
        if (!loaded.ok()) {
            return fail(error,
                        QStringLiteral("Cannot reload asset '%1'")
                            .arg(asset.manifest.name));
        }
        Manifest updated = *loaded.manifest;
        QStringList groups = updated.tags;
        bool assetChanged = false;
        if (oldName.isEmpty()) {
            const bool alreadyAssigned = std::any_of(
                groups.cbegin(), groups.cend(), [&newName](const QString &group) {
                    return group.trimmed().compare(newName,
                                                   Qt::CaseInsensitive) == 0;
                });
            if (!alreadyAssigned) {
                groups.append(newName);
                assetChanged = true;
            }
        } else {
            for (QString &group : groups) {
                if (group.trimmed().compare(oldName, Qt::CaseInsensitive) == 0) {
                    group = newName;
                    assetChanged = true;
                }
            }
        }
        updated.tags = cleanedTags(groups);
        if (assetChanged) {
            writes.append({.path = asset.manifestPath,
                           .before = *loaded.manifest,
                           .after = std::move(updated)});
        }
    }

    int completed = 0;
    for (const PendingWrite &write : writes) {
        QString writeError;
        if (!ManifestService().write(write.path, write.after, &writeError)) {
            for (int index = completed - 1; index >= 0; --index) {
                QString rollbackError;
                ManifestService().write(writes.at(index).path,
                                        writes.at(index).before,
                                        &rollbackError);
            }
            return fail(error, writeError);
        }
        ++completed;
    }
    if (changed) {
        *changed = completed;
    }
    return true;
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

WorkingCopyState AssetLibraryService::workingCopyState(
    const AssetRecord &asset) const
{
    WorkingCopyState state;
    QString versionsError;
    const QList<VersionInfo> savedVersions = versions(asset.assetRoot,
                                                       &versionsError);
    if (!versionsError.isEmpty()) {
        state.error = versionsError;
        return state;
    }
    if (savedVersions.isEmpty()) {
        return state;
    }
    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    if (!loaded.ok()) {
        state.error = QStringLiteral("Cannot reload the selected asset manifest");
        return state;
    }
    state.hasSavedVersion = true;
    state.latestVersion = savedVersions.first().version;
    Manifest comparable = *loaded.manifest;
    comparable.version = state.latestVersion;
    state.changed = AssetScanner::contentHash(comparable, asset.assetRoot)
                    != savedVersions.first().contentHash;
    return state;
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
        return fail(error, QStringLiteral("Cannot reload the selected asset manifest"));
    }
    const WorkingCopyState state = workingCopyState(asset);
    if (!state.error.isEmpty()) {
        return fail(error, state.error);
    }
    if (state.hasSavedVersion && !state.changed) {
        return fail(error,
                    QStringLiteral("The working copy has not changed since version %1")
                        .arg(state.latestVersion));
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

bool AssetLibraryService::deleteVersion(const AssetRecord &asset,
                                        const QString &version,
                                        const VersionDeleteMode mode,
                                        QString *error) const
{
    const QString cleanVersion = version.trimmed();
    if (cleanVersion.isEmpty()) {
        return fail(error, QStringLiteral("The working copy cannot be deleted here"));
    }
    QString versionsError;
    const QList<VersionInfo> available = versions(asset.assetRoot, &versionsError);
    if (!versionsError.isEmpty()) {
        return fail(error, versionsError);
    }
    const auto found = std::find_if(
        available.cbegin(), available.cend(), [&cleanVersion](const VersionInfo &entry) {
            return entry.version == cleanVersion;
        });
    if (found == available.cend()) {
        return fail(error,
                    QStringLiteral("Version not found: %1").arg(cleanVersion));
    }

    const ManifestLoadResult loaded = ManifestService().load(asset.manifestPath);
    if (!loaded.ok()) {
        return fail(error, QStringLiteral("Cannot reload the selected asset manifest"));
    }
    Manifest updated = *loaded.manifest;
    const bool updateCurrent = updated.version == cleanVersion;
    if (updateCurrent) {
        updated.version.clear();
        for (const VersionInfo &candidate : available) {
            if (candidate.version != cleanVersion) {
                updated.version = candidate.version;
                break;
            }
        }
        QString manifestError;
        if (!ManifestService().write(asset.manifestPath, updated, &manifestError)) {
            return fail(error, manifestError);
        }
    }

    bool removed = false;
    if (mode == VersionDeleteMode::MoveToTrash) {
        removed = QFile::moveToTrash(found->path);
    } else {
        removed = QDir(found->path).removeRecursively();
    }
    if (!removed) {
        if (updateCurrent) {
            QString rollbackError;
            ManifestService().write(asset.manifestPath,
                                    *loaded.manifest,
                                    &rollbackError);
        }
        return fail(error,
                    mode == VersionDeleteMode::MoveToTrash
                        ? QStringLiteral("Cannot move the saved version to the trash")
                        : QStringLiteral("Cannot delete the saved version"));
    }
    return true;
}

bool AssetLibraryService::copyVersionPayload(
    const AssetRecord &asset,
    const QString &version,
    const QString &destinationDirectory,
    QString *copiedPath,
    QString *error) const
{
    QString sourceRoot = asset.assetRoot;
    if (!version.trimmed().isEmpty()) {
        QString versionsError;
        const QList<VersionInfo> available = versions(asset.assetRoot,
                                                       &versionsError);
        if (!versionsError.isEmpty()) {
            return fail(error, versionsError);
        }
        const auto found = std::find_if(
            available.cbegin(), available.cend(), [&version](const VersionInfo &entry) {
                return entry.version == version.trimmed();
            });
        if (found == available.cend()) {
            return fail(error,
                        QStringLiteral("Version not found: %1")
                            .arg(version.trimmed()));
        }
        sourceRoot = found->path;
    }

    const QString destinationRoot = files::normalizedAbsolute(
        destinationDirectory);
    if (!QFileInfo(destinationRoot).isDir()) {
        return fail(error, QStringLiteral("Copy destination is not a directory"));
    }
    if (files::isWithin(destinationRoot, sourceRoot)) {
        return fail(error,
                    QStringLiteral("Copy destination cannot be inside the source asset"));
    }
    const QStringList payload = AssetScanner::assetFiles(sourceRoot);
    if (payload.isEmpty()) {
        return fail(error, QStringLiteral("The selected asset has no payload files"));
    }

    if (payload.size() == 1) {
        const QString source = QDir(sourceRoot).absoluteFilePath(payload.first());
        const QString target = QDir(destinationRoot).absoluteFilePath(
            QFileInfo(payload.first()).fileName());
        if (QFileInfo::exists(target)) {
            return fail(error,
                        QStringLiteral("Copy destination already exists: %1")
                            .arg(target));
        }
        if (!QFile::copy(source, target)) {
            return fail(error,
                        QStringLiteral("Cannot copy %1 to %2").arg(source, target));
        }
        if (copiedPath) {
            *copiedPath = target;
        }
        return true;
    }

    const QString targetRoot = QDir(destinationRoot).absoluteFilePath(
        asset.manifest.id);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("Copy destination already exists: %1")
                        .arg(targetRoot));
    }
    const QString stagingName = QStringLiteral(".xips-copy-%1")
                                    .arg(QUuid::createUuid().toString(
                                        QUuid::WithoutBraces));
    const QString stagingRoot = QDir(destinationRoot).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the copy staging directory"));
    }
    QString copyError;
    if (!copyPayload(sourceRoot, stagingRoot, &copyError)
        || !QDir(destinationRoot).rename(stagingName, asset.manifest.id)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    copyError.isEmpty()
                        ? QStringLiteral("Cannot publish the copied asset")
                        : copyError);
    }
    if (copiedPath) {
        *copiedPath = targetRoot;
    }
    return true;
}

} // namespace xips
