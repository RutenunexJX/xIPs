#include "library/AssetLibraryService.h"

#include "assetcore/JsonUtil.h"
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
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

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

struct SourcePayload {
    QString root;
    QStringList files;
    bool singleFile = false;
};

bool resolveSourcePayload(const QString &sourcePath,
                          SourcePayload &payload,
                          QString *error)
{
    const QString absolute = files::normalizedAbsolute(sourcePath);
    const QFileInfo info(absolute);
    if ((!info.isFile() && !info.isDir()) || files::isLinkLike(info)) {
        return fail(error,
                    QStringLiteral("Source must be a regular file or directory"));
    }
    if (info.isFile()) {
        if (info.fileName() == QStringLiteral(".xips.json")
            || info.fileName() == QStringLiteral(".snapshot.json")) {
            return fail(error,
                        QStringLiteral("xIPs metadata files cannot be used as payload"));
        }
        payload.root = info.absolutePath();
        payload.files = {info.fileName()};
        payload.singleFile = true;
        return true;
    }
    payload.root = absolute;
    if (!files::collectPayloadFiles(payload.root,
                                    payload.files,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    if (payload.files.isEmpty()) {
        return fail(error, QStringLiteral("Source directory has no payload files"));
    }
    return true;
}

bool filesEqual(const QString &leftPath, const QString &rightPath)
{
    const QFileInfo leftInfo(leftPath);
    const QFileInfo rightInfo(rightPath);
    if (!leftInfo.isFile() || !rightInfo.isFile()
        || leftInfo.size() != rightInfo.size()) {
        return false;
    }
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)) {
        return false;
    }
    while (!left.atEnd() && !right.atEnd()) {
        if (left.read(1024 * 1024) != right.read(1024 * 1024)) {
            return false;
        }
    }
    return left.atEnd() && right.atEnd();
}

UpdatePreview comparePayload(const QString &assetRoot,
                             const SourcePayload &source)
{
    UpdatePreview preview;
    const QStringList currentFiles = AssetScanner::assetFiles(assetRoot);
    if (source.singleFile && currentFiles.size() != 1) {
        preview.error = QStringLiteral(
            "A single file can only update an asset with one payload file");
        return preview;
    }
    const QSet<QString> currentSet(currentFiles.cbegin(), currentFiles.cend());
    const QSet<QString> sourceSet(source.files.cbegin(), source.files.cend());
    for (const QString &relative : source.files) {
        if (!currentSet.contains(relative)) {
            preview.addedFiles.append(relative);
            continue;
        }
        const QString currentPath = QDir(assetRoot).absoluteFilePath(relative);
        const QString replacementPath = QDir(source.root).absoluteFilePath(relative);
        if (filesEqual(currentPath, replacementPath)) {
            ++preview.unchangedCount;
        } else {
            preview.replacedFiles.append(relative);
        }
    }
    for (const QString &relative : currentFiles) {
        if (!sourceSet.contains(relative)) {
            preview.removedFiles.append(relative);
        }
    }
    return preview;
}

bool validateAssetRecord(const AssetRecord &asset,
                         Manifest *manifest,
                         QString *error)
{
    const QString root = files::normalizedAbsolute(asset.assetRoot);
    const QFileInfo rootInfo(root);
    const QString expectedManifest = QDir(root).absoluteFilePath(
        QStringLiteral(".xips.json"));
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)
        || files::normalizedAbsolute(asset.manifestPath) != expectedManifest) {
        return fail(error, QStringLiteral("Selected asset path is invalid"));
    }
    const ManifestLoadResult loaded = ManifestService().load(expectedManifest);
    if (!loaded.ok() || loaded.manifest->id != asset.manifest.id) {
        return fail(error, QStringLiteral("Selected asset manifest does not match"));
    }
    if (manifest) {
        *manifest = *loaded.manifest;
    }
    return true;
}

bool strictFingerprintAtRoot(const QString &root,
                             const QString &expectedAssetId,
                             Manifest *manifest,
                             QString *fingerprint,
                             QString *error)
{
    if (manifest) {
        *manifest = {};
    }
    if (fingerprint) {
        fingerprint->clear();
    }
    const QString absoluteRoot = files::normalizedAbsolute(root);
    const ManifestService manifestService;
    const ManifestLoadResult loaded = manifestService.load(
        QDir(absoluteRoot).absoluteFilePath(QStringLiteral(".xips.json")));
    if (!loaded.ok() || loaded.manifest->id != expectedAssetId) {
        return fail(error,
                    QStringLiteral(
                        "The asset manifest changed or became unreadable during the operation"));
    }
    QString hash;
    if (!AssetScanner::strictContentHash(*loaded.manifest,
                                         absoluteRoot,
                                         &hash,
                                         error)) {
        return false;
    }
    const ManifestLoadResult confirmed = manifestService.load(
        QDir(absoluteRoot).absoluteFilePath(QStringLiteral(".xips.json")));
    if (!confirmed.ok() || confirmed.manifest->id != expectedAssetId
        || json::canonicalJson(manifestService.toJson(*confirmed.manifest))
               != json::canonicalJson(
                   manifestService.toJson(*loaded.manifest))) {
        return fail(error,
                    QStringLiteral(
                        "The asset manifest changed while its content was being verified"));
    }
    if (manifest) {
        *manifest = *confirmed.manifest;
    }
    if (fingerprint) {
        *fingerprint = hash;
    }
    return true;
}

bool collectAllRecoveryEntries(const QString &directory,
                               const QString &root,
                               QStringList &entries,
                               QString *error)
{
    const QFileInfoList children = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : children) {
        if (files::isLinkLike(entry)) {
            return fail(
                error,
                QStringLiteral(
                    "The recovery contains linked data and will not be discarded automatically: %1")
                    .arg(entry.absoluteFilePath()));
        }
        const QString relative = QDir::fromNativeSeparators(
            QDir(root).relativeFilePath(entry.absoluteFilePath()));
        if (entry.isDir()) {
            entries.append(QStringLiteral("d:%1").arg(relative));
            if (!collectAllRecoveryEntries(entry.absoluteFilePath(),
                                           root,
                                           entries,
                                           error)) {
                return false;
            }
        } else if (entry.isFile()) {
            entries.append(QStringLiteral("f:%1").arg(relative));
        } else {
            return fail(
                error,
                QStringLiteral(
                    "The recovery contains unsupported data and will not be discarded automatically: %1")
                    .arg(entry.absoluteFilePath()));
        }
    }
    return true;
}

bool verifiedRecoveryLayout(const QString &root,
                            QStringList &entries,
                            QString *error)
{
    entries.clear();
    const QString absoluteRoot = files::normalizedAbsolute(root);
    const QFileInfo rootInfo(absoluteRoot);
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        return fail(error,
                    QStringLiteral("The recovery root is invalid or linked"));
    }

    QStringList actualEntries;
    if (!collectAllRecoveryEntries(absoluteRoot,
                                   absoluteRoot,
                                   actualEntries,
                                   error)) {
        return false;
    }

    QStringList payloadFiles;
    if (!files::collectPayloadFiles(absoluteRoot,
                                    payloadFiles,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    QSet<QString> expectedEntries{
        QStringLiteral("f:.xips.json"),
    };
    for (const QString &payloadFile : payloadFiles) {
        const QString normalized = QDir::fromNativeSeparators(payloadFile);
        expectedEntries.insert(QStringLiteral("f:%1").arg(normalized));
        QString parent = QDir::fromNativeSeparators(QFileInfo(normalized).path());
        while (!parent.isEmpty() && parent != QStringLiteral(".")) {
            expectedEntries.insert(QStringLiteral("d:%1").arg(parent));
            const qsizetype separator = parent.lastIndexOf(u'/');
            parent = separator < 0 ? QString() : parent.left(separator);
        }
    }

    std::sort(actualEntries.begin(), actualEntries.end());
    actualEntries.removeDuplicates();
    QStringList expected = expectedEntries.values();
    std::sort(expected.begin(), expected.end());
    if (actualEntries != expected) {
        QString unexpected;
        for (const QString &entry : actualEntries) {
            if (!expectedEntries.contains(entry)) {
                unexpected = entry.mid(2);
                break;
            }
        }
        return fail(
            error,
            unexpected.isEmpty()
                ? QStringLiteral(
                      "The recovery layout changed while it was being verified")
                : QStringLiteral(
                      "The recovery contains unrecognized data and will not be discarded automatically: %1")
                      .arg(QDir(absoluteRoot).absoluteFilePath(unexpected)));
    }
    entries = actualEntries;
    return true;
}

bool strictRecoveryFingerprintAtRoot(const QString &root,
                                     const QString &expectedAssetId,
                                     Manifest *manifest,
                                     QString *fingerprint,
                                     QString *error)
{
    if (manifest) {
        *manifest = {};
    }
    if (fingerprint) {
        fingerprint->clear();
    }
    QStringList initialEntries;
    if (!verifiedRecoveryLayout(root, initialEntries, error)) {
        return false;
    }
    Manifest verifiedManifest;
    QString verifiedFingerprint;
    if (!strictFingerprintAtRoot(root,
                                 expectedAssetId,
                                 &verifiedManifest,
                                 &verifiedFingerprint,
                                 error)) {
        return false;
    }
    QStringList confirmedEntries;
    if (!verifiedRecoveryLayout(root, confirmedEntries, error)
        || confirmedEntries != initialEntries) {
        return fail(
            error,
            QStringLiteral(
                "The recovery layout changed while it was being verified"));
    }
    if (manifest) {
        *manifest = verifiedManifest;
    }
    if (fingerprint) {
        *fingerprint = verifiedFingerprint;
    }
    return true;
}

bool validateWorkingCopyRecovery(const AssetRecord &asset,
                                 const QString &recoveryPath,
                                 QString *error)
{
    const QString assetRoot = files::normalizedAbsolute(asset.assetRoot);
    const QString recovery = files::normalizedAbsolute(recoveryPath);
    const QFileInfo recoveryInfo(recovery);
    const QString recoveryParent = files::normalizedAbsolute(
        recoveryInfo.absolutePath());
    const QString assetParent = files::normalizedAbsolute(
        QFileInfo(assetRoot).absolutePath());
    static const QRegularExpression recoveryName(
        QStringLiteral(
            "^\\.xips-create-recovery-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"),
        QRegularExpression::CaseInsensitiveOption);
    if (!recoveryInfo.isDir() || files::isLinkLike(recoveryInfo)
        || !files::isWithin(recoveryParent, assetParent)
        || !files::isWithin(assetParent, recoveryParent)
        || !recoveryName.match(recoveryInfo.fileName()).hasMatch()) {
        return fail(error,
                    QStringLiteral("The working-copy recovery path is invalid"));
    }
    const QFileInfo recoveryVersions(
        QDir(recovery).absoluteFilePath(QStringLiteral(".xips")));
    if (recoveryVersions.exists() || files::isLinkLike(recoveryVersions)) {
        return fail(
            error,
            QStringLiteral(
                "The working-copy recovery contains saved-version data and will not be used or discarded automatically"));
    }
    const ManifestLoadResult loaded = ManifestService().load(
        QDir(recovery).absoluteFilePath(QStringLiteral(".xips.json")));
    if (!loaded.ok() || loaded.manifest->id != asset.manifest.id) {
        return fail(error,
                    QStringLiteral("The working-copy recovery does not match this asset"));
    }
    return true;
}

bool samePath(const QString &left, const QString &right)
{
    return !left.isEmpty() && !right.isEmpty()
           && files::isWithin(left, right) && files::isWithin(right, left);
}

bool validFingerprint(const QString &fingerprint)
{
    static const QRegularExpression pattern(
        QStringLiteral("^sha256:[0-9a-f]{64}$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(fingerprint).hasMatch();
}

bool validateWorkingCopyUndoToken(const AssetRecord &asset,
                                  const WorkingCopyUndoToken &token,
                                  QString *error)
{
    if (!token.isValid() || token.assetId != asset.manifest.id
        || !samePath(token.assetRoot, asset.assetRoot)
        || !validFingerprint(token.publishedFingerprint)
        || !validFingerprint(token.recoveryFingerprint)) {
        return fail(error,
                    QStringLiteral("The working-copy Undo token is invalid"));
    }
    return validateWorkingCopyRecovery(asset,
                                       token.recoveryPath,
                                       error);
}

void appendWarning(QString &warning, const QString &message)
{
    if (!warning.isEmpty()) {
        warning += QStringLiteral("; ");
    }
    warning += message;
}

QString safeCopyName(QString name, const QString &fallback)
{
    static const QRegularExpression invalid(
        QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    name = name.trimmed();
    name.replace(invalid, QStringLiteral("_"));
    while (name.endsWith(u'.') || name.endsWith(u' ')) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        name = fallback;
    }
    static const QSet<QString> reserved{
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    if (reserved.contains(name.section(u'.', 0, 0).toUpper())) {
        name.prepend(u'_');
    }
    return name;
}

bool removeDirectory(const QString &path,
                     const RemovalMode mode,
                     QString *removedPath)
{
    if (mode == RemovalMode::MoveToTrash) {
        QString trashPath;
        const bool removed = QFile::moveToTrash(path, &trashPath);
        if (removed && removedPath) {
            *removedPath = trashPath;
        }
        return removed;
    }
    const bool removed = QDir(path).removeRecursively();
    if (removed && removedPath) {
        removedPath->clear();
    }
    return removed;
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

#ifdef XIPS_ENABLE_TEST_HOOKS
void AssetLibraryService::setWorkingCopyTestHook(WorkingCopyTestHook hook)
{
    m_workingCopyTestHook = std::move(hook);
}

void AssetLibraryService::invokeWorkingCopyTestHook(
    const WorkingCopyTestPoint point,
    const QString &path) const
{
    WorkingCopyTestHook hook = std::move(m_workingCopyTestHook);
    m_workingCopyTestHook = {};
    if (hook) {
        hook(point, path);
    }
}
#endif

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

UpdatePreview AssetLibraryService::previewUpdate(
    const AssetRecord &asset,
    const QString &sourcePath) const
{
    UpdatePreview preview;
    QString validationError;
    if (!validateAssetRecord(asset, nullptr, &validationError)) {
        preview.error = validationError;
        return preview;
    }
    SourcePayload source;
    if (!resolveSourcePayload(sourcePath, source, &validationError)) {
        preview.error = validationError;
        return preview;
    }
    const QString absoluteSource = files::normalizedAbsolute(sourcePath);
    const QString assetRoot = files::normalizedAbsolute(asset.assetRoot);
    if (files::isWithin(absoluteSource, assetRoot)
        || (QFileInfo(absoluteSource).isDir()
            && files::isWithin(assetRoot, absoluteSource))) {
        preview.error = QStringLiteral(
            "Update source and selected asset cannot contain each other");
        return preview;
    }

    return comparePayload(assetRoot, source);
}

UpdatePreview AssetLibraryService::previewRestore(
    const AssetRecord &asset,
    const QString &version) const
{
    UpdatePreview preview;
    QString validationError;
    if (!validateAssetRecord(asset, nullptr, &validationError)) {
        preview.error = validationError;
        return preview;
    }
    if (version.trimmed().isEmpty()) {
        preview.error = QStringLiteral("A saved version is required");
        return preview;
    }
    const CopyPlan plan = copyPlan(asset, version);
    if (!plan.ok()) {
        preview.error = plan.error;
        return preview;
    }
    SourcePayload source;
    if (!resolveSourcePayload(plan.sourceRoot, source, &validationError)) {
        preview.error = validationError;
        return preview;
    }
    return comparePayload(files::normalizedAbsolute(asset.assetRoot), source);
}

bool AssetLibraryService::updateAsset(const AssetRecord &asset,
                                      const QString &sourcePath,
                                      const WorkingCopyRecoveryMode recoveryMode,
                                      UpdateAssetResult *result,
                                      QString *error,
                                      const QString &expectedCurrentHash) const
{
    if (recoveryMode == WorkingCopyRecoveryMode::RetainForUndo && !result) {
        return fail(error,
                    QStringLiteral("Retained recovery requires an Undo result token"));
    }
    const UpdatePreview preview = previewUpdate(asset, sourcePath);
    if (!preview.ok()) {
        return fail(error, preview.error);
    }
    Manifest manifest;
    if (!validateAssetRecord(asset, &manifest, error)) {
        return false;
    }
    const QString expectedHash = expectedCurrentHash.trimmed();
    if (!expectedHash.isEmpty() && !validFingerprint(expectedHash)) {
        return fail(error,
                    QStringLiteral("The expected working-copy fingerprint is invalid"));
    }
    QString initialFingerprint;
    QString initialHashError;
    if (!AssetScanner::strictContentHash(manifest,
                                         asset.assetRoot,
                                         &initialFingerprint,
                                         &initialHashError)) {
        return fail(error,
                    QStringLiteral("Cannot verify the working copy before update: %1")
                        .arg(initialHashError));
    }
    if (!expectedHash.isEmpty() && initialFingerprint != expectedHash) {
        return fail(
            error,
            QStringLiteral(
                "The working copy changed after the operation; Undo will not overwrite newer edits"));
    }
    const QString requiredCurrentFingerprint = expectedHash.isEmpty()
                                                   ? initialFingerprint
                                                   : expectedHash;
    SourcePayload source;
    if (!resolveSourcePayload(sourcePath, source, error)) {
        return false;
    }

    const QString assetRoot = files::normalizedAbsolute(asset.assetRoot);
    const QString parent = QFileInfo(assetRoot).absolutePath();
    const QString assetName = QFileInfo(assetRoot).fileName();
    const QString operationId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    const QString stagingName = QStringLiteral(".xips-create-update-%1")
                                    .arg(operationId);
    const QString backupName = QStringLiteral(".xips-create-recovery-%1")
                                   .arg(operationId);
    const QString stagingRoot = QDir(parent).absoluteFilePath(stagingName);
    const QString backupRoot = QDir(parent).absoluteFilePath(backupName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the update staging directory"));
    }

    QString operationError;
    bool copied = false;
    if (source.singleFile) {
        copied = QFile::copy(
            QDir(source.root).absoluteFilePath(source.files.first()),
            QDir(stagingRoot).absoluteFilePath(source.files.first()));
        if (!copied) {
            operationError = QStringLiteral("Cannot copy the replacement file");
        }
    } else {
        copied = copyPayload(source.root, stagingRoot, &operationError);
    }
    if (!copied
        || !ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, operationError);
    }

    QDir parentDirectory(parent);
    if (!parentDirectory.rename(assetName, backupName)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error, QStringLiteral("Cannot stage the existing working copy"));
    }
    Manifest stagedManifest;
    QString stagedFingerprint;
    QString stagedError;
    if (!strictFingerprintAtRoot(backupRoot,
                                 manifest.id,
                                 &stagedManifest,
                                 &stagedFingerprint,
                                 &stagedError)
        || stagedFingerprint != requiredCurrentFingerprint) {
        const bool rolledBack = parentDirectory.rename(backupName,
                                                        assetName);
        QDir(stagingRoot).removeRecursively();
        return fail(
            error,
            rolledBack
                ? QStringLiteral(
                      "The working copy changed while the update was starting; newer edits were kept")
                : QStringLiteral(
                      "The working copy changed while the update was starting and rollback failed; recovery is at %1")
                      .arg(backupRoot));
    }
    manifest = stagedManifest;
    operationError.clear();
    if (!ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        const bool rolledBack = parentDirectory.rename(backupName,
                                                        assetName);
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    rolledBack
                        ? operationError
                        : QStringLiteral(
                              "%1; rollback failed and recovery is at %2")
                              .arg(operationError, backupRoot));
    }
    const QString backupInternal = QDir(backupRoot).absoluteFilePath(
        QStringLiteral(".xips"));
    const QString stagingInternal = QDir(stagingRoot).absoluteFilePath(
        QStringLiteral(".xips"));
    const bool hadInternal = QFileInfo(backupInternal).isDir();
    if (hadInternal && !QDir().rename(backupInternal, stagingInternal)) {
        const bool rolledBack = parentDirectory.rename(backupName, assetName);
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    rolledBack
                        ? QStringLiteral("Cannot preserve saved versions during update")
                        : QStringLiteral("Cannot preserve saved versions and rollback failed; recovery is at %1")
                              .arg(backupRoot));
    }
    Manifest finalBackupManifest;
    QString finalBackupFingerprint;
    QString finalBackupError;
    if (!strictFingerprintAtRoot(backupRoot,
                                 manifest.id,
                                 &finalBackupManifest,
                                 &finalBackupFingerprint,
                                 &finalBackupError)
        || finalBackupFingerprint != requiredCurrentFingerprint
        || QFileInfo(backupInternal).exists()
        || files::isLinkLike(QFileInfo(backupInternal))) {
        bool internalRolledBack = true;
        if (hadInternal) {
            internalRolledBack = QDir().rename(stagingInternal,
                                                backupInternal);
        }
        const bool rootRolledBack = parentDirectory.rename(backupName,
                                                            assetName);
        if (internalRolledBack) {
            QDir(stagingRoot).removeRecursively();
        }
        if (internalRolledBack && rootRolledBack) {
            return fail(
                error,
                QStringLiteral(
                    "The working copy changed before publish; newer edits were kept"));
        }
        QStringList recoveryLocations;
        if (!rootRolledBack) {
            recoveryLocations.append(backupRoot);
        }
        if (!internalRolledBack) {
            recoveryLocations.append(stagingInternal);
        }
        return fail(
            error,
            QStringLiteral(
                "The working copy changed before publish and rollback was incomplete; recovery data remains at %1")
                .arg(recoveryLocations.join(QStringLiteral(", "))));
    }

    Manifest intendedPublishedManifest;
    QString intendedPublishedFingerprint;
    QString intendedPublishedError;
    const ManifestService manifestService;
    const bool stagingVerified = strictFingerprintAtRoot(
        stagingRoot,
        manifest.id,
        &intendedPublishedManifest,
        &intendedPublishedFingerprint,
        &intendedPublishedError);
    const bool stagingManifestMatches =
        stagingVerified
        && json::canonicalJson(manifestService.toJson(intendedPublishedManifest))
               == json::canonicalJson(manifestService.toJson(manifest));
    if (!stagingVerified || !stagingManifestMatches) {
        bool internalRolledBack = true;
        if (hadInternal) {
            internalRolledBack = QDir().rename(stagingInternal,
                                                backupInternal);
        }
        const bool rootRolledBack = parentDirectory.rename(backupName,
                                                            assetName);
        if (internalRolledBack) {
            QDir(stagingRoot).removeRecursively();
        }
        if (internalRolledBack && rootRolledBack) {
            return fail(
                error,
                stagingVerified
                    ? QStringLiteral(
                          "The staged update manifest changed before publish; the existing working copy was kept")
                    : QStringLiteral(
                          "Cannot verify the staged update before publish: %1")
                          .arg(intendedPublishedError));
        }
        QStringList recoveryLocations;
        if (!rootRolledBack) {
            recoveryLocations.append(backupRoot);
        }
        if (!internalRolledBack) {
            recoveryLocations.append(stagingInternal);
        }
        return fail(
            error,
            QStringLiteral(
                "Cannot verify or fully rollback the staged update; recovery data remains at %1")
                .arg(recoveryLocations.join(QStringLiteral(", "))));
    }
#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::StagingVerifiedBeforePublish,
        stagingRoot);
#endif
    if (!parentDirectory.rename(stagingName, assetName)) {
        bool internalRolledBack = true;
        if (hadInternal) {
            internalRolledBack = QDir().rename(stagingInternal, backupInternal);
        }
        const bool rootRolledBack = parentDirectory.rename(backupName, assetName);
        if (internalRolledBack) {
            QDir(stagingRoot).removeRecursively();
        }
        if (internalRolledBack && rootRolledBack) {
            return fail(error,
                        QStringLiteral("Cannot publish the updated working copy"));
        }
        QStringList recoveryLocations;
        if (!rootRolledBack) {
            recoveryLocations.append(backupRoot);
        }
        if (!internalRolledBack) {
            recoveryLocations.append(stagingInternal);
        }
        return fail(
            error,
            QStringLiteral("Cannot publish or fully rollback the update; recovery data remains at %1")
                .arg(recoveryLocations.join(QStringLiteral(", "))));
    }

    Manifest actualPublishedManifest;
    QString actualPublishedFingerprint;
    QString actualPublishedError;
    const bool publishedVerified = strictFingerprintAtRoot(
        assetRoot,
        manifest.id,
        &actualPublishedManifest,
        &actualPublishedFingerprint,
        &actualPublishedError)
        && actualPublishedFingerprint == intendedPublishedFingerprint
        && json::canonicalJson(manifestService.toJson(actualPublishedManifest))
               == json::canonicalJson(
                   manifestService.toJson(intendedPublishedManifest));

    UpdateAssetResult completed;
    completed.preview = preview;
    completed.publishedAsIntended = publishedVerified;
    const ScanResult scan = AssetScanner().scan(assetRoot);
    if (!scan.assets.isEmpty()) {
        completed.updated = scan.assets.first();
    } else {
        completed.updated = asset;
        completed.updated.files = source.files;
        completed.updated.fileCount = source.files.size();
        completed.warning = QStringLiteral(
            "Updated files were published but the asset could not be rescanned");
    }
    if (!publishedVerified) {
        completed.retainedPaths.append(backupRoot);
        appendWarning(
            completed.warning,
            actualPublishedError.isEmpty()
                ? QStringLiteral(
                      "The published working copy changed before it could be verified; Undo and automatic cleanup are unavailable, and the previous working copy remains at %1")
                      .arg(backupRoot)
                : QStringLiteral(
                      "The published working copy could not be verified: %1; Undo and automatic cleanup are unavailable, and the previous working copy remains at %2")
                      .arg(actualPublishedError, backupRoot));
        if (result) {
            *result = completed;
        }
        return true;
    }
    if (recoveryMode == WorkingCopyRecoveryMode::RetainForUndo) {
        QString hashError;
        Manifest recoveryManifest;
        QString recoveryFingerprint;
        const bool recoveryVerified = strictRecoveryFingerprintAtRoot(
            backupRoot,
            manifest.id,
            &recoveryManifest,
            &recoveryFingerprint,
            &hashError);
        const QFileInfo unexpectedRecoveryVersions(backupInternal);
        const bool recoveryHasSavedVersions =
            unexpectedRecoveryVersions.exists()
            || files::isLinkLike(unexpectedRecoveryVersions);
        if (!recoveryVerified) {
            appendWarning(
                completed.warning,
                QStringLiteral("The retained recovery cannot be verified for Undo: %1")
                    .arg(hashError));
        } else if (recoveryHasSavedVersions) {
            appendWarning(
                completed.warning,
                QStringLiteral(
                    "The retained recovery contains saved-version data; Undo is unavailable and recovery remains at %1")
                    .arg(backupRoot));
        } else if (recoveryFingerprint != requiredCurrentFingerprint) {
            appendWarning(
                completed.warning,
                QStringLiteral(
                    "The retained recovery changed after publish; Undo is unavailable and recovery remains at %1")
                    .arg(backupRoot));
        }
        if (recoveryVerified && !recoveryHasSavedVersions
            && recoveryFingerprint == requiredCurrentFingerprint) {
            completed.undoToken = WorkingCopyUndoToken{
                .assetId = manifest.id,
                .assetRoot = assetRoot,
                .recoveryPath = backupRoot,
                .publishedFingerprint = intendedPublishedFingerprint,
                .recoveryFingerprint = recoveryFingerprint,
            };
        } else {
            completed.retainedPaths.append(backupRoot);
        }
    } else {
        const RemovalMode removalMode =
            recoveryMode == WorkingCopyRecoveryMode::MoveToTrash
                ? RemovalMode::MoveToTrash
                : RemovalMode::Permanent;
        QString cleanupFingerprint;
        QString cleanupError;
        const QFileInfo unexpectedCleanupVersions(backupInternal);
        const bool cleanupHasSavedVersions =
            unexpectedCleanupVersions.exists()
            || files::isLinkLike(unexpectedCleanupVersions);
        const bool cleanupVerified = !cleanupHasSavedVersions
                                     && strictRecoveryFingerprintAtRoot(
                                         backupRoot,
                                         manifest.id,
                                         nullptr,
                                         &cleanupFingerprint,
                                         &cleanupError)
                                     && cleanupFingerprint
                                            == requiredCurrentFingerprint;
        if (!cleanupVerified) {
            completed.retainedPaths.append(backupRoot);
            appendWarning(
                completed.warning,
                QStringLiteral(
                    "Updated files were published, but the previous working copy changed before cleanup and remains at %1")
                    .arg(backupRoot));
        } else {
            const WorkingCopyUndoToken cleanupToken{
                .assetId = manifest.id,
                .assetRoot = assetRoot,
                .recoveryPath = backupRoot,
                .publishedFingerprint = intendedPublishedFingerprint,
                .recoveryFingerprint = cleanupFingerprint,
            };
            RecoveryDiscardResult discarded;
            QString discardError;
            if (!discardWorkingCopyRecovery(asset,
                                            cleanupToken,
                                            removalMode,
                                            &discarded,
                                            &discardError)) {
                completed.retainedPaths.append(backupRoot);
                appendWarning(
                    completed.warning,
                    QStringLiteral(
                        "Updated files were published; the previous working copy remains at %1: %2")
                        .arg(backupRoot, discardError));
            } else if (!discarded.warning.isEmpty()) {
                if (!discarded.retainedPath.isEmpty()) {
                    completed.retainedPaths.append(discarded.retainedPath);
                }
                appendWarning(completed.warning, discarded.warning);
            }
        }
    }
    if (result) {
        *result = completed;
    }
    return true;
}

bool AssetLibraryService::restoreVersion(const AssetRecord &asset,
                                         const QString &version,
                                         const WorkingCopyRecoveryMode recoveryMode,
                                         UpdateAssetResult *result,
                                         QString *error) const
{
    if (recoveryMode == WorkingCopyRecoveryMode::RetainForUndo && !result) {
        return fail(error,
                    QStringLiteral("Retained recovery requires an Undo result token"));
    }
    const QString cleanVersion = version.trimmed();
    const UpdatePreview preview = previewRestore(asset, cleanVersion);
    if (!preview.ok()) {
        return fail(error, preview.error);
    }
    if (preview.addedFiles.isEmpty()
        && preview.replacedFiles.isEmpty()
        && preview.removedFiles.isEmpty()) {
        return fail(error,
                    QStringLiteral("The working copy already matches version %1")
                        .arg(cleanVersion));
    }

    const CopyPlan plan = copyPlan(asset, cleanVersion);
    if (!plan.ok()) {
        return fail(error, plan.error);
    }
    const QString parent = QFileInfo(asset.assetRoot).absolutePath();
    QTemporaryDir restoreSource(
        QDir(parent).absoluteFilePath(
            QStringLiteral(".xips-create-restore-XXXXXX")));
    restoreSource.setAutoRemove(false);
    if (!restoreSource.isValid()) {
        return fail(error,
                    QStringLiteral("Cannot create the restore staging directory"));
    }
    QString operationError;
    if (!copyPayload(plan.sourceRoot, restoreSource.path(), &operationError)) {
        const QString stagingPath = restoreSource.path();
        if (!restoreSource.remove()) {
            operationError += QStringLiteral(
                "; restore staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(error, operationError);
    }

    UpdateAssetResult completed;
    if (!updateAsset(asset,
                     restoreSource.path(),
                     recoveryMode,
                     &completed,
                     &operationError)) {
        const QString stagingPath = restoreSource.path();
        if (!restoreSource.remove()) {
            operationError += QStringLiteral(
                "; restore staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(error, operationError);
    }
    completed.preview = preview;
    const QString stagingPath = restoreSource.path();
    if (!restoreSource.remove()) {
        completed.retainedPaths.append(stagingPath);
        if (!completed.warning.isEmpty()) {
            completed.warning += QStringLiteral("; ");
        }
        completed.warning += QStringLiteral(
            "restored the working copy, but temporary data remains at %1")
                                 .arg(stagingPath);
    }
    if (result) {
        *result = completed;
    }
    return true;
}

bool AssetLibraryService::undoWorkingCopyChange(
    const AssetRecord &asset,
    const WorkingCopyUndoToken &token,
    UpdateAssetResult *result,
    QString *error) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral("Undo requires a result for cleanup reporting"));
    }
    if (!validateAssetRecord(asset, nullptr, error)) {
        return false;
    }
    if (!validateWorkingCopyUndoToken(asset,
                                      token,
                                      error)) {
        return false;
    }
    QString currentHash;
    QString hashError;
    if (!strictFingerprintAtRoot(asset.assetRoot,
                                 asset.manifest.id,
                                 nullptr,
                                 &currentHash,
                                 &hashError)
        || currentHash != token.publishedFingerprint) {
        return fail(
            error,
            QStringLiteral(
                "The working copy changed after the operation; Undo will not overwrite newer edits"));
    }
    Manifest recoveryManifest;
    QString recoveryHash;
    hashError.clear();
    if (!strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                         asset.manifest.id,
                                         &recoveryManifest,
                                         &recoveryHash,
                                         &hashError)
        || recoveryHash != token.recoveryFingerprint) {
        return fail(
            error,
            QStringLiteral(
                "The retained recovery changed after the operation; Undo will not restore unverified files"));
    }

    const QString parent = QFileInfo(asset.assetRoot).absolutePath();
    QTemporaryDir undoSource(
        QDir(parent).absoluteFilePath(
            QStringLiteral(".xips-create-undo-XXXXXX")));
    undoSource.setAutoRemove(false);
    if (!undoSource.isValid()) {
        return fail(error,
                    QStringLiteral("Cannot create the undo staging directory"));
    }
    QString operationError;
    if (!copyPayload(token.recoveryPath,
                     undoSource.path(),
                     &operationError)) {
        const QString stagingPath = undoSource.path();
        if (!undoSource.remove()) {
            operationError += QStringLiteral(
                "; undo staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(error, operationError);
    }
    Manifest confirmedRecoveryManifest;
    QString confirmedRecoveryHash;
    operationError.clear();
    if (!strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                         asset.manifest.id,
                                         &confirmedRecoveryManifest,
                                         &confirmedRecoveryHash,
                                         &operationError)
        || confirmedRecoveryHash != token.recoveryFingerprint) {
        const QString stagingPath = undoSource.path();
        if (!undoSource.remove()) {
            operationError += QStringLiteral(
                "; undo staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(
            error,
            QStringLiteral(
                "The retained recovery changed while it was being copied; Undo was not applied: %1")
                .arg(operationError));
    }
    QString copiedRecoveryHash;
    operationError.clear();
    if (!AssetScanner::strictContentHash(confirmedRecoveryManifest,
                                         undoSource.path(),
                                         &copiedRecoveryHash,
                                         &operationError)
        || copiedRecoveryHash != token.recoveryFingerprint) {
        const QString stagingPath = undoSource.path();
        if (!undoSource.remove()) {
            operationError += QStringLiteral(
                "; undo staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(
            error,
            QStringLiteral("Cannot verify the copied recovery before Undo: %1")
                .arg(operationError));
    }

    UpdateAssetResult completed;
    if (!updateAsset(asset,
                     undoSource.path(),
                     WorkingCopyRecoveryMode::Permanent,
                     &completed,
                     &operationError,
                     token.publishedFingerprint)) {
        const QString stagingPath = undoSource.path();
        if (!undoSource.remove()) {
            operationError += QStringLiteral(
                "; undo staging data remains at %1")
                                  .arg(stagingPath);
        }
        return fail(error, operationError);
    }
    const QString stagingPath = undoSource.path();
    if (!undoSource.remove()) {
        completed.retainedPaths.append(stagingPath);
        appendWarning(
            completed.warning,
            QStringLiteral(
                "restored the previous working copy, but temporary data remains at %1")
                .arg(stagingPath));
    }
    if (!completed.publishedAsIntended) {
        completed.retainedPaths.append(token.recoveryPath);
        appendWarning(
            completed.warning,
            QStringLiteral(
                "Undo could not be verified after publish; the original recovery remains at %1")
                .arg(token.recoveryPath));
        completed.retainedPaths.removeDuplicates();
        *result = completed;
        return fail(
            error,
            QStringLiteral(
                "Undo could not be verified after publish; current files and recovery copies were kept"));
    }

    RecoveryDiscardResult discarded;
    QString discardError;
    if (!discardWorkingCopyRecovery(asset,
                                    token,
                                    RemovalMode::Permanent,
                                    &discarded,
                                    &discardError)) {
        completed.retainedPaths.append(token.recoveryPath);
        appendWarning(
            completed.warning,
            QStringLiteral(
                "restored the previous working copy, but its recovery copy remains at %1: %2")
                .arg(token.recoveryPath, discardError));
    } else if (!discarded.warning.isEmpty()) {
        if (!discarded.retainedPath.isEmpty()) {
            completed.retainedPaths.append(discarded.retainedPath);
        }
        appendWarning(completed.warning, discarded.warning);
    }
    completed.retainedPaths.removeDuplicates();
    *result = completed;
    return true;
}

bool AssetLibraryService::discardWorkingCopyRecovery(
    const AssetRecord &asset,
    const WorkingCopyUndoToken &token,
    const RemovalMode mode,
    RecoveryDiscardResult *result,
    QString *error) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Discard requires a result for cleanup reporting"));
    }
    *result = {};
    if (!validateWorkingCopyUndoToken(asset,
                                      token,
                                      error)) {
        return false;
    }
    QString recoveryHash;
    QString hashError;
    if (!strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                         asset.manifest.id,
                                         nullptr,
                                         &recoveryHash,
                                         &hashError)
        || recoveryHash != token.recoveryFingerprint) {
        return fail(
            error,
            QStringLiteral(
                "The retained recovery changed or contains unrecognized data; it was not discarded automatically: %1")
                .arg(hashError));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::RecoveryVerifiedBeforeDiscardIsolation,
        token.recoveryPath);
#endif

    const QString parent = QFileInfo(token.recoveryPath).absolutePath();
    const QString recoveryName = QFileInfo(token.recoveryPath).fileName();
    const QString discardName = QStringLiteral(".xips-create-discard-%1")
                                    .arg(QUuid::createUuid().toString(
                                        QUuid::WithoutBraces));
    QDir parentDirectory(parent);
    if (!parentDirectory.rename(recoveryName, discardName)) {
        return fail(error,
                    QStringLiteral(
                        "Cannot isolate the previous working copy before discarding it"));
    }
    const QString discardPath = QDir(parent).absoluteFilePath(discardName);
    RecoveryDiscardResult completed;
    QString isolatedHash;
    hashError.clear();
    if (!strictRecoveryFingerprintAtRoot(discardPath,
                                         asset.manifest.id,
                                         nullptr,
                                         &isolatedHash,
                                         &hashError)
        || isolatedHash != token.recoveryFingerprint) {
        completed.retainedPath = discardPath;
        completed.warning = QStringLiteral(
            "The recovery was isolated but could not be reverified; data remains at %1%2")
                                .arg(
                                    discardPath,
                                    hashError.isEmpty()
                                        ? QString()
                                        : QStringLiteral(": %1").arg(hashError));
    } else if (!removeDirectory(discardPath, mode, nullptr)) {
        completed.retainedPath = discardPath;
        completed.warning =
            (mode == RemovalMode::MoveToTrash
                 ? QStringLiteral(
                       "The recovery was retired but could not be moved to the recycle bin; data remains at %1")
                 : QStringLiteral(
                       "The recovery was retired but could not be deleted completely; data remains at %1"))
                .arg(discardPath);
    }
    *result = completed;
    return true;
}

bool AssetLibraryService::deleteAsset(const QString &libraryRoot,
                                      const AssetRecord &asset,
                                      const RemovalMode mode,
                                      QString *removedPath,
                                      QString *error) const
{
    const QString library = files::normalizedAbsolute(libraryRoot);
    const QString assetRoot = files::normalizedAbsolute(asset.assetRoot);
    const QFileInfo rootInfo(assetRoot);
    if (!QFileInfo(library).isDir() || assetRoot == library
        || !files::isWithin(assetRoot, library) || files::isLinkLike(rootInfo)) {
        return fail(error, QStringLiteral("Selected asset is outside the library"));
    }
    if (!validateAssetRecord(asset, nullptr, error)) {
        return false;
    }
    if (!removeDirectory(assetRoot, mode, removedPath)) {
        return fail(error,
                    mode == RemovalMode::MoveToTrash
                        ? QStringLiteral("Cannot move the asset to the recycle bin")
                        : QStringLiteral("Cannot delete the asset"));
    }
    return true;
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
                                        const RemovalMode mode,
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
    if (mode == RemovalMode::MoveToTrash) {
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
                    mode == RemovalMode::MoveToTrash
                        ? QStringLiteral("Cannot move the saved version to the trash")
                        : QStringLiteral("Cannot delete the saved version"));
    }
    return true;
}

CopyPlan AssetLibraryService::copyPlan(const AssetRecord &asset,
                                       const QString &version) const
{
    CopyPlan plan;
    plan.version = version.trimmed();
    plan.sourceRoot = asset.assetRoot;
    if (!plan.version.isEmpty()) {
        QString versionsError;
        const QList<VersionInfo> available = versions(asset.assetRoot,
                                                       &versionsError);
        if (!versionsError.isEmpty()) {
            plan.error = versionsError;
            return plan;
        }
        const auto found = std::find_if(
            available.cbegin(), available.cend(), [&plan](const VersionInfo &entry) {
                return entry.version == plan.version;
            });
        if (found == available.cend()) {
            plan.error = QStringLiteral("Version not found: %1").arg(plan.version);
            return plan;
        }
        plan.sourceRoot = found->path;
    } else {
        QString validationError;
        if (!validateAssetRecord(asset, nullptr, &validationError)) {
            plan.error = validationError;
            return plan;
        }
    }
    plan.files = AssetScanner::assetFiles(plan.sourceRoot);
    if (plan.files.isEmpty()) {
        plan.error = QStringLiteral("The selected asset has no payload files");
        return plan;
    }
    plan.suggestedName = plan.isSingleFile()
                             ? QFileInfo(plan.files.first()).fileName()
                             : safeCopyName(asset.manifest.name,
                                            asset.manifest.id);
    return plan;
}

bool AssetLibraryService::copyVersionPayload(
    const AssetRecord &asset,
    const QString &version,
    const QString &destinationPath,
    QString *copiedPath,
    QString *error) const
{
    const CopyPlan plan = copyPlan(asset, version);
    if (!plan.ok()) {
        return fail(error, plan.error);
    }
    const QString targetPath = files::normalizedAbsolute(destinationPath);
    if (files::isWithin(targetPath, plan.sourceRoot)) {
        return fail(error,
                    QStringLiteral("Copy destination cannot be inside the source asset"));
    }
    if (QFileInfo::exists(targetPath)) {
        return fail(error,
                    QStringLiteral("Copy destination already exists: %1")
                        .arg(targetPath));
    }
    const QString parent = QFileInfo(targetPath).absolutePath();
    const QString targetName = QFileInfo(targetPath).fileName();
    if (!QFileInfo(parent).isDir() || targetName.isEmpty()) {
        return fail(error,
                    QStringLiteral("Copy destination parent is not a directory"));
    }

    if (plan.isSingleFile()) {
        const QString source = QDir(plan.sourceRoot).absoluteFilePath(
            plan.files.first());
        if (!QFile::copy(source, targetPath)) {
            return fail(error,
                        QStringLiteral("Cannot copy %1 to %2")
                            .arg(source, targetPath));
        }
        if (copiedPath) {
            *copiedPath = targetPath;
        }
        return true;
    }

    const QString stagingName = QStringLiteral(".xips-copy-%1")
                                    .arg(QUuid::createUuid().toString(
                                        QUuid::WithoutBraces));
    const QString stagingRoot = QDir(parent).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the copy staging directory"));
    }
    QString copyError;
    if (!copyPayload(plan.sourceRoot, stagingRoot, &copyError)
        || !QDir(parent).rename(stagingName, targetName)) {
        QDir(stagingRoot).removeRecursively();
        return fail(error,
                    copyError.isEmpty()
                        ? QStringLiteral("Cannot publish the copied asset")
                        : copyError);
    }
    if (copiedPath) {
        *copiedPath = targetPath;
    }
    return true;
}

} // namespace xips
