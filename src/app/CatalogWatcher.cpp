#include "CatalogWatcher.h"
#include "library/FileSystemUtil.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <algorithm>
#include <functional>
#include <utility>
#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#include <array>
#include <cstddef>
#endif

namespace xips
{
namespace
{
QString watchPath(const QString &path)
{
    auto result = QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}
bool transient(const QString &name)
{
    return name == ".xips-library.lock" || name.startsWith(".pending-") || name.startsWith(".xips-");
}
}
#ifdef Q_OS_WIN
// Qt's FindFirstChangeNotification backend can block directory renames on
// Windows (QTBUG-65683). Overlapped reads with delete sharing let editors and
// sync clients replace/move paths while this monitor is active.
class WinDirectoryMonitor final : public QObject
{
  public:
    using Callback = std::function<void(const QStringList &, bool)>;
    WinDirectoryMonitor(const QString &path, bool recursive, Callback callback, QObject *parent)
        : QObject(parent), m_path(path), m_recursive(recursive), m_callback(std::move(callback))
    {
        const auto native = QDir::toNativeSeparators(path);
        m_directory = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (m_directory == INVALID_HANDLE_VALUE) return;
        m_io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_io.hEvent) return;
        m_notifier = new QWinEventNotifier(m_io.hEvent, this);
        connect(m_notifier, &QWinEventNotifier::activated, this, [this] { completed(); });
        arm();
    }
    ~WinDirectoryMonitor() override
    {
        if (m_notifier) m_notifier->setEnabled(false);
        if (m_directory != INVALID_HANDLE_VALUE)
        {
            if (m_pending)
            {
                CancelIoEx(m_directory, &m_io);
                DWORD bytes = 0;
                GetOverlappedResult(m_directory, &m_io, &bytes, TRUE);
            }
            CloseHandle(m_directory);
        }
        if (m_io.hEvent) CloseHandle(m_io.hEvent);
    }
    bool active() const { return m_pending; }
    bool recursive() const { return m_recursive; }
  private:
    bool arm()
    {
        ResetEvent(m_io.hEvent);
        m_pending = ReadDirectoryChangesW(m_directory, m_buffer.data(), DWORD(m_buffer.size()), m_recursive,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES, nullptr, &m_io, nullptr);
        m_notifier->setEnabled(m_pending);
        return m_pending;
    }
    void completed()
    {
        m_notifier->setEnabled(false);
        DWORD bytes = 0;
        const bool ok = GetOverlappedResult(m_directory, &m_io, &bytes, FALSE);
        m_pending = false;
        QStringList paths;
        bool rescan = !ok || bytes == 0;
        constexpr auto header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        for (DWORD offset = 0; ok && bytes && offset < bytes;)
        {
            if (bytes - offset < header) { rescan = true; break; }
            const auto *entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(m_buffer.data() + offset);
            if (entry->FileNameLength > bytes - offset - header) { rescan = true; break; }
            paths.append(watchPath(QDir(m_path).filePath(QString::fromWCharArray(entry->FileName,
                int(entry->FileNameLength / sizeof(WCHAR))))));
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset < header || entry->NextEntryOffset > bytes - offset)
            { rescan = true; break; }
            offset += entry->NextEntryOffset;
        }
        if (!arm()) rescan = true;
        m_callback(paths, rescan);
    }
    QString m_path;
    bool m_recursive = false, m_pending = false;
    Callback m_callback;
    HANDLE m_directory = INVALID_HANDLE_VALUE;
    OVERLAPPED m_io{};
    alignas(DWORD) std::array<unsigned char, 32768> m_buffer{};
    QWinEventNotifier *m_notifier = nullptr;
};
#endif
QString CatalogWatcher::directoryStamp(const QString &path)
{
    if (!QFileInfo(path).isDir()) return QStringLiteral("missing");
    auto names = QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    const bool metadata = QFileInfo(path).fileName() == ".xips";
    names.removeIf([&](const QString &name) { return transient(name) ||
        (name == "objects" && metadata); });
    return QString::fromLatin1(QCryptographicHash::hash(names.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex());
}
void CatalogWatcher::add(Plan &plan, const QString &path, const QString &owner, bool recursive)
{
    const QFileInfo info(path);
    if (files::isLinkLike(info)) return;
    auto &entry = plan[watchPath(path)];
    entry.owners.insert(owner);
    entry.directory = info.isDir();
    entry.recursive |= recursive;
    if (entry.stamp.isEmpty()) entry.stamp = entry.directory ? directoryStamp(info.absoluteFilePath()) : fileStamp(info.absoluteFilePath());
}
QString CatalogWatcher::fileStamp(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? QString::number(info.size()) + ':' + QString::number(info.lastModified().toMSecsSinceEpoch())
                         : QStringLiteral("missing");
}
CatalogWatcher::Plan CatalogWatcher::assetPlan(const CatalogAsset &asset)
{
    Plan paths;
    const auto owner = asset.id;
    const auto addParent = [&](const QString &path)
    {
        QString parent = QFileInfo(path).absolutePath();
        while (!QFileInfo(parent).isDir() && QFileInfo(parent).absolutePath() != parent)
            parent = QFileInfo(parent).absolutePath();
        add(paths, parent, owner);
    };
    auto history = asset.historyRoot.isEmpty() ? asset.root : asset.historyRoot;
    if (!asset.referencePath.isEmpty() && !asset.referenceRecord.value("definition").toString().isEmpty())
        history = QDir(QFileInfo(asset.referencePath).absolutePath()).absoluteFilePath(asset.referenceRecord.value("definition").toString());
    addParent(history);
    add(paths, history, owner, true);
    add(paths, QDir(history).filePath(".xips.json"), owner);
    add(paths, QDir(history).filePath(".xips"), owner);
    for (const auto &directory : {".xips/revisions", ".xips/deleted-revisions"})
    {
        const auto path = QDir(history).filePath(directory);
        add(paths, path, owner);
        for (const auto &file : QDir(path).entryInfoList({"*.json"}, QDir::Files)) add(paths, file.absoluteFilePath(), owner);
    }
    if (!asset.referencePath.isEmpty())
    {
        add(paths, asset.referencePath, owner);
        return paths;
    }
    addParent(asset.root);
    const auto source = asset.sourceIsDirectory || !asset.discovered ? asset.root : QFileInfo(asset.root).absolutePath();
    add(paths, source, owner, true);
    const bool managed = asset.document.value("workingArea") == "managed";
    std::function<void(const QString &)> visit = [&](const QString &directory)
    {
        const QFileInfo info(directory);
        if (!info.isDir() || files::isLinkLike(info)) return;
        add(paths, directory, owner);
        auto filters = QDir::Dirs | QDir::NoDotAndDotDot;
        if (managed) filters |= QDir::Hidden | QDir::System;
        for (const auto &entry : QDir(directory).entryInfoList(filters))
        {
            const auto name = entry.fileName();
            const auto lower = name.toLower();
            const bool included = managed
                ? lower != ".git" && lower != ".xips" && !lower.startsWith(".xips-")
                : !name.startsWith('.') && !files::isIgnoredDirectory(name);
            if (included) visit(entry.absoluteFilePath());
        }
    };
    if (asset.sourceIsDirectory || asset.legacy) visit(source);
    else add(paths, source, owner);
    for (const auto &file : asset.workingFiles) add(paths, QDir(source).filePath(file), owner);
    return paths;
}
CatalogWatcher::Plan CatalogWatcher::plan(const QString &library, const QList<CatalogAsset> &assets)
{
    Plan result;
    if (library.isEmpty()) return result;
    QSet<QString> roots, histories;
    for (const auto &asset : assets)
    {
        roots.insert(QFileInfo(asset.root).absoluteFilePath());
        if (!asset.historyRoot.isEmpty()) histories.insert(QFileInfo(asset.historyRoot).absoluteFilePath());
        const auto paths = assetPlan(asset);
        for (auto it = paths.cbegin(); it != paths.cend(); ++it)
        {
            auto &entry = result[it.key()];
            entry.owners.unite(it->owners); entry.directory = it->directory; entry.stamp = it->stamp;
            entry.recursive |= it->recursive;
        }
    }
    // Watch unregistered directory structure as well, to discover newly synced
    // definitions. Known working trees are already covered by their asset owner.
    std::function<void(const QString &)> visit = [&](const QString &directory)
    {
        const QFileInfo info(directory);
        if (!info.isDir() || files::isLinkLike(info)) return;
        add(result, directory, {});
        add(result, QDir(directory).filePath(".xips.json"), {});
        for (const auto &entry : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!entry.fileName().startsWith('.') && !files::isIgnoredDirectory(entry.fileName()) &&
                !roots.contains(entry.absoluteFilePath())) visit(entry.absoluteFilePath());
    };
    QString parent = library;
    while (!QFileInfo(parent).isDir() && QFileInfo(parent).absolutePath() != parent) parent = QFileInfo(parent).absolutePath();
    add(result, parent, {});
    add(result, QFileInfo(parent).absolutePath(), {});
    add(result, library, {}, true);
    visit(library);
    for (const auto &relative : {".xips", ".xips/assets", ".xips/groups", ".xips/references"})
    {
        const auto directory = QDir(library).filePath(relative);
        add(result, directory, {});
        for (const auto &file : QDir(directory).entryInfoList({"*.json"}, QDir::Files)) add(result, file.absoluteFilePath(), {});
    }
    for (const auto &entry : QDir(QDir(library).filePath(".xips/assets")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!histories.contains(entry.absoluteFilePath()) && !files::isLinkLike(entry))
        {
            add(result, entry.absoluteFilePath(), {});
            add(result, QDir(entry.absoluteFilePath()).filePath(".xips.json"), {});
        }
    return result;
}
CatalogWatcher::CatalogWatcher(QObject *parent) : QObject(parent)
{
    setObjectName("catalogWatcher");
    m_settle.setSingleShot(true); m_settle.setInterval(700);
    connect(&m_settle, &QTimer::timeout, this, &CatalogWatcher::flush);
    m_subscribe.setInterval(0);
    connect(&m_subscribe, &QTimer::timeout, this, &CatalogWatcher::subscribe);
    const auto changed = [this](const QString &path) { m_changed.insert(path); m_settle.start(); };
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
}
CatalogWatcher::~CatalogWatcher()
{
    clear();
}
void CatalogWatcher::clear()
{
    ++m_context; m_lastCheck.invalidate();
    m_settle.stop(); m_subscribe.stop(); m_changed.clear(); m_ids.clear(); m_full = false;
    m_additions.clear(); m_plan.clear();
#ifdef Q_OS_WIN
    qDeleteAll(m_native); m_native.clear(); m_directories.clear();
#endif
    const auto paths = m_watcher.directories() + m_watcher.files();
    if (!paths.isEmpty()) m_watcher.removePaths(paths);
}
void CatalogWatcher::install(Plan plan)
{
    for (const auto &path : m_changed)
        if (plan.contains(path) && m_plan.contains(path)) plan[path].stamp = m_plan[path].stamp;
    m_plan = std::move(plan);
#ifdef Q_OS_WIN
    m_directories.clear();
    QStringList candidates;
    for (auto it = m_plan.cbegin(); it != m_plan.cend(); ++it)
        if (it->directory && it->stamp != "missing") candidates.append(it.key());
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.size() < b.size(); });
    QStringList recursiveRoots;
    for (const auto &path : candidates)
    {
        if (std::any_of(recursiveRoots.cbegin(), recursiveRoots.cend(), [&](const auto &root)
            { return path.startsWith(root.endsWith('/') ? root : root + '/'); })) continue;
        const bool recursive = m_plan[path].recursive;
        m_directories.insert(path, recursive);
        if (recursive) recursiveRoots.append(path);
    }
    for (auto it = m_native.begin(); it != m_native.end();)
        if (!m_directories.contains(it.key()) || m_directories[it.key()] != it.value()->recursive() || !it.value()->active())
        { delete it.value(); it = m_native.erase(it); } else ++it;
    m_additions.clear();
    for (auto it = m_directories.cbegin(); it != m_directories.cend(); ++it)
        if (!m_native.contains(it.key())) m_additions.append(it.key());
#else
    const auto watched = m_watcher.directories() + m_watcher.files();
    QSet<QString> present(watched.cbegin(), watched.cend());
    QStringList removed;
    for (const auto &path : present) if (!m_plan.contains(path)) removed.append(path);
    if (!removed.isEmpty()) m_watcher.removePaths(removed);
    m_additions.clear();
    for (auto it = m_plan.cbegin(); it != m_plan.cend(); ++it)
        if (!present.contains(it.key()) && it->stamp != "missing") m_additions.append(it.key());
    std::sort(m_additions.begin(), m_additions.end(), [this](const auto &a, const auto &b)
    { return m_plan[a].directory > m_plan[b].directory; });
#endif
    if (!m_additions.isEmpty()) m_subscribe.start();
}
void CatalogWatcher::updateAsset(const QString &id, const Plan &plan)
{
    updateAssets({{id, plan}});
}
void CatalogWatcher::updateAssets(const QHash<QString, Plan> &plans)
{
    auto next = m_plan;
    for (auto it = next.begin(); it != next.end();)
    {
        it->owners.removeIf([&](const QString &id) { return plans.contains(id); });
        if (it->owners.isEmpty()) it = next.erase(it); else ++it;
    }
    for (const auto &plan : plans)
        for (auto it = plan.cbegin(); it != plan.cend(); ++it)
        {
            auto &entry = next[it.key()];
            entry.owners.unite(it->owners); entry.directory = it->directory; entry.stamp = it->stamp;
            entry.recursive |= it->recursive;
        }
    install(std::move(next));
}
void CatalogWatcher::subscribe()
{
    const auto batch = m_additions.mid(0, 128);
    m_additions.remove(0, batch.size());
#ifdef Q_OS_WIN
    for (const auto &path : batch)
    {
        auto *monitor = new WinDirectoryMonitor(path, m_directories.value(path), [this](const QStringList &paths, bool rescan)
        {
            if (rescan) queue(true);
            for (const auto &path : paths)
                for (const auto &candidate : {path, watchPath(QFileInfo(path).absolutePath())})
                    if (m_plan.contains(candidate)) m_changed.insert(candidate);
            if (!m_changed.isEmpty()) m_settle.start();
        }, this);
        if (monitor->active()) m_native.insert(path, monitor); else delete monitor;
    }
#else
    if (!batch.isEmpty()) m_watcher.addPaths(batch);
#endif
    if (m_additions.isEmpty()) m_subscribe.stop();
}
void CatalogWatcher::queue(bool full, const QStringList &ids)
{
    m_full |= full;
    m_ids.unite(QSet<QString>(ids.cbegin(), ids.cend()));
    m_settle.start();
}
void CatalogWatcher::checkNow()
{
    if (m_checking || m_plan.isEmpty() || (m_lastCheck.isValid() && m_lastCheck.elapsed() < 1500)) return;
    m_lastCheck.start(); m_checking = true;
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, context = m_context]
    {
        const auto changed = watcher->result(); watcher->deleteLater(); m_checking = false;
        if (context != m_context) return;
        for (const auto &path : changed) if (m_plan.contains(path)) m_changed.insert(path);
        if (!m_changed.isEmpty()) m_settle.start();
    });
    watcher->setFuture(QtConcurrent::run([plan = m_plan]
    {
        QStringList changed;
        for (auto it = plan.cbegin(); it != plan.cend(); ++it)
            if ((it->directory ? directoryStamp(it.key()) : fileStamp(it.key())) != it->stamp) changed.append(it.key());
        return changed;
    }));
}
void CatalogWatcher::flush()
{
    const auto changed = std::exchange(m_changed, {});
#ifndef Q_OS_WIN
    const auto files = m_watcher.files();
    const QSet<QString> watchedFiles(files.cbegin(), files.cend());
    QSet<QString> scheduled(m_additions.cbegin(), m_additions.cend());
#endif
    for (const auto &path : changed)
    {
        auto it = m_plan.find(path);
        if (it == m_plan.end()) continue;
        if (it->directory)
        {
            const auto stamp = directoryStamp(path);
            if (stamp == it->stamp) continue;
            it->stamp = stamp;
        }
        // Atomic replacement drops a file watch; restore it for the next edit.
        else
        {
            it->stamp = fileStamp(path);
#ifndef Q_OS_WIN
            if (it->stamp != "missing" && !watchedFiles.contains(path) && !scheduled.contains(path))
            { m_additions.append(path); scheduled.insert(path); }
#endif
        }
        m_full |= it->owners.contains(QString());
        m_ids.unite(it->owners);
    }
    m_ids.remove(QString());
    if (!m_additions.isEmpty()) m_subscribe.start();
    const bool full = std::exchange(m_full, false);
    const auto ids = std::exchange(m_ids, {}).values();
    if (full || !ids.isEmpty()) emit refreshRequested(full, ids);
}
}
