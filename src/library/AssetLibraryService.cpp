#include "library/AssetLibraryService.h"

#include "assetcore/JsonUtil.h"
#include "library/AssetScanner.h"
#include "library/FileSystemUtil.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
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

bool strictPayloadFingerprint(const QString &root,
                              const QStringList *expectedFiles,
                              QStringList *verifiedFiles,
                              QString *fingerprint,
                              QString *error)
{
    if (verifiedFiles) {
        verifiedFiles->clear();
    }
    if (fingerprint) {
        fingerprint->clear();
    }
    QStringList filesAtStart;
    if (!files::collectPayloadFiles(root,
                                    filesAtStart,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    if (filesAtStart.isEmpty()) {
        return fail(error, QStringLiteral("The selected asset has no payload files"));
    }
    if (expectedFiles && filesAtStart != *expectedFiles) {
        return fail(error,
                    QStringLiteral("The payload file list changed during the operation"));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-payload-v1\0", 16));
    const auto addSizedData = [&hash](const QByteArray &data) {
        hash.addData(QByteArray::number(static_cast<qlonglong>(data.size())));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(data);
    };
    for (const QString &relative : filesAtStart) {
        hash.addData(QByteArrayView("\0path\0", 6));
        addSizedData(relative.toUtf8());
        QFile file(QDir(root).absoluteFilePath(relative));
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(error,
                        QStringLiteral("Cannot read payload file for verification: %1")
                            .arg(file.fileName()));
        }
        const qint64 expectedSize = file.size();
        const QDateTime expectedModified = file.fileTime(
            QFileDevice::FileModificationTime);
        if (expectedSize < 0) {
            return fail(error,
                        QStringLiteral("Cannot determine payload file size: %1")
                            .arg(file.fileName()));
        }
        hash.addData(QByteArrayView("\0data\0", 6));
        hash.addData(QByteArray::number(expectedSize));
        hash.addData(QByteArrayView(":", 1));
        qint64 bytesRead = 0;
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                return fail(error,
                            QStringLiteral("Cannot finish reading payload file: %1")
                                .arg(file.fileName()));
            }
            hash.addData(chunk);
            bytesRead += static_cast<qint64>(chunk.size());
        }
        if (file.error() != QFileDevice::NoError
            || bytesRead != expectedSize || file.size() != expectedSize
            || file.fileTime(QFileDevice::FileModificationTime)
                   != expectedModified) {
            return fail(error,
                        QStringLiteral("Payload file changed while it was being verified: %1")
                            .arg(file.fileName()));
        }
    }
    QStringList filesAtEnd;
    if (!files::collectPayloadFiles(root,
                                    filesAtEnd,
                                    files::LinkPolicy::Reject,
                                    error)
        || filesAtEnd != filesAtStart) {
        return fail(error,
                    QStringLiteral("The payload file list changed while it was being verified"));
    }
    if (verifiedFiles) {
        *verifiedFiles = filesAtEnd;
    }
    if (fingerprint) {
        *fingerprint = QStringLiteral("sha256:")
                       + QString::fromLatin1(hash.result().toHex());
    }
    return true;
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

bool strictNonEmptyAssetFingerprintAtRoot(const QString &root,
                                          const QString &expectedAssetId,
                                          Manifest *manifest,
                                          QStringList *files,
                                          QString *contentFingerprint,
                                          QString *payloadFingerprint,
                                          QString *error)
{
    QStringList initialFiles;
    QString initialPayloadFingerprint;
    if (!strictPayloadFingerprint(root,
                                  nullptr,
                                  &initialFiles,
                                  &initialPayloadFingerprint,
                                  error)) {
        return false;
    }
    Manifest verifiedManifest;
    QString verifiedContentFingerprint;
    if (!strictFingerprintAtRoot(root,
                                 expectedAssetId,
                                 &verifiedManifest,
                                 &verifiedContentFingerprint,
                                 error)) {
        return false;
    }
    QStringList confirmedFiles;
    QString confirmedPayloadFingerprint;
    if (!strictPayloadFingerprint(root,
                                  &initialFiles,
                                  &confirmedFiles,
                                  &confirmedPayloadFingerprint,
                                  error)
        || confirmedPayloadFingerprint != initialPayloadFingerprint) {
        return fail(error,
                    QStringLiteral("The asset payload changed while it was being verified"));
    }
    if (manifest) {
        *manifest = verifiedManifest;
    }
    if (files) {
        *files = confirmedFiles;
    }
    if (contentFingerprint) {
        *contentFingerprint = verifiedContentFingerprint;
    }
    if (payloadFingerprint) {
        *payloadFingerprint = confirmedPayloadFingerprint;
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

bool validateWorkingCopyUndoTokenEnvelope(const AssetRecord &asset,
                                          const WorkingCopyUndoToken &token,
                                          QString *error)
{
    const QString assetRoot = files::normalizedAbsolute(asset.assetRoot);
    const QString recovery = files::normalizedAbsolute(token.recoveryPath);
    const QFileInfo recoveryInfo(recovery);
    const QString recoveryParent = files::normalizedAbsolute(
        recoveryInfo.absolutePath());
    const QString assetParent = files::normalizedAbsolute(
        QFileInfo(assetRoot).absolutePath());
    static const QRegularExpression recoveryName(
        QStringLiteral(
            "^\\.xips-create-recovery-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"),
        QRegularExpression::CaseInsensitiveOption);
    if (!token.isValid() || token.assetId != asset.manifest.id
        || !samePath(token.assetRoot, asset.assetRoot)
        || !validFingerprint(token.publishedFingerprint)
        || !validFingerprint(token.recoveryFingerprint)
        || !samePath(recoveryParent, assetParent)
        || !recoveryName.match(recoveryInfo.fileName()).hasMatch()) {
        return fail(error,
                    QStringLiteral("The working-copy Undo token is invalid"));
    }
    return true;
}

bool validateWorkingCopyUndoToken(const AssetRecord &asset,
                                  const WorkingCopyUndoToken &token,
                                  QString *error)
{
    if (!validateWorkingCopyUndoTokenEnvelope(asset, token, error)) {
        return false;
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
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("version"), version.version},
        {QStringLiteral("createdAt"), version.createdAt.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("contentHash"), version.contentHash},
        {QStringLiteral("strictContentHash"), version.strictContentHash},
    };
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        return fail(error,
                    QStringLiteral("Cannot publish version metadata: %1")
                        .arg(file.errorString()));
    }
    return true;
}

struct SnapshotProof {
    VersionInfo info;
    Manifest manifest;
    QStringList files;
    QString payloadFingerprint;
    QString proofFingerprint;
    QByteArray metadataCanonical;
    QByteArray manifestCanonical;
};

bool readStableFile(const QString &path, QByteArray *contents, QString *error)
{
    if (contents) {
        contents->clear();
    }
    const QFileInfo before(path);
    if (!before.isFile() || files::isLinkLike(before)) {
        return fail(error,
                    QStringLiteral("Required snapshot file is missing or linked: %1")
                        .arg(files::normalizedAbsolute(path)));
    }
    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error,
                    QStringLiteral("Cannot read snapshot file: %1")
                        .arg(before.absoluteFilePath()));
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return fail(error,
                    QStringLiteral("Cannot finish reading snapshot file: %1")
                        .arg(before.absoluteFilePath()));
    }
    const QFileInfo after(path);
    if (!after.isFile() || files::isLinkLike(after)
        || after.size() != before.size()
        || after.lastModified() != before.lastModified()
        || data.size() != before.size()) {
        return fail(error,
                    QStringLiteral("Snapshot file changed while it was being read: %1")
                        .arg(before.absoluteFilePath()));
    }
    if (contents) {
        *contents = data;
    }
    return true;
}

bool readSnapshotMetadataStable(const QString &path,
                                VersionInfo *version,
                                QByteArray *canonical,
                                QString *error)
{
    QByteArray contents;
    if (!readStableFile(path, &contents, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error,
                    QStringLiteral("Invalid version metadata: %1")
                        .arg(files::normalizedAbsolute(path)));
    }
    const QJsonObject object = document.object();
    const QJsonValue schemaValue = object.value(QStringLiteral("schemaVersion"));
    if (!schemaValue.isDouble()
        || (schemaValue.toInt() != 1 && schemaValue.toInt() != 2)
        || schemaValue.toDouble() != static_cast<double>(schemaValue.toInt())) {
        return fail(error,
                    QStringLiteral("Unsupported version metadata schema: %1")
                        .arg(files::normalizedAbsolute(path)));
    }
    VersionInfo parsed;
    parsed.schemaVersion = schemaValue.toInt();
    const QJsonValue versionValue = object.value(QStringLiteral("version"));
    const QJsonValue createdAtValue = object.value(QStringLiteral("createdAt"));
    const QJsonValue contentHashValue = object.value(QStringLiteral("contentHash"));
    const QJsonValue strictHashValue = object.value(
        QStringLiteral("strictContentHash"));
    if (!versionValue.isString() || !createdAtValue.isString()
        || !contentHashValue.isString()
        || (parsed.schemaVersion == 2 && !strictHashValue.isString())) {
        return fail(error,
                    QStringLiteral("Incomplete version metadata: %1")
                        .arg(files::normalizedAbsolute(path)));
    }
    parsed.version = versionValue.toString();
    parsed.createdAt = QDateTime::fromString(createdAtValue.toString(),
                                             Qt::ISODateWithMs);
    parsed.contentHash = contentHashValue.toString();
    parsed.strictContentHash = strictHashValue.isString()
                                   ? strictHashValue.toString()
                                   : QString();
    if (!validVersion(parsed.version) || !parsed.createdAt.isValid()
        || !validFingerprint(parsed.contentHash)
        || (parsed.schemaVersion == 2
            && !validFingerprint(parsed.strictContentHash))) {
        return fail(error,
                    QStringLiteral("Invalid version metadata fields: %1")
                        .arg(files::normalizedAbsolute(path)));
    }
    if (version) {
        *version = parsed;
    }
    if (canonical) {
        *canonical = json::canonicalJson(object);
    }
    return true;
}

bool collectSnapshotEntries(const QString &directory,
                            const QString &root,
                            QStringList &entries,
                            QString *error)
{
    const QFileInfoList children = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : children) {
        if (files::isLinkLike(entry)) {
            return fail(error,
                        QStringLiteral("Saved version contains linked data: %1")
                            .arg(entry.absoluteFilePath()));
        }
        const QString relative = QDir::fromNativeSeparators(
            QDir(root).relativeFilePath(entry.absoluteFilePath()));
        if (entry.isDir()) {
            entries.append(QStringLiteral("d:%1").arg(relative));
            if (!collectSnapshotEntries(entry.absoluteFilePath(),
                                        root,
                                        entries,
                                        error)) {
                return false;
            }
        } else if (entry.isFile()) {
            entries.append(QStringLiteral("f:%1").arg(relative));
        } else {
            return fail(error,
                        QStringLiteral("Saved version contains unsupported data: %1")
                            .arg(entry.absoluteFilePath()));
        }
    }
    return true;
}

QStringList expectedPayloadEntries(const QStringList &payloadFiles)
{
    QSet<QString> expected;
    for (const QString &payloadFile : payloadFiles) {
        const QString normalized = QDir::fromNativeSeparators(payloadFile);
        expected.insert(QStringLiteral("f:%1").arg(normalized));
        QString parent = QDir::fromNativeSeparators(QFileInfo(normalized).path());
        while (!parent.isEmpty() && parent != QStringLiteral(".")) {
            expected.insert(QStringLiteral("d:%1").arg(parent));
            const qsizetype separator = parent.lastIndexOf(u'/');
            parent = separator < 0 ? QString() : parent.left(separator);
        }
    }
    QStringList result = expected.values();
    std::sort(result.begin(), result.end());
    return result;
}

QStringList expectedSnapshotEntries(const QStringList &payloadFiles)
{
    QStringList result = expectedPayloadEntries(payloadFiles);
    result.append(QStringLiteral("f:.snapshot.json"));
    result.append(QStringLiteral("f:.xips.json"));
    std::sort(result.begin(), result.end());
    return result;
}

QString snapshotProofFingerprint(const SnapshotProof &proof)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-snapshot-proof-v1\0", 23));
    const auto addSizedData = [&hash](const QByteArray &data) {
        hash.addData(QByteArray::number(static_cast<qlonglong>(data.size())));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(data);
    };
    addSizedData(proof.metadataCanonical);
    addSizedData(proof.manifestCanonical);
    for (const QString &file : proof.files) {
        addSizedData(file.toUtf8());
    }
    addSizedData(proof.info.contentHash.toUtf8());
    addSizedData(proof.info.strictContentHash.toUtf8());
    addSizedData(proof.payloadFingerprint.toUtf8());
    return QStringLiteral("sha256:")
           + QString::fromLatin1(hash.result().toHex());
}

bool verifySavedSnapshot(const QString &assetRoot,
                         const QString &expectedAssetId,
                         const QString &expectedVersion,
                         const QString &snapshotRoot,
                         const bool requireFinalDirectoryName,
                         SnapshotProof *proof,
                         QString *error)
{
    if (proof) {
        *proof = {};
    }
    const QString root = files::normalizedAbsolute(assetRoot);
    const QString versionsRoot = QDir(root).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    const QFileInfo internalInfo(QDir(root).absoluteFilePath(
        QStringLiteral(".xips")));
    const QFileInfo versionsInfo(versionsRoot);
    const QString snapshot = files::normalizedAbsolute(snapshotRoot);
    const QFileInfo snapshotInfo(snapshot);
    if (!internalInfo.isDir() || files::isLinkLike(internalInfo)
        || !versionsInfo.isDir() || files::isLinkLike(versionsInfo)
        || !snapshotInfo.isDir() || files::isLinkLike(snapshotInfo)
        || !samePath(snapshotInfo.absolutePath(), versionsRoot)
        || (requireFinalDirectoryName
            && snapshotInfo.fileName() != expectedVersion)) {
        return fail(error,
                    QStringLiteral("Saved version path is invalid or linked: %1")
                        .arg(snapshot));
    }

    const QString metadataPath = QDir(snapshot).absoluteFilePath(
        QStringLiteral(".snapshot.json"));
    const QString manifestPath = QDir(snapshot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    VersionInfo metadata;
    QByteArray metadataCanonical;
    if (!readSnapshotMetadataStable(metadataPath,
                                    &metadata,
                                    &metadataCanonical,
                                    error)) {
        return false;
    }
    if (metadata.version != expectedVersion) {
        return fail(error,
                    QStringLiteral("Saved version metadata does not match its directory: %1")
                        .arg(snapshot));
    }
    const QFileInfo manifestInfo(manifestPath);
    if (!manifestInfo.isFile() || files::isLinkLike(manifestInfo)) {
        return fail(error,
                    QStringLiteral("Saved version manifest is missing or linked: %1")
                        .arg(manifestPath));
    }
    const ManifestService manifestService;
    const ManifestLoadResult loaded = manifestService.load(manifestPath);
    if (!loaded.ok() || loaded.manifest->id != expectedAssetId
        || loaded.manifest->version != expectedVersion) {
        return fail(error,
                    QStringLiteral("Saved version manifest does not match the asset or version: %1")
                        .arg(manifestPath));
    }
    const QByteArray manifestCanonical = json::canonicalJson(
        manifestService.toJson(*loaded.manifest));

    QStringList payloadFiles;
    QString payloadFingerprint;
    if (!strictPayloadFingerprint(snapshot,
                                  nullptr,
                                  &payloadFiles,
                                  &payloadFingerprint,
                                  error)) {
        return false;
    }
    QStringList initialEntries;
    if (!collectSnapshotEntries(snapshot, snapshot, initialEntries, error)) {
        return false;
    }
    std::sort(initialEntries.begin(), initialEntries.end());
    if (initialEntries != expectedSnapshotEntries(payloadFiles)) {
        return fail(error,
                    QStringLiteral("Saved version contains unrecognized or incomplete data: %1")
                        .arg(snapshot));
    }

    QString legacyHash;
    QString strictHash;
    if (!AssetScanner::verifiedContentHash(*loaded.manifest,
                                           snapshot,
                                           &legacyHash,
                                           error)
        || !AssetScanner::strictContentHash(*loaded.manifest,
                                            snapshot,
                                            &strictHash,
                                            error)) {
        return false;
    }
    if (legacyHash != metadata.contentHash
        || (metadata.schemaVersion == 2
            && strictHash != metadata.strictContentHash)) {
        return fail(error,
                    QStringLiteral("Saved version content no longer matches its metadata: %1")
                        .arg(snapshot));
    }

    VersionInfo confirmedMetadata;
    QByteArray confirmedMetadataCanonical;
    const ManifestLoadResult confirmedManifest = manifestService.load(manifestPath);
    QStringList confirmedFiles;
    QString confirmedPayloadFingerprint;
    QStringList confirmedEntries;
    if (!readSnapshotMetadataStable(metadataPath,
                                    &confirmedMetadata,
                                    &confirmedMetadataCanonical,
                                    error)
        || !confirmedManifest.ok()
        || json::canonicalJson(manifestService.toJson(*confirmedManifest.manifest))
               != manifestCanonical
        || confirmedMetadataCanonical != metadataCanonical
        || !strictPayloadFingerprint(snapshot,
                                     &payloadFiles,
                                     &confirmedFiles,
                                     &confirmedPayloadFingerprint,
                                     error)
        || confirmedPayloadFingerprint != payloadFingerprint
        || !collectSnapshotEntries(snapshot, snapshot, confirmedEntries, error)) {
        return fail(error,
                    QStringLiteral("Saved version changed while it was being verified: %1")
                        .arg(snapshot));
    }
    std::sort(confirmedEntries.begin(), confirmedEntries.end());
    if (confirmedEntries != initialEntries) {
        return fail(error,
                    QStringLiteral("Saved version layout changed while it was being verified: %1")
                        .arg(snapshot));
    }

    SnapshotProof verified;
    verified.info = metadata;
    verified.info.path = snapshot;
    verified.info.strictContentHash = strictHash;
    verified.manifest = *confirmedManifest.manifest;
    verified.files = confirmedFiles;
    verified.payloadFingerprint = confirmedPayloadFingerprint;
    verified.metadataCanonical = confirmedMetadataCanonical;
    verified.manifestCanonical = manifestCanonical;
    verified.proofFingerprint = snapshotProofFingerprint(verified);
    if (proof) {
        *proof = verified;
    }
    return true;
}

bool sameSnapshotProof(const SnapshotProof &left, const SnapshotProof &right)
{
    return !left.proofFingerprint.isEmpty()
           && left.proofFingerprint == right.proofFingerprint
           && left.info.path == right.info.path
           && left.files == right.files;
}

bool sameSnapshotContentProof(const SnapshotProof &left,
                              const SnapshotProof &right)
{
    return !left.proofFingerprint.isEmpty()
           && left.proofFingerprint == right.proofFingerprint
           && left.files == right.files
           && left.info.schemaVersion == right.info.schemaVersion
           && left.info.version == right.info.version
           && left.info.contentHash == right.info.contentHash
           && left.info.strictContentHash == right.info.strictContentHash
           && left.payloadFingerprint == right.payloadFingerprint;
}

bool copyPayloadFiles(const QString &sourceRoot,
                      const QStringList &payloadFiles,
                      const QString &destinationRoot,
                      QString *error)
{
    for (const QString &relative : payloadFiles) {
        const QString source = QDir(sourceRoot).absoluteFilePath(relative);
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.isFile() || files::isLinkLike(sourceInfo)) {
            return fail(error,
                        QStringLiteral("Payload source changed or became linked: %1")
                            .arg(source));
        }
        const QString destination = QDir(destinationRoot).absoluteFilePath(relative);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())
            || !QFile::copy(source, destination)) {
            return fail(error,
                        QStringLiteral("Cannot copy %1 to %2")
                            .arg(source, destination));
        }
    }
    return true;
}

bool verifiedCopiedPayload(const QString &root,
                           const QStringList &expectedFiles,
                           const QString &expectedFingerprint,
                           QString *error)
{
    QStringList files;
    QString fingerprint;
    if (!strictPayloadFingerprint(root,
                                  &expectedFiles,
                                  &files,
                                  &fingerprint,
                                  error)) {
        return false;
    }
    if (fingerprint != expectedFingerprint) {
        return fail(error,
                    QStringLiteral("Copied payload does not match the verified source"));
    }
    return true;
}

struct PayloadProof {
    QString root;
    QStringList files;
    QString payloadFingerprint;
    QStringList entries;
};

struct StagingRetireResult {
    QString retainedPath;
    QString warning;
};

bool verifyPayloadProofAtRoot(const QString &root,
                              const QStringList &expectedFiles,
                              const QString &expectedFingerprint,
                              PayloadProof *proof,
                              QString *error)
{
    if (proof) {
        *proof = {};
    }
    const QString normalizedRoot = files::normalizedAbsolute(root);
    const QFileInfo rootInfo(normalizedRoot);
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        return fail(error,
                    QStringLiteral("Payload staging path is invalid or linked: %1")
                        .arg(normalizedRoot));
    }
    QStringList entries;
    if (!collectSnapshotEntries(normalizedRoot,
                                normalizedRoot,
                                entries,
                                error)) {
        return false;
    }
    std::sort(entries.begin(), entries.end());
    if (entries != expectedPayloadEntries(expectedFiles)) {
        return fail(error,
                    QStringLiteral("Payload staging contains unrecognized or incomplete data: %1")
                        .arg(normalizedRoot));
    }
    if (!verifiedCopiedPayload(normalizedRoot,
                               expectedFiles,
                               expectedFingerprint,
                               error)) {
        return false;
    }
    if (proof) {
        proof->root = normalizedRoot;
        proof->files = expectedFiles;
        proof->payloadFingerprint = expectedFingerprint;
        proof->entries = entries;
    }
    return true;
}

bool samePayloadProof(const PayloadProof &left, const PayloadProof &right)
{
    return !left.payloadFingerprint.isEmpty()
           && left.files == right.files
           && left.payloadFingerprint == right.payloadFingerprint
           && left.entries == right.entries;
}

bool validOwnedStagingDirectory(const QString &path,
                                const QString &expectedParent,
                                const QString &prefix)
{
    const QString normalized = files::normalizedAbsolute(path);
    const QFileInfo info(normalized);
    const QRegularExpression namePattern(
        QStringLiteral("^%1[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
            .arg(QRegularExpression::escape(prefix)),
        QRegularExpression::CaseInsensitiveOption);
    return info.isDir() && !files::isLinkLike(info)
           && samePath(info.absolutePath(), expectedParent)
           && namePattern.match(info.fileName()).hasMatch();
}

bool removeExactTree(const QString &root,
                     const QStringList &expectedEntries)
{
    QStringList confirmedEntries;
    QString ignoredError;
    if (!collectSnapshotEntries(root,
                                root,
                                confirmedEntries,
                                &ignoredError)) {
        return false;
    }
    std::sort(confirmedEntries.begin(), confirmedEntries.end());
    if (confirmedEntries != expectedEntries) {
        return false;
    }

    QStringList directories;
    for (const QString &entry : expectedEntries) {
        if (entry.startsWith(QStringLiteral("f:"))) {
            const QString relative = entry.mid(2);
            const QString path = QDir(root).absoluteFilePath(relative);
            const QFileInfo info(path);
            if (!info.isFile() || files::isLinkLike(info)
                || !QFile::remove(path)) {
                return false;
            }
        } else if (entry.startsWith(QStringLiteral("d:"))) {
            directories.append(entry.mid(2));
        } else {
            return false;
        }
    }
    std::sort(directories.begin(), directories.end(),
              [](const QString &left, const QString &right) {
                  if (left.count(u'/') != right.count(u'/')) {
                      return left.count(u'/') > right.count(u'/');
                  }
                  return left.size() > right.size();
              });
    for (const QString &relative : directories) {
        if (!QDir().rmdir(QDir(root).absoluteFilePath(relative))) {
            return false;
        }
    }
    return QDir().rmdir(root);
}

struct AssetTreeProof {
    QString root;
    QString assetId;
    QString fingerprint;
    QStringList entries;
};

bool verifyAssetTreeProofAtRoot(const QString &root,
                                const QString &expectedAssetId,
                                AssetTreeProof *proof,
                                QString *error)
{
    if (proof) {
        *proof = {};
    }
    const QString normalizedRoot = files::normalizedAbsolute(root);
    const QFileInfo rootInfo(normalizedRoot);
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        return fail(error,
                    QStringLiteral("Asset deletion proof root is invalid or linked: %1")
                        .arg(normalizedRoot));
    }

    const QString manifestPath = QDir(normalizedRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    const ManifestService manifestService;
    const ManifestLoadResult initialManifest = manifestService.load(manifestPath);
    if (!initialManifest.ok()
        || initialManifest.manifest->id != expectedAssetId) {
        return fail(error,
                    QStringLiteral(
                        "The asset manifest changed before its deletion proof could be recorded"));
    }
    const QByteArray initialManifestCanonical = json::canonicalJson(
        manifestService.toJson(*initialManifest.manifest));

    QStringList initialEntries;
    if (!collectSnapshotEntries(normalizedRoot,
                                normalizedRoot,
                                initialEntries,
                                error)) {
        return false;
    }
    std::sort(initialEntries.begin(), initialEntries.end());

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("xips-asset-delete-v1"));
    const auto addSizedData = [&hash](const QByteArray &data) {
        hash.addData(QByteArray::number(static_cast<qlonglong>(data.size())));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(data);
    };
    for (const QString &entry : initialEntries) {
        hash.addData(QByteArrayView("\0entry\0", 7));
        addSizedData(entry.toUtf8());
        if (!entry.startsWith(QStringLiteral("f:"))) {
            continue;
        }

        const QString path = QDir(normalizedRoot).absoluteFilePath(entry.mid(2));
        const QFileInfo before(path);
        if (!before.isFile() || files::isLinkLike(before)) {
            return fail(error,
                        QStringLiteral(
                            "Asset data changed while its deletion proof was being recorded: %1")
                            .arg(path));
        }
        QFile file(before.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(error,
                        QStringLiteral("Cannot read asset data for deletion proof: %1")
                            .arg(before.absoluteFilePath()));
        }
        const qint64 expectedSize = file.size();
        const QDateTime expectedModified = file.fileTime(
            QFileDevice::FileModificationTime);
        if (expectedSize < 0) {
            return fail(error,
                        QStringLiteral(
                            "Cannot determine asset file size for deletion proof: %1")
                            .arg(before.absoluteFilePath()));
        }
        hash.addData(QByteArrayView("\0data\0", 6));
        hash.addData(QByteArray::number(expectedSize));
        hash.addData(QByteArrayView(":", 1));
        qint64 bytesRead = 0;
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                return fail(error,
                            QStringLiteral(
                                "Cannot finish reading asset data for deletion proof: %1")
                                .arg(before.absoluteFilePath()));
            }
            hash.addData(chunk);
            bytesRead += static_cast<qint64>(chunk.size());
        }
        const QFileInfo after(path);
        if (file.error() != QFileDevice::NoError
            || bytesRead != expectedSize || !after.isFile()
            || files::isLinkLike(after) || after.size() != expectedSize
            || after.fileTime(QFileDevice::FileModificationTime)
                   != expectedModified) {
            return fail(error,
                        QStringLiteral(
                            "Asset data changed while its deletion proof was being recorded: %1")
                            .arg(before.absoluteFilePath()));
        }
    }

    QStringList confirmedEntries;
    if (!collectSnapshotEntries(normalizedRoot,
                                normalizedRoot,
                                confirmedEntries,
                                error)) {
        return false;
    }
    std::sort(confirmedEntries.begin(), confirmedEntries.end());
    const ManifestLoadResult confirmedManifest = manifestService.load(
        manifestPath);
    if (confirmedEntries != initialEntries || !confirmedManifest.ok()
        || confirmedManifest.manifest->id != expectedAssetId
        || json::canonicalJson(
               manifestService.toJson(*confirmedManifest.manifest))
               != initialManifestCanonical) {
        return fail(error,
                    QStringLiteral(
                        "The asset changed while its deletion proof was being recorded"));
    }

    if (proof) {
        proof->root = normalizedRoot;
        proof->assetId = expectedAssetId;
        proof->fingerprint = QStringLiteral("sha256:")
                             + QString::fromLatin1(hash.result().toHex());
        proof->entries = confirmedEntries;
    }
    return true;
}

bool sameAssetTreeContentProof(const AssetTreeProof &left,
                               const AssetTreeProof &right)
{
    return !left.fingerprint.isEmpty()
           && left.assetId == right.assetId
           && left.fingerprint == right.fingerprint
           && left.entries == right.entries;
}

bool removeExpectedEmptyDirectories(const QString &root,
                                    const QStringList &originalEntries)
{
    QStringList expectedDirectories;
    for (const QString &entry : originalEntries) {
        if (entry.startsWith(QStringLiteral("d:"))) {
            expectedDirectories.append(entry);
        }
    }
    QStringList confirmedEntries;
    QString ignoredError;
    if (!collectSnapshotEntries(root,
                                root,
                                confirmedEntries,
                                &ignoredError)) {
        return false;
    }
    std::sort(expectedDirectories.begin(), expectedDirectories.end());
    std::sort(confirmedEntries.begin(), confirmedEntries.end());
    if (confirmedEntries != expectedDirectories) {
        return false;
    }
    QStringList directories;
    for (const QString &entry : expectedDirectories) {
        directories.append(entry.mid(2));
    }
    std::sort(directories.begin(), directories.end(),
              [](const QString &left, const QString &right) {
                  if (left.count(u'/') != right.count(u'/')) {
                      return left.count(u'/') > right.count(u'/');
                  }
                  return left.size() > right.size();
              });
    for (const QString &relative : directories) {
        if (!QDir().rmdir(QDir(root).absoluteFilePath(relative))) {
            return false;
        }
    }
    return QDir().rmdir(root);
}

bool retireVerifiedPayloadStaging(const PayloadProof &proof,
                                  const QString &expectedParent,
                                  const QString &prefix,
                                  StagingRetireResult *result)
{
    if (result) {
        *result = {};
    }
    PayloadProof confirmed;
    QString verificationError;
    if (!validOwnedStagingDirectory(proof.root, expectedParent, prefix)
        || !verifyPayloadProofAtRoot(proof.root,
                                     proof.files,
                                     proof.payloadFingerprint,
                                     &confirmed,
                                     &verificationError)
        || !samePayloadProof(proof, confirmed)) {
        if (result) {
            result->retainedPath = proof.root;
            result->warning = QStringLiteral(
                "Temporary data was retained because it no longer matches the verified staging proof at %1%2")
                                  .arg(proof.root,
                                       verificationError.isEmpty()
                                           ? QString()
                                           : QStringLiteral(": %1")
                                                 .arg(verificationError));
        }
        return false;
    }

    const QString parent = files::normalizedAbsolute(expectedParent);
    const QString originalName = QFileInfo(proof.root).fileName();
    const QString retireName = prefix.startsWith(QStringLiteral(".staging-"))
                                   ? QStringLiteral(".staging-retire-%1")
                                         .arg(QUuid::createUuid().toString(
                                             QUuid::WithoutBraces))
                                   : QStringLiteral(".xips-create-retire-%1")
                                         .arg(QUuid::createUuid().toString(
                                             QUuid::WithoutBraces));
    QDir parentDirectory(parent);
    if (!parentDirectory.rename(originalName, retireName)) {
        if (result) {
            result->retainedPath = proof.root;
            result->warning = QStringLiteral(
                "Verified temporary data could not be isolated for cleanup and remains at %1")
                                  .arg(proof.root);
        }
        return false;
    }
    const QString retiredPath = QDir(parent).absoluteFilePath(retireName);
    PayloadProof isolated;
    verificationError.clear();
    if (!verifyPayloadProofAtRoot(retiredPath,
                                  proof.files,
                                  proof.payloadFingerprint,
                                  &isolated,
                                  &verificationError)
        || !samePayloadProof(proof, isolated)) {
        const bool restored = !QFileInfo::exists(proof.root)
                              && parentDirectory.rename(retireName,
                                                        originalName);
        if (result) {
            result->retainedPath = restored ? proof.root : retiredPath;
            result->warning = QStringLiteral(
                "Temporary data changed after cleanup isolation and was not deleted; it remains at %1%2")
                                  .arg(result->retainedPath,
                                       verificationError.isEmpty()
                                           ? QString()
                                           : QStringLiteral(": %1")
                                                 .arg(verificationError));
        }
        return false;
    }
    if (!removeExactTree(retiredPath, isolated.entries)) {
        if (result) {
            result->retainedPath = retiredPath;
            result->warning = QStringLiteral(
                "Verified temporary data could not be completely retired and remains at %1")
                                  .arg(retiredPath);
        }
        return false;
    }
    return true;
}

bool retireVerifiedSnapshotStaging(const QString &assetRoot,
                                   const QString &assetId,
                                   const QString &version,
                                   const SnapshotProof &proof,
                                   StagingRetireResult *result)
{
    if (result) {
        *result = {};
    }
    const QString versionsRoot = QDir(assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    SnapshotProof confirmed;
    QString verificationError;
    if (!validOwnedStagingDirectory(proof.info.path,
                                    versionsRoot,
                                    QStringLiteral(".staging-"))
        || !verifySavedSnapshot(assetRoot,
                                assetId,
                                version,
                                proof.info.path,
                                false,
                                &confirmed,
                                &verificationError)
        || !sameSnapshotProof(proof, confirmed)) {
        if (result) {
            result->retainedPath = proof.info.path;
            result->warning = QStringLiteral(
                "Version staging was retained because it no longer matches its verified snapshot proof at %1%2")
                                  .arg(proof.info.path,
                                       verificationError.isEmpty()
                                           ? QString()
                                           : QStringLiteral(": %1")
                                                 .arg(verificationError));
        }
        return false;
    }

    const QString originalName = QFileInfo(proof.info.path).fileName();
    const QString retireName = QStringLiteral(".staging-retire-%1")
                                   .arg(QUuid::createUuid().toString(
                                       QUuid::WithoutBraces));
    QDir versionsDirectory(versionsRoot);
    if (!versionsDirectory.rename(originalName, retireName)) {
        if (result) {
            result->retainedPath = proof.info.path;
            result->warning = QStringLiteral(
                "Verified version staging could not be isolated for cleanup and remains at %1")
                                  .arg(proof.info.path);
        }
        return false;
    }
    const QString retiredPath = QDir(versionsRoot).absoluteFilePath(retireName);
    SnapshotProof isolated;
    verificationError.clear();
    if (!verifySavedSnapshot(assetRoot,
                             assetId,
                             version,
                             retiredPath,
                             false,
                             &isolated,
                             &verificationError)
        || !sameSnapshotContentProof(proof, isolated)) {
        const bool restored = !QFileInfo::exists(proof.info.path)
                              && versionsDirectory.rename(retireName,
                                                          originalName);
        if (result) {
            result->retainedPath = restored ? proof.info.path : retiredPath;
            result->warning = QStringLiteral(
                "Version staging changed after cleanup isolation and was not deleted; it remains at %1%2")
                                  .arg(result->retainedPath,
                                       verificationError.isEmpty()
                                           ? QString()
                                           : QStringLiteral(": %1")
                                                 .arg(verificationError));
        }
        return false;
    }
    const QStringList expectedEntries = expectedSnapshotEntries(isolated.files);
    if (!removeExactTree(retiredPath, expectedEntries)) {
        if (result) {
            result->retainedPath = retiredPath;
            result->warning = QStringLiteral(
                "Verified version staging could not be completely retired and remains at %1")
                                  .arg(retiredPath);
        }
        return false;
    }
    return true;
}

bool verifyCopyPlanSource(const AssetRecord &asset,
                          const CopyPlan &plan,
                          QString *error)
{
    Manifest liveManifest;
    if (!validateAssetRecord(asset, &liveManifest, error)) {
        return false;
    }
    if (!plan.version.isEmpty()) {
        SnapshotProof confirmed;
        if (!verifySavedSnapshot(asset.assetRoot,
                                 liveManifest.id,
                                 plan.version,
                                 plan.sourceRoot,
                                 true,
                                 &confirmed,
                                 error)) {
            return false;
        }
        if (confirmed.files != plan.files
            || confirmed.info.contentHash != plan.contentHash
            || confirmed.info.strictContentHash != plan.strictContentHash
            || confirmed.payloadFingerprint != plan.payloadFingerprint
            || confirmed.proofFingerprint != plan.proofFingerprint) {
            return fail(error,
                        QStringLiteral("Saved version changed during the operation: %1")
                            .arg(plan.sourceRoot));
        }
        return true;
    }

    QStringList files;
    QString strictFingerprint;
    QString payloadFingerprint;
    if (!strictNonEmptyAssetFingerprintAtRoot(asset.assetRoot,
                                              liveManifest.id,
                                              nullptr,
                                              &files,
                                              &strictFingerprint,
                                              &payloadFingerprint,
                                              error)) {
        return false;
    }
    QString legacyHash;
    if (!AssetScanner::verifiedContentHash(liveManifest,
                                           asset.assetRoot,
                                           &legacyHash,
                                           error)) {
        return false;
    }
    if (files != plan.files || strictFingerprint != plan.strictContentHash
        || payloadFingerprint != plan.payloadFingerprint
        || legacyHash != plan.contentHash
        || strictFingerprint != plan.proofFingerprint) {
        return fail(error,
                    QStringLiteral("The working copy changed during the operation"));
    }
    return true;
}

constexpr auto ManifestTransactionOwner = "xips-manifest-cas";
constexpr auto ManifestTransactionFileName = "transaction.json";
constexpr auto ManifestTransactionNextFileName = "next.xips.json";
constexpr auto ManifestTransactionOriginalFileName = "original.xips.json";

struct ManifestTransactionRecord {
    QString operation;
    QString transactionId;
    QString assetDirectory;
    QString assetId;
    QString expectedCanonicalSha256;
    QString replacementCanonicalSha256;
};

bool supportedManifestTransactionOperation(const QString &operation)
{
    return operation == QStringLiteral("metadata")
           || operation == QStringLiteral("group-membership")
           || operation == QStringLiteral("version-marker");
}

QString canonicalSha256(const QByteArray &canonical)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)
            .toHex());
}

QString manifestCanonicalSha256(const Manifest &manifest)
{
    const ManifestService service;
    return canonicalSha256(
        json::canonicalJson(service.toJson(manifest)));
}

bool validCanonicalSha256(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool validManifestTransactionId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(value).hasMatch();
}

bool validAssetDirectoryName(const QString &value)
{
    return !value.isEmpty() && value != QStringLiteral(".")
           && value != QStringLiteral("..")
           && !value.contains(u'/') && !value.contains(u'\\')
           && QFileInfo(value).fileName() == value;
}

QJsonObject manifestTransactionObject(
    const ManifestTransactionRecord &record)
{
    return {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("owner"), QString::fromLatin1(ManifestTransactionOwner)},
        {QStringLiteral("operation"), record.operation},
        {QStringLiteral("transactionId"), record.transactionId},
        {QStringLiteral("assetDirectory"), record.assetDirectory},
        {QStringLiteral("assetId"), record.assetId},
        {QStringLiteral("expectedCanonicalSha256"),
         record.expectedCanonicalSha256},
        {QStringLiteral("replacementCanonicalSha256"),
         record.replacementCanonicalSha256},
    };
}

bool sameManifestTransactionRecord(
    const ManifestTransactionRecord &left,
    const ManifestTransactionRecord &right)
{
    return left.operation == right.operation
           && left.transactionId.compare(right.transactionId,
                                         Qt::CaseInsensitive)
                  == 0
           && left.assetDirectory == right.assetDirectory
           && left.assetId == right.assetId
           && left.expectedCanonicalSha256
                  == right.expectedCanonicalSha256
           && left.replacementCanonicalSha256
                  == right.replacementCanonicalSha256;
}

bool writeManifestTransactionRecord(
    const QString &path,
    const ManifestTransactionRecord &record,
    QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error,
                    QStringLiteral("Cannot create manifest transaction record: %1")
                        .arg(file.errorString()));
    }
    const QByteArray data = QJsonDocument(manifestTransactionObject(record))
                                .toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        return fail(error,
                    QStringLiteral("Cannot publish manifest transaction record: %1")
                        .arg(file.errorString()));
    }
    return true;
}

bool readManifestTransactionRecord(
    const QString &path,
    ManifestTransactionRecord *record,
    QString *error)
{
    if (record) {
        *record = {};
    }
    QByteArray contents;
    if (!readStableFile(path, &contents, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents,
                                                            &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return fail(error,
                    QStringLiteral("Invalid manifest transaction record"));
    }
    const QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schemaVersion"));
    const QJsonValue owner = object.value(QStringLiteral("owner"));
    const QJsonValue operation = object.value(QStringLiteral("operation"));
    const QJsonValue transactionId = object.value(
        QStringLiteral("transactionId"));
    const QJsonValue assetDirectory = object.value(
        QStringLiteral("assetDirectory"));
    const QJsonValue assetId = object.value(QStringLiteral("assetId"));
    const QJsonValue expectedHash = object.value(
        QStringLiteral("expectedCanonicalSha256"));
    const QJsonValue replacementHash = object.value(
        QStringLiteral("replacementCanonicalSha256"));
    if (object.size() != 8 || !schema.isDouble()
        || schema.toDouble() != 1.0 || !owner.isString()
        || owner.toString() != QString::fromLatin1(ManifestTransactionOwner)
        || !operation.isString() || !transactionId.isString()
        || !assetDirectory.isString() || !assetId.isString()
        || !expectedHash.isString() || !replacementHash.isString()) {
        return fail(error,
                    QStringLiteral("Incomplete manifest transaction record"));
    }
    ManifestTransactionRecord parsed;
    parsed.operation = operation.toString();
    parsed.transactionId = transactionId.toString();
    parsed.assetDirectory = assetDirectory.toString();
    parsed.assetId = assetId.toString();
    parsed.expectedCanonicalSha256 = expectedHash.toString();
    parsed.replacementCanonicalSha256 = replacementHash.toString();
    if (!supportedManifestTransactionOperation(parsed.operation)
        || !validManifestTransactionId(parsed.transactionId)
        || !validAssetDirectoryName(parsed.assetDirectory)
        || parsed.assetId.isEmpty()
        || parsed.assetId != parsed.assetId.trimmed()
        || !validCanonicalSha256(parsed.expectedCanonicalSha256)
        || !validCanonicalSha256(parsed.replacementCanonicalSha256)) {
        return fail(error,
                    QStringLiteral("Invalid manifest transaction fields"));
    }
    if (record) {
        *record = parsed;
    }
    return true;
}

bool manifestTransactionName(const QString &name,
                             QString *operation,
                             QString *transactionId)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^\\.xips-create-(metadata|group-membership|version-marker)-"
            "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch()) {
        return false;
    }
    if (operation) {
        *operation = match.captured(1).toLower();
    }
    if (transactionId) {
        *transactionId = match.captured(2);
    }
    return true;
}

bool manifestTransactionDirectoryHasKnownEntries(const QString &root,
                                                 QString *error)
{
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        return fail(error,
                    QStringLiteral("Manifest transaction path is not a regular directory"));
    }
    const QStringList entries = QDir(root).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
            | QDir::System,
        QDir::Name);
    const QSet<QString> allowed{
        QString::fromLatin1(ManifestTransactionFileName),
        QString::fromLatin1(ManifestTransactionNextFileName),
        QString::fromLatin1(ManifestTransactionOriginalFileName),
    };
    for (const QString &entry : entries) {
        if (!allowed.contains(entry)) {
            return fail(error,
                        QStringLiteral("Manifest transaction contains unexpected data: %1")
                            .arg(entry));
        }
    }
    if (!entries.contains(
            QString::fromLatin1(ManifestTransactionFileName))) {
        return fail(error,
                    QStringLiteral("Manifest transaction record is missing"));
    }
    return true;
}

bool readManifestHashStable(const QString &path,
                            const QString &expectedAssetId,
                            Manifest *manifest,
                            QString *hash,
                            QString *error)
{
    QByteArray contents;
    if (!readStableFile(path, &contents, error)) {
        return false;
    }
    const ManifestLoadResult loaded = ManifestService().parse(contents);
    if (!loaded.ok()
        || (!expectedAssetId.isEmpty()
            && loaded.manifest->id != expectedAssetId)) {
        return fail(error,
                    QStringLiteral("Manifest transaction file does not match its asset"));
    }
    if (manifest) {
        *manifest = *loaded.manifest;
    }
    if (hash) {
        *hash = manifestCanonicalSha256(*loaded.manifest);
    }
    return true;
}

bool validateManifestTransactionContents(
    const QString &root,
    const ManifestTransactionRecord &expectedRecord,
    QString *error)
{
    if (!manifestTransactionDirectoryHasKnownEntries(root, error)) {
        return false;
    }
    ManifestTransactionRecord confirmed;
    if (!readManifestTransactionRecord(
            QDir(root).absoluteFilePath(
                QString::fromLatin1(ManifestTransactionFileName)),
            &confirmed,
            error)
        || !sameManifestTransactionRecord(expectedRecord, confirmed)) {
        return fail(error,
                    QStringLiteral("Manifest transaction record changed"));
    }
    return true;
}

bool validateManifestTransactionRoot(
    const QString &operationRoot,
    const QString &expectedParent,
    const ManifestTransactionRecord &expectedRecord,
    QString *error)
{
    const QString root = files::normalizedAbsolute(operationRoot);
    const QString parent = files::normalizedAbsolute(expectedParent);
    const QString actualParent = files::normalizedAbsolute(
        QFileInfo(root).absolutePath());
    QString operation;
    QString transactionId;
    if (!files::isWithin(root, parent)
        || !files::isWithin(parent, actualParent)
        || !files::isWithin(actualParent, parent)
        || !manifestTransactionName(QFileInfo(root).fileName(),
                                    &operation,
                                    &transactionId)
        || operation != expectedRecord.operation
        || transactionId.compare(expectedRecord.transactionId,
                                 Qt::CaseInsensitive)
               != 0) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("Manifest transaction path does not match its record");
        }
        return false;
    }
    return validateManifestTransactionContents(root,
                                               expectedRecord,
                                               error);
}

bool retireVerifiedManifestTransaction(
    const QString &operationRoot,
    const QString &expectedParent,
    const ManifestTransactionRecord &record,
    const QString &liveManifestPath,
    const QString &anchoredLiveAssetId,
    const QString &anchoredLiveHash,
    QString *retainedPath,
    QString *error
#ifdef XIPS_ENABLE_TEST_HOOKS
    , const WorkingCopyTestHook &testHook = {}
#endif
    )
{
    if (retainedPath) {
        retainedPath->clear();
    }
    const QString root = files::normalizedAbsolute(operationRoot);
    const QString parent = files::normalizedAbsolute(expectedParent);
    const auto retain = [&](const QString &path,
                            const QString &message) {
        if (retainedPath) {
            *retainedPath = path;
        }
        return fail(error, message);
    };
    if (!validateManifestTransactionRoot(operationRoot,
                                         expectedParent,
                                         record,
                                         error)) {
        if (retainedPath) {
            *retainedPath = root;
        }
        return false;
    }
    const QString nextPath = QDir(root).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionNextFileName));
    const QString originalPath = QDir(root).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionOriginalFileName));
    const auto verifyOptionalManifest = [&](const QString &path,
                                            const QString &expectedHash,
                                            bool *present) {
        if (present) {
            *present = false;
        }
        const QFileInfo info(path);
        if (!info.exists() && !files::isLinkLike(info)) {
            return true;
        }
        QString hash;
        QString verificationError;
        if (!readManifestHashStable(path,
                                    record.assetId,
                                    nullptr,
                                    &hash,
                                    &verificationError)
            || hash != expectedHash) {
            return fail(error,
                        verificationError.isEmpty()
                            ? QStringLiteral("Manifest transaction data changed before retirement")
                            : verificationError);
        }
        if (present) {
            *present = true;
        }
        return true;
    };
    bool hasNext = false;
    bool hasOriginal = false;
    if (!verifyOptionalManifest(nextPath,
                                record.replacementCanonicalSha256,
                                &hasNext)
        || !verifyOptionalManifest(originalPath,
                                   record.expectedCanonicalSha256,
                                   &hasOriginal)) {
        if (retainedPath) {
            *retainedPath = root;
        }
        return false;
    }
    Manifest anchoredLive;
    QString confirmedLiveHash;
    QString liveError;
    if (!validCanonicalSha256(anchoredLiveHash)
        || !readManifestHashStable(liveManifestPath,
                                   anchoredLiveAssetId,
                                   &anchoredLive,
                                   &confirmedLiveHash,
                                   &liveError)
        || confirmedLiveHash != anchoredLiveHash) {
        return retain(root,
                      liveError.isEmpty()
                          ? QStringLiteral("Live manifest changed before transaction retirement")
                          : liveError);
    }

    const QString originalName = QFileInfo(root).fileName();
    const QString retireName = QStringLiteral(".xips-create-retire-%1")
                                   .arg(QUuid::createUuid().toString(
                                       QUuid::WithoutBraces));
    const QString isolatedRoot = QDir(parent).absoluteFilePath(retireName);
    QDir parentDirectory(parent);
    if (QFileInfo::exists(isolatedRoot)
        || !parentDirectory.rename(originalName, retireName)) {
        return retain(root,
                      QStringLiteral("Verified manifest transaction could not be isolated for retirement"));
    }
    const auto restoreIsolation = [&]() {
        if (QFileInfo::exists(root)) {
            return false;
        }
        return parentDirectory.rename(retireName, originalName);
    };
    const auto retainAfterIsolation = [&](const QString &message) {
        const bool restored = restoreIsolation();
        return retain(restored ? root : isolatedRoot,
                      QStringLiteral("%1; recovery data remains at %2")
                          .arg(message,
                               restored ? root : isolatedRoot));
    };

#ifdef XIPS_ENABLE_TEST_HOOKS
    if (testHook) {
        testHook(
            WorkingCopyTestPoint::ManifestTransactionIsolatedBeforeRetireLiveVerification,
            isolatedRoot);
    }
#endif

    const QString isolatedNext = QDir(isolatedRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionNextFileName));
    const QString isolatedOriginal = QDir(isolatedRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionOriginalFileName));
    bool isolatedHasNext = false;
    bool isolatedHasOriginal = false;
    if (!validateManifestTransactionContents(isolatedRoot, record, error)
        || !verifyOptionalManifest(isolatedNext,
                                   record.replacementCanonicalSha256,
                                   &isolatedHasNext)
        || !verifyOptionalManifest(isolatedOriginal,
                                   record.expectedCanonicalSha256,
                                   &isolatedHasOriginal)
        || isolatedHasNext != hasNext
        || isolatedHasOriginal != hasOriginal) {
        const QString message = error && !error->isEmpty()
                                    ? *error
                                    : QStringLiteral("Isolated manifest transaction changed before retirement");
        return retainAfterIsolation(message);
    }
    liveError.clear();
    confirmedLiveHash.clear();
    if (!readManifestHashStable(liveManifestPath,
                                anchoredLiveAssetId,
                                nullptr,
                                &confirmedLiveHash,
                                &liveError)
        || confirmedLiveHash != anchoredLiveHash) {
        return retainAfterIsolation(
            liveError.isEmpty()
                ? QStringLiteral("Live manifest changed after transaction isolation")
                : liveError);
    }

    const QString isolatedRecord = QDir(isolatedRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionFileName));
    if ((hasNext && !QFile::remove(isolatedNext))
        || (hasOriginal && !QFile::remove(isolatedOriginal))
        || !QFile::remove(isolatedRecord)
        || !QDir().rmdir(isolatedRoot)) {
        return retain(isolatedRoot,
                      QStringLiteral("Manifest transaction could not be completely retired; recovery data remains at %1")
                          .arg(isolatedRoot));
    }
    return true;
}

void collectManifestTransactionDirectories(const QString &directory,
                                           QStringList *transactions)
{
    if (!transactions) {
        return;
    }
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir() || files::isLinkLike(directoryInfo)) {
        return;
    }
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
            | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        QString operation;
        QString transactionId;
        if (manifestTransactionName(entry.fileName(),
                                    &operation,
                                    &transactionId)) {
            transactions->append(files::normalizedAbsolute(
                entry.absoluteFilePath()));
            continue;
        }
        if (!entry.isDir() || files::isLinkLike(entry)) {
            continue;
        }
        if (files::isIgnoredDirectory(entry.fileName())) {
            continue;
        }
        const QFileInfo manifestInfo(QDir(entry.absoluteFilePath())
                                         .absoluteFilePath(
                                             QStringLiteral(".xips.json")));
        if (manifestInfo.exists() || files::isLinkLike(manifestInfo)) {
            continue;
        }
        collectManifestTransactionDirectories(entry.absoluteFilePath(),
                                              transactions);
    }
}

QString manifestRecoveryPathKey(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(
        files::normalizedAbsolute(path));
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

struct VerifiedManifestTransaction {
    ManifestTransactionRecord record;
    ManifestTransactionRecoveryItem item;
    QString parent;
    QString nextPath;
    QString originalPath;
    bool hasNext = false;
    bool hasOriginal = false;
};

bool verifyOptionalTransactionManifest(const QString &path,
                                       const QString &assetId,
                                       const QString &expectedHash,
                                       bool *present,
                                       QString *error)
{
    if (present) {
        *present = false;
    }
    const QFileInfo info(path);
    if (!info.exists() && !files::isLinkLike(info)) {
        return true;
    }
    QString actualHash;
    if (!readManifestHashStable(path,
                                assetId,
                                nullptr,
                                &actualHash,
                                error)
        || actualHash != expectedHash) {
        return fail(error,
                    QStringLiteral(
                        "Manifest transaction data does not match its recorded hash"));
    }
    if (present) {
        *present = true;
    }
    return true;
}

bool reverifyManifestTransaction(const VerifiedManifestTransaction &candidate,
                                 QString *error)
{
    if (!validateManifestTransactionRoot(candidate.item.transactionPath,
                                         candidate.parent,
                                         candidate.record,
                                         error)) {
        return false;
    }
    bool hasNext = false;
    bool hasOriginal = false;
    if (!verifyOptionalTransactionManifest(
            candidate.nextPath,
            candidate.record.assetId,
            candidate.record.replacementCanonicalSha256,
            &hasNext,
            error)
        || !verifyOptionalTransactionManifest(
            candidate.originalPath,
            candidate.record.assetId,
            candidate.record.expectedCanonicalSha256,
            &hasOriginal,
            error)
        || hasNext != candidate.hasNext
        || hasOriginal != candidate.hasOriginal) {
        return fail(error,
                    QStringLiteral(
                        "Manifest transaction changed during recovery"));
    }
    return true;
}

ManifestTransactionRecoveryResult recoverManifestTransactionsGrouped(
    const QString &libraryRoot
#ifdef XIPS_ENABLE_TEST_HOOKS
    , const WorkingCopyTestHook &testHook = {}
#endif
    )
{
    ManifestTransactionRecoveryResult result;
    const QString library = files::normalizedAbsolute(libraryRoot);
    const QFileInfo libraryInfo(library);
    if (!libraryInfo.exists()) {
        return result;
    }
    if (!libraryInfo.isDir() || files::isLinkLike(libraryInfo)) {
        result.fatalError = QStringLiteral(
            "The library path is not a regular directory");
        return result;
    }

    const auto appendRetained = [&](ManifestTransactionRecoveryItem item,
                                    const QString &message) {
        item.outcome = ManifestTransactionRecoveryOutcome::Retained;
        item.message = message;
        ++result.retained;
        result.items.append(item);
    };

    QStringList transactionRoots;
    collectManifestTransactionDirectories(library, &transactionRoots);
    QMap<QString, QList<VerifiedManifestTransaction>> byAssetRoot;
    QSet<QString> blockedParents;
    QSet<QString> blockedAssetRoots;
    for (const QString &transactionRoot : transactionRoots) {
        VerifiedManifestTransaction candidate;
        candidate.item.transactionPath = transactionRoot;
        const QFileInfo transactionInfo(transactionRoot);
        QString operation;
        QString transactionId;
        manifestTransactionName(transactionInfo.fileName(),
                                &operation,
                                &transactionId);
        candidate.parent = files::normalizedAbsolute(
            transactionInfo.absolutePath());
        if (!files::isWithin(candidate.parent, library)) {
            appendRetained(candidate.item,
                           QStringLiteral(
                               "Manifest transaction is outside the library boundary"));
            continue;
        }
        if (!transactionInfo.isDir()
            || files::isLinkLike(transactionInfo)) {
            blockedParents.insert(manifestRecoveryPathKey(candidate.parent));
            appendRetained(candidate.item,
                           QStringLiteral(
                               "Manifest transaction path is linked or is not a directory"));
            continue;
        }

        QString verificationError;
        const QString recordPath = QDir(transactionRoot).absoluteFilePath(
            QString::fromLatin1(ManifestTransactionFileName));
        if (!readManifestTransactionRecord(recordPath,
                                           &candidate.record,
                                           &verificationError)) {
            blockedParents.insert(manifestRecoveryPathKey(candidate.parent));
            appendRetained(candidate.item,
                           verificationError.isEmpty()
                               ? QStringLiteral(
                                     "Manifest transaction record cannot be parsed safely")
                               : verificationError);
            continue;
        }

        candidate.item.assetId = candidate.record.assetId;
        candidate.item.assetRoot = files::normalizedAbsolute(
            QDir(candidate.parent).absoluteFilePath(
                candidate.record.assetDirectory));
        const QFileInfo assetInfo(candidate.item.assetRoot);
        const QString targetParent = files::normalizedAbsolute(
            assetInfo.absolutePath());
        const bool sameParent = files::isWithin(targetParent,
                                                candidate.parent)
                                && files::isWithin(candidate.parent,
                                                   targetParent);
        const bool resolvedAssetRoot = sameParent
                                       && files::isWithin(
                                           candidate.item.assetRoot,
                                           library)
                                       && candidate.record.assetDirectory
                                              != transactionInfo.fileName();
        if (!resolvedAssetRoot) {
            blockedParents.insert(manifestRecoveryPathKey(candidate.parent));
            appendRetained(candidate.item,
                           QStringLiteral(
                               "Manifest transaction target is not a sibling regular asset directory"));
            continue;
        }
        if (candidate.record.operation != operation
            || candidate.record.transactionId.compare(transactionId,
                                                       Qt::CaseInsensitive)
                   != 0
            || !validateManifestTransactionRoot(transactionRoot,
                                                candidate.parent,
                                                candidate.record,
                                                &verificationError)
            || !assetInfo.isDir() || files::isLinkLike(assetInfo)) {
            blockedAssetRoots.insert(
                manifestRecoveryPathKey(candidate.item.assetRoot));
            appendRetained(candidate.item,
                           verificationError.isEmpty()
                               ? QStringLiteral(
                                     "Manifest transaction validation failed after resolving its asset")
                               : verificationError);
            continue;
        }

        candidate.nextPath = QDir(transactionRoot).absoluteFilePath(
            QString::fromLatin1(ManifestTransactionNextFileName));
        candidate.originalPath = QDir(transactionRoot).absoluteFilePath(
            QString::fromLatin1(ManifestTransactionOriginalFileName));
        verificationError.clear();
        if (!verifyOptionalTransactionManifest(
                candidate.nextPath,
                candidate.record.assetId,
                candidate.record.replacementCanonicalSha256,
                &candidate.hasNext,
                &verificationError)
            || !verifyOptionalTransactionManifest(
                candidate.originalPath,
                candidate.record.assetId,
                candidate.record.expectedCanonicalSha256,
                &candidate.hasOriginal,
                &verificationError)) {
            blockedAssetRoots.insert(
                manifestRecoveryPathKey(candidate.item.assetRoot));
            appendRetained(candidate.item,
                           verificationError.isEmpty()
                               ? QStringLiteral(
                                     "Manifest transaction data could not be verified")
                               : verificationError);
            continue;
        }
        byAssetRoot[manifestRecoveryPathKey(candidate.item.assetRoot)]
            .append(candidate);
    }

    for (auto groupIt = byAssetRoot.begin();
         groupIt != byAssetRoot.end();
         ++groupIt) {
        QList<VerifiedManifestTransaction> &candidates = groupIt.value();
        const bool parentBlocked = !candidates.isEmpty()
                                   && blockedParents.contains(
                                       manifestRecoveryPathKey(
                                           candidates.first().parent));
        if (parentBlocked || blockedAssetRoots.contains(groupIt.key())) {
            for (const VerifiedManifestTransaction &candidate :
                 std::as_const(candidates)) {
                appendRetained(
                    candidate.item,
                    QStringLiteral(
                        "Automatic recovery is blocked by another unverified transaction for this asset boundary"));
            }
            continue;
        }
        QString preflightError;
        bool preflightValid = true;
        for (const VerifiedManifestTransaction &candidate :
             std::as_const(candidates)) {
            preflightError.clear();
            if (!reverifyManifestTransaction(candidate,
                                             &preflightError)) {
                preflightValid = false;
                break;
            }
        }
        if (!preflightValid) {
            for (const VerifiedManifestTransaction &candidate :
                 std::as_const(candidates)) {
                appendRetained(
                    candidate.item,
                    preflightError.isEmpty()
                        ? QStringLiteral(
                              "A transaction changed during recovery preflight; all recovery data was preserved")
                        : preflightError);
            }
            continue;
        }
        const QString livePath = QDir(candidates.first().item.assetRoot)
                                     .absoluteFilePath(
            QStringLiteral(".xips.json"));
        const QFileInfo initialLiveInfo(livePath);
        const bool liveWasMissing = !initialLiveInfo.exists()
                                    && !files::isLinkLike(initialLiveInfo);
        int restoredTerminal = -1;
        Manifest liveManifest;
        QString liveHash;
        QString groupError;

        if (liveWasMissing) {
            bool linear = !candidates.isEmpty();
            QString chainError;
            QMap<QString, int> outgoing;
            QMap<QString, int> incoming;
            QSet<QString> nodes;
            const QString chainAssetId = candidates.isEmpty()
                                             ? QString()
                                             : candidates.first().record.assetId;
            for (int index = 0; index < candidates.size(); ++index) {
                const VerifiedManifestTransaction &candidate =
                    candidates.at(index);
                const QString expected =
                    candidate.record.expectedCanonicalSha256;
                const QString replacement =
                    candidate.record.replacementCanonicalSha256;
                if (candidate.record.assetId != chainAssetId
                    || expected == replacement
                    || outgoing.contains(expected)
                    || incoming.contains(replacement)) {
                    linear = false;
                    chainError = QStringLiteral(
                        "Manifest transactions do not form a unique linear chain");
                    break;
                }
                outgoing.insert(expected, index);
                incoming.insert(replacement, index);
                nodes.insert(expected);
                nodes.insert(replacement);
            }

            QString startNode;
            QString endNode;
            int starts = 0;
            int ends = 0;
            if (linear) {
                for (const QString &node : std::as_const(nodes)) {
                    if (!incoming.contains(node)) {
                        startNode = node;
                        ++starts;
                    }
                    if (!outgoing.contains(node)) {
                        endNode = node;
                        ++ends;
                    }
                }
                linear = starts == 1 && ends == 1;
            }
            QSet<int> visited;
            QString cursor = startNode;
            int terminal = -1;
            while (linear && outgoing.contains(cursor)) {
                const int index = outgoing.value(cursor);
                if (visited.contains(index)) {
                    linear = false;
                    break;
                }
                visited.insert(index);
                terminal = index;
                cursor = candidates.at(index)
                             .record.replacementCanonicalSha256;
            }
            if (linear
                && (visited.size() != candidates.size()
                    || cursor != endNode || terminal < 0
                    || !candidates.at(terminal).hasOriginal)) {
                linear = false;
            }
            if (!linear) {
                const QString message = chainError.isEmpty()
                                            ? QStringLiteral(
                                                  "Live manifest is missing and transactions are not one verified linear chain")
                                            : chainError;
                for (const VerifiedManifestTransaction &candidate :
                     std::as_const(candidates)) {
                    appendRetained(candidate.item, message);
                }
                continue;
            }

            VerifiedManifestTransaction &terminalCandidate =
                candidates[terminal];
            QString terminalOriginalHash;
            groupError.clear();
            const QFileInfo confirmedLiveInfo(livePath);
            if (confirmedLiveInfo.exists()
                || files::isLinkLike(confirmedLiveInfo)
                || !readManifestHashStable(
                    terminalCandidate.originalPath,
                    terminalCandidate.record.assetId,
                    nullptr,
                    &terminalOriginalHash,
                    &groupError)
                || terminalOriginalHash
                       != terminalCandidate.record.expectedCanonicalSha256
                || !QFile::rename(terminalCandidate.originalPath,
                                  livePath)
                || !readManifestHashStable(
                    livePath,
                    terminalCandidate.record.assetId,
                    &liveManifest,
                    &liveHash,
                    &groupError)
                || liveHash
                       != terminalCandidate.record.expectedCanonicalSha256) {
                const QString message = groupError.isEmpty()
                                            ? QStringLiteral(
                                                  "The terminal verified original could not be restored safely")
                                            : groupError;
                for (const VerifiedManifestTransaction &candidate :
                     std::as_const(candidates)) {
                    appendRetained(candidate.item, message);
                }
                continue;
            }
            restoredTerminal = terminal;
        } else {
            groupError.clear();
            if (!readManifestHashStable(livePath,
                                        {},
                                        &liveManifest,
                                        &liveHash,
                                        &groupError)) {
                for (const VerifiedManifestTransaction &candidate :
                     std::as_const(candidates)) {
                    appendRetained(
                        candidate.item,
                        QStringLiteral(
                            "The live manifest is linked or unreadable; it was preserved"));
                }
                continue;
            }
        }

        for (int index = 0; index < candidates.size(); ++index) {
            const VerifiedManifestTransaction &candidate =
                candidates.at(index);
            Manifest confirmedLive;
            QString confirmedLiveHash;
            QString liveVerificationError;
            if (!readManifestHashStable(livePath,
                                        {},
                                        &confirmedLive,
                                        &confirmedLiveHash,
                                        &liveVerificationError)
                || confirmedLive.id != liveManifest.id
                || confirmedLiveHash != liveHash) {
                for (int remaining = index;
                     remaining < candidates.size();
                     ++remaining) {
                    appendRetained(
                        candidates.at(remaining).item,
                        QStringLiteral(
                            "The live manifest changed during transaction cleanup; remaining recovery data was preserved"));
                }
                break;
            }
            const bool liveIsExpected =
                liveManifest.id == candidate.record.assetId
                && liveHash
                       == candidate.record.expectedCanonicalSha256;
            const bool liveIsReplacement =
                liveManifest.id == candidate.record.assetId
                && liveHash
                       == candidate.record.replacementCanonicalSha256;
            const bool verifiedSupersededAncestor =
                restoredTerminal >= 0 && index != restoredTerminal
                && liveManifest.id == candidate.record.assetId;
            if (!liveIsExpected && !liveIsReplacement
                && !verifiedSupersededAncestor) {
                appendRetained(
                    candidate.item,
                    QStringLiteral(
                        "The live manifest contains a concurrent third-party state; the transaction was preserved"));
                continue;
            }
            QString cleanupError;
            QString retainedPath;
            if (!retireVerifiedManifestTransaction(
                    candidate.item.transactionPath,
                    candidate.parent,
                    candidate.record,
                    livePath,
                    liveManifest.id,
                    liveHash,
                    &retainedPath,
                    &cleanupError
#ifdef XIPS_ENABLE_TEST_HOOKS
                    , testHook
#endif
                    )) {
                ManifestTransactionRecoveryItem retainedItem =
                    candidate.item;
                if (!retainedPath.isEmpty()) {
                    retainedItem.transactionPath = retainedPath;
                }
                if (index == restoredTerminal) {
                    retainedItem.outcome =
                        ManifestTransactionRecoveryOutcome::RestoredOriginal;
                    retainedItem.message = QStringLiteral(
                        "The terminal original was restored, but recovery data remains: %1")
                                               .arg(cleanupError);
                    ++result.restoredOriginal;
                    ++result.retained;
                    result.items.append(retainedItem);
                } else {
                    appendRetained(
                        retainedItem,
                        QStringLiteral(
                            "Verified manifest recovery data could not be retired: %1")
                            .arg(cleanupError));
                }
                for (int remaining = index + 1;
                     remaining < candidates.size();
                     ++remaining) {
                    appendRetained(
                        candidates.at(remaining).item,
                        QStringLiteral(
                            "A transaction failed final verification; remaining recovery data was preserved"));
                }
                break;
            }

            ManifestTransactionRecoveryItem completed = candidate.item;
            if (index == restoredTerminal) {
                completed.outcome =
                    ManifestTransactionRecoveryOutcome::RestoredOriginal;
                completed.message = QStringLiteral(
                    "The terminal verified original manifest was restored");
                ++result.restoredOriginal;
            } else if (verifiedSupersededAncestor
                       && !liveIsExpected && !liveIsReplacement) {
                completed.outcome =
                    ManifestTransactionRecoveryOutcome::KeptReplacement;
                completed.message = QStringLiteral(
                    "A verified superseded manifest transaction was retired");
                ++result.cleanedReplacement;
            } else if (liveIsExpected) {
                completed.outcome =
                    ManifestTransactionRecoveryOutcome::KeptExpected;
                completed.message = QStringLiteral(
                    "The expected live manifest was preserved");
                ++result.cleanedExpected;
            } else {
                completed.outcome =
                    ManifestTransactionRecoveryOutcome::KeptReplacement;
                completed.message = QStringLiteral(
                    "The published replacement manifest was preserved");
                ++result.cleanedReplacement;
            }
            result.items.append(completed);
        }
    }
    return result;
}

enum class ManifestCasOutcome {
    Published,
    ExpectedChanged,
    Failed
};

struct ManifestCasResult {
    ManifestCasOutcome outcome = ManifestCasOutcome::Failed;
    Manifest publishedManifest;
    Manifest observedManifest;
    QString retainedPath;
    QString warning;
};

ManifestCasResult compareAndSwapManifest(
    const QString &assetRoot,
    const QString &expectedAssetId,
    const Manifest &expectedManifest,
    const Manifest &replacementManifest,
    const QString &operationLabel
#ifdef XIPS_ENABLE_TEST_HOOKS
    , const WorkingCopyTestHook &testHook = {}
#endif
    )
{
    ManifestCasResult result;
    const ManifestService manifestService;
    if (expectedManifest.id != expectedAssetId
        || replacementManifest.id != expectedAssetId
        || !supportedManifestTransactionOperation(operationLabel)) {
        result.warning = QStringLiteral(
            "The manifest transaction does not match the selected asset");
        return result;
    }
    const QByteArray expectedCanonical = json::canonicalJson(
        manifestService.toJson(expectedManifest));
    const QByteArray nextCanonical = json::canonicalJson(
        manifestService.toJson(replacementManifest));
    const QString expectedHash = canonicalSha256(expectedCanonical);
    const QString replacementHash = canonicalSha256(nextCanonical);
    if (expectedCanonical == nextCanonical) {
        const QString root = files::normalizedAbsolute(assetRoot);
        const QFileInfo rootInfo(root);
        const QString liveManifestPath = QDir(root).absoluteFilePath(
            QStringLiteral(".xips.json"));
        Manifest current;
        QString currentError;
        if (!rootInfo.isDir() || files::isLinkLike(rootInfo)
            || !readManifestHashStable(liveManifestPath,
                                       expectedAssetId,
                                       &current,
                                       nullptr,
                                       &currentError)) {
            result.warning = currentError.isEmpty()
                                 ? QStringLiteral(
                                       "The asset manifest became unavailable before the no-op update could be verified")
                                 : currentError;
            return result;
        }
        const QByteArray currentCanonical = json::canonicalJson(
            manifestService.toJson(current));
        if (currentCanonical != expectedCanonical) {
            result.outcome = ManifestCasOutcome::ExpectedChanged;
            result.observedManifest = current;
            result.warning = QStringLiteral(
                "The asset manifest changed before the no-op update could be verified");
            return result;
        }
        result.outcome = ManifestCasOutcome::Published;
        result.publishedManifest = current;
        return result;
    }
    const QString normalizedRoot = files::normalizedAbsolute(assetRoot);
    const QFileInfo normalizedRootInfo(normalizedRoot);
    if (!normalizedRootInfo.isDir()
        || files::isLinkLike(normalizedRootInfo)) {
        result.warning = QStringLiteral("The selected asset path is invalid");
        return result;
    }
    const QString parent = QFileInfo(normalizedRoot).absolutePath();
    const QString transactionId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    const QString operationName = QStringLiteral(".xips-create-%1-%2")
                                      .arg(operationLabel, transactionId);
    const QString operationRoot = QDir(parent).absoluteFilePath(operationName);
    const QString nextPath = QDir(operationRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionNextFileName));
    const QString backupPath = QDir(operationRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionOriginalFileName));
    const QString manifestPath = QDir(normalizedRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    const QString transactionPath = QDir(operationRoot).absoluteFilePath(
        QString::fromLatin1(ManifestTransactionFileName));
    const ManifestTransactionRecord transaction{
        .operation = operationLabel,
        .transactionId = transactionId,
        .assetDirectory = QFileInfo(normalizedRoot).fileName(),
        .assetId = expectedAssetId,
        .expectedCanonicalSha256 = expectedHash,
        .replacementCanonicalSha256 = replacementHash,
    };
    QString actualRetainedOperationPath;
    QString retirementWarning;
    const auto setResult = [&result,
                            &operationRoot,
                            &actualRetainedOperationPath,
                            &retirementWarning](
                               const ManifestCasOutcome outcome,
                               const QString &message,
                               const bool retained) {
        result.outcome = outcome;
        const QString retainedPath = actualRetainedOperationPath.isEmpty()
                                         ? operationRoot
                                         : actualRetainedOperationPath;
        result.retainedPath = retained ? retainedPath : QString();
        result.warning = retained
                             ? QStringLiteral("%1 Data remains at %2%3")
                                   .arg(message,
                                        retainedPath,
                                        retirementWarning.isEmpty()
                                            ? QString()
                                            : QStringLiteral(": %1")
                                                  .arg(retirementWarning))
                             : message;
    };
    const auto removeVerifiedNextAndDirectory = [&]() {
        QString actualHash;
        QString cleanupError;
        if (QFileInfo::exists(nextPath)
            && (!readManifestHashStable(nextPath,
                                        expectedAssetId,
                                        nullptr,
                                        &actualHash,
                                        &cleanupError)
                || actualHash != replacementHash
                || !QFile::remove(nextPath))) {
            return false;
        }
        return QDir().rmdir(operationRoot);
    };
    const auto retireTransaction = [&](const QString &anchoredLiveHash) {
        return retireVerifiedManifestTransaction(operationRoot,
                                                  parent,
                                                  transaction,
                                                  manifestPath,
                                                  expectedAssetId,
                                                  anchoredLiveHash,
                                                  &actualRetainedOperationPath,
                                                  &retirementWarning
#ifdef XIPS_ENABLE_TEST_HOOKS
                                                  , testHook
#endif
                                                  );
    };

    if (!QDir().mkpath(operationRoot)) {
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral("The manifest update could not be prepared"),
                  false);
        return result;
    }
    QString operationError;
    if (!manifestService.write(nextPath,
                               replacementManifest,
                               &operationError)) {
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral("The manifest update could not be prepared: %1")
                      .arg(operationError),
                  !QDir().rmdir(operationRoot));
        return result;
    }
    QString preparedHash;
    operationError.clear();
    if (!readManifestHashStable(nextPath,
                                expectedAssetId,
                                nullptr,
                                &preparedHash,
                                &operationError)
        || preparedHash != replacementHash) {
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "The prepared manifest update could not be verified: %1")
                      .arg(operationError),
                  true);
        return result;
    }

    Manifest current;
    QString currentHash;
    operationError.clear();
    const bool currentReadable = readManifestHashStable(manifestPath,
                                                        expectedAssetId,
                                                        &current,
                                                        &currentHash,
                                                        &operationError);
    if (!currentReadable || currentHash != expectedHash) {
        const bool cleaned = removeVerifiedNextAndDirectory();
        const bool expectedChanged = currentReadable;
        if (expectedChanged) {
            result.observedManifest = current;
        }
        setResult(expectedChanged ? ManifestCasOutcome::ExpectedChanged
                                  : ManifestCasOutcome::Failed,
                  expectedChanged
                      ? QStringLiteral(
                            "The asset manifest changed before it could be updated")
                      : QStringLiteral(
                            "The asset manifest became unavailable before it could be updated"),
                  !cleaned);
        return result;
    }
    operationError.clear();
    if (!writeManifestTransactionRecord(transactionPath,
                                        transaction,
                                        &operationError)) {
        const bool cleaned = removeVerifiedNextAndDirectory();
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "The manifest recovery record could not be prepared: %1")
                      .arg(operationError),
                  !cleaned);
        return result;
    }
    ManifestTransactionRecord confirmedTransaction;
    operationError.clear();
    if (!readManifestTransactionRecord(transactionPath,
                                       &confirmedTransaction,
                                       &operationError)
        || !sameManifestTransactionRecord(transaction,
                                          confirmedTransaction)) {
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "The manifest recovery record could not be verified: %1")
                      .arg(operationError),
                  true);
        return result;
    }
    if (!QFile::rename(manifestPath, backupPath)) {
        const bool cleaned = retireTransaction(expectedHash);
        const ManifestLoadResult observed = manifestService.load(manifestPath);
        const bool expectedChanged = observed.ok()
                                     && observed.manifest->id
                                            == expectedAssetId
                                     && json::canonicalJson(
                                            manifestService.toJson(
                                                *observed.manifest))
                                            != expectedCanonical;
        if (expectedChanged) {
            result.observedManifest = *observed.manifest;
        }
        setResult(expectedChanged ? ManifestCasOutcome::ExpectedChanged
                                  : ManifestCasOutcome::Failed,
                  expectedChanged
                      ? QStringLiteral(
                            "The asset manifest changed before isolation")
                      : QStringLiteral(
                            "The asset manifest could not be isolated"),
                  !cleaned);
        return result;
    }

    QString isolatedHash;
    operationError.clear();
    const bool isolatedMatches = readManifestHashStable(backupPath,
                                                        expectedAssetId,
                                                        nullptr,
                                                        &isolatedHash,
                                                        &operationError)
                                 && isolatedHash == expectedHash;
    if (!isolatedMatches || QFileInfo::exists(manifestPath)) {
        bool restored = false;
        if (!QFileInfo::exists(manifestPath)) {
            restored = QFile::rename(backupPath, manifestPath);
        }
        bool cleaned = false;
        if (restored) {
            cleaned = retireTransaction(expectedHash);
        }
        const ManifestLoadResult observed = manifestService.load(manifestPath);
        const bool expectedChanged = cleaned && observed.ok()
                                     && observed.manifest->id
                                            == expectedAssetId
                                     && json::canonicalJson(
                                            manifestService.toJson(
                                                *observed.manifest))
                                            != expectedCanonical;
        if (expectedChanged) {
            result.observedManifest = *observed.manifest;
        }
        setResult(expectedChanged ? ManifestCasOutcome::ExpectedChanged
                                  : ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "A concurrent manifest change prevented the manifest update"),
                  !cleaned);
        return result;
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    if (testHook) {
        testHook(WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
                 operationRoot);
    }
#endif

    QString confirmedNextHash;
    operationError.clear();
    const bool nextStillMatches = readManifestHashStable(
        nextPath,
        expectedAssetId,
        nullptr,
        &confirmedNextHash,
        &operationError)
                                  && confirmedNextHash == replacementHash;
    if (!nextStillMatches || QFileInfo::exists(manifestPath)
        || !QFile::rename(nextPath, manifestPath)) {
        bool restored = false;
        if (!QFileInfo::exists(manifestPath)) {
            restored = QFile::rename(backupPath, manifestPath);
        }
        bool cleaned = false;
        if (restored) {
            cleaned = retireTransaction(expectedHash);
        }
        const ManifestLoadResult observed = manifestService.load(manifestPath);
        const bool expectedChanged = cleaned && observed.ok()
                                     && observed.manifest->id
                                            == expectedAssetId
                                     && json::canonicalJson(
                                            manifestService.toJson(
                                                *observed.manifest))
                                            != expectedCanonical;
        if (expectedChanged) {
            result.observedManifest = *observed.manifest;
        }
        setResult(expectedChanged ? ManifestCasOutcome::ExpectedChanged
                                  : ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "A concurrent manifest change prevented the manifest update"),
                  !cleaned);
        return result;
    }

    Manifest published;
    QString publishedHash;
    operationError.clear();
    if (!readManifestHashStable(manifestPath,
                                expectedAssetId,
                                &published,
                                &publishedHash,
                                &operationError)
        || publishedHash != replacementHash) {
        setResult(ManifestCasOutcome::Failed,
                  QStringLiteral(
                      "The published asset manifest could not be verified"),
                  true);
        return result;
    }
#ifdef XIPS_ENABLE_TEST_HOOKS
    if (testHook) {
        testHook(
            WorkingCopyTestPoint::ManifestReplacementPublishedBeforeCleanup,
            operationRoot);
    }
#endif
    result.outcome = ManifestCasOutcome::Published;
    result.publishedManifest = published;
    if (!retireTransaction(replacementHash)) {
        setResult(ManifestCasOutcome::Published,
                  QStringLiteral(
                      "The asset manifest was updated, but manifest recovery data could not be retired"),
                  true);
        return result;
    }
    return result;
}

struct VersionMarkerCasResult {
    bool published = false;
    Manifest publishedManifest;
    QString retainedPath;
    QString warning;
};

VersionMarkerCasResult compareAndSwapVersionMarker(
    const QString &assetRoot,
    const QString &expectedAssetId,
    const Manifest &baselineManifest,
    const QString &version
#ifdef XIPS_ENABLE_TEST_HOOKS
    , const WorkingCopyTestHook &testHook = {}
#endif
    )
{
    Manifest replacementManifest = baselineManifest;
    replacementManifest.version = version;
    const ManifestCasResult manifestResult = compareAndSwapManifest(
        assetRoot,
        expectedAssetId,
        baselineManifest,
        replacementManifest,
        QStringLiteral("version-marker")
#ifdef XIPS_ENABLE_TEST_HOOKS
        , testHook
#endif
        );
    VersionMarkerCasResult result;
    result.published = manifestResult.outcome
                       == ManifestCasOutcome::Published;
    result.publishedManifest = manifestResult.publishedManifest;
    result.retainedPath = manifestResult.retainedPath;
    result.warning = manifestResult.warning;
    return result;
}

bool containsTag(const QStringList &tags, const QString &candidate)
{
    return std::any_of(tags.cbegin(), tags.cend(),
                       [&candidate](const QString &tag) {
                           return tag.trimmed().compare(
                                      candidate.trimmed(),
                                      Qt::CaseInsensitive)
                                  == 0;
                       });
}

void removeTag(QStringList &tags, const QString &candidate)
{
    tags.erase(std::remove_if(tags.begin(), tags.end(),
                              [&candidate](const QString &tag) {
                                  return tag.trimmed().compare(
                                             candidate.trimmed(),
                                             Qt::CaseInsensitive)
                                         == 0;
                              }),
               tags.end());
}

QStringList applyTagDelta(const QStringList &baseline,
                          const QStringList &desired,
                          const QStringList &current)
{
    const QStringList cleanBaseline = cleanedTags(baseline);
    const QStringList cleanDesired = cleanedTags(desired);
    QStringList merged = cleanedTags(current);
    for (const QString &tag : cleanBaseline) {
        if (!containsTag(cleanDesired, tag)) {
            removeTag(merged, tag);
        }
    }
    for (const QString &tag : cleanDesired) {
        if (!containsTag(cleanBaseline, tag)
            && !containsTag(merged, tag)) {
            merged.append(tag);
        }
    }
    return cleanedTags(merged);
}

bool applyGroupMembershipChange(Manifest &manifest,
                                const QString &oldGroup,
                                const QString &newGroup)
{
    const QString oldName = oldGroup.trimmed();
    const QString newName = newGroup.trimmed();
    const QStringList before = cleanedTags(manifest.tags);
    QStringList after = before;
    if (oldName.isEmpty()) {
        if (!containsTag(after, newName)) {
            after.append(newName);
        }
    } else {
        QStringList replaced;
        for (const QString &tag : after) {
            if (tag.compare(oldName, Qt::CaseInsensitive) == 0) {
                if (!newName.isEmpty() && !containsTag(replaced, newName)) {
                    replaced.append(newName);
                }
            } else if (!containsTag(replaced, tag)) {
                replaced.append(tag);
            }
        }
        after = std::move(replaced);
    }
    manifest.tags = cleanedTags(after);
    return manifest.tags != before;
}

} // namespace

#ifdef XIPS_ENABLE_TEST_HOOKS
void AssetLibraryService::setWorkingCopyTestHook(
    const WorkingCopyTestPoint point,
    WorkingCopyTestHook hook)
{
    m_workingCopyTestPoint = point;
    m_hasWorkingCopyTestHook = static_cast<bool>(hook);
    m_workingCopyTestHook = std::move(hook);
}

void AssetLibraryService::invokeWorkingCopyTestHook(
    const WorkingCopyTestPoint point,
    const QString &path) const
{
    if (!m_hasWorkingCopyTestHook || point != m_workingCopyTestPoint) {
        return;
    }
    WorkingCopyTestHook hook = std::move(m_workingCopyTestHook);
    m_hasWorkingCopyTestHook = false;
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
        if (QDir().rmdir(stagingRoot)) {
            return fail(error, operationError);
        }
        return fail(error,
                    QStringLiteral("%1; import staging remains at %2")
                        .arg(operationError, stagingRoot));
    }
    if (!QDir(libraryRoot).rename(stagingName, manifest.id)) {
        return fail(error,
                    QStringLiteral(
                        "Cannot publish the imported asset; verified import staging remains at %1")
                        .arg(stagingRoot));
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
            result.createdProofs.append(assetDeletionProof(created));
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
    MetadataUpdateResult result;
    return updateMetadata(asset, metadata, &result, error);
}

bool AssetLibraryService::updateMetadata(const AssetRecord &asset,
                                         const AssetMetadata &metadata,
                                         MetadataUpdateResult *result,
                                         QString *error) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Updating asset details requires a merge result"));
    }
    *result = {};
    if (!validateAssetRecord(asset, nullptr, error)) {
        return false;
    }
    const Manifest baseline = asset.manifest;
    if (!metadata.id.trimmed().isEmpty()
        && metadata.id.trimmed() != baseline.id) {
        return fail(error, QStringLiteral("Asset id is stable and cannot be changed"));
    }

    const QString desiredName = metadata.name.trimmed();
    const QString desiredDescription = metadata.description.trimmed();
    const QStringList desiredTags = cleanedTags(metadata.tags);
    Manifest desiredValidation = baseline;
    desiredValidation.name = desiredName;
    desiredValidation.description = desiredDescription;
    desiredValidation.tags = desiredTags;
    const ManifestService manifestService;
    const QStringList validationErrors = manifestService.validate(
        desiredValidation);
    if (!validationErrors.isEmpty()) {
        return fail(error, validationErrors.first());
    }

    const QString baselineName = baseline.name.trimmed();
    const QString baselineDescription = baseline.description.trimmed();
    constexpr int MaximumMergeAttempts = 4;
    for (int attempt = 0; attempt < MaximumMergeAttempts; ++attempt) {
        const ManifestLoadResult loaded = manifestService.load(
            asset.manifestPath);
        if (!loaded.ok() || loaded.manifest->id != baseline.id) {
            return fail(error,
                        QStringLiteral(
                            "Cannot reload the selected asset manifest for a safe merge"));
        }
        const Manifest current = *loaded.manifest;
        Manifest next = current;
        QStringList conflicts;
        const auto mergeField = [&conflicts](const QString &field,
                                             const QString &baselineValue,
                                             const QString &desiredValue,
                                             const QString &currentValue,
                                             QString *target) {
            const bool userChanged = desiredValue != baselineValue;
            const bool currentChanged = currentValue != baselineValue;
            if (userChanged && currentChanged
                && desiredValue != currentValue) {
                conflicts.append(field);
                return;
            }
            if (userChanged) {
                *target = desiredValue;
            }
        };
        mergeField(QStringLiteral("name"),
                   baselineName,
                   desiredName,
                   current.name.trimmed(),
                   &next.name);
        mergeField(QStringLiteral("description"),
                   baselineDescription,
                   desiredDescription,
                   current.description.trimmed(),
                   &next.description);
        next.tags = applyTagDelta(baseline.tags,
                                  desiredTags,
                                  current.tags);
        if (!conflicts.isEmpty()) {
            result->conflictingFields = conflicts;
            result->manifest = current;
            return fail(
                error,
                QStringLiteral("Asset details changed concurrently: %1")
                    .arg(conflicts.join(QStringLiteral(", "))));
        }

        const QByteArray currentCanonical = json::canonicalJson(
            manifestService.toJson(current));
        const QByteArray nextCanonical = json::canonicalJson(
            manifestService.toJson(next));
        const bool manifestChanged = currentCanonical != nextCanonical;

#ifdef XIPS_ENABLE_TEST_HOOKS
        invokeWorkingCopyTestHook(
            WorkingCopyTestPoint::MetadataMergedBeforeManifestCas,
            asset.manifestPath);
#endif

        const ManifestCasResult published = compareAndSwapManifest(
            asset.assetRoot,
            baseline.id,
            current,
            next,
            QStringLiteral("metadata")
#ifdef XIPS_ENABLE_TEST_HOOKS
            , [this](const WorkingCopyTestPoint point,
                     const QString &path) {
                  invokeWorkingCopyTestHook(point, path);
              }
#endif
            );
        if (published.outcome == ManifestCasOutcome::Published) {
            result->published = true;
            result->changed = manifestChanged;
            result->manifest = published.publishedManifest;
            result->retainedPath = published.retainedPath;
            result->warning = published.warning;
            return true;
        }
        if (published.outcome == ManifestCasOutcome::ExpectedChanged
            && published.retainedPath.isEmpty()) {
            continue;
        }
        result->retainedPath = published.retainedPath;
        result->warning = published.warning;
        return fail(error,
                    published.warning.isEmpty()
                        ? QStringLiteral("Cannot safely update asset details")
                        : published.warning);
    }
    return fail(error,
                QStringLiteral(
                    "The asset manifest kept changing; details were not overwritten"));
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
                                      const QString &expectedCurrentHash,
                                      const QStringList &expectedSourceFiles,
                                      const QString &expectedSourcePayloadFingerprint) const
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
    const QString expectedPayloadFingerprint =
        expectedSourcePayloadFingerprint.trimmed();
    const bool hasExpectedSourceProof = !expectedSourceFiles.isEmpty()
                                        || !expectedPayloadFingerprint.isEmpty();
    if (hasExpectedSourceProof
        && (expectedSourceFiles.isEmpty()
            || !validFingerprint(expectedPayloadFingerprint))) {
        return fail(error,
                    QStringLiteral("The expected source payload proof is invalid"));
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
    PayloadProof expectedSourceProof;
    if (hasExpectedSourceProof) {
        if (source.singleFile || source.files != expectedSourceFiles
            || !verifyPayloadProofAtRoot(source.root,
                                         expectedSourceFiles,
                                         expectedPayloadFingerprint,
                                         &expectedSourceProof,
                                         error)) {
            return fail(error,
                        error && !error->isEmpty()
                            ? QStringLiteral(
                                  "The verified restore source changed before update: %1")
                                  .arg(*error)
                            : QStringLiteral(
                                  "The verified restore source changed before update"));
        }
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
    if (hasExpectedSourceProof) {
        copied = copyPayloadFiles(source.root,
                                  expectedSourceFiles,
                                  stagingRoot,
                                  &operationError);
    } else if (source.singleFile) {
        copied = QFile::copy(
            QDir(source.root).absoluteFilePath(source.files.first()),
            QDir(stagingRoot).absoluteFilePath(source.files.first()));
        if (!copied) {
            operationError = QStringLiteral("Cannot copy the replacement file");
        }
    } else {
        copied = copyPayload(source.root, stagingRoot, &operationError);
    }
    if (!copied) {
        if (!QDir().rmdir(stagingRoot)) {
            operationError += QStringLiteral("; update staging remains at %1")
                                  .arg(stagingRoot);
        }
        return fail(error, operationError);
    }
    if (hasExpectedSourceProof) {
        PayloadProof confirmedSourceProof;
        PayloadProof copiedSourceProof;
        QString sourceProofError;
        QString copiedProofError;
        const bool sourceUnchanged = verifyPayloadProofAtRoot(
                                         source.root,
                                         expectedSourceFiles,
                                         expectedPayloadFingerprint,
                                         &confirmedSourceProof,
                                         &sourceProofError)
                                     && samePayloadProof(expectedSourceProof,
                                                         confirmedSourceProof);
        const bool copiedAsExpected = verifyPayloadProofAtRoot(
            stagingRoot,
            expectedSourceFiles,
            expectedPayloadFingerprint,
            &copiedSourceProof,
            &copiedProofError);
        if (!sourceUnchanged || !copiedAsExpected) {
            return fail(
                error,
                QStringLiteral(
                    "The verified restore source changed while it was copied; the working copy was not isolated and update staging remains at %1%2%3")
                    .arg(stagingRoot,
                         sourceProofError.isEmpty()
                             ? QString()
                             : QStringLiteral(": %1").arg(sourceProofError),
                         copiedProofError.isEmpty()
                             ? QString()
                             : QStringLiteral("; %1").arg(copiedProofError)));
        }
    }
    if (!ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        return fail(error,
                    QStringLiteral("%1; update staging remains at %2")
                        .arg(operationError, stagingRoot));
    }

    QDir parentDirectory(parent);
    if (!parentDirectory.rename(assetName, backupName)) {
        return fail(error,
                    QStringLiteral(
                        "Cannot stage the existing working copy; update staging remains at %1")
                        .arg(stagingRoot));
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
        return fail(
            error,
            rolledBack
                ? QStringLiteral(
                      "The working copy changed while the update was starting; newer edits were kept and update staging remains at %1")
                      .arg(stagingRoot)
                : QStringLiteral(
                      "The working copy changed while the update was starting and rollback failed; recovery is at %1 and update staging remains at %2")
                      .arg(backupRoot, stagingRoot));
    }
    manifest = stagedManifest;
    operationError.clear();
    if (!ManifestService().write(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".xips.json")),
            manifest,
            &operationError)) {
        const bool rolledBack = parentDirectory.rename(backupName,
                                                        assetName);
        return fail(error,
                    rolledBack
                        ? QStringLiteral("%1; update staging remains at %2")
                              .arg(operationError, stagingRoot)
                        : QStringLiteral(
                              "%1; rollback failed and recovery is at %2; update staging remains at %3")
                              .arg(operationError, backupRoot, stagingRoot));
    }
    const QString backupInternal = QDir(backupRoot).absoluteFilePath(
        QStringLiteral(".xips"));
    const QString stagingInternal = QDir(stagingRoot).absoluteFilePath(
        QStringLiteral(".xips"));
    const bool hadInternal = QFileInfo(backupInternal).isDir();
    if (hadInternal && !QDir().rename(backupInternal, stagingInternal)) {
        const bool rolledBack = parentDirectory.rename(backupName, assetName);
        return fail(error,
                    rolledBack
                        ? QStringLiteral(
                              "Cannot preserve saved versions during update; update staging remains at %1")
                              .arg(stagingRoot)
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
        if (internalRolledBack && rootRolledBack) {
            return fail(
                error,
                QStringLiteral(
                    "The working copy changed before publish; newer edits were kept and update staging remains at %1")
                    .arg(stagingRoot));
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

    const ManifestService manifestService;
    Manifest anchoredPublishedManifest;
    QString anchoredPublishedFingerprint;
    QString anchoredPublishedError;
    const bool anchoredStagingVerified = strictFingerprintAtRoot(
        stagingRoot,
        manifest.id,
        &anchoredPublishedManifest,
        &anchoredPublishedFingerprint,
        &anchoredPublishedError);
    const bool anchoredManifestMatches =
        anchoredStagingVerified
        && json::canonicalJson(
               manifestService.toJson(anchoredPublishedManifest))
               == json::canonicalJson(manifestService.toJson(manifest));
    QString anchoredBoundPayloadError;
    const bool anchoredBoundPayloadMatches =
        !hasExpectedSourceProof
        || verifiedCopiedPayload(stagingRoot,
                                 expectedSourceFiles,
                                 expectedPayloadFingerprint,
                                 &anchoredBoundPayloadError);
#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::StagingVerifiedBeforePublish,
        stagingRoot);
#endif
    Manifest intendedPublishedManifest;
    QString intendedPublishedFingerprint;
    QString intendedPublishedError;
    const bool stagingVerified = strictFingerprintAtRoot(
        stagingRoot,
        manifest.id,
        &intendedPublishedManifest,
        &intendedPublishedFingerprint,
        &intendedPublishedError);
    const bool stagingManifestMatches =
        anchoredManifestMatches && stagingVerified
        && intendedPublishedFingerprint == anchoredPublishedFingerprint
        && json::canonicalJson(manifestService.toJson(intendedPublishedManifest))
               == json::canonicalJson(
                   manifestService.toJson(anchoredPublishedManifest));
    QString boundPayloadError;
    const bool boundPayloadMatches =
        anchoredBoundPayloadMatches
        && (!hasExpectedSourceProof
            || verifiedCopiedPayload(stagingRoot,
                                     expectedSourceFiles,
                                     expectedPayloadFingerprint,
                                     &boundPayloadError));
    if (!anchoredStagingVerified || !stagingVerified
        || !stagingManifestMatches
        || !boundPayloadMatches) {
        bool internalRolledBack = true;
        if (hadInternal) {
            internalRolledBack = QDir().rename(stagingInternal,
                                                backupInternal);
        }
        const bool rootRolledBack = parentDirectory.rename(backupName,
                                                            assetName);
        if (internalRolledBack && rootRolledBack) {
            return fail(
                error,
                anchoredStagingVerified && stagingVerified
                        && boundPayloadMatches
                    ? QStringLiteral(
                          "The staged update manifest changed before publish; the existing working copy was kept and update staging remains at %1")
                          .arg(stagingRoot)
                    : QStringLiteral(
                          "Cannot verify the staged update before publish: %1%2; the existing working copy was kept and update staging remains at %3")
                          .arg(!anchoredPublishedError.isEmpty()
                                   ? anchoredPublishedError
                                   : intendedPublishedError,
                               !anchoredBoundPayloadError.isEmpty()
                                   ? anchoredBoundPayloadError
                                   : boundPayloadError.isEmpty()
                                   ? QString()
                                   : QStringLiteral("; %1")
                                         .arg(boundPayloadError),
                               stagingRoot));
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
    if (!parentDirectory.rename(stagingName, assetName)) {
        bool internalRolledBack = true;
        if (hadInternal) {
            internalRolledBack = QDir().rename(stagingInternal, backupInternal);
        }
        const bool rootRolledBack = parentDirectory.rename(backupName, assetName);
        if (internalRolledBack && rootRolledBack) {
            return fail(
                error,
                QStringLiteral(
                    "Cannot publish the updated working copy; verified update staging remains at %1")
                    .arg(stagingRoot));
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
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Restoring a version requires a result for recovery and staging reporting"));
    }

    const CopyPlan plan = copyPlan(asset, cleanVersion);
    if (!plan.ok()) {
        return fail(error, plan.error);
    }
    const QString parent = QFileInfo(asset.assetRoot).absolutePath();
    const QString restorePrefix = QStringLiteral(".xips-create-restore-");
    const QString restoreName = restorePrefix
                                + QUuid::createUuid().toString(
                                    QUuid::WithoutBraces);
    const QString restoreRoot = QDir(parent).absoluteFilePath(restoreName);
    if (!QDir().mkpath(restoreRoot)) {
        return fail(error,
                    QStringLiteral("Cannot create the restore staging directory"));
    }
    UpdateAssetResult completed;
    const auto retainUnverifiedRestore = [&](const QString &message) {
        completed.retainedPaths.append(restoreRoot);
        completed.retainedPaths.removeDuplicates();
        appendWarning(completed.warning,
                      QStringLiteral("%1 Restore staging remains at %2")
                          .arg(message, restoreRoot));
        if (result) {
            *result = completed;
        }
    };
    const auto retireRestore = [&](const PayloadProof &proof,
                                   QString *cleanupMessage) {
        StagingRetireResult retired;
        const bool removed = retireVerifiedPayloadStaging(proof,
                                                          parent,
                                                          restorePrefix,
                                                          &retired);
        if (!removed) {
            completed.retainedPaths.append(retired.retainedPath.isEmpty()
                                               ? restoreRoot
                                               : retired.retainedPath);
            completed.retainedPaths.removeDuplicates();
            appendWarning(completed.warning,
                          retired.warning.isEmpty()
                              ? QStringLiteral(
                                    "Restore staging could not be safely retired and remains at %1")
                                    .arg(restoreRoot)
                              : retired.warning);
            if (cleanupMessage) {
                *cleanupMessage = completed.warning;
            }
        }
        return removed;
    };
    QString operationError;
    if (!copyPayloadFiles(plan.sourceRoot,
                          plan.files,
                          restoreRoot,
                          &operationError)) {
        retainUnverifiedRestore(
            QStringLiteral("The saved version could not be copied: %1.")
                .arg(operationError));
        return fail(error,
                    QStringLiteral("%1; restore staging remains at %2")
                        .arg(operationError, restoreRoot));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::RestoreStagingPreparedBeforeInitialVerification,
        restoreRoot);
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::SavedVersionCopiedBeforeVerification,
        plan.sourceRoot);
#endif

    QString sourceError;
    QString stagingError;
    PayloadProof restoreProof;
    const bool sourceVerified = verifyCopyPlanSource(asset,
                                                     plan,
                                                     &sourceError);
    const bool stagingVerified = verifyPayloadProofAtRoot(
        restoreRoot,
        plan.files,
        plan.payloadFingerprint,
        &restoreProof,
        &stagingError);
    if (!sourceVerified || !stagingVerified) {
        QString cleanupMessage;
        if (stagingVerified) {
            retireRestore(restoreProof, &cleanupMessage);
        } else {
            retainUnverifiedRestore(
                QStringLiteral(
                    "Restore staging no longer matches the verified saved version."));
        }
        return fail(error,
                    QStringLiteral(
                        "Saved version or restore staging changed while it was copied: %1%2%3")
                        .arg(sourceError,
                             stagingError.isEmpty()
                                 ? QString()
                                 : QStringLiteral("; %1").arg(stagingError),
                             cleanupMessage.isEmpty()
                                 ? QString()
                                 : QStringLiteral("; %1").arg(cleanupMessage)));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::RestoreStagingVerifiedBeforeUpdateAsset,
        restoreRoot);
#endif

    if (!updateAsset(asset,
                     restoreRoot,
                     recoveryMode,
                     &completed,
                     &operationError,
                     {},
                     plan.files,
                     plan.payloadFingerprint)) {
        QString cleanupMessage;
        retireRestore(restoreProof, &cleanupMessage);
        if (result) {
            *result = completed;
        }
        return fail(error,
                    cleanupMessage.isEmpty()
                        ? operationError
                        : QStringLiteral("%1; %2")
                              .arg(operationError, cleanupMessage));
    }
    completed.preview = preview;
    QString postPublishError;
    if (!verifiedCopiedPayload(asset.assetRoot,
                               plan.files,
                               plan.payloadFingerprint,
                               &postPublishError)) {
        completed.publishedAsIntended = false;
        completed.retainedPaths.append(restoreRoot);
        appendWarning(
            completed.warning,
            QStringLiteral(
                "The restored working copy could not be verified against the saved version; restore staging remains at %1: %2")
                .arg(restoreRoot, postPublishError));
        if (result) {
            *result = completed;
        }
        return true;
    }
    postPublishError.clear();
    if (!verifyCopyPlanSource(asset, plan, &postPublishError)) {
        appendWarning(
            completed.warning,
            QStringLiteral(
                "The working copy was restored from verified staging, but the saved version changed afterward: %1")
                .arg(postPublishError));
    }
    retireRestore(restoreProof, nullptr);
    if (result) {
        *result = completed;
    }
    return true;
}

bool AssetLibraryService::undoWorkingCopyChange(
    const AssetRecord &asset,
    const WorkingCopyUndoToken &token,
    UpdateAssetResult *result,
    QString *error,
    const RemovalMode cleanupMode) const
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
    QStringList recoveryFiles;
    QString recoveryPayloadFingerprint;
    hashError.clear();
    if (!strictPayloadFingerprint(token.recoveryPath,
                                  nullptr,
                                  &recoveryFiles,
                                  &recoveryPayloadFingerprint,
                                  &hashError)) {
        return fail(error,
                    QStringLiteral(
                        "The retained recovery payload could not be verified for Undo: %1")
                        .arg(hashError));
    }

    const QString parent = QFileInfo(asset.assetRoot).absolutePath();
    const QString undoPrefix = QStringLiteral(".xips-create-undo-");
    const QString undoName = undoPrefix
                             + QUuid::createUuid().toString(
                                 QUuid::WithoutBraces);
    const QString undoRoot = QDir(parent).absoluteFilePath(undoName);
    if (!QDir().mkpath(undoRoot)) {
        return fail(error,
                    QStringLiteral("Cannot create the undo staging directory"));
    }
    UpdateAssetResult completed;
    const auto retainUndoStaging = [&](const QString &message) {
        completed.retainedPaths.append(undoRoot);
        completed.retainedPaths.removeDuplicates();
        appendWarning(completed.warning,
                      QStringLiteral("%1 Undo staging remains at %2")
                          .arg(message, undoRoot));
        *result = completed;
    };
    const auto retireUndoStaging = [&](const PayloadProof &proof) {
        StagingRetireResult retired;
        if (!retireVerifiedPayloadStaging(proof,
                                          parent,
                                          undoPrefix,
                                          &retired)) {
            completed.retainedPaths.append(retired.retainedPath.isEmpty()
                                               ? undoRoot
                                               : retired.retainedPath);
            completed.retainedPaths.removeDuplicates();
            appendWarning(completed.warning,
                          retired.warning.isEmpty()
                              ? QStringLiteral(
                                    "Undo staging could not be safely retired and remains at %1")
                                    .arg(undoRoot)
                              : retired.warning);
            return false;
        }
        return true;
    };
    QString operationError;
    if (!copyPayloadFiles(token.recoveryPath,
                          recoveryFiles,
                          undoRoot,
                          &operationError)) {
        retainUndoStaging(
            QStringLiteral("The recovery could not be copied: %1.")
                .arg(operationError));
        return fail(error,
                    QStringLiteral("%1; undo staging remains at %2")
                        .arg(operationError, undoRoot));
    }
    QString confirmedRecoveryHash;
    QString confirmedRecoveryPayloadFingerprint;
    operationError.clear();
    const bool recoveryStillVerified =
        strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                        asset.manifest.id,
                                        nullptr,
                                        &confirmedRecoveryHash,
                                        &operationError)
        && confirmedRecoveryHash == token.recoveryFingerprint
        && strictPayloadFingerprint(token.recoveryPath,
                                    &recoveryFiles,
                                    nullptr,
                                    &confirmedRecoveryPayloadFingerprint,
                                    &operationError)
        && confirmedRecoveryPayloadFingerprint
               == recoveryPayloadFingerprint;
    PayloadProof undoProof;
    QString undoProofError;
    const bool copiedRecoveryVerified = verifyPayloadProofAtRoot(
        undoRoot,
        recoveryFiles,
        recoveryPayloadFingerprint,
        &undoProof,
        &undoProofError);
    if (!recoveryStillVerified || !copiedRecoveryVerified) {
        if (copiedRecoveryVerified) {
            retireUndoStaging(undoProof);
        } else {
            retainUndoStaging(
                QStringLiteral(
                    "Copied recovery no longer matches its verified payload."));
        }
        return fail(
            error,
            QStringLiteral(
                "The retained recovery changed while it was being copied; Undo was not applied: %1%2")
                .arg(operationError,
                     undoProofError.isEmpty()
                         ? QString()
                         : QStringLiteral("; %1").arg(undoProofError)));
    }

    const WorkingCopyRecoveryMode undoRecoveryMode =
        cleanupMode == RemovalMode::MoveToTrash
            ? WorkingCopyRecoveryMode::MoveToTrash
            : WorkingCopyRecoveryMode::Permanent;
    if (!updateAsset(asset,
                     undoRoot,
                     undoRecoveryMode,
                     &completed,
                     &operationError,
                     token.publishedFingerprint,
                     recoveryFiles,
                     recoveryPayloadFingerprint)) {
        retireUndoStaging(undoProof);
        *result = completed;
        return fail(error, operationError);
    }
    retireUndoStaging(undoProof);
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
    WorkingCopyUndoToken cleanupToken = token;
    cleanupToken.publishedFingerprint = token.recoveryFingerprint;
    if (!discardWorkingCopyRecovery(asset,
                                    cleanupToken,
                                    cleanupMode,
                                    &discarded,
                                    &discardError)) {
        completed.retainedPaths.append(
            discarded.retainedPath.isEmpty() ? token.recoveryPath
                                             : discarded.retainedPath);
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
    QString *error,
    const WorkingCopyDiscardPolicy policy) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Discard requires a result for cleanup reporting"));
    }
    *result = {};
    if (!validateWorkingCopyUndoTokenEnvelope(asset, token, error)) {
        return false;
    }

    QString recoveryHash;
    QString recoveryError;
    const bool recoveryVerified =
        validateWorkingCopyRecovery(asset, token.recoveryPath, &recoveryError)
        && strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                           asset.manifest.id,
                                           nullptr,
                                           &recoveryHash,
                                           &recoveryError)
        && recoveryHash == token.recoveryFingerprint;
    if (!recoveryVerified) {
        result->outcome = RecoveryDiscardOutcome::TokenRetired;
        const QFileInfo recoveryInfo(token.recoveryPath);
        if (recoveryInfo.exists() || files::isLinkLike(recoveryInfo)) {
            result->retainedPath = files::normalizedAbsolute(
                token.recoveryPath);
            result->warning = QStringLiteral(
                "The Undo recovery is no longer valid and was not deleted; review retained data at %1: %2")
                                  .arg(result->retainedPath,
                                       recoveryError.isEmpty()
                                           ? QStringLiteral(
                                                 "its verified fingerprint changed")
                                           : recoveryError);
        } else {
            result->warning = QStringLiteral(
                "The Undo recovery is no longer available; the obsolete Undo token was retired");
        }
        return true;
    }

    QString liveHash;
    QString livePayloadHash;
    QStringList liveFiles;
    QString hashError;
    if (!strictNonEmptyAssetFingerprintAtRoot(asset.assetRoot,
                                              asset.manifest.id,
                                              nullptr,
                                              &liveFiles,
                                              &liveHash,
                                              &livePayloadHash,
                                              &hashError)) {
        result->outcome = RecoveryDiscardOutcome::PendingUndoPreserved;
        result->retainedPath = token.recoveryPath;
        return fail(
            error,
            QStringLiteral(
                "The current working copy is missing, empty, linked, or unreadable; its Undo recovery was preserved: %1")
                .arg(hashError));
    }
    if (policy == WorkingCopyDiscardPolicy::RequirePublishedCopy
        && liveHash != token.publishedFingerprint) {
        result->outcome = RecoveryDiscardOutcome::PendingUndoPreserved;
        result->retainedPath = token.recoveryPath;
        return fail(
            error,
            QStringLiteral(
                "The working copy changed after the operation; its Undo recovery was preserved"));
    }
    const QString anchoredLiveHash = liveHash;
    const QString anchoredPayloadHash = livePayloadHash;
    const QStringList anchoredLiveFiles = liveFiles;

    hashError.clear();
    if (!strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                         asset.manifest.id,
                                         nullptr,
                                         &recoveryHash,
                                         &hashError)
        || recoveryHash != token.recoveryFingerprint) {
        result->outcome = RecoveryDiscardOutcome::TokenRetired;
        result->retainedPath = token.recoveryPath;
        result->warning = QStringLiteral(
            "The Undo recovery changed after verification and was not deleted; review retained data at %1: %2")
                              .arg(token.recoveryPath,
                                   hashError.isEmpty()
                                       ? QStringLiteral(
                                             "its verified fingerprint changed")
                                       : hashError);
        return true;
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
    const auto retireIsolatedRecovery = [&](const QString &reason) {
        completed.outcome = RecoveryDiscardOutcome::TokenRetired;
        if (!QFileInfo::exists(token.recoveryPath)
            && parentDirectory.rename(discardName, recoveryName)) {
            completed.retainedPath = token.recoveryPath;
            completed.warning = QStringLiteral("%1 Recovery remains at %2")
                                    .arg(reason, token.recoveryPath);
            *result = completed;
            return true;
        }
        completed.retainedPath = discardPath;
        completed.warning = QStringLiteral("%1 Data remains at %2")
                                .arg(reason, discardPath);
        *result = completed;
        return true;
    };
    QString isolatedHash;
    hashError.clear();
    if (!strictRecoveryFingerprintAtRoot(discardPath,
                                         asset.manifest.id,
                                         nullptr,
                                         &isolatedHash,
                                         &hashError)
        || isolatedHash != token.recoveryFingerprint) {
        return retireIsolatedRecovery(
            QStringLiteral("The recovery was isolated but could not be reverified;%1")
                .arg(hashError.isEmpty()
                         ? QString()
                         : QStringLiteral(" %1;").arg(hashError)));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::RecoveryIsolatedBeforeLiveReverification,
        asset.assetRoot);
#endif

    QString confirmedLiveHash;
    QString confirmedPayloadHash;
    QStringList confirmedLiveFiles;
    hashError.clear();
    if (!strictNonEmptyAssetFingerprintAtRoot(asset.assetRoot,
                                              asset.manifest.id,
                                              nullptr,
                                              &confirmedLiveFiles,
                                              &confirmedLiveHash,
                                              &confirmedPayloadHash,
                                              &hashError)
        || confirmedLiveHash != anchoredLiveHash
        || confirmedPayloadHash != anchoredPayloadHash
        || confirmedLiveFiles != anchoredLiveFiles) {
        if (!QFileInfo::exists(token.recoveryPath)
            && parentDirectory.rename(discardName, recoveryName)) {
            QString rolledBackHash;
            QString rolledBackError;
            if (strictRecoveryFingerprintAtRoot(token.recoveryPath,
                                                asset.manifest.id,
                                                nullptr,
                                                &rolledBackHash,
                                                &rolledBackError)
                && rolledBackHash == token.recoveryFingerprint) {
                completed.outcome =
                    RecoveryDiscardOutcome::PendingUndoPreserved;
                completed.retainedPath = token.recoveryPath;
                *result = completed;
                return fail(
                    error,
                    QStringLiteral(
                        "The working copy changed or became unreadable before cleanup; its verified Undo recovery was preserved at %1%2")
                        .arg(token.recoveryPath,
                             hashError.isEmpty()
                                 ? QString()
                                 : QStringLiteral(": %1").arg(hashError)));
            }
            completed.outcome = RecoveryDiscardOutcome::TokenRetired;
            completed.retainedPath = token.recoveryPath;
            completed.warning = QStringLiteral(
                "The working copy changed before cleanup, and the restored recovery no longer matches its Undo token; data remains at %1%2")
                                    .arg(token.recoveryPath,
                                         rolledBackError.isEmpty()
                                             ? QString()
                                             : QStringLiteral(": %1")
                                                   .arg(rolledBackError));
            *result = completed;
            return true;
        }
        completed.outcome = RecoveryDiscardOutcome::TokenRetired;
        completed.retainedPath = discardPath;
        completed.warning = QStringLiteral(
            "The working copy changed before cleanup, and the recovery could not be restored to its Undo path; data remains at %1%2")
                                .arg(discardPath,
                                     hashError.isEmpty()
                                         ? QString()
                                         : QStringLiteral(": %1")
                                               .arg(hashError));
        *result = completed;
        return true;
    }

    completed.outcome = RecoveryDiscardOutcome::TokenRetired;
    if (!removeDirectory(discardPath, mode, nullptr)) {
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

AssetDeletionProof AssetLibraryService::assetDeletionProof(
    const AssetRecord &asset) const
{
    AssetDeletionProof proof;
    proof.assetId = asset.manifest.id;
    proof.assetRoot = files::normalizedAbsolute(asset.assetRoot);
    if (!validateAssetRecord(asset, nullptr, &proof.error)) {
        return proof;
    }
    AssetTreeProof treeProof;
    if (!verifyAssetTreeProofAtRoot(proof.assetRoot,
                                    proof.assetId,
                                    &treeProof,
                                    &proof.error)) {
        return proof;
    }
    proof.fingerprint = treeProof.fingerprint;
    return proof;
}

bool AssetLibraryService::deleteAsset(const QString &libraryRoot,
                                      const AssetRecord &asset,
                                      const RemovalMode mode,
                                      QString *removedPath,
                                      QString *error) const
{
    const AssetDeletionProof proof = assetDeletionProof(asset);
    if (!proof.ok()) {
        return fail(error, proof.error);
    }
    return deleteAsset(libraryRoot,
                       asset,
                       proof,
                       mode,
                       removedPath,
                       error);
}

bool AssetLibraryService::deleteAsset(
    const QString &libraryRoot,
    const AssetRecord &asset,
    const AssetDeletionProof &expectedProof,
    const RemovalMode mode,
    QString *removedPath,
    QString *error) const
{
    if (removedPath) {
        removedPath->clear();
    }
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
    if (!expectedProof.ok() || expectedProof.assetId != asset.manifest.id
        || !samePath(expectedProof.assetRoot, assetRoot)
        || !validFingerprint(expectedProof.fingerprint)) {
        return fail(error, QStringLiteral("The asset deletion proof is invalid"));
    }

    AssetTreeProof initialProof;
    QString proofError;
    if (!verifyAssetTreeProofAtRoot(assetRoot,
                                    asset.manifest.id,
                                    &initialProof,
                                    &proofError)
        || initialProof.fingerprint != expectedProof.fingerprint) {
        return fail(
            error,
            QStringLiteral(
                "The asset changed after deletion was confirmed and was not deleted%1")
                .arg(proofError.isEmpty()
                         ? QString()
                         : QStringLiteral(": %1").arg(proofError)));
    }

    const QString parent = QFileInfo(assetRoot).absolutePath();
    const QString assetName = QFileInfo(assetRoot).fileName();
    const QString operationName = QStringLiteral(".xips-create-delete-%1")
                                      .arg(QUuid::createUuid().toString(
                                          QUuid::WithoutBraces));
    const QString operationRoot = QDir(parent).absoluteFilePath(operationName);
    const QString isolatedRoot = QDir(operationRoot).absoluteFilePath(assetName);
    if (!QDir().mkpath(operationRoot)) {
        return fail(error,
                    QStringLiteral(
                        "Cannot create the asset deletion isolation directory"));
    }
    if (!QDir().rename(assetRoot, isolatedRoot)) {
        QDir().rmdir(operationRoot);
        return fail(error,
                    QStringLiteral(
                        "Cannot isolate the verified asset before deletion"));
    }
    const auto restoreIsolatedAsset = [&]() {
        if (!QFileInfo::exists(assetRoot)
            && QDir().rename(isolatedRoot, assetRoot)) {
            QDir().rmdir(operationRoot);
            return assetRoot;
        }
        return isolatedRoot;
    };

    AssetTreeProof isolatedProof;
    proofError.clear();
    if (!verifyAssetTreeProofAtRoot(isolatedRoot,
                                    asset.manifest.id,
                                    &isolatedProof,
                                    &proofError)
        || !sameAssetTreeContentProof(initialProof, isolatedProof)) {
        const QString retainedPath = restoreIsolatedAsset();
        return fail(
            error,
            QStringLiteral(
                "The isolated asset changed before deletion and was not removed; data remains at %1%2")
                .arg(retainedPath,
                     proofError.isEmpty()
                         ? QString()
                         : QStringLiteral(": %1").arg(proofError)));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteAssetIsolatedBeforeRemoval,
        isolatedRoot);
#endif

    AssetTreeProof confirmedProof;
    proofError.clear();
    if (!verifyAssetTreeProofAtRoot(isolatedRoot,
                                    asset.manifest.id,
                                    &confirmedProof,
                                    &proofError)
        || !sameAssetTreeContentProof(isolatedProof, confirmedProof)) {
        const QString retainedPath = restoreIsolatedAsset();
        return fail(
            error,
            QStringLiteral(
                "The isolated asset changed at the deletion boundary and was not removed; data remains at %1%2")
                .arg(retainedPath,
                     proofError.isEmpty()
                         ? QString()
                         : QStringLiteral(": %1").arg(proofError)));
    }

    QString actualRemovedPath;
    const bool removed = mode == RemovalMode::MoveToTrash
                             ? QFile::moveToTrash(isolatedRoot,
                                                  &actualRemovedPath)
                             : removeExactTree(isolatedRoot,
                                               confirmedProof.entries);
    if (!removed) {
        AssetTreeProof remainingProof;
        QString remainingError;
        const bool stillComplete = verifyAssetTreeProofAtRoot(
                                       isolatedRoot,
                                       asset.manifest.id,
                                       &remainingProof,
                                       &remainingError)
                                   && sameAssetTreeContentProof(
                                       confirmedProof,
                                       remainingProof);
        const QString retainedPath = stillComplete ? restoreIsolatedAsset()
                                                   : isolatedRoot;
        return fail(
            error,
            (mode == RemovalMode::MoveToTrash
                 ? QStringLiteral(
                       "Cannot move the verified asset to the recycle bin; data remains at %1")
                 : QStringLiteral(
                       "Cannot completely delete the verified asset; remaining data is at %1"))
                .arg(retainedPath));
    }
    if (removedPath) {
        *removedPath = actualRemovedPath;
    }
    QDir().rmdir(operationRoot);
    return true;
}

bool AssetLibraryService::changeGroupMembership(
    const QList<AssetRecord> &assets,
    const QString &oldGroup,
    const QString &newGroup,
    int *changed,
    QString *error) const
{
    GroupChangeResult result;
    const bool complete = changeGroupMembership(assets,
                                                oldGroup,
                                                newGroup,
                                                &result,
                                                error);
    if (changed) {
        *changed = result.updated;
    }
    return complete;
}

bool AssetLibraryService::changeGroupMembership(
    const QList<AssetRecord> &assets,
    const QString &oldGroup,
    const QString &newGroup,
    GroupChangeResult *result,
    QString *error) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Changing groups requires a per-asset result"));
    }
    *result = {};
    const QString oldName = oldGroup.trimmed();
    const QString newName = newGroup.trimmed();
    if (oldName.isEmpty() && newName.isEmpty()) {
        return fail(error, QStringLiteral("A group name is required"));
    }

    constexpr int MaximumMergeAttempts = 4;
    const ManifestService manifestService;
    for (const AssetRecord &asset : assets) {
        GroupChangeItemResult item;
        item.assetId = asset.manifest.id;
        item.assetName = asset.manifest.name;
        QString validationError;
        if (!validateAssetRecord(asset, nullptr, &validationError)) {
            item.outcome = GroupChangeOutcome::Failed;
            item.message = validationError;
            ++result->failed;
            result->items.append(item);
            continue;
        }

        bool completed = false;
        for (int attempt = 0; attempt < MaximumMergeAttempts; ++attempt) {
            const ManifestLoadResult loaded = manifestService.load(
                asset.manifestPath);
            if (!loaded.ok()
                || loaded.manifest->id != asset.manifest.id) {
                item.outcome = GroupChangeOutcome::Failed;
                item.message = QStringLiteral("Cannot reload asset manifest");
                ++result->failed;
                completed = true;
                break;
            }
            const Manifest current = *loaded.manifest;
            item.assetName = current.name;
            Manifest next = current;
            if (!applyGroupMembershipChange(next, oldName, newName)) {
                item.outcome = GroupChangeOutcome::Unchanged;
                ++result->unchanged;
                completed = true;
                break;
            }

#ifdef XIPS_ENABLE_TEST_HOOKS
            invokeWorkingCopyTestHook(
                WorkingCopyTestPoint::GroupMembershipMergedBeforeManifestCas,
                asset.manifestPath);
#endif

            const ManifestCasResult published = compareAndSwapManifest(
                asset.assetRoot,
                asset.manifest.id,
                current,
                next,
                QStringLiteral("group-membership")
#ifdef XIPS_ENABLE_TEST_HOOKS
                , [this](const WorkingCopyTestPoint point,
                         const QString &path) {
                      invokeWorkingCopyTestHook(point, path);
                  }
#endif
                );
            if (published.outcome == ManifestCasOutcome::Published) {
                item.outcome = GroupChangeOutcome::Updated;
                item.retainedPath = published.retainedPath;
                item.warning = published.warning;
                ++result->updated;
                completed = true;
                break;
            }
            if (published.outcome == ManifestCasOutcome::ExpectedChanged
                && published.retainedPath.isEmpty()) {
                continue;
            }
            item.outcome = GroupChangeOutcome::Failed;
            item.retainedPath = published.retainedPath;
            item.warning = published.warning;
            item.message = published.warning.isEmpty()
                               ? QStringLiteral(
                                     "Cannot safely update asset groups")
                               : published.warning;
            ++result->failed;
            completed = true;
            break;
        }
        if (!completed) {
            item.outcome = GroupChangeOutcome::Conflict;
            item.message = QStringLiteral(
                "Asset groups kept changing and were not overwritten");
            ++result->conflicts;
        }
        result->items.append(item);
    }

    if (!result->complete()) {
        return fail(
            error,
            QStringLiteral(
                "Group change was partially applied: %1 updated, %2 conflict(s), %3 failed")
                .arg(result->updated)
                .arg(result->conflicts)
                .arg(result->failed));
    }
    return true;
}

ManifestTransactionRecoveryResult
AssetLibraryService::recoverManifestTransactions(
    const QString &libraryRoot) const
{
    return recoverManifestTransactionsGrouped(
        libraryRoot
#ifdef XIPS_ENABLE_TEST_HOOKS
        , [this](const WorkingCopyTestPoint point,
                 const QString &path) {
              invokeWorkingCopyTestHook(point, path);
          }
#endif
        );
}

VersionInventoryResult AssetLibraryService::versionInventory(
    const QString &assetRoot) const
{
    VersionInventoryResult result;
    const QString normalizedAssetRoot = files::normalizedAbsolute(assetRoot);
    const QFileInfo assetRootInfo(normalizedAssetRoot);
    const QString liveManifestPath = QDir(normalizedAssetRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    const QFileInfo liveManifestInfo(liveManifestPath);
    if (!assetRootInfo.isDir() || files::isLinkLike(assetRootInfo)
        || !liveManifestInfo.isFile() || files::isLinkLike(liveManifestInfo)) {
        result.fatalError = QStringLiteral("Asset path or manifest is invalid");
        return result;
    }
    const ManifestLoadResult liveManifest = ManifestService().load(
        liveManifestPath);
    if (!liveManifest.ok()) {
        result.fatalError = QStringLiteral(
            "Cannot reload the selected asset manifest");
        return result;
    }
    const QString root = QDir(normalizedAssetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists()) {
        return result;
    }
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        result.fatalError = QStringLiteral(
            "Saved versions directory is invalid or linked: %1")
                                .arg(root);
        return result;
    }
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().startsWith(u'.')) {
            continue;
        }
        if (files::isLinkLike(entry)) {
            result.problems.append(
                QStringLiteral("Saved version directory is linked: %1")
                    .arg(entry.absoluteFilePath()));
            continue;
        }
        SnapshotProof proof;
        QString versionError;
        if (!verifySavedSnapshot(normalizedAssetRoot,
                                 liveManifest.manifest->id,
                                 entry.fileName(),
                                 entry.absoluteFilePath(),
                                 true,
                                 &proof,
                                 &versionError)) {
            result.problems.append(
                versionError.isEmpty()
                    ? QStringLiteral("Cannot verify saved version: %1")
                          .arg(entry.absoluteFilePath())
                    : versionError);
            continue;
        }
        result.validVersions.append(proof.info);
    }
    std::sort(result.validVersions.begin(),
              result.validVersions.end(),
              [](const VersionInfo &left, const VersionInfo &right) {
                  if (left.createdAt != right.createdAt) {
                      return left.createdAt > right.createdAt;
                  }
                  return left.version > right.version;
              });
    return result;
}

QList<VersionInfo> AssetLibraryService::versions(const QString &assetRoot,
                                                 QString *error) const
{
    if (error) {
        error->clear();
    }
    const VersionInventoryResult inventory = versionInventory(assetRoot);
    if (!inventory.fatalError.isEmpty()) {
        fail(error, inventory.fatalError);
        return {};
    }
    if (!inventory.problems.isEmpty()) {
        fail(error, inventory.problems.first());
        return {};
    }
    return inventory.validVersions;
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
    QString liveStrictHash;
    if (!AssetScanner::strictContentHash(comparable,
                                         asset.assetRoot,
                                         &liveStrictHash,
                                         &state.error)) {
        return state;
    }
    const ManifestLoadResult confirmed = ManifestService().load(
        asset.manifestPath);
    const ManifestService manifestService;
    if (!confirmed.ok()
        || json::canonicalJson(manifestService.toJson(*confirmed.manifest))
               != json::canonicalJson(manifestService.toJson(*loaded.manifest))) {
        state.error = QStringLiteral(
            "The working-copy manifest changed while its version state was being verified");
        return state;
    }
    state.changed = liveStrictHash
                    != savedVersions.first().strictContentHash;
    return state;
}

bool AssetLibraryService::createVersion(const AssetRecord &asset,
                                        const QString &version,
                                        VersionInfo *created,
                                        QString *error) const
{
    if (!created) {
        return fail(error,
                    QStringLiteral(
                        "Saving a version requires a result for marker-warning reporting"));
    }
    if (created) {
        *created = {};
    }
    if (error) {
        error->clear();
    }
    const QString cleanVersion = version.trimmed();
    if (!validVersion(cleanVersion)) {
        return fail(error,
                    QStringLiteral("Version must use letters, digits, '.', '_', '+', or '-'"));
    }

    Manifest baselineManifest;
    QStringList baselineFiles;
    QString baselineContentFingerprint;
    QString baselinePayloadFingerprint;
    if (!validateAssetRecord(asset, nullptr, error)
        || !strictNonEmptyAssetFingerprintAtRoot(
            asset.assetRoot,
            asset.manifest.id,
            &baselineManifest,
            &baselineFiles,
            &baselineContentFingerprint,
            &baselinePayloadFingerprint,
            error)) {
        return false;
    }
    QString versionsError;
    const QList<VersionInfo> savedVersions = versions(asset.assetRoot,
                                                       &versionsError);
    if (!versionsError.isEmpty()) {
        return fail(error, versionsError);
    }
    if (!savedVersions.isEmpty()) {
        Manifest comparable = baselineManifest;
        comparable.version = savedVersions.first().version;
        QString comparableStrictHash;
        if (!AssetScanner::strictContentHash(comparable,
                                             asset.assetRoot,
                                             &comparableStrictHash,
                                             error)) {
            return false;
        }
        if (comparableStrictHash
            == savedVersions.first().strictContentHash) {
            return fail(error,
                        QStringLiteral("The working copy has not changed since version %1")
                            .arg(savedVersions.first().version));
        }
    }

    const QString versionsRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    const QFileInfo internalInfo(QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips")));
    const QFileInfo existingVersionsInfo(versionsRoot);
    if ((internalInfo.exists()
         && (!internalInfo.isDir() || files::isLinkLike(internalInfo)))
        || (existingVersionsInfo.exists()
            && (!existingVersionsInfo.isDir()
                || files::isLinkLike(existingVersionsInfo)))) {
        return fail(error,
                    QStringLiteral("The versions path is invalid or linked"));
    }
    if (!QDir().mkpath(versionsRoot)) {
        return fail(error, QStringLiteral("Cannot create the versions directory"));
    }
    const QFileInfo confirmedVersionsInfo(versionsRoot);
    if (!confirmedVersionsInfo.isDir()
        || files::isLinkLike(confirmedVersionsInfo)) {
        return fail(error,
                    QStringLiteral("The versions path became invalid or linked"));
    }
    const QString targetRoot = QDir(versionsRoot).absoluteFilePath(cleanVersion);
    if (QFileInfo::exists(targetRoot)) {
        return fail(error,
                    QStringLiteral("Version already exists: %1").arg(cleanVersion));
    }
    const QString stagingName = QStringLiteral(".staging-")
                                + QUuid::createUuid().toString(
                                    QUuid::WithoutBraces);
    const QString stagingRoot = QDir(versionsRoot).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the version staging directory"));
    }

    QString operationError;
    if (!copyPayloadFiles(asset.assetRoot,
                          baselineFiles,
                          stagingRoot,
                          &operationError)
        || !verifiedCopiedPayload(stagingRoot,
                                  baselineFiles,
                                  baselinePayloadFingerprint,
                                  &operationError)) {
        return fail(error,
                    QStringLiteral("%1; unverified version staging remains at %2")
                        .arg(operationError, stagingRoot));
    }
    Manifest snapshotManifest = baselineManifest;
    snapshotManifest.version = cleanVersion;
    const QString snapshotManifestPath = QDir(stagingRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    if (!ManifestService().write(snapshotManifestPath,
                                 snapshotManifest,
                                 &operationError)) {
        return fail(error,
                    QStringLiteral("%1; version staging remains at %2")
                        .arg(operationError, stagingRoot));
    }

    VersionInfo versionInfo;
    versionInfo.schemaVersion = 2;
    versionInfo.version = cleanVersion;
    versionInfo.createdAt = QDateTime::currentDateTimeUtc();
    if (!AssetScanner::verifiedContentHash(snapshotManifest,
                                           stagingRoot,
                                           &versionInfo.contentHash,
                                           &operationError)
        || !AssetScanner::strictContentHash(snapshotManifest,
                                            stagingRoot,
                                            &versionInfo.strictContentHash,
                                            &operationError)) {
        return fail(error,
                    QStringLiteral("%1; version staging remains at %2")
                        .arg(operationError, stagingRoot));
    }
    versionInfo.path = targetRoot;
    if (!writeSnapshotMetadata(
            QDir(stagingRoot).absoluteFilePath(QStringLiteral(".snapshot.json")),
            versionInfo,
            &operationError)) {
        return fail(error,
                    QStringLiteral("%1; version staging remains at %2")
                        .arg(operationError, stagingRoot));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::VersionStagingPreparedBeforeInitialVerification,
        stagingRoot);
#endif

    SnapshotProof stagedProof;
    if (!verifySavedSnapshot(asset.assetRoot,
                             baselineManifest.id,
                             cleanVersion,
                             stagingRoot,
                             false,
                             &stagedProof,
                             &operationError)) {
        return fail(error,
                    QStringLiteral("%1; unverified version staging remains at %2")
                        .arg(operationError, stagingRoot));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::VersionStagingVerifiedBeforePublish,
        stagingRoot);
#endif

    const auto failBeforePublish = [&](const QString &message) {
        QString completedMessage = message;
        StagingRetireResult retired;
        if (!retireVerifiedSnapshotStaging(asset.assetRoot,
                                           baselineManifest.id,
                                           cleanVersion,
                                           stagedProof,
                                           &retired)) {
            completedMessage += QStringLiteral("; %1")
                                    .arg(retired.warning);
        }
        return fail(error, completedMessage);
    };

    SnapshotProof confirmedStagingProof;
    if (!verifySavedSnapshot(asset.assetRoot,
                             baselineManifest.id,
                             cleanVersion,
                             stagingRoot,
                             false,
                             &confirmedStagingProof,
                             &operationError)
        || !sameSnapshotProof(stagedProof, confirmedStagingProof)) {
        return failBeforePublish(
            QStringLiteral("The version staging data changed before publish: %1")
                .arg(operationError));
    }
    Manifest confirmedLiveManifest;
    QStringList confirmedLiveFiles;
    QString confirmedLiveContentFingerprint;
    QString confirmedLivePayloadFingerprint;
    operationError.clear();
    if (!strictNonEmptyAssetFingerprintAtRoot(
            asset.assetRoot,
            baselineManifest.id,
            &confirmedLiveManifest,
            &confirmedLiveFiles,
            &confirmedLiveContentFingerprint,
            &confirmedLivePayloadFingerprint,
            &operationError)
        || confirmedLiveFiles != baselineFiles
        || confirmedLiveContentFingerprint != baselineContentFingerprint
        || confirmedLivePayloadFingerprint != baselinePayloadFingerprint
        || json::canonicalJson(
               ManifestService().toJson(confirmedLiveManifest))
               != json::canonicalJson(
                   ManifestService().toJson(baselineManifest))) {
        return failBeforePublish(
            QStringLiteral("The working copy changed while the version was being prepared: %1")
                .arg(operationError));
    }
    if (QFileInfo::exists(targetRoot)) {
        return failBeforePublish(
            QStringLiteral("Version already exists: %1").arg(cleanVersion));
    }
    if (!QDir(versionsRoot).rename(stagingName, cleanVersion)) {
        return failBeforePublish(
            QStringLiteral("Cannot publish the version snapshot"));
    }

    SnapshotProof publishedProof;
    operationError.clear();
    if (!verifySavedSnapshot(asset.assetRoot,
                             baselineManifest.id,
                             cleanVersion,
                             targetRoot,
                             true,
                             &publishedProof,
                             &operationError)
        || publishedProof.proofFingerprint
               != stagedProof.proofFingerprint) {
        return fail(error,
                    QStringLiteral(
                        "The version snapshot was published at %1 but could not be verified: %2")
                        .arg(targetRoot, operationError));
    }

    const VersionMarkerCasResult marker = compareAndSwapVersionMarker(
        asset.assetRoot,
        baselineManifest.id,
        baselineManifest,
        cleanVersion
#ifdef XIPS_ENABLE_TEST_HOOKS
        , [this](const WorkingCopyTestPoint point,
                 const QString &path) {
              invokeWorkingCopyTestHook(point, path);
          }
#endif
        );
    versionInfo = publishedProof.info;
    versionInfo.warning = marker.warning;
    if (created) {
        *created = versionInfo;
    }
    return true;
}

bool AssetLibraryService::deleteVersion(const AssetRecord &asset,
                                        const QString &version,
                                        const RemovalMode mode,
                                        DeleteVersionResult *result,
                                        QString *error) const
{
    if (!result) {
        return fail(error,
                    QStringLiteral(
                        "Version deletion requires a result for recovery reporting"));
    }
    *result = {};
    const QString cleanVersion = version.trimmed();
    if (cleanVersion.isEmpty()) {
        return fail(error, QStringLiteral("The working copy cannot be deleted here"));
    }
    Manifest selectedManifest;
    if (!validateAssetRecord(asset, &selectedManifest, error)) {
        return false;
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

    SnapshotProof initialProof;
    if (!verifySavedSnapshot(asset.assetRoot,
                             selectedManifest.id,
                             cleanVersion,
                             found->path,
                             true,
                             &initialProof,
                             error)) {
        return false;
    }
#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteVersionVerifiedBeforeIsolation,
        initialProof.info.path);
#endif

    SnapshotProof beforeIsolation;
    QString operationError;
    if (!verifySavedSnapshot(asset.assetRoot,
                             selectedManifest.id,
                             cleanVersion,
                             initialProof.info.path,
                             true,
                             &beforeIsolation,
                             &operationError)
        || !sameSnapshotProof(initialProof, beforeIsolation)) {
        return fail(error,
                    QStringLiteral(
                        "The saved version changed before deletion and was not isolated; review data at %1: %2")
                        .arg(initialProof.info.path, operationError));
    }

    const QString versionsRoot = QDir(asset.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions"));
    const QString isolatedName = QStringLiteral(".staging-delete-")
                                 + QUuid::createUuid().toString(
                                     QUuid::WithoutBraces);
    const QString isolatedRoot = QDir(versionsRoot).absoluteFilePath(
        isolatedName);
    if (!QDir(versionsRoot).rename(cleanVersion, isolatedName)) {
        return fail(error,
                    QStringLiteral(
                        "Cannot isolate saved version %1 before deletion")
                        .arg(cleanVersion));
    }

    const auto appendRetained = [&](const QString &path,
                                    const QString &message) {
        if (!path.isEmpty()) {
            result->retainedPaths.append(files::normalizedAbsolute(path));
            result->retainedPaths.removeDuplicates();
        }
        if (!message.isEmpty()) {
            appendWarning(result->warning, message);
        }
    };
    const auto restoreIsolatedSnapshot = [&]() {
        const QString targetRoot = QDir(versionsRoot).absoluteFilePath(
            cleanVersion);
        if (!QFileInfo::exists(targetRoot)
            && QDir(versionsRoot).rename(isolatedName, cleanVersion)) {
            return true;
        }
        appendRetained(
            isolatedRoot,
            QStringLiteral(
                "The isolated saved version could not be restored without overwriting concurrent data; recovery remains at %1")
                .arg(isolatedRoot));
        return false;
    };
    const auto verifiedIsolatedSnapshot = [&](SnapshotProof *proof,
                                               QString *verificationError) {
        SnapshotProof confirmed;
        if (!verifySavedSnapshot(asset.assetRoot,
                                 selectedManifest.id,
                                 cleanVersion,
                                 isolatedRoot,
                                 false,
                                 &confirmed,
                                 verificationError)
            || !sameSnapshotContentProof(initialProof, confirmed)) {
            return false;
        }
        if (proof) {
            *proof = confirmed;
        }
        return true;
    };

    SnapshotProof isolatedProof;
    operationError.clear();
    if (!verifiedIsolatedSnapshot(&isolatedProof, &operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        return fail(error,
                    QStringLiteral(
                        "The isolated saved version could not be reverified and was not deleted; data remains at %1: %2")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteVersionIsolatedBeforeMarkerCas,
        isolatedRoot);
#endif
    operationError.clear();
    if (!verifiedIsolatedSnapshot(&isolatedProof, &operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        return fail(error,
                    QStringLiteral(
                        "The isolated saved version changed before its marker transaction and was not deleted; data remains at %1: %2")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError));
    }

    const QList<VersionInfo> remainingVersions = versions(asset.assetRoot,
                                                           &operationError);
    if (!operationError.isEmpty()) {
        const bool restored = restoreIsolatedSnapshot();
        return fail(error,
                    QStringLiteral(
                        "Remaining saved versions could not be verified; deletion was cancelled and data remains at %1: %2")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError));
    }
    const QString nextVersion = remainingVersions.isEmpty()
                                    ? QString()
                                    : remainingVersions.first().version;
    SnapshotProof nextVersionProof;
    if (!nextVersion.isEmpty()
        && !verifySavedSnapshot(asset.assetRoot,
                                selectedManifest.id,
                                nextVersion,
                                remainingVersions.first().path,
                                true,
                                &nextVersionProof,
                                &operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        return fail(error,
                    QStringLiteral(
                        "The replacement version marker target could not be proved; deletion was cancelled and data remains at %1: %2")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError));
    }
    Manifest liveManifest;
    QString liveFingerprint;
    operationError.clear();
    if (!strictFingerprintAtRoot(asset.assetRoot,
                                 selectedManifest.id,
                                 &liveManifest,
                                 &liveFingerprint,
                                 &operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        return fail(error,
                    QStringLiteral(
                        "The working-copy manifest could not be verified; deletion was cancelled and data remains at %1: %2")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError));
    }

    bool markerChangedByThisOperation = false;
    if (liveManifest.version == cleanVersion) {
        const VersionMarkerCasResult marker = compareAndSwapVersionMarker(
            asset.assetRoot,
            selectedManifest.id,
            liveManifest,
            nextVersion
#ifdef XIPS_ENABLE_TEST_HOOKS
            , [this](const WorkingCopyTestPoint point,
                     const QString &path) {
                  invokeWorkingCopyTestHook(point, path);
              }
#endif
            );
        if (!marker.retainedPath.isEmpty()) {
            appendRetained(marker.retainedPath, marker.warning);
        }
        if (!marker.published) {
            const bool restored = restoreIsolatedSnapshot();
            return fail(error,
                        QStringLiteral(
                            "The saved version marker could not be updated safely; deletion was cancelled and snapshot data remains at %1: %2")
                            .arg(restored ? initialProof.info.path
                                          : isolatedRoot,
                                 marker.warning));
        }
        markerChangedByThisOperation = true;
        result->markerUpdated = true;
    }

    const auto rollbackMarkerIfSafe = [&]() {
        if (!markerChangedByThisOperation) {
            return true;
        }
        Manifest currentManifest;
        QString currentFingerprint;
        QString rollbackError;
        if (!strictFingerprintAtRoot(asset.assetRoot,
                                     selectedManifest.id,
                                     &currentManifest,
                                     &currentFingerprint,
                                     &rollbackError)) {
            appendRetained(
                {},
                QStringLiteral(
                    "The saved version was restored, but its working-copy marker could not be verified for rollback: %1")
                    .arg(rollbackError));
            return false;
        }
        if (currentManifest.version == cleanVersion) {
            result->markerUpdated = false;
            return true;
        }
        if (currentManifest.version != nextVersion) {
            appendRetained(
                {},
                QStringLiteral(
                    "The saved version was restored, but a concurrent version-marker change was preserved"));
            return false;
        }
        const VersionMarkerCasResult rollback = compareAndSwapVersionMarker(
            asset.assetRoot,
            selectedManifest.id,
            currentManifest,
            cleanVersion
#ifdef XIPS_ENABLE_TEST_HOOKS
            , [this](const WorkingCopyTestPoint point,
                     const QString &path) {
                  invokeWorkingCopyTestHook(point, path);
              }
#endif
            );
        if (!rollback.retainedPath.isEmpty()) {
            appendRetained(rollback.retainedPath, rollback.warning);
        }
        if (!rollback.published) {
            appendRetained(
                {},
                QStringLiteral(
                    "The saved version was restored, but its version marker could not be rolled back safely: %1")
                    .arg(rollback.warning));
            return false;
        }
        result->markerUpdated = false;
        return true;
    };
    const auto deletionStateIsSafe = [&](QString *verificationError) {
        SnapshotProof confirmed;
        if (!verifiedIsolatedSnapshot(&confirmed, verificationError)) {
            return false;
        }
        const QString targetRoot = QDir(versionsRoot).absoluteFilePath(
            cleanVersion);
        if (QFileInfo::exists(targetRoot)) {
            return fail(verificationError,
                        QStringLiteral(
                            "Concurrent data appeared at the saved version path"));
        }
        Manifest confirmedLiveManifest;
        QString confirmedLiveFingerprint;
        if (!strictFingerprintAtRoot(asset.assetRoot,
                                     selectedManifest.id,
                                     &confirmedLiveManifest,
                                     &confirmedLiveFingerprint,
                                     verificationError)) {
            return false;
        }
        if (confirmedLiveManifest.version == cleanVersion) {
            return fail(verificationError,
                        QStringLiteral(
                            "The working copy points to the version being deleted"));
        }
        if (markerChangedByThisOperation) {
            if (confirmedLiveManifest.version != nextVersion) {
                return fail(
                    verificationError,
                    QStringLiteral(
                        "The working-copy version marker changed during deletion"));
            }
            if (nextVersion.isEmpty()) {
                QString remainingError;
                const QList<VersionInfo> confirmedRemaining = versions(
                    asset.assetRoot,
                    &remainingError);
                if (!remainingError.isEmpty()
                    || !confirmedRemaining.isEmpty()) {
                    return fail(
                        verificationError,
                        remainingError.isEmpty()
                            ? QStringLiteral(
                                  "The remaining saved-version set changed during deletion")
                            : remainingError);
                }
            } else {
                SnapshotProof confirmedNext;
                const QString nextRoot = QDir(asset.assetRoot).absoluteFilePath(
                    QStringLiteral(".xips/versions/%1").arg(nextVersion));
                if (!verifySavedSnapshot(asset.assetRoot,
                                         selectedManifest.id,
                                         nextVersion,
                                         nextRoot,
                                         true,
                                         &confirmedNext,
                                         verificationError)
                    || !sameSnapshotProof(nextVersionProof,
                                          confirmedNext)) {
                    return fail(
                        verificationError,
                        QStringLiteral(
                            "The replacement saved version changed during deletion"));
                }
            }
        }
        return true;
    };

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteVersionMarkerPublishedBeforeFinalProof,
        isolatedRoot);
#endif
    operationError.clear();
    if (!deletionStateIsSafe(&operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        if (restored) {
            rollbackMarkerIfSafe();
        }
        return fail(error,
                    QStringLiteral(
                        "Saved version deletion was cancelled before removal; data remains at %1: %2%3")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError,
                             result->warning.isEmpty()
                                 ? QString()
                                 : QStringLiteral("; %1")
                                       .arg(result->warning)));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::DeleteVersionVerifiedBeforeRemoval,
        isolatedRoot);
#endif
    operationError.clear();
    if (!deletionStateIsSafe(&operationError)) {
        const bool restored = restoreIsolatedSnapshot();
        if (restored) {
            rollbackMarkerIfSafe();
        }
        return fail(error,
                    QStringLiteral(
                        "Saved version deletion was cancelled at the removal boundary; data remains at %1: %2%3")
                        .arg(restored ? initialProof.info.path : isolatedRoot,
                             operationError,
                             result->warning.isEmpty()
                                 ? QString()
                                 : QStringLiteral("; %1")
                                       .arg(result->warning)));
    }

    bool removed = false;
    if (mode == RemovalMode::MoveToTrash) {
        removed = QFile::moveToTrash(isolatedRoot, &result->removedPath);
    } else {
        removed = removeExactTree(isolatedRoot,
                                  expectedSnapshotEntries(initialProof.files));
    }
    if (!removed) {
        SnapshotProof stillComplete;
        QString retainedError;
        const bool canRestore = verifiedIsolatedSnapshot(&stillComplete,
                                                         &retainedError);
        const bool restored = canRestore && restoreIsolatedSnapshot();
        if (!restored) {
            appendRetained(
                isolatedRoot,
                QStringLiteral(
                    "Saved version removal was incomplete; remaining data is retained at %1")
                    .arg(isolatedRoot));
        } else {
            rollbackMarkerIfSafe();
        }
        return fail(error,
                    mode == RemovalMode::MoveToTrash
                        ? QStringLiteral(
                              "Cannot move the saved version to the recycle bin%1")
                              .arg(result->warning.isEmpty()
                                       ? QString()
                                       : QStringLiteral("; %1")
                                             .arg(result->warning))
                        : QStringLiteral(
                              "Cannot completely delete the verified saved version%1")
                              .arg(result->warning.isEmpty()
                                       ? QString()
                                       : QStringLiteral("; %1")
                                             .arg(result->warning)));
    }
    result->snapshotRemoved = true;
    return true;
}

CopyPlan AssetLibraryService::copyPlan(const AssetRecord &asset,
                                       const QString &version) const
{
    CopyPlan plan;
    plan.version = version.trimmed();
    if (!plan.version.isEmpty()) {
        if (!validVersion(plan.version)) {
            plan.error = QStringLiteral("The saved version name is invalid");
            return plan;
        }
        Manifest liveManifest;
        QString validationError;
        if (!validateAssetRecord(asset, &liveManifest, &validationError)) {
            plan.error = validationError;
            return plan;
        }
        const QString snapshotRoot = QDir(asset.assetRoot).absoluteFilePath(
            QStringLiteral(".xips/versions/%1").arg(plan.version));
        if (!QFileInfo::exists(snapshotRoot)) {
            plan.error = QStringLiteral("Version not found: %1").arg(plan.version);
            return plan;
        }
        SnapshotProof proof;
        if (!verifySavedSnapshot(asset.assetRoot,
                                 liveManifest.id,
                                 plan.version,
                                 snapshotRoot,
                                 true,
                                 &proof,
                                 &plan.error)) {
            return plan;
        }
        plan.sourceRoot = proof.info.path;
        plan.files = proof.files;
        plan.contentHash = proof.info.contentHash;
        plan.strictContentHash = proof.info.strictContentHash;
        plan.payloadFingerprint = proof.payloadFingerprint;
        plan.proofFingerprint = proof.proofFingerprint;
    } else {
        Manifest liveManifest;
        QString contentFingerprint;
        if (!validateAssetRecord(asset, &liveManifest, &plan.error)) {
            return plan;
        }
        if (!strictNonEmptyAssetFingerprintAtRoot(asset.assetRoot,
                                                  liveManifest.id,
                                                  nullptr,
                                                  &plan.files,
                                                  &contentFingerprint,
                                                  &plan.payloadFingerprint,
                                                  &plan.error)) {
            return plan;
        }
        if (!AssetScanner::verifiedContentHash(liveManifest,
                                               asset.assetRoot,
                                               &plan.contentHash,
                                               &plan.error)) {
            return plan;
        }
        plan.sourceRoot = files::normalizedAbsolute(asset.assetRoot);
        plan.strictContentHash = contentFingerprint;
        plan.proofFingerprint = contentFingerprint;
        QString confirmedFingerprint;
        if (!strictFingerprintAtRoot(asset.assetRoot,
                                     liveManifest.id,
                                     nullptr,
                                     &confirmedFingerprint,
                                     &plan.error)
            || confirmedFingerprint != contentFingerprint) {
            plan.error = QStringLiteral(
                "The working copy changed while the copy plan was prepared");
            return plan;
        }
        if (plan.files.isEmpty()) {
            plan.error = QStringLiteral("The selected asset has no payload files");
            return plan;
        }
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

    const QString stagingPrefix = QStringLiteral(".xips-copy-");
    const QString stagingName = stagingPrefix
                                + QUuid::createUuid().toString(
                                    QUuid::WithoutBraces);
    const QString stagingRoot = QDir(parent).absoluteFilePath(stagingName);
    if (!QDir().mkpath(stagingRoot)) {
        return fail(error, QStringLiteral("Cannot create the copy staging directory"));
    }
    QString copyError;
    if (!copyPayloadFiles(plan.sourceRoot,
                          plan.files,
                          stagingRoot,
                          &copyError)) {
        return fail(error,
                    QStringLiteral("%1; unverified copy staging remains at %2")
                        .arg(copyError, stagingRoot));
    }

#ifdef XIPS_ENABLE_TEST_HOOKS
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::CopyStagingPreparedBeforeInitialVerification,
        stagingRoot);
    invokeWorkingCopyTestHook(
        WorkingCopyTestPoint::SavedVersionCopiedBeforeVerification,
        plan.sourceRoot);
#endif

    QString sourceError;
    QString stagingError;
    PayloadProof stagingProof;
    const bool sourceVerified = verifyCopyPlanSource(asset,
                                                     plan,
                                                     &sourceError);
    const bool stagingVerified = verifyPayloadProofAtRoot(
        stagingRoot,
        plan.files,
        plan.payloadFingerprint,
        &stagingProof,
        &stagingError);
    if (!sourceVerified || !stagingVerified) {
        QString completedError = QStringLiteral(
            "Copy source or staging changed or could not be verified: %1%2")
                                     .arg(sourceError,
                                          stagingError.isEmpty()
                                              ? QString()
                                              : QStringLiteral("; %1")
                                                    .arg(stagingError));
        if (stagingVerified) {
            StagingRetireResult retired;
            if (!retireVerifiedPayloadStaging(stagingProof,
                                              parent,
                                              stagingPrefix,
                                              &retired)) {
                completedError += QStringLiteral("; %1")
                                      .arg(retired.warning);
            }
        } else {
            completedError += QStringLiteral(
                "; unverified copy staging remains at %1")
                                  .arg(stagingRoot);
        }
        return fail(error, completedError);
    }

    bool published = false;
    if (plan.isSingleFile()) {
        const QString stagedFile = QDir(stagingRoot).absoluteFilePath(
            plan.files.first());
        published = QFile::rename(stagedFile, targetPath);
        if (published
            && !removeExpectedEmptyDirectories(stagingRoot,
                                               stagingProof.entries)) {
            return fail(error,
                        QStringLiteral(
                            "Copied file was published at %1, but copy staging remains at %2")
                            .arg(targetPath, stagingRoot));
        }
    } else {
        published = QDir(parent).rename(stagingName, targetName);
    }
    if (!published) {
        StagingRetireResult retired;
        if (!retireVerifiedPayloadStaging(stagingProof,
                                          parent,
                                          stagingPrefix,
                                          &retired)) {
            copyError = QStringLiteral("; %1").arg(retired.warning);
        }
        return fail(error,
                    QStringLiteral("Cannot publish the copied asset%1")
                        .arg(copyError));
    }

    bool outputVerified = false;
    if (plan.isSingleFile()) {
        outputVerified = filesEqual(
            targetPath,
            QDir(plan.sourceRoot).absoluteFilePath(plan.files.first()));
    } else {
        outputVerified = verifiedCopiedPayload(targetPath,
                                               plan.files,
                                               plan.payloadFingerprint,
                                               &copyError);
    }
    QString finalSourceError;
    const bool sourceStillVerified = verifyCopyPlanSource(asset,
                                                          plan,
                                                          &finalSourceError);
    if (!outputVerified || !sourceStillVerified) {
        return fail(
            error,
            QStringLiteral(
                "Copied data remains at %1, but final verification failed: %2%3")
                .arg(targetPath,
                     copyError,
                     finalSourceError.isEmpty()
                         ? QString()
                         : QStringLiteral("; %1").arg(finalSourceError)));
    }
    if (copiedPath) {
        *copiedPath = targetPath;
    }
    return true;
}

} // namespace xips
