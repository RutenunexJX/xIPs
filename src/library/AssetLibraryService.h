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

struct ImportBatchResult {
    QList<AssetRecord> created;
    QStringList errors;
};

struct WorkingCopyState {
    bool hasSavedVersion = false;
    bool changed = false;
    QString latestVersion;
    QString error;
};

enum class VersionDeleteMode {
    MoveToTrash,
    Permanent
};

class AssetLibraryService {
public:
    [[nodiscard]] static AssetMetadata suggestedMetadata(
        const QString &sourcePath);
    [[nodiscard]] static QString suggestedId(const QString &text);
    [[nodiscard]] static QString suggestedNextVersion(
        const QString &currentVersion);

    bool importAsset(const ImportAssetRequest &request,
                     AssetRecord *created = nullptr,
                     QString *error = nullptr) const;
    [[nodiscard]] ImportBatchResult importAssets(
        const QString &libraryRoot,
        const QStringList &sourcePaths,
        const QStringList &groups = {}) const;
    bool updateMetadata(const AssetRecord &asset,
                        const AssetMetadata &metadata,
                        QString *error = nullptr) const;
    bool changeGroupMembership(const QList<AssetRecord> &assets,
                               const QString &oldGroup,
                               const QString &newGroup,
                               int *changed = nullptr,
                               QString *error = nullptr) const;

    [[nodiscard]] QList<VersionInfo> versions(
        const QString &assetRoot,
        QString *error = nullptr) const;
    bool createVersion(const AssetRecord &asset,
                       const QString &version,
                       VersionInfo *created = nullptr,
                       QString *error = nullptr) const;
    [[nodiscard]] WorkingCopyState workingCopyState(
        const AssetRecord &asset) const;
    bool deleteVersion(const AssetRecord &asset,
                       const QString &version,
                       VersionDeleteMode mode = VersionDeleteMode::MoveToTrash,
                       QString *error = nullptr) const;
    bool copyVersionPayload(const AssetRecord &asset,
                            const QString &version,
                            const QString &destinationDirectory,
                            QString *copiedPath = nullptr,
                            QString *error = nullptr) const;
};

} // namespace xips
