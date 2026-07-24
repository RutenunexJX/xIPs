#pragma once

#include "assetcore/Asset.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

namespace xips {

class LibraryController final : public QObject {
    Q_OBJECT

public:
    explicit LibraryController(QObject *parent = nullptr);
    ~LibraryController() override;

    void setLibraryRoot(const QString &path);
    [[nodiscard]] QString libraryRoot() const;
    [[nodiscard]] QList<AssetRecord> assets() const;

    void rebuild();
    void cancel();
    [[nodiscard]] QList<SearchHit> search(const QString &query,
                                          int limit = 1000) const;

signals:
    void indexingStarted();
    void indexingFinished(const QList<AssetRecord> &assets,
                          const QList<ScanIssue> &issues);
    void indexingFailed(const QString &message);

private:
    void configureWatchers();
    void scheduleRefresh();

    QString m_libraryRoot;
    QList<AssetRecord> m_assets;
    QFutureWatcher<ScanResult> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_refreshTimer;
    bool m_rebuildQueued = false;
};

} // namespace xips
