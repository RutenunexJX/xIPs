#include "assetindex/AssetScanner.h"

#include "assetcore/JsonUtil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    const QString normalized = name.toLower();
    return normalized == QStringLiteral(".git") || normalized == QStringLiteral(".xips")
           || normalized == QStringLiteral(".cache") || normalized == QStringLiteral("build")
           || normalized.startsWith(QStringLiteral("build-"))
           || normalized.startsWith(QStringLiteral(".xips-create-"));
}

QString resolvedPath(const QString &assetRoot, const QString &path)
{
    if (QDir::isAbsolutePath(path)) {
        return QDir::cleanPath(path);
    }
    return QDir(assetRoot).absoluteFilePath(path);
}

void collectDirectoryFiles(const QString &directory, QStringList &files)
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
                collectDirectoryFiles(entry.absoluteFilePath(), files);
            }
        } else if (entry.isFile()) {
            files.append(entry.absoluteFilePath());
        }
    }
}

QStringList declaredFiles(const Manifest &manifest, const QString &assetRoot)
{
    QStringList paths = manifest.sources;
    paths.append(manifest.constraints);
    paths.append(manifest.tests);
    paths.append(manifest.examples);
    paths.append(manifest.documentation);
    for (const QString &includeDirectory : manifest.includeDirs) {
        const QString absoluteDirectory = resolvedPath(assetRoot, includeDirectory);
        QStringList includeFiles;
        if (QFileInfo(absoluteDirectory).isDir()) {
            collectDirectoryFiles(absoluteDirectory, includeFiles);
        }
        for (const QString &includeFile : includeFiles) {
            if (QDir::isAbsolutePath(includeDirectory)) {
                paths.append(includeFile);
            } else {
                QString relative = QDir(assetRoot).relativeFilePath(includeFile);
                relative = QDir::cleanPath(relative);
                relative.replace(u'\\', u'/');
                paths.append(relative);
            }
        }
    }
    paths.removeDuplicates();
    std::sort(paths.begin(), paths.end());
    return paths;
}

} // namespace

ScanResult AssetScanner::scan(const QList<LibraryRoot> &roots,
                              const std::atomic_bool *cancelled) const
{
    ScanResult result;
    QHash<QString, QString> firstManifestById;

    for (const LibraryRoot &root : roots) {
        if (isCancelled(cancelled)) {
            result.cancelled = true;
            return result;
        }

        const QFileInfo rootInfo(root.path);
        if (!rootInfo.exists() || !rootInfo.isDir()) {
            result.issues.append(ScanIssue{
                .severity = Diagnostic::Severity::Error,
                .path = root.path,
                .message = QStringLiteral("Library root does not exist or is not a directory"),
            });
            continue;
        }

        QStringList manifests;
        discoverManifests(rootInfo.absoluteFilePath(), manifests, cancelled);
        std::sort(manifests.begin(), manifests.end());

        for (const QString &manifestPath : manifests) {
            if (isCancelled(cancelled)) {
                result.cancelled = true;
                return result;
            }

            const ManifestLoadResult loadResult = m_manifestService.load(manifestPath);
            if (!loadResult.ok()) {
                for (const Diagnostic &diagnostic : loadResult.diagnostics) {
                    result.issues.append(ScanIssue{
                        .severity = diagnostic.severity,
                        .path = manifestPath,
                        .message = diagnostic.message,
                    });
                }
                continue;
            }

            const Manifest &manifest = *loadResult.manifest;
            if (firstManifestById.contains(manifest.id)) {
                const QString firstPath = firstManifestById.value(manifest.id);
                result.issues.append(ScanIssue{
                    .severity = Diagnostic::Severity::Error,
                    .assetId = manifest.id,
                    .path = manifestPath,
                    .message = QStringLiteral("Duplicate asset id '%1'; first seen at %2")
                                   .arg(manifest.id, firstPath),
                });
                continue;
            }
            firstManifestById.insert(manifest.id, manifestPath);

            AssetRecord record;
            record.manifest = manifest;
            record.assetRoot = QFileInfo(manifestPath).absolutePath();
            record.manifestPath = QFileInfo(manifestPath).absoluteFilePath();
            record.origin = root.origin;
            record.contentHash = contentHash(manifest, record.assetRoot);
            record.sourceRepository =
                manifest.rawObject.value(QStringLiteral("sourceRepository")).toString();
            if (record.sourceRepository.isEmpty()) {
                record.sourceRepository = rootInfo.absoluteFilePath();
            }
            record.lastUsed = QDateTime::fromString(
                manifest.rawObject.value(QStringLiteral("lastUsed")).toString(),
                Qt::ISODateWithMs);

            bool hasMissingFiles = false;
            for (const QString &source : manifest.sources) {
                const QString absolutePath = resolvedPath(record.assetRoot, source);
                if (!QFileInfo::exists(absolutePath)) {
                    hasMissingFiles = true;
                    result.issues.append(ScanIssue{
                        .severity = Diagnostic::Severity::Error,
                        .assetId = manifest.id,
                        .path = absolutePath,
                        .message = QStringLiteral("Declared source file is missing"),
                    });
                }
            }
            for (const QString &includeDirectory : manifest.includeDirs) {
                const QString absolutePath =
                    resolvedPath(record.assetRoot, includeDirectory);
                if (!QFileInfo(absolutePath).isDir()) {
                    hasMissingFiles = true;
                    result.issues.append(ScanIssue{
                        .severity = Diagnostic::Severity::Error,
                        .assetId = manifest.id,
                        .path = absolutePath,
                        .message = QStringLiteral("Declared include directory is missing"),
                    });
                }
            }
            record.diagnosticsStatus =
                hasMissingFiles ? QStringLiteral("manifest-error")
                                : QStringLiteral("not-analyzed");
            result.assets.append(record);

            for (const Diagnostic &diagnostic : loadResult.diagnostics) {
                result.issues.append(ScanIssue{
                    .severity = diagnostic.severity,
                    .assetId = manifest.id,
                    .path = manifestPath,
                    .message = diagnostic.message,
                });
            }
        }
    }
    return result;
}

QString AssetScanner::contentHash(const Manifest &manifest, const QString &assetRoot)
{
    ManifestService service;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("xips-content-v1\0", 16));
    hash.addData(json::canonicalJson(service.toJson(manifest)));

    for (const QString &declaredPath : declaredFiles(manifest, assetRoot)) {
        hash.addData(QByteArrayView("\0path\0", 6));
        hash.addData(declaredPath.toUtf8());

        QFile file(resolvedPath(assetRoot, declaredPath));
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
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isCancelled(cancelled)) {
            return;
        }
        if (isLinkLike(entry)) {
            continue;
        }
        if (entry.isDir()) {
            if (!isIgnoredDirectory(entry.fileName())) {
                discoverManifests(entry.absoluteFilePath(), manifests, cancelled);
            }
            continue;
        }
        if (entry.isFile() && entry.fileName() == QStringLiteral(".xips.json")) {
            manifests.append(entry.absoluteFilePath());
        }
    }
}

} // namespace xips
