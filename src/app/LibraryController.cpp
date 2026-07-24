#include "app/LibraryController.h"

#include "assetcore/JsonUtil.h"
#include "assetindex/AssetScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QtConcurrent>

#include <algorithm>

namespace xips {
namespace {

QString absolutePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

double fieldScore(const QString &field,
                  const QString &query,
                  const double exact,
                  const double prefix,
                  const double contains)
{
    const QString normalized = json::normalizeSearchText(field);
    if (normalized == query) {
        return exact;
    }
    if (normalized.startsWith(query)) {
        return prefix;
    }
    return normalized.contains(query) ? contains : 0.0;
}

} // namespace

LibraryController::LibraryController(QObject *parent)
    : QObject(parent)
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(350);
    connect(&m_refreshTimer, &QTimer::timeout, this, &LibraryController::rebuild);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, &LibraryController::scheduleRefresh);
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged,
            this, &LibraryController::scheduleRefresh);
    connect(&m_watcher, &QFutureWatcher<ScanResult>::finished, this, [this] {
        const ScanResult result = m_watcher.result();
        if (m_rebuildQueued) {
            m_rebuildQueued = false;
            rebuild();
            return;
        }
        if (result.cancelled) {
            emit indexingFailed(QStringLiteral("Library refresh cancelled"));
            return;
        }
        m_assets = result.assets;
        configureWatchers();
        emit indexingFinished(m_assets, result.issues);
    });
}

LibraryController::~LibraryController()
{
    cancel();
    m_watcher.waitForFinished();
}

void LibraryController::setLibraryRoot(const QString &path)
{
    m_libraryRoot = absolutePath(path);
}

QString LibraryController::libraryRoot() const
{
    return m_libraryRoot;
}

QList<AssetRecord> LibraryController::assets() const
{
    return m_assets;
}

void LibraryController::rebuild()
{
    if (m_watcher.isRunning()) {
        cancel();
        m_rebuildQueued = true;
        return;
    }
    if (m_libraryRoot.isEmpty()) {
        emit indexingFailed(QStringLiteral("No IP library is selected"));
        return;
    }
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const QString root = m_libraryRoot;
    emit indexingStarted();
    m_watcher.setFuture(QtConcurrent::run([root, cancelled] {
        return AssetScanner().scan(root, cancelled.get());
    }));
}

void LibraryController::cancel()
{
    if (m_cancelled) {
        m_cancelled->store(true, std::memory_order_relaxed);
    }
}

QList<SearchHit> LibraryController::search(const QString &query,
                                           const int limit) const
{
    const QString normalizedQuery = json::normalizeSearchText(query);
    const QStringList terms = normalizedQuery.split(u' ', Qt::SkipEmptyParts);
    QList<SearchHit> hits;
    for (const AssetRecord &asset : m_assets) {
        const QList<QPair<QString, QString>> fields{
            {QStringLiteral("name"), asset.manifest.name},
            {QStringLiteral("id"), asset.manifest.id},
            {QStringLiteral("version"), asset.manifest.version},
            {QStringLiteral("tags"), asset.manifest.tags.join(u' ')},
            {QStringLiteral("description"), asset.manifest.description},
            {QStringLiteral("path"), asset.assetRoot},
        };
        bool matches = true;
        QStringList matchedFields;
        for (const QString &term : terms) {
            bool termMatched = false;
            for (const auto &[name, value] : fields) {
                if (json::normalizeSearchText(value).contains(term)) {
                    termMatched = true;
                    matchedFields.append(name);
                }
            }
            if (!termMatched) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        matchedFields.removeDuplicates();
        double score = 0.0;
        if (!normalizedQuery.isEmpty()) {
            score += fieldScore(asset.manifest.name, normalizedQuery, 100, 80, 55);
            score += fieldScore(asset.manifest.id, normalizedQuery, 95, 75, 50);
            score += fieldScore(asset.manifest.tags.join(u' '), normalizedQuery, 45, 40, 35);
            score += fieldScore(asset.manifest.version, normalizedQuery, 30, 25, 20);
            score += fieldScore(asset.manifest.description, normalizedQuery, 20, 15, 10);
            score += fieldScore(asset.assetRoot, normalizedQuery, 10, 8, 5);
        }
        hits.append({.asset = asset,
                     .matchedFields = matchedFields,
                     .score = score});
    }
    std::sort(hits.begin(), hits.end(), [](const SearchHit &left, const SearchHit &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return QString::compare(left.asset.manifest.name,
                                right.asset.manifest.name,
                                Qt::CaseInsensitive) < 0;
    });
    if (limit >= 0 && hits.size() > limit) {
        hits.erase(hits.begin() + limit, hits.end());
    }
    return hits;
}

void LibraryController::configureWatchers()
{
    if (!m_fileWatcher.files().isEmpty()) {
        m_fileWatcher.removePaths(m_fileWatcher.files());
    }
    if (!m_fileWatcher.directories().isEmpty()) {
        m_fileWatcher.removePaths(m_fileWatcher.directories());
    }

    QStringList directories;
    QStringList files;
    if (QFileInfo(m_libraryRoot).isDir()) {
        directories.append(m_libraryRoot);
    }
    for (const AssetRecord &asset : m_assets) {
        directories.append(asset.assetRoot);
        files.append(asset.manifestPath);
        for (const QString &relative : AssetScanner::assetFiles(asset.assetRoot)) {
            files.append(QDir(asset.assetRoot).absoluteFilePath(relative));
        }
        const QString metadata = QDir(asset.assetRoot).absoluteFilePath(
            QStringLiteral(".xips"));
        const QString versions = QDir(metadata).absoluteFilePath(
            QStringLiteral("versions"));
        if (QFileInfo(metadata).isDir()) {
            directories.append(metadata);
        }
        if (QFileInfo(versions).isDir()) {
            directories.append(versions);
        }
    }
    directories.removeDuplicates();
    files.removeDuplicates();
    if (!directories.isEmpty()) {
        m_fileWatcher.addPaths(directories);
    }
    if (!files.isEmpty()) {
        m_fileWatcher.addPaths(files);
    }
}

void LibraryController::scheduleRefresh()
{
    m_refreshTimer.start();
}

} // namespace xips
