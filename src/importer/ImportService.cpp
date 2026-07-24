#include "importer/ImportService.h"

#include "assetcore/JsonUtil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace xips {
namespace {

QString normalizedRelative(QString path)
{
    path = QDir::cleanPath(path);
    path.replace(u'\\', u'/');
    while (path.startsWith(QStringLiteral("./"))) {
        path.remove(0, 2);
    }
    return path;
}

bool isSafeRelativePath(const QString &path)
{
    const QString normalized = normalizedRelative(path);
    return !normalized.isEmpty() && normalized != QStringLiteral(".")
           && !QDir::isAbsolutePath(normalized)
           && normalized != QStringLiteral("..")
           && !normalized.startsWith(QStringLiteral("../"));
}

QString absolutePath(const QString &root, const QString &path)
{
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path)
                                      : QDir(root).absoluteFilePath(path);
}

QString projectRelativePath(const QString &targetRoot, const QString &absolute)
{
    return normalizedRelative(QDir(targetRoot).relativeFilePath(absolute));
}

bool writeJsonAtomic(const QString &path, const QJsonObject &document, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Cannot create directory for %1").arg(path);
        }
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QByteArray encoded = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        file.cancelWriting();
        return false;
    }
    return true;
}

QJsonObject readJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

QHash<QString, QString> ownedHashes(const QJsonObject &lockfile)
{
    QHash<QString, QString> result;
    for (const QJsonValue &assetValue : lockfile.value(QStringLiteral("assets")).toArray()) {
        for (const QJsonValue &fileValue :
             assetValue.toObject().value(QStringLiteral("files")).toArray()) {
            const QJsonObject file = fileValue.toObject();
            const QString path = normalizedRelative(file.value(QStringLiteral("path")).toString());
            const QString hash = file.value(QStringLiteral("hash")).toString();
            if (!path.isEmpty() && !hash.isEmpty()) {
                result.insert(path, hash);
            }
        }
    }
    return result;
}

QHash<QString, QString> ownedAssetIds(const QJsonObject &lockfile)
{
    QHash<QString, QString> result;
    for (const QJsonValue &assetValue : lockfile.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = assetValue.toObject();
        const QString assetId = asset.value(QStringLiteral("id")).toString();
        for (const QJsonValue &fileValue : asset.value(QStringLiteral("files")).toArray()) {
            const QString path = normalizedRelative(
                fileValue.toObject().value(QStringLiteral("path")).toString());
            if (!path.isEmpty()) {
                result.insert(path, assetId);
            }
        }
    }
    return result;
}

bool isLinkLike(const QFileInfo &info);

bool ignoredVendorDirectory(const QString &name, const QSet<QString> &manifestExclusions)
{
    const QString normalized = name.toLower();
    if (normalized == QStringLiteral(".git") || normalized == QStringLiteral(".xips")
        || normalized == QStringLiteral(".xil") || normalized == QStringLiteral("build")
        || normalized == QStringLiteral("cache")
        || normalized == QStringLiteral("ip_user_files")
        || normalized.startsWith(QStringLiteral("build-"))) {
        return true;
    }
    for (const QString &excluded : manifestExclusions) {
        if (normalized == excluded.toLower()) {
            return true;
        }
    }
    return false;
}

void collectDirectoryFiles(const QString &directory,
                           QStringList &files,
                           const QSet<QString> &manifestExclusions)
{
    const QDir dir(directory);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry)) {
            continue;
        }
        if (entry.isDir()) {
            if (!ignoredVendorDirectory(entry.fileName(), manifestExclusions)) {
                collectDirectoryFiles(entry.absoluteFilePath(), files, manifestExclusions);
            }
        } else if (entry.isFile()) {
            files.append(entry.absoluteFilePath());
        }
    }
}

QList<QPair<QString, QString>> vendorSourceFiles(const AssetRecord &asset,
                                                QList<ImportIssue> &issues)
{
    QStringList declared = asset.manifest.sources;
    declared.append(asset.manifest.constraints);
    declared.append(asset.manifest.tests);
    declared.append(asset.manifest.examples);
    declared.append(asset.manifest.documentation);
    declared.removeDuplicates();

    QSet<QString> exclusions;
    for (const QJsonValue &value :
         asset.manifest.rawObject.value(QStringLiteral("excludedGeneratedDirectories")).toArray()) {
        if (value.isString()) {
            exclusions.insert(value.toString());
        }
    }

    QStringList absoluteFiles;
    if (QFileInfo(asset.manifestPath).isFile()) {
        absoluteFiles.append(QFileInfo(asset.manifestPath).absoluteFilePath());
    }
    for (const QString &path : declared) {
        const QString absolute = absolutePath(asset.assetRoot, path);
        const QFileInfo info(absolute);
        if (!info.exists() || !info.isFile() || isLinkLike(info)) {
            issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message = QStringLiteral(
                    "Declared asset file is missing or link-backed"),
                .path = absolute,
            });
            continue;
        }
        absoluteFiles.append(info.absoluteFilePath());
    }
    for (const QString &includeDirectory : asset.manifest.includeDirs) {
        const QString absolute = absolutePath(asset.assetRoot, includeDirectory);
        if (!QFileInfo(absolute).isDir()
            || isLinkLike(QFileInfo(absolute))) {
            issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message = QStringLiteral(
                    "Declared include directory is missing or link-backed"),
                .path = absolute,
            });
            continue;
        }
        collectDirectoryFiles(absolute, absoluteFiles, exclusions);
    }
    absoluteFiles.removeDuplicates();
    std::sort(absoluteFiles.begin(), absoluteFiles.end());

    QList<QPair<QString, QString>> result;
    for (const QString &absolute : absoluteFiles) {
        QString relative = normalizedRelative(QDir(asset.assetRoot).relativeFilePath(absolute));
        if (!isSafeRelativePath(relative)) {
            const QString digest = ImportService::fileHash(absolute)
                                       .remove(QStringLiteral("sha256:"))
                                       .left(12);
            relative = QStringLiteral("external/%1/%2")
                           .arg(digest, QFileInfo(absolute).fileName());
        }
        result.append({absolute, relative});
    }
    return result;
}

QJsonObject referenceDocument(const DependencyResolution &resolution,
                              const QString &targetRoot)
{
    QJsonArray assets;
    for (const AssetRecord &asset : resolution.orderedAssets) {
        const QString sourceRoot =
            projectRelativePath(targetRoot, QFileInfo(asset.assetRoot).absoluteFilePath());
        QJsonArray sources;
        for (const QString &source : asset.manifest.sources) {
            sources.append(projectRelativePath(targetRoot,
                                               absolutePath(asset.assetRoot, source)));
        }
        QJsonArray includeDirectories;
        for (const QString &includeDirectory : asset.manifest.includeDirs) {
            includeDirectories.append(projectRelativePath(
                targetRoot,
                absolutePath(asset.assetRoot, includeDirectory)));
        }
        assets.append(QJsonObject{
            {QStringLiteral("id"), asset.manifest.id},
            {QStringLiteral("version"), asset.manifest.version},
            {QStringLiteral("commit"), asset.gitCommit},
            {QStringLiteral("contentHash"), asset.contentHash},
            {QStringLiteral("source"), sourceRoot},
            {QStringLiteral("sources"), sources},
            {QStringLiteral("includeDirs"), includeDirectories},
            {QStringLiteral("defines"), json::toArray(asset.manifest.defines)},
        });
    }
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("mode"), QStringLiteral("reference")},
        {QStringLiteral("assets"), assets},
    };
}

bool validateReferenceDocument(const QJsonObject &object, QString *error)
{
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1
        || object.value(QStringLiteral("mode")).toString()
               != QStringLiteral("reference")
        || !object.value(QStringLiteral("assets")).isArray()) {
        if (error) {
            *error = QStringLiteral("Unsupported Reference document schema");
        }
        return false;
    }

    QSet<QString> ids;
    for (const QJsonValue &value :
         object.value(QStringLiteral("assets")).toArray()) {
        if (!value.isObject()) {
            if (error) {
                *error =
                    QStringLiteral("Reference asset entries must be objects");
            }
            return false;
        }
        const QString id =
            value.toObject().value(QStringLiteral("id")).toString();
        if (id.isEmpty() || ids.contains(id)) {
            if (error) {
                *error = id.isEmpty()
                             ? QStringLiteral("Reference asset ID is missing")
                             : QStringLiteral("Duplicate Reference asset ID: %1")
                                   .arg(id);
            }
            return false;
        }
        ids.insert(id);
    }
    return true;
}

bool readReferenceDocument(const QString &path,
                           QJsonObject *document,
                           QString *error)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        if (error) {
            *error =
                QStringLiteral("Reference document must not be a symbolic link: %1")
                    .arg(path);
        }
        return false;
    }
    if (!info.exists()) {
        *document = {};
        return true;
    }
    if (!info.isFile()) {
        if (error) {
            *error =
                QStringLiteral("Reference document must be a regular file: %1")
                    .arg(path);
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot read Reference document: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid Reference JSON: %1")
                         .arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject object = parsed.object();
    if (!validateReferenceDocument(object, error)) {
        return false;
    }
    *document = object;
    return true;
}

QJsonObject mergeReferenceDocuments(const QJsonObject &existing,
                                    const QJsonObject &generated)
{
    if (existing.isEmpty()) {
        return generated;
    }

    QJsonArray mergedAssets = existing.value(QStringLiteral("assets")).toArray();
    QHash<QString, qsizetype> positions;
    for (qsizetype index = 0; index < mergedAssets.size(); ++index) {
        positions.insert(
            mergedAssets.at(index)
                .toObject()
                .value(QStringLiteral("id"))
                .toString(),
            index);
    }

    for (const QJsonValue &value :
         generated.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject generatedAsset = value.toObject();
        const QString id =
            generatedAsset.value(QStringLiteral("id")).toString();
        if (positions.contains(id)) {
            QJsonObject merged = mergedAssets.at(positions.value(id)).toObject();
            for (auto iterator = generatedAsset.constBegin();
                 iterator != generatedAsset.constEnd();
                 ++iterator) {
                merged.insert(iterator.key(), iterator.value());
            }
            mergedAssets.replace(positions.value(id), merged);
        } else {
            positions.insert(id, mergedAssets.size());
            mergedAssets.append(generatedAsset);
        }
    }

    QJsonObject result = existing;
    result.insert(QStringLiteral("schemaVersion"), 1);
    result.insert(QStringLiteral("mode"), QStringLiteral("reference"));
    result.insert(QStringLiteral("assets"), mergedAssets);
    return result;
}

bool readVendorLockDocument(const QString &path,
                            QJsonObject *document,
                            QString *error)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        if (error) {
            *error =
                QStringLiteral("Vendor lockfile must not be a symbolic link: %1")
                    .arg(path);
        }
        return false;
    }
    if (!info.exists() || !info.isFile()) {
        if (error) {
            *error = QStringLiteral("Vendor lockfile is not a regular file: %1")
                         .arg(path);
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error =
                QStringLiteral("Cannot read Vendor lockfile: %1")
                    .arg(file.errorString());
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        if (error) {
            *error =
                QStringLiteral("Invalid Vendor lockfile JSON: %1")
                    .arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject object = parsed.object();
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1
        || object.value(QStringLiteral("mode")).toString()
               != QStringLiteral("vendor")
        || !object.value(QStringLiteral("assets")).isArray()) {
        if (error) {
            *error = QStringLiteral("Unsupported Vendor lockfile schema");
        }
        return false;
    }

    QSet<QString> assetIds;
    QSet<QString> ownedPaths;
    for (const QJsonValue &assetValue :
         object.value(QStringLiteral("assets")).toArray()) {
        if (!assetValue.isObject()) {
            if (error) {
                *error = QStringLiteral(
                    "Vendor lockfile asset entries must be objects");
            }
            return false;
        }
        const QJsonObject asset = assetValue.toObject();
        const QString assetId =
            asset.value(QStringLiteral("id")).toString();
        if (assetId.isEmpty() || assetIds.contains(assetId)
            || !asset.value(QStringLiteral("files")).isArray()) {
            if (error) {
                *error = assetId.isEmpty()
                             ? QStringLiteral("Vendor lockfile asset ID is missing")
                             : assetIds.contains(assetId)
                                   ? QStringLiteral(
                                         "Duplicate Vendor lockfile asset ID: %1")
                                         .arg(assetId)
                                   : QStringLiteral(
                                         "Vendor lockfile files must be an array");
            }
            return false;
        }
        assetIds.insert(assetId);
        for (const QJsonValue &fileValue :
             asset.value(QStringLiteral("files")).toArray()) {
            const QJsonObject fileObject = fileValue.toObject();
            const QString ownedPath = normalizedRelative(
                fileObject.value(QStringLiteral("path")).toString());
            const QString hash =
                fileObject.value(QStringLiteral("hash")).toString();
            if (!fileValue.isObject() || !isSafeRelativePath(ownedPath)
                || hash.isEmpty() || ownedPaths.contains(ownedPath)) {
                if (error) {
                    *error = QStringLiteral(
                        "Invalid or duplicate Vendor-owned path: %1")
                                 .arg(ownedPath);
                }
                return false;
            }
            ownedPaths.insert(ownedPath);
        }
    }
    *document = object;
    return true;
}

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink()) {
        return true;
    }
#ifdef Q_OS_WIN
    return info.isJunction();
#else
    return false;
#endif
}

bool hasLinkComponent(const QString &root, const QString &relativePath)
{
    QFileInfo current(root);
    if (isLinkLike(current)) {
        return true;
    }
    QString currentPath = current.absoluteFilePath();
    const QStringList components =
        normalizedRelative(relativePath).split(u'/', Qt::SkipEmptyParts);
    for (const QString &component : components) {
        currentPath = QDir(currentPath).absoluteFilePath(component);
        current.setFile(currentPath);
        if (isLinkLike(current)) {
            return true;
        }
    }
    return false;
}

bool absolutePathHasLinkComponent(const QString &path)
{
    QString currentPath = QFileInfo(path).absoluteFilePath();
    while (!currentPath.isEmpty()) {
        const QFileInfo current(currentPath);
        if (isLinkLike(current)) {
            return true;
        }
        const QString parent = current.absolutePath();
        if (parent == currentPath) {
            break;
        }
        currentPath = parent;
    }
    return false;
}

QJsonObject lockDocument(const DependencyResolution &resolution,
                         const QList<PlannedFile> &files,
                         const QString &targetRoot)
{
    QHash<QString, QList<PlannedFile>> filesByAsset;
    for (const PlannedFile &file : files) {
        filesByAsset[file.assetId].append(file);
    }

    QJsonArray assets;
    for (const AssetRecord &asset : resolution.orderedAssets) {
        QList<PlannedFile> assetFiles = filesByAsset.value(asset.manifest.id);
        std::sort(assetFiles.begin(),
                  assetFiles.end(),
                  [](const PlannedFile &left, const PlannedFile &right) {
                      return left.destinationPath < right.destinationPath;
                  });
        QJsonArray fileArray;
        for (const PlannedFile &file : assetFiles) {
            if (file.action == PlannedFileAction::Remove
                || file.action == PlannedFileAction::Conflict) {
                continue;
            }
            fileArray.append(QJsonObject{
                {QStringLiteral("path"), file.destinationPath},
                {QStringLiteral("hash"), file.sourceHash},
            });
        }
        assets.append(QJsonObject{
            {QStringLiteral("id"), asset.manifest.id},
            {QStringLiteral("version"), asset.manifest.version},
            {QStringLiteral("commit"), asset.gitCommit},
            {QStringLiteral("contentHash"), asset.contentHash},
            {QStringLiteral("source"),
             projectRelativePath(targetRoot,
                                 QFileInfo(asset.assetRoot).absoluteFilePath())},
            {QStringLiteral("files"), fileArray},
        });
    }
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("mode"), QStringLiteral("vendor")},
        {QStringLiteral("assets"), assets},
    };
}

bool planHasDependencyErrors(const DependencyResolution &resolution)
{
    return resolution.hasErrors();
}

void appendUncheckedToolWarning(ImportPlan &plan,
                                const QJsonObject &targetTools)
{
    if (!targetTools.isEmpty()) {
        return;
    }
    QSet<QString> requiredTools;
    for (const AssetRecord &asset : plan.dependencies.orderedAssets) {
        for (const QString &tool : asset.manifest.tools.keys()) {
            requiredTools.insert(tool);
        }
    }
    if (requiredTools.isEmpty()) {
        return;
    }
    QStringList tools(requiredTools.begin(), requiredTools.end());
    std::sort(tools.begin(), tools.end());
    plan.issues.append(ImportIssue{
        .severity = Diagnostic::Severity::Warning,
        .message =
            QStringLiteral("Target tool versions were not supplied; compatibility is unverified for: %1")
                .arg(tools.join(QStringLiteral(", "))),
        .path = plan.targetRoot,
    });
}

QString actionName(const PlannedFileAction action)
{
    return plannedFileActionToString(action);
}

struct JournalEntry {
    QString relativePath;
    PlannedFileAction action = PlannedFileAction::Add;
};

QJsonObject journalDocument(const QString &targetRoot,
                            const QList<JournalEntry> &entries,
                            const QString &lockfileRelative,
                            const bool lockfileExisted)
{
    QJsonArray fileArray;
    for (const JournalEntry &entry : entries) {
        fileArray.append(QJsonObject{
            {QStringLiteral("path"), entry.relativePath},
            {QStringLiteral("action"), actionName(entry.action)},
        });
    }
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("targetRoot"), QFileInfo(targetRoot).absoluteFilePath()},
        {QStringLiteral("files"), fileArray},
        {QStringLiteral("lockfile"), lockfileRelative},
        {QStringLiteral("lockfileExisted"), lockfileExisted},
    };
}

PlannedFileAction actionFromString(const QString &value)
{
    if (value == QStringLiteral("overwrite")) {
        return PlannedFileAction::Overwrite;
    }
    if (value == QStringLiteral("remove")) {
        return PlannedFileAction::Remove;
    }
    if (value == QStringLiteral("conflict")) {
        return PlannedFileAction::Conflict;
    }
    if (value == QStringLiteral("skip")) {
        return PlannedFileAction::Skip;
    }
    return PlannedFileAction::Add;
}

bool removeFileIfExists(const QString &path)
{
    return !QFileInfo::exists(path) || QFile::remove(path);
}

bool restoreTransaction(const QString &transactionRoot,
                        const QJsonObject &journal,
                        QString *error)
{
    const QString targetRoot = journal.value(QStringLiteral("targetRoot")).toString();
    if (!QFileInfo(targetRoot).isDir()
        || absolutePathHasLinkComponent(targetRoot)
        || absolutePathHasLinkComponent(transactionRoot)) {
        if (error) {
            *error =
                QStringLiteral("Transaction path is unavailable or link-backed: %1")
                    .arg(targetRoot);
        }
        return false;
    }

    QList<JournalEntry> entries;
    for (const QJsonValue &value : journal.value(QStringLiteral("files")).toArray()) {
        const QJsonObject object = value.toObject();
        entries.append(JournalEntry{
            .relativePath = normalizedRelative(object.value(QStringLiteral("path")).toString()),
            .action = actionFromString(object.value(QStringLiteral("action")).toString()),
        });
    }
    std::reverse(entries.begin(), entries.end());

    const QString stageRoot = QDir(transactionRoot).absoluteFilePath(QStringLiteral("stage"));
    const QString backupRoot = QDir(transactionRoot).absoluteFilePath(QStringLiteral("backup"));
    for (const JournalEntry &entry : entries) {
        if (!isSafeRelativePath(entry.relativePath)) {
            if (error) {
                *error = QStringLiteral("Unsafe recovery path: %1").arg(entry.relativePath);
            }
            return false;
        }
        const QString destination =
            QDir(targetRoot).absoluteFilePath(entry.relativePath);
        const QString staged = QDir(stageRoot).absoluteFilePath(entry.relativePath);
        const QString backup = QDir(backupRoot).absoluteFilePath(entry.relativePath);
        if (hasLinkComponent(targetRoot, entry.relativePath)
            || hasLinkComponent(transactionRoot,
                                normalizedRelative(
                                    QStringLiteral("stage/%1")
                                        .arg(entry.relativePath)))
            || hasLinkComponent(transactionRoot,
                                normalizedRelative(
                                    QStringLiteral("backup/%1")
                                        .arg(entry.relativePath)))) {
            if (error) {
                *error = QStringLiteral(
                    "Recovery path contains a symbolic link or junction: %1")
                             .arg(entry.relativePath);
            }
            return false;
        }
        if (QFileInfo::exists(backup)) {
            if (!removeFileIfExists(destination)
                || !QDir().mkpath(QFileInfo(destination).absolutePath())
                || !QFile::rename(backup, destination)) {
                if (error) {
                    *error = QStringLiteral("Cannot restore backup for %1").arg(destination);
                }
                return false;
            }
        } else if (entry.action == PlannedFileAction::Add && !QFileInfo::exists(staged)) {
            if (!removeFileIfExists(destination)) {
                if (error) {
                    *error = QStringLiteral("Cannot remove partially added file %1")
                                 .arg(destination);
                }
                return false;
            }
        }
    }

    const QString lockfileRelative =
        normalizedRelative(journal.value(QStringLiteral("lockfile")).toString());
    if (isSafeRelativePath(lockfileRelative)) {
        const QString lockfile = QDir(targetRoot).absoluteFilePath(lockfileRelative);
        const QString stagedLock =
            QDir(stageRoot).absoluteFilePath(QStringLiteral("__lockfile.json"));
        const QString backupLock =
            QDir(backupRoot).absoluteFilePath(QStringLiteral("__lockfile.json"));
        if (hasLinkComponent(targetRoot, lockfileRelative)
            || hasLinkComponent(transactionRoot,
                                QStringLiteral("stage/__lockfile.json"))
            || hasLinkComponent(transactionRoot,
                                QStringLiteral("backup/__lockfile.json"))) {
            if (error) {
                *error = QStringLiteral(
                    "Recovery lockfile path contains a symbolic link or junction");
            }
            return false;
        }
        if (QFileInfo::exists(backupLock)) {
            if (!removeFileIfExists(lockfile) || !QFile::rename(backupLock, lockfile)) {
                if (error) {
                    *error = QStringLiteral("Cannot restore previous lockfile");
                }
                return false;
            }
        } else if (!journal.value(QStringLiteral("lockfileExisted")).toBool()
                   && !QFileInfo::exists(stagedLock)) {
            if (!removeFileIfExists(lockfile)) {
                if (error) {
                    *error = QStringLiteral("Cannot remove partial lockfile");
                }
                return false;
            }
        }
    }
    return true;
}

void removeEmptyParents(const QStringList &directories, const QString &targetRoot)
{
    QStringList sorted = directories;
    std::sort(sorted.begin(), sorted.end(), [](const QString &left, const QString &right) {
        return left.size() > right.size();
    });
    const QString canonicalTarget = QFileInfo(targetRoot).absoluteFilePath();
    for (const QString &directory : sorted) {
        const QString absolute = QFileInfo(directory).absoluteFilePath();
        if (absolute != canonicalTarget && absolute.startsWith(canonicalTarget)) {
            QDir().rmdir(absolute);
        }
    }
}

} // namespace

QString plannedFileActionToString(const PlannedFileAction action)
{
    switch (action) {
    case PlannedFileAction::Add:
        return QStringLiteral("add");
    case PlannedFileAction::Overwrite:
        return QStringLiteral("overwrite");
    case PlannedFileAction::Remove:
        return QStringLiteral("remove");
    case PlannedFileAction::Conflict:
        return QStringLiteral("conflict");
    case PlannedFileAction::Skip:
        return QStringLiteral("skip");
    }
    return QStringLiteral("conflict");
}

bool ImportPlan::hasErrors() const
{
    if (dependencies.hasErrors()) {
        return true;
    }
    return std::any_of(issues.cbegin(), issues.cend(), [](const ImportIssue &issue) {
        return issue.severity == Diagnostic::Severity::Error;
    });
}

bool ImportPlan::hasConflicts() const
{
    return std::any_of(files.cbegin(), files.cend(), [](const PlannedFile &file) {
        return file.action == PlannedFileAction::Conflict;
    });
}

bool ImportPlan::canExecute() const
{
    return !hasErrors() && !hasConflicts();
}

QMap<PlannedFileAction, int> ImportPlan::actionCounts() const
{
    QMap<PlannedFileAction, int> result;
    for (const PlannedFile &file : files) {
        result[file.action] += 1;
    }
    return result;
}

bool VendorUpgradePlan::hasChanges() const
{
    return std::any_of(assets.cbegin(), assets.cend(), [](const AssetUpgrade &asset) {
        return asset.kind != AssetUpgradeKind::Unchanged;
    });
}

bool VendorUpgradePlan::canExecute() const
{
    return importPlan.canExecute();
}

ImportPlan ImportService::planReference(const QList<AssetRecord> &catalog,
                                        const QStringList &rootAssetIds,
                                        const QString &targetRoot,
                                        const QJsonObject &targetTools) const
{
    ImportPlan plan;
    plan.mode = ImportMode::Reference;
    plan.targetRoot = QFileInfo(targetRoot).absoluteFilePath();
    plan.outputPath =
        QDir(plan.targetRoot).absoluteFilePath(QStringLiteral(".xips/references.json"));
    plan.dependencies = DependencyResolver().resolve(catalog, rootAssetIds, targetTools);
    const QJsonObject generated =
        referenceDocument(plan.dependencies, plan.targetRoot);
    QJsonObject existing;
    const QFileInfo outputInfo(plan.outputPath);
    if (outputInfo.exists() || outputInfo.isSymLink()) {
        plan.outputDocumentExisted = true;
        QString readError;
        if (!readReferenceDocument(plan.outputPath, &existing, &readError)) {
            plan.issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message = readError,
                .path = plan.outputPath,
            });
        } else {
            plan.expectedOutputHash = fileHash(plan.outputPath);
            if (plan.expectedOutputHash.isEmpty()) {
                plan.issues.append(ImportIssue{
                    .severity = Diagnostic::Severity::Error,
                    .message =
                        QStringLiteral("Cannot hash existing Reference document"),
                    .path = plan.outputPath,
                });
            }
        }
    }
    plan.outputDocument = mergeReferenceDocuments(existing, generated);
    appendUncheckedToolWarning(plan, targetTools);
    if (!QFileInfo(plan.targetRoot).isDir()) {
        plan.issues.append(ImportIssue{
            .severity = Diagnostic::Severity::Error,
            .message = QStringLiteral("Target project directory does not exist"),
            .path = plan.targetRoot,
        });
    } else if (hasLinkComponent(plan.targetRoot,
                                QStringLiteral(".xips/references.json"))) {
        plan.issues.append(ImportIssue{
            .severity = Diagnostic::Severity::Error,
            .message = QStringLiteral(
                "Reference destination contains a symbolic link or junction"),
            .path = plan.outputPath,
        });
    }
    return plan;
}

bool ImportService::writeReference(const ImportPlan &plan, QString *error) const
{
    if (plan.mode != ImportMode::Reference) {
        if (error) {
            *error = QStringLiteral("Not a Reference import plan");
        }
        return false;
    }
    if (!plan.canExecute()) {
        if (error) {
            *error = QStringLiteral("Reference plan contains errors");
        }
        return false;
    }
    const QString expectedOutputPath =
        QDir(plan.targetRoot).absoluteFilePath(
            QStringLiteral(".xips/references.json"));
    if (!QFileInfo(plan.targetRoot).isDir()
        || QFileInfo(plan.outputPath).absoluteFilePath().compare(
               QFileInfo(expectedOutputPath).absoluteFilePath(),
               Qt::CaseInsensitive)
               != 0
        || plan.outputDocument.value(QStringLiteral("schemaVersion")).toInt()
               != 1
        || plan.outputDocument.value(QStringLiteral("mode")).toString()
               != QStringLiteral("reference")
        || !plan.outputDocument.value(QStringLiteral("assets")).isArray()) {
        if (error) {
            *error = QStringLiteral(
                "Unsafe or invalid Reference output document");
        }
        return false;
    }
    if (hasLinkComponent(plan.targetRoot,
                         QStringLiteral(".xips/references.json"))) {
        if (error) {
            *error = QStringLiteral(
                "Reference destination contains a symbolic link or junction");
        }
        return false;
    }
    const QFileInfo current(plan.outputPath);
    if (current.isSymLink()) {
        if (error) {
            *error = QStringLiteral(
                "Reference document changed to a symbolic link after planning");
        }
        return false;
    }
    const bool currentlyExists = current.exists();
    if (currentlyExists != plan.outputDocumentExisted
        || (currentlyExists
            && fileHash(plan.outputPath) != plan.expectedOutputHash)) {
        if (error) {
            *error =
                QStringLiteral("Reference document changed after planning");
        }
        return false;
    }
    return writeJsonAtomic(plan.outputPath, plan.outputDocument, error);
}

QList<ReferenceState> ImportService::inspectReferences(
    const QJsonObject &referenceDocument,
    const QList<AssetRecord> &catalog,
    const QString &targetRoot) const
{
    QHash<QString, QList<AssetRecord>> assetsById;
    for (const AssetRecord &asset : catalog) {
        assetsById[asset.manifest.id].append(asset);
    }

    QList<ReferenceState> result;
    for (const QJsonValue &assetValue :
         referenceDocument.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject object = assetValue.toObject();
        ReferenceState state;
        state.assetId = object.value(QStringLiteral("id")).toString();
        const QString configured = object.value(QStringLiteral("source")).toString();
        state.resolvedSourcePath = QDir::isAbsolutePath(configured)
                                       ? QDir::cleanPath(configured)
                                       : QDir(targetRoot).absoluteFilePath(configured);
        state.available = QFileInfo(state.resolvedSourcePath).isDir();
        const QString expectedHash = object.value(QStringLiteral("contentHash")).toString();
        for (const AssetRecord &candidate : assetsById.value(state.assetId)) {
            const bool samePath =
                QFileInfo(candidate.assetRoot).absoluteFilePath().compare(
                    QFileInfo(state.resolvedSourcePath).absoluteFilePath(),
                    Qt::CaseInsensitive)
                == 0;
            if (samePath && candidate.contentHash == expectedHash) {
                state.hashMatches = true;
            }
            if (candidate.contentHash == expectedHash || expectedHash.isEmpty()) {
                state.repairableById = true;
            }
        }
        result.append(state);
    }
    return result;
}

bool ImportService::repairReferences(QJsonObject &referenceDocument,
                                     const QList<AssetRecord> &catalog,
                                     const QString &targetRoot,
                                     QString *error) const
{
    QHash<QString, QList<AssetRecord>> assetsById;
    for (const AssetRecord &asset : catalog) {
        assetsById[asset.manifest.id].append(asset);
    }

    QJsonArray updatedAssets;
    for (const QJsonValue &assetValue :
         referenceDocument.value(QStringLiteral("assets")).toArray()) {
        QJsonObject object = assetValue.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        const QString expectedHash = object.value(QStringLiteral("contentHash")).toString();
        const QList<AssetRecord> candidates = assetsById.value(id);
        const auto iterator = std::find_if(
            candidates.cbegin(),
            candidates.cend(),
            [&expectedHash](const AssetRecord &candidate) {
                return expectedHash.isEmpty() || candidate.contentHash == expectedHash;
            });
        if (iterator == candidates.cend()) {
            if (error) {
                *error = QStringLiteral("No compatible asset found to repair reference '%1'")
                             .arg(id);
            }
            return false;
        }

        object.insert(QStringLiteral("source"),
                      projectRelativePath(targetRoot, iterator->assetRoot));
        QJsonArray sources;
        for (const QString &source : iterator->manifest.sources) {
            sources.append(projectRelativePath(targetRoot,
                                               absolutePath(iterator->assetRoot, source)));
        }
        QJsonArray includeDirectories;
        for (const QString &includeDirectory : iterator->manifest.includeDirs) {
            includeDirectories.append(projectRelativePath(
                targetRoot,
                absolutePath(iterator->assetRoot, includeDirectory)));
        }
        object.insert(QStringLiteral("sources"), sources);
        object.insert(QStringLiteral("includeDirs"), includeDirectories);
        object.insert(QStringLiteral("defines"), json::toArray(iterator->manifest.defines));
        updatedAssets.append(object);
    }
    referenceDocument.insert(QStringLiteral("assets"), updatedAssets);
    return true;
}

bool ImportService::writeReferenceDocument(
    const QString &path,
    const QJsonObject &referenceDocument,
    QString *error) const
{
    if (!validateReferenceDocument(referenceDocument, error)) {
        return false;
    }
    if (absolutePathHasLinkComponent(path)) {
        if (error) {
            *error = QStringLiteral(
                "Reference document path contains a symbolic link or junction");
        }
        return false;
    }
    return writeJsonAtomic(path, referenceDocument, error);
}

bool ImportService::writeReferenceDocumentIfUnchanged(
    const QString &path,
    const QJsonObject &referenceDocument,
    const QString &expectedContentHash,
    QString *error) const
{
    if (expectedContentHash.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "Reference repair requires the inspected document hash");
        }
        return false;
    }
    if (!validateReferenceDocument(referenceDocument, error)) {
        return false;
    }
    if (absolutePathHasLinkComponent(path)) {
        if (error) {
            *error = QStringLiteral(
                "Reference document path contains a symbolic link or junction");
        }
        return false;
    }
    const QFileInfo current(path);
    if (!current.exists() || !current.isFile() || isLinkLike(current)
        || fileHash(path) != expectedContentHash) {
        if (error) {
            *error =
                QStringLiteral("Reference document changed after inspection");
        }
        return false;
    }
    return writeJsonAtomic(path, referenceDocument, error);
}

ImportPlan ImportService::planVendor(const QList<AssetRecord> &catalog,
                                     const QStringList &rootAssetIds,
                                     const QString &targetRoot,
                                     const QJsonObject &targetTools) const
{
    ImportPlan plan;
    plan.mode = ImportMode::Vendor;
    plan.targetRoot = QFileInfo(targetRoot).absoluteFilePath();
    plan.outputPath = QDir(plan.targetRoot).absoluteFilePath(QStringLiteral("xips-lock.json"));
    plan.dependencies = DependencyResolver().resolve(catalog, rootAssetIds, targetTools);
    appendUncheckedToolWarning(plan, targetTools);
    if (!QFileInfo(plan.targetRoot).isDir()) {
        plan.issues.append(ImportIssue{
            .severity = Diagnostic::Severity::Error,
            .message = QStringLiteral("Target project directory does not exist"),
            .path = plan.targetRoot,
        });
        return plan;
    }
    if (hasLinkComponent(plan.targetRoot, QStringLiteral("xips-lock.json"))
        || hasLinkComponent(plan.targetRoot,
                            QStringLiteral(".xips/transactions"))) {
        plan.issues.append(ImportIssue{
            .severity = Diagnostic::Severity::Error,
            .message = QStringLiteral(
                "Vendor metadata destination contains a symbolic link or junction"),
            .path = plan.targetRoot,
        });
        return plan;
    }
    if (planHasDependencyErrors(plan.dependencies)) {
        return plan;
    }

    QJsonObject previousLock;
    const QFileInfo previousLockInfo(plan.outputPath);
    if (previousLockInfo.exists() || previousLockInfo.isSymLink()) {
        plan.outputDocumentExisted = true;
        QString lockError;
        if (!readVendorLockDocument(plan.outputPath,
                                    &previousLock,
                                    &lockError)) {
            plan.issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message = lockError,
                .path = plan.outputPath,
            });
            return plan;
        }
        plan.expectedOutputHash = fileHash(plan.outputPath);
        if (plan.expectedOutputHash.isEmpty()) {
            plan.issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message = QStringLiteral("Cannot hash existing Vendor lockfile"),
                .path = plan.outputPath,
            });
            return plan;
        }
    }
    const QHash<QString, QString> previousHashes = ownedHashes(previousLock);
    const QHash<QString, QString> previousAssetIds = ownedAssetIds(previousLock);
    QSet<QString> requestedDestinations;

    for (const AssetRecord &asset : plan.dependencies.orderedAssets) {
        const QList<QPair<QString, QString>> sources = vendorSourceFiles(asset, plan.issues);
        for (const auto &[source, assetRelative] : sources) {
            const QString destinationRelative =
                normalizedRelative(QStringLiteral("vendor/xips/%1/%2")
                                       .arg(asset.manifest.id, assetRelative));
            if (!isSafeRelativePath(destinationRelative)) {
                plan.issues.append(ImportIssue{
                    .severity = Diagnostic::Severity::Error,
                    .message = QStringLiteral("Unsafe Vendor destination path"),
                    .path = destinationRelative,
                });
                continue;
            }
            const QString destination =
                QDir(plan.targetRoot).absoluteFilePath(destinationRelative);
            if (hasLinkComponent(plan.targetRoot, destinationRelative)) {
                plan.issues.append(ImportIssue{
                    .severity = Diagnostic::Severity::Error,
                    .message =
                        QStringLiteral("Vendor destination contains a symbolic link or junction"),
                    .path = destination,
                });
                continue;
            }
            PlannedFile file;
            file.assetId = asset.manifest.id;
            file.sourcePath = source;
            file.destinationPath = destinationRelative;
            file.sourceHash = fileHash(source);
            file.previouslyOwnedHash = previousHashes.value(destinationRelative);
            if (file.sourceHash.isEmpty()) {
                plan.issues.append(ImportIssue{
                    .severity = Diagnostic::Severity::Error,
                    .message = QStringLiteral("Cannot hash source file"),
                    .path = source,
                });
                continue;
            }

            if (!QFileInfo::exists(destination)) {
                file.action = PlannedFileAction::Add;
            } else {
                file.existingHash = fileHash(destination);
                if (file.existingHash == file.sourceHash) {
                    file.action = PlannedFileAction::Skip;
                } else if (!file.previouslyOwnedHash.isEmpty()
                           && file.existingHash == file.previouslyOwnedHash) {
                    file.action = PlannedFileAction::Overwrite;
                } else {
                    file.action = PlannedFileAction::Conflict;
                }
            }
            plan.files.append(file);
            requestedDestinations.insert(destinationRelative);
        }
    }
    QStringList previouslyOwnedPaths = previousHashes.keys();
    std::sort(previouslyOwnedPaths.begin(), previouslyOwnedPaths.end());
    for (const QString &path : previouslyOwnedPaths) {
        if (requestedDestinations.contains(path)) {
            continue;
        }
        if (!isSafeRelativePath(path)
            || hasLinkComponent(plan.targetRoot, path)) {
            plan.issues.append(ImportIssue{
                .severity = Diagnostic::Severity::Error,
                .message =
                    QStringLiteral("Unsafe path in existing Vendor lockfile"),
                .path = path,
            });
            continue;
        }
        const QString destination = QDir(plan.targetRoot).absoluteFilePath(path);
        if (!QFileInfo::exists(destination)) {
            continue;
        }
        PlannedFile file;
        file.assetId = previousAssetIds.value(path);
        file.destinationPath = path;
        file.previouslyOwnedHash = previousHashes.value(path);
        file.existingHash = fileHash(destination);
        file.action = file.existingHash == file.previouslyOwnedHash
                          ? PlannedFileAction::Remove
                          : PlannedFileAction::Conflict;
        plan.files.append(file);
    }
    std::sort(plan.files.begin(),
              plan.files.end(),
              [](const PlannedFile &left, const PlannedFile &right) {
                  if (left.assetId != right.assetId) {
                      return left.assetId < right.assetId;
                  }
                  return left.destinationPath < right.destinationPath;
              });
    plan.outputDocument =
        lockDocument(plan.dependencies, plan.files, plan.targetRoot);
    return plan;
}

VendorUpgradePlan ImportService::planVendorUpgrade(
    const QList<AssetRecord> &catalog,
    const QStringList &rootAssetIds,
    const QString &targetRoot,
    const QJsonObject &targetTools) const
{
    VendorUpgradePlan upgrade;
    upgrade.importPlan =
        planVendor(catalog, rootAssetIds, targetRoot, targetTools);
    const QJsonObject previousLock = readJsonObject(
        QDir(targetRoot).absoluteFilePath(QStringLiteral("xips-lock.json")));

    QMap<QString, QJsonObject> before;
    QMap<QString, QJsonObject> after;
    for (const QJsonValue &value : previousLock.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        before.insert(asset.value(QStringLiteral("id")).toString(), asset);
    }
    for (const QJsonValue &value :
         upgrade.importPlan.outputDocument.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        after.insert(asset.value(QStringLiteral("id")).toString(), asset);
    }

    QSet<QString> ids(before.keyBegin(), before.keyEnd());
    ids.unite(QSet<QString>(after.keyBegin(), after.keyEnd()));
    QStringList ordered(ids.begin(), ids.end());
    std::sort(ordered.begin(), ordered.end());
    for (const QString &id : ordered) {
        const QJsonObject oldAsset = before.value(id);
        const QJsonObject newAsset = after.value(id);
        AssetUpgrade asset{
            .assetId = id,
            .beforeVersion = oldAsset.value(QStringLiteral("version")).toString(),
            .afterVersion = newAsset.value(QStringLiteral("version")).toString(),
            .beforeContentHash =
                oldAsset.value(QStringLiteral("contentHash")).toString(),
            .afterContentHash =
                newAsset.value(QStringLiteral("contentHash")).toString(),
        };
        if (!before.contains(id)) {
            asset.kind = AssetUpgradeKind::Added;
        } else if (!after.contains(id)) {
            asset.kind = AssetUpgradeKind::Removed;
        } else if (asset.beforeVersion != asset.afterVersion
                   || asset.beforeContentHash != asset.afterContentHash) {
            asset.kind = AssetUpgradeKind::Changed;
        } else {
            asset.kind = AssetUpgradeKind::Unchanged;
        }
        upgrade.assets.append(asset);
    }
    return upgrade;
}

ImportExecutionResult ImportService::executeVendor(
    const ImportPlan &plan,
    const ImportExecutionOptions &options) const
{
    ImportExecutionResult result;
    if (plan.mode != ImportMode::Vendor) {
        result.error = QStringLiteral("Not a Vendor import plan");
        return result;
    }
    if (options.cancelled
        && options.cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.error = QStringLiteral("Vendor import cancelled");
        return result;
    }
    if (!options.confirmed) {
        result.error = QStringLiteral("Vendor import requires explicit confirmation");
        return result;
    }
    if (!plan.canExecute()) {
        result.error = QStringLiteral("Vendor plan contains errors or file conflicts");
        return result;
    }

    QString recoveryError;
    if (!recoverVendorTransactions(plan.targetRoot, nullptr, &recoveryError)) {
        result.error = QStringLiteral("Cannot recover previous transaction: %1")
                           .arg(recoveryError);
        return result;
    }

    const QString expectedOutputPath =
        QDir(plan.targetRoot).absoluteFilePath(QStringLiteral("xips-lock.json"));
    if (!QFileInfo(plan.targetRoot).isDir()
        || QFileInfo(plan.outputPath).absoluteFilePath().compare(
               QFileInfo(expectedOutputPath).absoluteFilePath(),
               Qt::CaseInsensitive)
               != 0
        || hasLinkComponent(plan.targetRoot,
                            QStringLiteral(".xips/transactions"))
        || hasLinkComponent(plan.targetRoot,
                            QStringLiteral("xips-lock.json"))) {
        result.error =
            QStringLiteral("Unsafe Vendor target or metadata destination");
        return result;
    }
    const QFileInfo currentLock(plan.outputPath);
    const bool currentLockExists =
        currentLock.exists() || currentLock.isSymLink();
    if (currentLockExists != plan.outputDocumentExisted
        || (currentLockExists
            && (currentLock.isSymLink()
                || fileHash(plan.outputPath)
                       != plan.expectedOutputHash))) {
        result.error = QStringLiteral("Vendor lockfile changed after planning");
        return result;
    }
    for (const PlannedFile &file : plan.files) {
        if (!isSafeRelativePath(file.destinationPath)
            || hasLinkComponent(plan.targetRoot, file.destinationPath)) {
            result.error =
                QStringLiteral("Unsafe Vendor destination: %1")
                    .arg(file.destinationPath);
            return result;
        }
    }

    const QString transactionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString transactionRoot = QDir(plan.targetRoot).absoluteFilePath(
        QStringLiteral(".xips/transactions/%1").arg(transactionId));
    const QString stageRoot =
        QDir(transactionRoot).absoluteFilePath(QStringLiteral("stage"));
    const QString backupRoot =
        QDir(transactionRoot).absoluteFilePath(QStringLiteral("backup"));
    if (!QDir().mkpath(stageRoot) || !QDir().mkpath(backupRoot)) {
        result.error = QStringLiteral("Cannot create Vendor transaction directory");
        return result;
    }

    QList<JournalEntry> entries;
    for (const PlannedFile &file : plan.files) {
        if (options.cancelled
            && options.cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.error = QStringLiteral("Vendor import cancelled");
            QDir(transactionRoot).removeRecursively();
            return result;
        }
        if (file.action == PlannedFileAction::Skip) {
            result.skipped += 1;
            continue;
        }
        entries.append(JournalEntry{
            .relativePath = file.destinationPath,
            .action = file.action,
        });
        if (hasLinkComponent(plan.targetRoot, file.destinationPath)) {
            result.error =
                QStringLiteral("Vendor destination changed to a symbolic link or junction: %1")
                    .arg(file.destinationPath);
            QDir(transactionRoot).removeRecursively();
            return result;
        }
        const QString destination =
            QDir(plan.targetRoot).absoluteFilePath(file.destinationPath);
        if (file.action == PlannedFileAction::Remove) {
            if (!QFileInfo::exists(destination)
                || fileHash(destination) != file.existingHash) {
                result.error =
                    QStringLiteral("Destination changed after planning: %1").arg(destination);
                QDir(transactionRoot).removeRecursively();
                return result;
            }
            continue;
        }
        if (isLinkLike(QFileInfo(file.sourcePath))
            || fileHash(file.sourcePath) != file.sourceHash) {
            result.error = QStringLiteral("Source changed after planning: %1").arg(file.sourcePath);
            QDir(transactionRoot).removeRecursively();
            return result;
        }
        if (file.action == PlannedFileAction::Add && QFileInfo::exists(destination)) {
            result.error =
                QStringLiteral("Destination appeared after planning: %1").arg(destination);
            QDir(transactionRoot).removeRecursively();
            return result;
        }
        if (file.action == PlannedFileAction::Overwrite
            && fileHash(destination) != file.existingHash) {
            result.error =
                QStringLiteral("Destination changed after planning: %1").arg(destination);
            QDir(transactionRoot).removeRecursively();
            return result;
        }

        const QString staged =
            QDir(stageRoot).absoluteFilePath(file.destinationPath);
        if (!QDir().mkpath(QFileInfo(staged).absolutePath())
            || !QFile::copy(file.sourcePath, staged)
            || fileHash(staged) != file.sourceHash) {
            result.error = QStringLiteral("Cannot stage Vendor file: %1").arg(file.sourcePath);
            QDir(transactionRoot).removeRecursively();
            return result;
        }
    }

    const QString stagedLock =
        QDir(stageRoot).absoluteFilePath(QStringLiteral("__lockfile.json"));
    if (!writeJsonAtomic(stagedLock, plan.outputDocument, &result.error)) {
        QDir(transactionRoot).removeRecursively();
        return result;
    }

    const QString lockfileRelative =
        normalizedRelative(QDir(plan.targetRoot).relativeFilePath(plan.outputPath));
    const bool lockfileExisted = QFileInfo::exists(plan.outputPath);
    const QString journalPath =
        QDir(transactionRoot).absoluteFilePath(QStringLiteral("journal.json"));
    if (!writeJsonAtomic(journalPath,
                         journalDocument(plan.targetRoot,
                                         entries,
                                         lockfileRelative,
                                         lockfileExisted),
                         &result.error)) {
        QDir(transactionRoot).removeRecursively();
        return result;
    }

    QStringList createdDirectories;
    QStringList removedDirectories;
    int operations = 0;
    for (const PlannedFile &file : plan.files) {
        if (options.cancelled
            && options.cancelled->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.error = QStringLiteral("Vendor import cancelled");
            break;
        }
        if (file.action == PlannedFileAction::Skip) {
            continue;
        }
        const QString destination =
            QDir(plan.targetRoot).absoluteFilePath(file.destinationPath);
        if (hasLinkComponent(plan.targetRoot, file.destinationPath)) {
            result.error =
                QStringLiteral("Vendor destination changed to a symbolic link or junction: %1")
                    .arg(file.destinationPath);
            break;
        }
        const QString staged =
            QDir(stageRoot).absoluteFilePath(file.destinationPath);
        const QString backup =
            QDir(backupRoot).absoluteFilePath(file.destinationPath);
        const QString destinationDirectory = QFileInfo(destination).absolutePath();
        if (file.action == PlannedFileAction::Remove) {
            if (!QDir().mkpath(QFileInfo(backup).absolutePath())
                || !QFile::rename(destination, backup)) {
                result.error = QStringLiteral("Cannot stage removal of destination file: %1")
                                   .arg(destination);
                break;
            }
            removedDirectories.append(destinationDirectory);
            result.removed += 1;
            operations += 1;
            if (options.failAfterFileOperations >= 0
                && operations >= options.failAfterFileOperations) {
                result.error = QStringLiteral("Injected Vendor transaction failure");
                break;
            }
            continue;
        }
        if (!QFileInfo(destinationDirectory).exists()) {
            QString directory = destinationDirectory;
            const QString targetAbsolute = QFileInfo(plan.targetRoot).absoluteFilePath();
            while (directory != targetAbsolute && !QFileInfo(directory).exists()) {
                const QString relative =
                    normalizedRelative(QDir(targetAbsolute).relativeFilePath(directory));
                if (!isSafeRelativePath(relative)) {
                    break;
                }
                createdDirectories.append(directory);
                directory = QFileInfo(directory).absolutePath();
            }
            if (!QDir().mkpath(destinationDirectory)) {
                result.error = QStringLiteral("Cannot create destination directory: %1")
                                   .arg(destinationDirectory);
                break;
            }
        }

        if (file.action == PlannedFileAction::Overwrite) {
            if (!QDir().mkpath(QFileInfo(backup).absolutePath())
                || !QFile::rename(destination, backup)) {
                result.error = QStringLiteral("Cannot back up destination file: %1")
                                   .arg(destination);
                break;
            }
        }
        if (!QFile::rename(staged, destination)) {
            if (QFileInfo::exists(backup)) {
                QFile::rename(backup, destination);
            }
            result.error = QStringLiteral("Cannot publish Vendor file: %1").arg(destination);
            break;
        }

        if (file.action == PlannedFileAction::Add) {
            result.added += 1;
        } else {
            result.overwritten += 1;
        }
        operations += 1;
        if (options.failAfterFileOperations >= 0
            && operations >= options.failAfterFileOperations) {
            result.error = QStringLiteral("Injected Vendor transaction failure");
            break;
        }
    }

    if (result.error.isEmpty() && options.cancelled
        && options.cancelled->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        result.error = QStringLiteral("Vendor import cancelled");
    }

    if (result.error.isEmpty()) {
        if (hasLinkComponent(plan.targetRoot,
                             QStringLiteral("xips-lock.json"))) {
            result.error = QStringLiteral(
                "Vendor lockfile destination changed to a symbolic link or junction");
        }
        const QString backupLock =
            QDir(backupRoot).absoluteFilePath(QStringLiteral("__lockfile.json"));
        if (result.error.isEmpty() && lockfileExisted
            && !QFile::rename(plan.outputPath, backupLock)) {
            result.error = QStringLiteral("Cannot back up existing lockfile");
        } else if (result.error.isEmpty()
                   && (!QDir().mkpath(QFileInfo(plan.outputPath).absolutePath())
                       || !QFile::rename(stagedLock, plan.outputPath))) {
            if (QFileInfo::exists(backupLock)) {
                QFile::rename(backupLock, plan.outputPath);
            }
            result.error = QStringLiteral("Cannot publish Vendor lockfile");
        }
    }

    if (!result.error.isEmpty()) {
        QString rollbackError;
        const bool restored =
            restoreTransaction(transactionRoot, readJsonObject(journalPath), &rollbackError);
        result.rolledBack = restored;
        if (!restored) {
            result.error += QStringLiteral("; rollback failed: ") + rollbackError;
        } else {
            result.added = 0;
            result.overwritten = 0;
            result.removed = 0;
        }
        removeEmptyParents(createdDirectories, plan.targetRoot);
        QDir(transactionRoot).removeRecursively();
        return result;
    }

    QDir(transactionRoot).removeRecursively();
    removeEmptyParents(removedDirectories, plan.targetRoot);
    result.success = true;
    return result;
}

bool ImportService::recoverVendorTransactions(const QString &targetRoot,
                                              QStringList *recovered,
                                              QString *error) const
{
    const QString transactionsPath =
        QDir(targetRoot).absoluteFilePath(QStringLiteral(".xips/transactions"));
    const QDir transactions(transactionsPath);
    if (!transactions.exists()) {
        return true;
    }
    if (hasLinkComponent(targetRoot,
                         QStringLiteral(".xips/transactions"))) {
        if (error) {
            *error = QStringLiteral(
                "Transaction directory must not contain a symbolic link or junction: %1")
                         .arg(transactionsPath);
        }
        return false;
    }
    const QFileInfoList entries =
        transactions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry)) {
            if (error) {
                *error = QStringLiteral("Transaction entry must not be a symbolic link: %1")
                             .arg(entry.absoluteFilePath());
            }
            return false;
        }
        const QString journalPath =
            QDir(entry.absoluteFilePath()).absoluteFilePath(QStringLiteral("journal.json"));
        const QJsonObject journal = readJsonObject(journalPath);
        if (journal.isEmpty()) {
            if (error) {
                *error = QStringLiteral("Transaction has no readable journal: %1")
                             .arg(entry.absoluteFilePath());
            }
            return false;
        }
        if (QFileInfo(journal.value(QStringLiteral("targetRoot")).toString()).absoluteFilePath()
                .compare(QFileInfo(targetRoot).absoluteFilePath(), Qt::CaseInsensitive)
            != 0) {
            if (error) {
                *error = QStringLiteral("Transaction target mismatch: %1")
                             .arg(entry.absoluteFilePath());
            }
            return false;
        }
        if (!restoreTransaction(entry.absoluteFilePath(), journal, error)) {
            return false;
        }
        if (recovered) {
            recovered->append(entry.fileName());
        }
        QDir(entry.absoluteFilePath()).removeRecursively();
    }
    return true;
}

QString ImportService::fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

} // namespace xips
