#pragma once
#include "library/SnapshotLibrary.h"
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>

namespace xips
{
#ifdef Q_OS_WIN
class WinDirectoryMonitor;
#endif
// Detects/coalesces filesystem changes; catalog loading stays in BrowserPanel's
// existing worker pipeline. An empty owner denotes catalog structure/groups.
class CatalogWatcher final : public QObject
{
    Q_OBJECT
  public:
    struct Path { QSet<QString> owners; bool directory = false; QString stamp; bool recursive = false; };
    using Plan = QHash<QString, Path>;
    explicit CatalogWatcher(QObject *parent = nullptr);
    ~CatalogWatcher() override;
    static Plan plan(const QString &library, const QList<CatalogAsset> &assets);
    static Plan assetPlan(const CatalogAsset &asset);
    void clear();
    void install(Plan plan);
    void updateAsset(const QString &id, const Plan &plan);
    void updateAssets(const QHash<QString, Plan> &plans);
    void queue(bool full, const QStringList &ids = {});
    void checkNow();
  signals:
    void refreshRequested(bool full, const QStringList &ids);
  private:
    static QString directoryStamp(const QString &path);
    static QString fileStamp(const QString &path);
    static void add(Plan &plan, const QString &path, const QString &owner, bool recursive = false);
    void flush();
    void subscribe();
    QFileSystemWatcher m_watcher;
#ifdef Q_OS_WIN
    QHash<QString, WinDirectoryMonitor *> m_native;
    QHash<QString, bool> m_directories;
#endif
    QTimer m_settle, m_subscribe;
    Plan m_plan;
    QSet<QString> m_changed, m_ids;
    bool m_full = false;
    QStringList m_additions;
    QElapsedTimer m_lastCheck;
    int m_context = 0;
    bool m_checking = false;
};
}
