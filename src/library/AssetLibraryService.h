#pragma once

#include "assetcore/Asset.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace xips {

struct AssetMetadata {
    QString id;
    QString name;
    QString description;
    QStringList tags;
};

struct ImportAssetRequest {
    QString libraryRoot;
    QString sourcePath;
    AssetMetadata metadata;
};

struct VersionInfo {
    QString version;
    QDateTime createdAt;
    QString contentHash;
    QString path;
};

class AssetLibraryService {
public:
    [[nodiscard]] static AssetMetadata suggestedMetadata(
        const QString &sourcePath);
    [[nodiscard]] static QString suggestedId(const QString &text);

    bool importAsset(const ImportAssetRequest &request,
                     AssetRecord *created = nullptr,
                     QString *error = nullptr) const;
    bool updateMetadata(const AssetRecord &asset,
                        const AssetMetadata &metadata,
                        QString *error = nullptr) const;

    [[nodiscard]] QList<VersionInfo> versions(
        const QString &assetRoot,
        QString *error = nullptr) const;
    bool createVersion(const AssetRecord &asset,
                       const QString &version,
                       VersionInfo *created = nullptr,
                       QString *error = nullptr) const;
    bool exportVersion(const AssetRecord &asset,
                       const QString &version,
                       const QString &destination,
                       QString *error = nullptr) const;
};

} // namespace xips
