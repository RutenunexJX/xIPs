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
    static bool updateAsset(const QString &library, const CatalogAsset &asset,
                            const QString &expectedGeneration, const QString &generation);
    static bool updateAssets(const QString &library, const QList<CatalogAsset> &assets,
                             const QString &expectedGeneration, const QString &generation);
    static std::optional<QSet<QString>> matchingRoots(const QString &library,
                                                    const QString &generation,
                                                    const QString &category,
                                                    const QStringList &terms);
    static QString searchText(const CatalogAsset &asset);
    static bool matches(const CatalogAsset &asset, const QStringList &terms);
    static bool matches(const CatalogAsset &asset, const QStringList &terms, const QString &searchText);
    static QStringList queryTerms(const QString &query, QString *error = nullptr);
};
}
