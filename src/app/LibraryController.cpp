#include "app/LibraryController.h"

#include "assetcore/JsonUtil.h"
#include "library/AssetScanner.h"
#include "library/FileSystemUtil.h"

#include <QtConcurrent>

#include <algorithm>

namespace xips {
namespace {

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
    connect(&m_watcher, &QFutureWatcher<ScanResult>::finished, this, [this] {
        const ScanResult result = m_watcher.result();
        if (m_rebuildQueued) {
            m_rebuildQueued = false;
            rebuild();
            return;
        }
        if (result.cancelled) {
            emit refreshFailed(QStringLiteral("Library refresh cancelled"));
            return;
        }
        m_assets = result.assets;
        emit refreshFinished(m_assets, result.errors);
    });
}

LibraryController::~LibraryController()
{
    cancel();
    m_watcher.waitForFinished();
}

void LibraryController::setLibraryRoot(const QString &path)
{
    m_libraryRoot = files::normalizedAbsolute(path);
}

void LibraryController::rebuild()
{
    if (m_watcher.isRunning()) {
        cancel();
        m_rebuildQueued = true;
        return;
    }
    if (m_libraryRoot.isEmpty()) {
        emit refreshFailed(QStringLiteral("No asset library is selected"));
        return;
    }
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const QString root = m_libraryRoot;
    emit refreshStarted();
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
        QStringList fields{
            asset.manifest.name,
            asset.manifest.id,
            asset.manifest.version,
            asset.manifest.tags.join(u' '),
            asset.manifest.description,
        };
        fields.append(asset.files);
        bool matches = true;
        for (const QString &term : terms) {
            bool termMatched = false;
            for (const QString &value : fields) {
                if (json::normalizeSearchText(value).contains(term)) {
                    termMatched = true;
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
        double score = 0.0;
        if (!normalizedQuery.isEmpty()) {
            score += fieldScore(asset.manifest.name, normalizedQuery, 100, 80, 55);
            score += fieldScore(asset.manifest.id, normalizedQuery, 95, 75, 50);
            score += fieldScore(asset.manifest.tags.join(u' '), normalizedQuery, 45, 40, 35);
            score += fieldScore(asset.manifest.version, normalizedQuery, 30, 25, 20);
            score += fieldScore(asset.manifest.description, normalizedQuery, 20, 15, 10);
        }
        QString matchedFile;
        double matchedFileScore = 0.0;
        if (!normalizedQuery.isEmpty()) {
            for (const QString &file : asset.files) {
                const double fileScore = fieldScore(file,
                                                    normalizedQuery,
                                                    50,
                                                    40,
                                                    30);
                if (fileScore > matchedFileScore) {
                    matchedFileScore = fileScore;
                    matchedFile = file;
                }
            }
        }
        score += matchedFileScore;
        hits.append({.asset = asset,
                     .score = score,
                     .matchedFile = matchedFile});
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

const QList<AssetRecord> &LibraryController::assets() const
{
    return m_assets;
}

} // namespace xips
