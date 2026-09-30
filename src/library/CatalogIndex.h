#pragma once

#include "SnapshotLibrary.h"
#include <QSet>
#include <optional>

namespace xips
{
// Disposable local search index; JSON manifests and source files remain authoritative.
class CatalogIndex
{
  public:
    static QString path(const QString &library);
    static QString generation(const QList<CatalogAsset> &assets);
    static bool rebuild(const QString &library, const QList<CatalogAsset> &assets);
    static std::optional<QSet<QString>> matchingRoots(const QString &library,
                                                    const QString &generation,
                                                    const QString &category,
                                                    const QStringList &terms);
    static QString searchText(const CatalogAsset &asset);
    static bool matches(const CatalogAsset &asset, const QStringList &terms);
    static QStringList queryTerms(const QString &query, QString *error = nullptr);
};
}
