#include "app/LibraryController.h"

#include "assetindex/AssetIndex.h"
#include "assetindex/AssetScanner.h"
#include "gitservice/GitService.h"
#include "semantic/SlangService.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QtConcurrent>

#include <algorithm>

namespace xips {
namespace {

QString normalizedAbsolute(const QString &path)
{
    return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
}

QString declaredAbsolute(const AssetRecord &asset, const QString &path)
{
    return QDir::isAbsolutePath(path)
               ? normalizedAbsolute(path)
               : normalizedAbsolute(QDir(asset.assetRoot).absoluteFilePath(path));
}

void appendWatchedTree(const QString &root,
                       const QString &assetId,
                       QHash<QString, QSet<QString>> &mapping,
                       QStringList &files,
                       QStringList &directories)
{
    if (!QFileInfo(root).isDir()) {
        return;
    }
    const QString normalizedRoot = normalizedAbsolute(root);
    mapping[normalizedRoot].insert(assetId);
    directories.append(normalizedRoot);
    QDirIterator iterator(
        normalizedRoot,
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            continue;
        }
        const QString normalized = normalizedAbsolute(info.absoluteFilePath());
        mapping[normalized].insert(assetId);
        if (info.isDir()) {
            directories.append(normalized);
        } else if (info.isFile()) {
            files.append(normalized);
        }
    }
}

} // namespace

LibraryController::LibraryController(QString indexPath, QObject *parent)
    : QObject(parent)
    , m_indexPath(std::move(indexPath))
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(250);
    connect(&m_refreshTimer,
            &QTimer::timeout,
            this,
            &LibraryController::refreshPendingAssets);
    connect(&m_fileWatcher,
            &QFileSystemWatcher::fileChanged,
            this,
            &LibraryController::queueChangedPath);
    connect(&m_fileWatcher,
            &QFileSystemWatcher::directoryChanged,
            this,
            &LibraryController::queueChangedPath);
    connect(&m_watcher, &QFutureWatcher<RebuildResult>::finished, this, [this] {
        const RebuildResult result = m_watcher.result();
        if (m_fullRebuildQueued) {
            m_fullRebuildQueued = false;
            rebuild();
            return;
        }
        if (!result.error.isEmpty()) {
            emit indexingFailed(result.error);
            return;
        }
        configureWatchers(result.scan.assets);
        emit indexingFinished(result.scan.assets, result.scan.issues, result.generation);
        if (!m_pendingAssetIds.isEmpty()) {
            m_refreshTimer.start();
        }
    });
}

LibraryController::~LibraryController()
{
    cancel();
    m_watcher.waitForFinished();
}

void LibraryController::setRoots(QList<LibraryRoot> roots)
{
    m_roots = std::move(roots);
}

QList<LibraryRoot> LibraryController::roots() const
{
    return m_roots;
}

QString LibraryController::indexPath() const
{
    return m_indexPath;
}

void LibraryController::rebuild()
{
    if (m_watcher.isRunning()) {
        cancel();
        m_fullRebuildQueued = true;
        return;
    }
    m_pendingAssetIds.clear();
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = m_cancelled;
    const QList<LibraryRoot> rootsSnapshot = m_roots;
    const QString indexPathSnapshot = m_indexPath;

    emit indexingStarted();
    m_watcher.setFuture(QtConcurrent::run(
        [cancellation, rootsSnapshot, indexPathSnapshot]() -> RebuildResult {
            RebuildResult result;
            AssetIndex index(indexPathSnapshot);
            result.generation = index.reserveGeneration(&result.error);
            if (result.generation < 0) {
                return result;
            }

            AssetScanner scanner;
            result.scan = scanner.scan(rootsSnapshot, cancellation.get());
            if (result.scan.cancelled) {
                result.error = QStringLiteral("Indexing cancelled");
                return result;
            }

            struct RootGitInfo {
                QString rootPath;
                GitInfo info;
            };
            QList<RootGitInfo> gitRoots;
            GitService git;
            for (const LibraryRoot &root : rootsSnapshot) {
                if (cancellation->load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Indexing cancelled");
                    return result;
                }
                gitRoots.append(RootGitInfo{
                    .rootPath = QFileInfo(root.path).absoluteFilePath(),
                    .info = git.query(root.path, 3000, cancellation.get()),
                });
            }
            for (AssetRecord &record : result.scan.assets) {
                record.generation = result.generation;
                qsizetype bestLength = -1;
                const RootGitInfo *best = nullptr;
                const QString assetRoot = QFileInfo(record.assetRoot).absoluteFilePath();
                for (const RootGitInfo &root : gitRoots) {
                    const QString normalizedAssetRoot =
                        QDir::fromNativeSeparators(assetRoot);
                    const QString normalizedRoot =
                        QDir::fromNativeSeparators(root.rootPath);
                    const QString prefix = normalizedRoot + u'/';
                    if ((normalizedAssetRoot.compare(normalizedRoot,
                                                     Qt::CaseInsensitive)
                             == 0
                         || normalizedAssetRoot.startsWith(prefix,
                                                           Qt::CaseInsensitive))
                        && normalizedRoot.size() > bestLength) {
                        bestLength = normalizedRoot.size();
                        best = &root;
                    }
                }
                if (best && best->info.available) {
                    record.sourceRepository = best->info.repositoryRoot;
                    record.gitCommit = best->info.commit;
                    record.gitTag = best->info.tag;
                    record.modifiedStatus =
                        best->info.dirty ? QStringLiteral("modified")
                                         : QStringLiteral("clean");
                } else {
                    record.modifiedStatus = QStringLiteral("not-versioned");
                }
            }
            if (!index.rebuild(result.scan.assets, result.generation, &result.error)) {
                return result;
            }

            QList<AssetRecord> indexedAssets = index.allAssets(&result.error);
            if (!result.error.isEmpty()) {
                return result;
            }
            SlangService slang;
            for (const AssetRecord &asset : indexedAssets) {
                if (cancellation->load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Indexing cancelled");
                    return result;
                }
                if (!SlangService::supports(asset)) {
                    continue;
                }
                const bool needsAnalysis =
                    asset.stale || asset.semantic.contentHash != asset.contentHash
                    || asset.semantic.engineVersion.isEmpty();
                if (!needsAnalysis) {
                    continue;
                }

                const SemanticResult semantic = slang.analyze(
                    SlangService::Request{
                        .asset = asset,
                        .generation = result.generation,
                    },
                    cancellation.get());
                if (cancellation->load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Indexing cancelled");
                    return result;
                }
                const SemanticPublishStatus status =
                    index.publishSemantic(asset.manifest.id,
                                          asset.contentHash,
                                          result.generation,
                                          semantic,
                                          &result.error);
                if (status == SemanticPublishStatus::Error) {
                    return result;
                }
            }
            result.scan.assets = index.allAssets(&result.error);
            return result;
        }));
}

void LibraryController::cancel()
{
    if (m_cancelled) {
        m_cancelled->store(true, std::memory_order_relaxed);
    }
}

void LibraryController::configureWatchers(const QList<AssetRecord> &assets)
{
    const QStringList existingFiles = m_fileWatcher.files();
    const QStringList existingDirectories = m_fileWatcher.directories();
    if (!existingFiles.isEmpty()) {
        m_fileWatcher.removePaths(existingFiles);
    }
    if (!existingDirectories.isEmpty()) {
        m_fileWatcher.removePaths(existingDirectories);
    }
    m_assetsById.clear();
    m_watchedPathAssets.clear();
    m_libraryRootPaths.clear();

    QStringList files;
    QStringList directories;
    for (const LibraryRoot &root : m_roots) {
        const QString path = normalizedAbsolute(root.path);
        if (QFileInfo(path).isDir()) {
            m_libraryRootPaths.insert(path);
            directories.append(path);
        }
    }
    for (const AssetRecord &asset : assets) {
        m_assetsById.insert(asset.manifest.id, asset);
        const QString assetRoot = normalizedAbsolute(asset.assetRoot);
        m_watchedPathAssets[assetRoot].insert(asset.manifest.id);
        directories.append(assetRoot);

        QStringList declared = asset.manifest.sources;
        declared.append(asset.manifest.constraints);
        declared.append(asset.manifest.tests);
        declared.append(asset.manifest.examples);
        declared.append(asset.manifest.documentation);
        declared.append(asset.manifestPath);
        for (const QString &path : declared) {
            const QString absolute = path == asset.manifestPath
                                         ? normalizedAbsolute(path)
                                         : declaredAbsolute(asset, path);
            if (QFileInfo(absolute).isFile()) {
                m_watchedPathAssets[absolute].insert(asset.manifest.id);
                files.append(absolute);
            }
        }
        for (const QString &includeDirectory : asset.manifest.includeDirs) {
            appendWatchedTree(declaredAbsolute(asset, includeDirectory),
                              asset.manifest.id,
                              m_watchedPathAssets,
                              files,
                              directories);
        }
    }
    files.removeDuplicates();
    directories.removeDuplicates();
    if (!files.isEmpty()) {
        m_fileWatcher.addPaths(files);
    }
    if (!directories.isEmpty()) {
        m_fileWatcher.addPaths(directories);
    }
}

void LibraryController::queueChangedPath(const QString &path)
{
    const QString normalized = normalizedAbsolute(path);
    const QSet<QString> assets = m_watchedPathAssets.value(normalized);
    if (!assets.isEmpty()) {
        m_pendingAssetIds.unite(assets);
        m_refreshTimer.start();
        return;
    }
    if (m_libraryRootPaths.contains(normalized)) {
        if (m_watcher.isRunning()) {
            m_fullRebuildQueued = true;
            cancel();
        } else {
            rebuild();
        }
    }
}

void LibraryController::refreshPendingAssets()
{
    if (m_pendingAssetIds.isEmpty()) {
        return;
    }
    if (m_watcher.isRunning()) {
        m_refreshTimer.start();
        return;
    }

    const QSet<QString> requested = m_pendingAssetIds;
    m_pendingAssetIds.subtract(requested);
    QList<AssetRecord> oldAssets;
    QStringList ids(requested.begin(), requested.end());
    std::sort(ids.begin(), ids.end());
    for (const QString &id : ids) {
        if (m_assetsById.contains(id)) {
            oldAssets.append(m_assetsById.value(id));
        }
    }
    if (oldAssets.isEmpty()) {
        rebuild();
        return;
    }

    const QString indexPath = m_indexPath;
    const QHash<QString, AssetRecord> catalogSnapshot = m_assetsById;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = m_cancelled;
    emit incrementalRefreshStarted(ids);
    m_watcher.setFuture(QtConcurrent::run(
        [oldAssets, indexPath, catalogSnapshot, cancellation]() -> RebuildResult {
            RebuildResult result;
            AssetIndex index(indexPath);
            result.generation = index.reserveGeneration(&result.error);
            if (result.generation < 0) {
                return result;
            }

            QList<AssetRecord> changed;
            QStringList removedIds;
            QHash<QString, AssetRecord> previousById = catalogSnapshot;
            AssetScanner scanner;
            GitService git;
            for (const AssetRecord &old : oldAssets) {
                if (cancellation->load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Indexing cancelled");
                    return result;
                }
                if (!QFileInfo(old.manifestPath).isFile()) {
                    removedIds.append(old.manifest.id);
                    continue;
                }
                const ScanResult scan = scanner.scan(
                    {LibraryRoot{
                        .path = old.assetRoot,
                        .origin = old.origin,
                    }},
                    cancellation.get());
                result.scan.issues.append(scan.issues);
                const auto iterator = std::find_if(
                    scan.assets.cbegin(),
                    scan.assets.cend(),
                    [&old](const AssetRecord &candidate) {
                        return normalizedAbsolute(candidate.manifestPath)
                               == normalizedAbsolute(old.manifestPath);
                    });
                if (iterator == scan.assets.cend()) {
                    removedIds.append(old.manifest.id);
                    continue;
                }
                AssetRecord current = *iterator;
                if (current.manifest.id != old.manifest.id
                    && previousById.contains(current.manifest.id)) {
                    result.error =
                        QStringLiteral("Incremental refresh found duplicate asset ID '%1'")
                            .arg(current.manifest.id);
                    return result;
                }
                if (current.manifest.id != old.manifest.id) {
                    removedIds.append(old.manifest.id);
                }
                current.generation = result.generation;
                const GitInfo gitInfo =
                    git.query(current.assetRoot, 3000, cancellation.get());
                if (gitInfo.available) {
                    current.sourceRepository = gitInfo.repositoryRoot;
                    current.gitCommit = gitInfo.commit;
                    current.gitTag = gitInfo.tag;
                    current.modifiedStatus =
                        gitInfo.dirty ? QStringLiteral("modified")
                                      : QStringLiteral("clean");
                } else {
                    current.modifiedStatus = QStringLiteral("not-versioned");
                }
                changed.append(current);
            }

            if (!removedIds.isEmpty()
                && !index.removeAssets(removedIds, result.generation, &result.error)) {
                return result;
            }
            if (!changed.isEmpty()
                && !index.updateAssets(changed, result.generation, &result.error)) {
                return result;
            }

            SlangService slang;
            for (const AssetRecord &asset : changed) {
                const AssetRecord previous =
                    previousById.value(asset.manifest.id);
                const bool needsAnalysis =
                    SlangService::supports(asset)
                    && (previous.contentHash != asset.contentHash
                        || previous.stale || !previous.semantic.available);
                if (!needsAnalysis) {
                    continue;
                }
                const SemanticResult semantic = slang.analyze(
                    SlangService::Request{
                        .asset = asset,
                        .generation = result.generation,
                    },
                    cancellation.get());
                if (cancellation->load(std::memory_order_relaxed)) {
                    result.error = QStringLiteral("Indexing cancelled");
                    return result;
                }
                const SemanticPublishStatus status =
                    index.publishSemantic(asset.manifest.id,
                                          asset.contentHash,
                                          result.generation,
                                          semantic,
                                          &result.error);
                if (status == SemanticPublishStatus::Error) {
                    return result;
                }
            }
            result.scan.assets = index.allAssets(&result.error);
            return result;
        }));
}

QList<SearchHit> LibraryController::search(const QString &query,
                                           const AssetType type,
                                           const int limit,
                                           QString *error) const
{
    return AssetIndex(m_indexPath).search(query, type, limit, error);
}

bool LibraryController::markUsed(const QString &assetId,
                                 const QDateTime &when,
                                 QString *error) const
{
    return AssetIndex(m_indexPath).markUsed(assetId, when, error);
}

} // namespace xips
