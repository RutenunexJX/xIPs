#pragma once

#include "assetcore/Asset.h"

#include <QList>
#include <QMap>
#include <QString>

namespace xips {

enum class DifferenceCategory {
    File,
    Manifest,
    Semantic,
    Dependency
};

enum class DifferenceKind {
    Added,
    Removed,
    Modified
};

QString differenceCategoryToString(DifferenceCategory category);
QString differenceKindToString(DifferenceKind kind);

struct DifferenceEntry {
    DifferenceCategory category = DifferenceCategory::File;
    DifferenceKind kind = DifferenceKind::Modified;
    QString key;
    QString before;
    QString after;
};

struct AssetDifference {
    QString beforeId;
    QString afterId;
    QString beforeContentHash;
    QString afterContentHash;
    QList<DifferenceEntry> entries;

    [[nodiscard]] bool identical() const;
    [[nodiscard]] QMap<DifferenceCategory, int> counts() const;
};

class DiffService {
public:
    [[nodiscard]] AssetDifference compare(const AssetRecord &before,
                                          const AssetRecord &after) const;
    [[nodiscard]] static QMap<QString, QString> fileHashes(const AssetRecord &asset);
};

} // namespace xips
