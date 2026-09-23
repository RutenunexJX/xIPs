#include "library/AssetScanner.h"

#include "assetcore/JsonUtil.h"
#include "library/FileSystemUtil.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace xips {
namespace {

bool isCancelled(const std::atomic_bool *cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

void discoverVersionStagingPaths(const QString &assetRoot,
                                 QStringList &paths)
{
    const QFileInfo manifest(
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json")));
    if (!manifest.isFile() || files::isLinkLike(manifest)) {
        return;
    }
    const QFileInfo internal(
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips")));
    if (!internal.isDir() || files::isLinkLike(internal)) {
        return;
    }
    const QFileInfo versions(
        QDir(internal.absoluteFilePath()).absoluteFilePath(
            QStringLiteral("versions")));
    if (!versions.isDir() || files::isLinkLike(versions)) {
        return;
    }
    const QFileInfoList entries = QDir(versions.absoluteFilePath()).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName().startsWith(QStringLiteral(".staging-"),
                                        Qt::CaseInsensitive)) {
            paths.append(files::normalizedAbsolute(entry.absoluteFilePath()));
        }
    }
}

void discoverUnfinishedOperationPaths(const QString &directory,
                                      QStringList &paths)
{
    discoverVersionStagingPaths(directory, paths);
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (files::isLinkLike(entry)) {
            continue;
        }
        if (entry.fileName().startsWith(QStringLiteral(".xips-create-"),
                                        Qt::CaseInsensitive)) {
            paths.append(files::normalizedAbsolute(entry.absoluteFilePath()));
            continue;
        }
        if (!files::isIgnoredDirectory(entry.fileName())) {
            discoverUnfinishedOperationPaths(entry.absoluteFilePath(), paths);
        }
    }
}

} // namespace

ScanResult AssetScanner::scan(const QString &libraryRoot,
                              const std::atomic_bool *cancelled) const
{
    ScanResult result;
    const QFileInfo rootInfo(files::normalizedAbsolute(libraryRoot));
    if (!rootInfo.isDir() || files::isLinkLike(rootInfo)) {
        result.errors.append(
            QStringLiteral("Library root does not exist, is not a directory, or is linked: %1")
                .arg(files::normalizedAbsolute(libraryRoot)));
        return result;
    }

    QStringList manifests;
    discoverManifests(rootInfo.absoluteFilePath(), manifests, cancelled);
    std::sort(manifests.begin(), manifests.end());
    QHash<QString, QList<AssetRecord>> candidatesById;
    QStringList identityOrder;
    for (const QString &manifestPath : manifests) {
        if (isCancelled(cancelled)) {
            result.cancelled = true;
            return result;
        }
        const ManifestLoadResult loaded = m_manifestService.load(manifestPath);
        if (!loaded.ok()) {
            result.errors.append(loaded.errors);
            continue;
        }
        const Manifest &manifest = *loaded.manifest;
        result.discoveredAssetIds.append(manifest.id);
        const QString identityKey = manifest.id.toCaseFolded();
        if (!candidatesById.contains(identityKey)) {
            identityOrder.append(identityKey);
        }

        AssetRecord record;
        record.manifest = manifest;
        record.manifestPath = files::normalizedAbsolute(manifestPath);
        record.assetRoot = QFileInfo(manifestPath).absolutePath();
        record.lastModified = QFileInfo(manifestPath).lastModified();
        record.files = assetFiles(record.assetRoot);
        record.fileCount = record.files.size();
        for (const QString &relative : record.files) {
            const QFileInfo info(QDir(record.assetRoot).absoluteFilePath(relative));
            record.totalBytes += info.size();
            if (info.lastModified() > record.lastModified) {
                record.lastModified = info.lastModified();
            }
        }
        candidatesById[identityKey].append(std::move(record));
    }

    for (const QString &identityKey : std::as_const(identityOrder)) {
        const QList<AssetRecord> &candidates = candidatesById[identityKey];
        if (candidates.size() == 1) {
            result.assets.append(candidates.first());
            continue;
        }
        for (const AssetRecord &candidate : candidates) {
            result.errors.append(
                QStringLiteral("%1: Duplicate asset id '%2'")
                    .arg(candidate.manifestPath,
                         candidates.first().manifest.id));
        }
    }

    std::sort(result.assets.begin(), result.assets.end(),
              [](const AssetRecord &left, const AssetRecord &right) {
                  return QString::compare(left.manifest.name,
                                          right.manifest.name,
                                          Qt::CaseInsensitive) < 0;
              });
    return result;
}

QStringList AssetScanner::assetFiles(const QString &assetRoot)
{
    QStringList result;
    files::collectPayloadFiles(assetRoot,
                               result,
                               files::LinkPolicy::Skip);
    return result;
}

QStringList AssetScanner::unfinishedOperationPaths(const QString &libraryRoot)
{
    QStringList paths;
    const QFileInfo root(files::normalizedAbsolute(libraryRoot));
    if (!root.isDir() || files::isLinkLike(root)) {
        return paths;
    }
    discoverUnfinishedOperationPaths(root.absoluteFilePath(), paths);
    std::sort(paths.begin(), paths.end());
    paths.removeDuplicates();
    return paths;
}

QString AssetScanner::contentHash(const Manifest &manifest,
                                  const QString &assetRoot)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-ip-v1\0", 11));
    hash.addData(json::canonicalJson(ManifestService().toJson(manifest)));
    for (const QString &relative : assetFiles(assetRoot)) {
        hash.addData(QByteArrayView("\0path\0", 6));
        hash.addData(relative.toUtf8());
        QFile file(QDir(assetRoot).absoluteFilePath(relative));
        if (!file.open(QIODevice::ReadOnly)) {
            hash.addData(QByteArrayView("\0missing\0", 9));
            continue;
        }
        hash.addData(QByteArrayView("\0data\0", 6));
        while (!file.atEnd()) {
            hash.addData(file.read(1024 * 1024));
        }
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

bool AssetScanner::verifiedContentHash(const Manifest &manifest,
                                       const QString &assetRoot,
                                       QString *contentHash,
                                       QString *error)
{
    if (contentHash) {
        contentHash->clear();
    }
    QStringList relativeFiles;
    if (!files::collectPayloadFiles(assetRoot,
                                    relativeFiles,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-ip-v1\0", 11));
    hash.addData(json::canonicalJson(ManifestService().toJson(manifest)));
    for (const QString &relative : relativeFiles) {
        hash.addData(QByteArrayView("\0path\0", 6));
        hash.addData(relative.toUtf8());
        QFile file(QDir(assetRoot).absoluteFilePath(relative));
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
                        QStringLiteral("Payload file changed while being verified: %1")
                            .arg(file.fileName()));
        }
    }
    QStringList confirmedFiles;
    if (!files::collectPayloadFiles(assetRoot,
                                    confirmedFiles,
                                    files::LinkPolicy::Reject,
                                    error)
        || confirmedFiles != relativeFiles) {
        return fail(error,
                    QStringLiteral(
                        "The payload file list changed while it was being verified"));
    }
    if (contentHash) {
        *contentHash = QStringLiteral("sha256:")
                       + QString::fromLatin1(hash.result().toHex());
    }
    return true;
}

bool AssetScanner::strictContentHash(const Manifest &manifest,
                                     const QString &assetRoot,
                                     QString *contentHash,
                                     QString *error)
{
    if (contentHash) {
        contentHash->clear();
    }
    QStringList relativeFiles;
    if (!files::collectPayloadFiles(assetRoot,
                                    relativeFiles,
                                    files::LinkPolicy::Reject,
                                    error)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-strict-v1\0", 15));
    const auto addSizedData = [&hash](const QByteArray &data) {
        hash.addData(QByteArray::number(
            static_cast<qlonglong>(data.size())));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(data);
    };
    addSizedData(json::canonicalJson(ManifestService().toJson(manifest)));
    for (const QString &relative : relativeFiles) {
        hash.addData(QByteArrayView("\0path\0", 6));
        addSizedData(relative.toUtf8());
        QFile file(QDir(assetRoot).absoluteFilePath(relative));
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
                        QStringLiteral("Payload file changed while being verified: %1")
                            .arg(file.fileName()));
        }
    }
    QStringList confirmedFiles;
    if (!files::collectPayloadFiles(assetRoot,
                                    confirmedFiles,
                                    files::LinkPolicy::Reject,
                                    error)
        || confirmedFiles != relativeFiles) {
        return fail(error,
                    QStringLiteral(
                        "The payload file list changed while it was being verified"));
    }
    if (contentHash) {
        *contentHash = QStringLiteral("sha256:")
                       + QString::fromLatin1(hash.result().toHex());
    }
    return true;
}

void AssetScanner::discoverManifests(
    const QString &directory,
    QStringList &manifests,
    const std::atomic_bool *cancelled) const
{
    if (isCancelled(cancelled)) {
        return;
    }
    const QDir dir(directory);
    const QString localManifest = dir.absoluteFilePath(
        QStringLiteral(".xips.json"));
    if (QFileInfo(localManifest).isFile()) {
        manifests.append(files::normalizedAbsolute(localManifest));
        return;
    }

    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isCancelled(cancelled)) {
            return;
        }
        if (!files::isLinkLike(entry)
            && !files::isIgnoredDirectory(entry.fileName())) {
            discoverManifests(entry.absoluteFilePath(), manifests, cancelled);
        }
    }
}

} // namespace xips
