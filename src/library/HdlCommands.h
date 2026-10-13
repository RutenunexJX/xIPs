#pragma once
#include "SnapshotLibrary.h"
#include <QVariantMap>

namespace xips {
struct HdlCatalogResult { QVariantList entries; QVariantList errors; };

// A source-backed facet of the existing asset library, not a separate database.
class HdlCommands {
public:
    static QString manifestName();
    static QString helperPath();
    static QByteArray workingManifest(const CatalogAsset &asset);
    static QString digest(const QByteArray &bytes);
    static HdlCatalogResult catalog(const QList<CatalogAsset> &assets, const QVariantList &previous = {});
    static SnapshotResult installBuiltins(const QString &library);
    static SnapshotResult edit(const CatalogAsset &asset, QJsonObject command, const QString &createKind,
                               const QString &expectedManifestHash);
    static SnapshotResult saveVersion(const CatalogAsset &asset);
    static SnapshotResult prepareUse(const CatalogAsset &asset, const QVariantMap &entry);
    static QVariantMap receiptDetails(const QVariantMap &entry);
};
}
