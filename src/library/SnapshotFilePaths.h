#pragma once

#include <QDir>
#include <QSet>
#include <QStringList>

namespace xips::detail
{
// Keep persisted paths portable, independently of the host filesystem's casing rules.
inline QString snapshotFilePathsError(const QStringList &paths)
{
    QSet<QString> seen;
    for (const auto &path : paths)
    {
        const auto key = path.toCaseFolded();
        if (path.isEmpty() || QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path ||
            path.startsWith("../") || path == ".." || path == "." || path.contains('\\') ||
            path.contains(':') || seen.contains(key))
            return QStringLiteral("Invalid revision file path: %1").arg(path);
        seen.insert(key);
    }
    return {};
}
} // namespace xips::detail
