#pragma once

#include "assetcore/Asset.h"

#include <QFutureWatcher>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

namespace xips {

class LibraryController final : public QObject {
    Q_OBJECT

public:
    explicit LibraryController(QString indexPath, QObject *parent = nullptr);
    ~LibraryController() override;

    void setRoots(QList<LibraryRoot> roots);
    [[nodiscard]] QList<LibraryRoot> roots() const;
    [[nodiscard]] QString indexPath() const;

    void rebuild();
    void cancel();
    QList<SearchHit> search(const QString &query,
                            AssetType type = AssetType::Unknown,
                            int limit = 500,
                            QString *error = nullptr) const;
    bool markUsed(const QString &assetId,
                  const QDateTime &when,
                  QString *error = nullptr) const;

signals:
    void indexingStarted();
    void incrementalRefreshStarted(const QStringList &assetIds);
    void indexingFinished(const QList<AssetRecord> &assets,
                          const QList<ScanIssue> &issues,
                          qint64 generation);
    void indexingFailed(const QString &message);

private:
    struct RebuildResult {
        ScanResult scan;
        qint64 generation = -1;
        QString error;
    };

    void configureWatchers(const QList<AssetRecord> &assets);
    void queueChangedPath(const QString &path);
    void refreshPendingAssets();

    QString m_indexPath;
    QList<LibraryRoot> m_roots;
    QFutureWatcher<RebuildResult> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_refreshTimer;
    QHash<QString, AssetRecord> m_assetsById;
    QHash<QString, QSet<QString>> m_watchedPathAssets;
    QSet<QString> m_libraryRootPaths;
    QSet<QString> m_pendingAssetIds;
    bool m_fullRebuildQueued = false;
};

} // namespace xips
