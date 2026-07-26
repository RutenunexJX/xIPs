#pragma once

#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace xips::files {

enum class LinkPolicy {
    Skip,
    Reject
};

[[nodiscard]] QString normalizedAbsolute(const QString &path);
[[nodiscard]] bool isWithin(const QString &path, const QString &root);
[[nodiscard]] bool isIgnoredDirectory(const QString &name);
[[nodiscard]] bool isLinkLike(const QFileInfo &info);
bool collectPayloadFiles(const QString &root,
                         QStringList &relativeFiles,
                         LinkPolicy linkPolicy,
                         QString *error = nullptr);

} // namespace xips::files
