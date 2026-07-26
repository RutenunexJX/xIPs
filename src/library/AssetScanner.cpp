#include "library/AssetScanner.h"

#include "assetcore/JsonUtil.h"
#include "library/FileSystemUtil.h"

#include <QCryptographicHash>
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

} // namespace

ScanResult AssetScanner::scan(const QString &libraryRoot,
                              const std::atomic_bool *cancelled) const
{
    ScanResult result;
    const QFileInfo rootInfo(libraryRoot);
    if (!rootInfo.isDir()) {
        result.errors.append(
            QStringLiteral("Library root does not exist or is not a directory: %1")
                .arg(files::normalizedAbsolute(libraryRoot)));
        return result;
    }

    QStringList manifests;
    discoverManifests(rootInfo.absoluteFilePath(), manifests, cancelled);
    std::sort(manifests.begin(), manifests.end());
    QHash<QString, QString> firstPathById;
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
        if (firstPathById.contains(manifest.id)) {
            result.errors.append(
                QStringLiteral("Duplicate asset id '%1': %2 and %3")
                    .arg(manifest.id,
                         firstPathById.value(manifest.id),
                         manifestPath));
            continue;
        }
        firstPathById.insert(manifest.id, manifestPath);

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
        result.assets.append(record);
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
