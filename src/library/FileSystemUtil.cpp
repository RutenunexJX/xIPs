#include "library/FileSystemUtil.h"

#include <QDir>

#include <algorithm>

namespace xips::files {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool collect(const QString &directory,
             const QString &root,
             QStringList &relativeFiles,
             const LinkPolicy linkPolicy,
             QString *error)
{
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry)) {
            if (linkPolicy == LinkPolicy::Reject) {
                return fail(error,
                            QStringLiteral("Linked files or directories are not portable: %1")
                                .arg(entry.absoluteFilePath()));
            }
            continue;
        }
        if (entry.isDir()) {
            if (!isIgnoredDirectory(entry.fileName())
                && !collect(entry.absoluteFilePath(),
                            root,
                            relativeFiles,
                            linkPolicy,
                            error)) {
                return false;
            }
            continue;
        }
        if (!entry.isFile()
            || entry.fileName() == QStringLiteral(".xips.json")
            || entry.fileName() == QStringLiteral(".snapshot.json")) {
            continue;
        }
        QString relative = QDir(root).relativeFilePath(entry.absoluteFilePath());
        relativeFiles.append(QDir::fromNativeSeparators(QDir::cleanPath(relative)));
    }
    return true;
}

} // namespace

QString normalizedAbsolute(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isWithin(const QString &path, const QString &root)
{
    const QString candidate = QDir::fromNativeSeparators(normalizedAbsolute(path));
    const QString boundary = QDir::fromNativeSeparators(normalizedAbsolute(root));
    const Qt::CaseSensitivity sensitivity = pathCaseSensitivity();
    return candidate.compare(boundary, sensitivity) == 0
           || candidate.startsWith(boundary + u'/', sensitivity);
}

bool isIgnoredDirectory(const QString &name)
{
    const QString value = name.toLower();
    return value == QStringLiteral(".git")
           || value == QStringLiteral(".xips")
           || value == QStringLiteral(".cache")
           || value == QStringLiteral(".xil")
           || value == QStringLiteral("ip_user_files")
           || value == QStringLiteral("build")
           || value.startsWith(QStringLiteral("build-"))
           || value.startsWith(QStringLiteral(".xips-create-"));
}

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink()) {
        return true;
    }
#ifdef Q_OS_WIN
    return info.isJunction();
#else
    return false;
#endif
}

bool collectPayloadFiles(const QString &root,
                         QStringList &relativeFiles,
                         const LinkPolicy linkPolicy,
                         QString *error)
{
    relativeFiles.clear();
    const QString absoluteRoot = normalizedAbsolute(root);
    const QFileInfo rootInfo(absoluteRoot);
    if (!rootInfo.isDir()) {
        return fail(error, QStringLiteral("Payload root does not exist"));
    }
    if (linkPolicy == LinkPolicy::Reject && isLinkLike(rootInfo)) {
        return fail(error,
                    QStringLiteral("Payload root cannot be a linked directory: %1")
                        .arg(absoluteRoot));
    }
    if (!collect(absoluteRoot,
                 absoluteRoot,
                 relativeFiles,
                 linkPolicy,
                 error)) {
        return false;
    }
    std::sort(relativeFiles.begin(), relativeFiles.end());
    relativeFiles.removeDuplicates();
    return true;
}

} // namespace xips::files
