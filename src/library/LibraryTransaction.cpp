#include "LibraryTransaction.h"
#include "ContentStore.h"
#include "FileSystemUtil.h"
#include <QDir>
#include <QHash>
#include <QLockFile>
#include <stdexcept>
namespace xips
{
namespace
{
thread_local QHash<QString, int> depths;
}
LibraryTransaction::LibraryTransaction(const QString &library)
{
    const auto root = files::normalizedAbsolute(library);
    ContentStore::safePath(root);
    if (!QFileInfo(root).isDir())
        throw std::runtime_error("Library directory unavailable");
    key = root;
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    if (depths.value(key) > 0)
    {
        ++depths[key];
        return;
    }
    const auto file = QDir(root).filePath(".xips-library.lock");
    ContentStore::safePath(file);
    lock = std::make_unique<QLockFile>(file);
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        throw std::runtime_error("Another library operation is running. Try again shortly.");
    depths.insert(key, 1);
}
LibraryTransaction::~LibraryTransaction()
{
    if (depths.contains(key) && --depths[key] == 0)
        depths.remove(key);
}
} // namespace xips
