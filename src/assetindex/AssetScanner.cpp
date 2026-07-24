#include "assetindex/AssetScanner.h"

#include "assetcore/JsonUtil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace xips {
namespace {

bool isCancelled(const std::atomic_bool *cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
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

bool isIgnoredDirectory(const QString &name)
{
    const QString value = name.toLower();
    return value == QStringLiteral(".git")
           || value == QStringLiteral(".xips")
           || value == QStringLiteral(".cache")
           || value == QStringLiteral(".xil")
           || value == QStringLiteral("ip_user_files")
           || value == QStringLiteral("build")
           || value.startsWith(QStringLiteral("build-"))
           || value.startsWith(QStringLiteral(".xips-create-"));
}

QString normalizedAbsolute(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool pathIsWithin(const QString &path, const QString &root)
{
    const QString candidate = QDir::fromNativeSeparators(normalizedAbsolute(path));
    const QString boundary = QDir::fromNativeSeparators(normalizedAbsolute(root));
    return candidate.compare(boundary, Qt::CaseInsensitive) == 0
           || candidate.startsWith(boundary + u'/', Qt::CaseInsensitive);
}

void collectFiles(const QString &directory,
                  const QString &assetRoot,
                  QStringList &files)
{
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry)) {
            continue;
        }
        if (entry.isDir()) {
            if (!isIgnoredDirectory(entry.fileName())) {
                collectFiles(entry.absoluteFilePath(), assetRoot, files);
            }
            continue;
        }
        if (!entry.isFile() || entry.fileName() == QStringLiteral(".xips.json")) {
            continue;
        }
        QString relative = QDir(assetRoot).relativeFilePath(entry.absoluteFilePath());
        relative = QDir::cleanPath(relative);
        relative = QDir::fromNativeSeparators(relative);
        files.append(relative);
    }
}

} // namespace

ScanResult AssetScanner::scan(const QString &libraryRoot,
                              const std::atomic_bool *cancelled) const
{
    ScanResult result;
    const QFileInfo rootInfo(libraryRoot);
    if (!rootInfo.isDir()) {
        result.issues.append({
            .severity = Diagnostic::Severity::Error,
            .assetId = {},
            .path = normalizedAbsolute(libraryRoot),
            .message = QStringLiteral("Library root does not exist or is not a directory"),
        });
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
            for (const Diagnostic &entry : loaded.diagnostics) {
                result.issues.append({
                    .severity = entry.severity,
                    .assetId = {},
                    .path = manifestPath,
                    .message = entry.message,
                });
            }
            continue;
        }
        const Manifest &manifest = *loaded.manifest;
        if (firstPathById.contains(manifest.id)) {
            result.issues.append({
                .severity = Diagnostic::Severity::Error,
                .assetId = manifest.id,
                .path = manifestPath,
                .message = QStringLiteral("Duplicate IP id '%1'; first seen at %2")
                               .arg(manifest.id, firstPathById.value(manifest.id)),
            });
            continue;
        }
        firstPathById.insert(manifest.id, manifestPath);

        AssetRecord record;
        record.manifest = manifest;
        record.manifestPath = normalizedAbsolute(manifestPath);
        record.assetRoot = QFileInfo(manifestPath).absolutePath();
        record.contentHash = contentHash(manifest, record.assetRoot);
        record.lastModified = QFileInfo(manifestPath).lastModified();

        const QStringList files = assetFiles(record.assetRoot);
        record.fileCount = files.size();
        for (const QString &relative : files) {
            const QFileInfo info(QDir(record.assetRoot).absoluteFilePath(relative));
            record.totalBytes += info.size();
            if (info.lastModified() > record.lastModified) {
                record.lastModified = info.lastModified();
            }
        }

        QStringList declared = manifest.sources;
        declared.append(manifest.constraints);
        declared.append(manifest.documentation);
        declared.removeDuplicates();
        for (const QString &path : declared) {
            const QString absolute = QDir::isAbsolutePath(path)
                                         ? normalizedAbsolute(path)
                                         : normalizedAbsolute(
                                               QDir(record.assetRoot).absoluteFilePath(path));
            if (!pathIsWithin(absolute, record.assetRoot)) {
                result.issues.append({
                    .severity = Diagnostic::Severity::Error,
                    .assetId = manifest.id,
                    .path = absolute,
                    .message = QStringLiteral("Manifest path escapes the IP directory"),
                });
            } else if (!QFileInfo::exists(absolute)) {
                result.issues.append({
                    .severity = Diagnostic::Severity::Error,
                    .assetId = manifest.id,
                    .path = absolute,
                    .message = QStringLiteral("Declared file is missing"),
                });
            }
        }
        for (const Diagnostic &entry : loaded.diagnostics) {
            result.issues.append({
                .severity = entry.severity,
                .assetId = manifest.id,
                .path = manifestPath,
                .message = entry.message,
            });
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
    QStringList files;
    if (QFileInfo(assetRoot).isDir()) {
        collectFiles(normalizedAbsolute(assetRoot), normalizedAbsolute(assetRoot), files);
    }
    std::sort(files.begin(), files.end());
    files.removeDuplicates();
    return files;
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

void AssetScanner::discoverManifests(const QString &directory,
                                     QStringList &manifests,
                                     const std::atomic_bool *cancelled) const
{
    if (isCancelled(cancelled)) {
        return;
    }
    const QDir dir(directory);
    const QString localManifest = dir.absoluteFilePath(QStringLiteral(".xips.json"));
    if (QFileInfo(localManifest).isFile()) {
        manifests.append(normalizedAbsolute(localManifest));
        return;
    }

    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isCancelled(cancelled)) {
            return;
        }
        if (!isLinkLike(entry) && !isIgnoredDirectory(entry.fileName())) {
            discoverManifests(entry.absoluteFilePath(), manifests, cancelled);
        }
    }
}

} // namespace xips
