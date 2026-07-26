#pragma once

#include "assetcore/Asset.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

namespace xips {

class LibraryController final : public QObject {
    Q_OBJECT

public:
    explicit LibraryController(QObject *parent = nullptr);
    ~LibraryController() override;

    void setLibraryRoot(const QString &path);

    void rebuild();
    [[nodiscard]] QList<SearchHit> search(const QString &query,
                                          int limit = 1000) const;
    [[nodiscard]] const QList<AssetRecord> &assets() const;

signals:
    void refreshStarted();
    void refreshFinished(const QList<AssetRecord> &assets,
                         const QStringList &errors);
    void refreshFailed(const QString &message);

private:
    void cancel();

    QString m_libraryRoot;
    QList<AssetRecord> m_assets;
    QFutureWatcher<ScanResult> m_watcher;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    bool m_rebuildQueued = false;
};

} // namespace xips
