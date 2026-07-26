#pragma once

#include "assetcore/Asset.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#ifdef XIPS_ENABLE_TEST_HOOKS
#include <functional>
#endif

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
    int schemaVersion = 1;
    QString version;
    QDateTime createdAt;
    QString contentHash;
    QString strictContentHash;
    QString path;
    QString warning;
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

struct UpdatePreview {
    QStringList addedFiles;
    QStringList replacedFiles;
    QStringList removedFiles;
    qsizetype unchangedCount = 0;
    QString error;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

struct WorkingCopyUndoToken {
    QString assetId;
    QString assetRoot;
    QString recoveryPath;
    QString publishedFingerprint;
    QString recoveryFingerprint;

    [[nodiscard]] bool isValid() const
    {
        return !assetId.isEmpty() && !assetRoot.isEmpty()
               && !recoveryPath.isEmpty()
               && !publishedFingerprint.isEmpty()
               && !recoveryFingerprint.isEmpty();
    }
};

struct UpdateAssetResult {
    AssetRecord updated;
    UpdatePreview preview;
    WorkingCopyUndoToken undoToken;
    QStringList retainedPaths;
    QString warning;
    bool publishedAsIntended = false;
};

enum class RecoveryDiscardOutcome {
    Rejected,
    PendingUndoPreserved,
    TokenRetired
};

struct RecoveryDiscardResult {
    RecoveryDiscardOutcome outcome = RecoveryDiscardOutcome::Rejected;
    QString retainedPath;
    QString warning;
};

struct DeleteVersionResult {
    bool snapshotRemoved = false;
    bool markerUpdated = false;
    QString removedPath;
    QStringList retainedPaths;
    QString warning;
};

struct CopyPlan {
    QString version;
    QString sourceRoot;
    QStringList files;
    QString contentHash;
    QString strictContentHash;
    QString payloadFingerprint;
    QString proofFingerprint;
    QString suggestedName;
    QString error;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
    [[nodiscard]] bool isSingleFile() const { return files.size() == 1; }
};

enum class RemovalMode {
    MoveToTrash,
    Permanent
};

enum class WorkingCopyRecoveryMode {
    RetainForUndo,
    MoveToTrash,
    Permanent
};

enum class WorkingCopyDiscardPolicy {
    RequirePublishedCopy,
    AllowVerifiedCurrentCopy
};

#ifdef XIPS_ENABLE_TEST_HOOKS
enum class WorkingCopyTestPoint {
    StagingVerifiedBeforePublish,
    RecoveryVerifiedBeforeDiscardIsolation,
    RecoveryIsolatedBeforeLiveReverification,
    VersionStagingVerifiedBeforePublish,
    VersionStagingPreparedBeforeInitialVerification,
    SavedVersionCopiedBeforeVerification,
    CopyStagingPreparedBeforeInitialVerification,
    RestoreStagingPreparedBeforeInitialVerification,
    RestoreStagingVerifiedBeforeUpdateAsset,
    DeleteVersionVerifiedBeforeIsolation,
    DeleteVersionIsolatedBeforeMarkerCas,
    DeleteVersionMarkerPublishedBeforeFinalProof,
    DeleteVersionVerifiedBeforeRemoval
};

using WorkingCopyTestHook = std::function<void(WorkingCopyTestPoint,
                                               const QString &)>;
#endif

class AssetLibraryService {
public:
    [[nodiscard]] static AssetMetadata suggestedMetadata(
        const QString &sourcePath);
    [[nodiscard]] static QString suggestedId(const QString &text);
    [[nodiscard]] static QString suggestedNextVersion(
        const QString &currentVersion);

#ifdef XIPS_ENABLE_TEST_HOOKS
    void setWorkingCopyTestHook(WorkingCopyTestPoint point,
                                WorkingCopyTestHook hook);
#endif

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
    [[nodiscard]] UpdatePreview previewUpdate(
        const AssetRecord &asset,
        const QString &sourcePath) const;
    [[nodiscard]] UpdatePreview previewRestore(
        const AssetRecord &asset,
        const QString &version) const;
    bool updateAsset(const AssetRecord &asset,
                     const QString &sourcePath,
                     WorkingCopyRecoveryMode recoveryMode = WorkingCopyRecoveryMode::MoveToTrash,
                     UpdateAssetResult *result = nullptr,
                     QString *error = nullptr,
                     const QString &expectedCurrentHash = {},
                     const QStringList &expectedSourceFiles = {},
                     const QString &expectedSourcePayloadFingerprint = {}) const;
    bool restoreVersion(const AssetRecord &asset,
                        const QString &version,
                        WorkingCopyRecoveryMode recoveryMode = WorkingCopyRecoveryMode::MoveToTrash,
                        UpdateAssetResult *result = nullptr,
                        QString *error = nullptr) const;
    bool undoWorkingCopyChange(const AssetRecord &asset,
                               const WorkingCopyUndoToken &token,
                               UpdateAssetResult *result,
                               QString *error = nullptr,
                               RemovalMode cleanupMode = RemovalMode::MoveToTrash) const;
    bool discardWorkingCopyRecovery(const AssetRecord &asset,
                                    const WorkingCopyUndoToken &token,
                                    RemovalMode mode,
                                    RecoveryDiscardResult *result,
                                    QString *error = nullptr,
                                    WorkingCopyDiscardPolicy policy = WorkingCopyDiscardPolicy::RequirePublishedCopy) const;
    bool deleteAsset(const QString &libraryRoot,
                     const AssetRecord &asset,
                     RemovalMode mode = RemovalMode::MoveToTrash,
                     QString *removedPath = nullptr,
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
                       VersionInfo *created,
                       QString *error = nullptr) const;
    [[nodiscard]] WorkingCopyState workingCopyState(
        const AssetRecord &asset) const;
    bool deleteVersion(const AssetRecord &asset,
                       const QString &version,
                       RemovalMode mode = RemovalMode::MoveToTrash,
                       DeleteVersionResult *result = nullptr,
                       QString *error = nullptr) const;
    [[nodiscard]] CopyPlan copyPlan(const AssetRecord &asset,
                                    const QString &version) const;
    bool copyVersionPayload(const AssetRecord &asset,
                            const QString &version,
                            const QString &destinationPath,
                            QString *copiedPath = nullptr,
                            QString *error = nullptr) const;

#ifdef XIPS_ENABLE_TEST_HOOKS
private:
    void invokeWorkingCopyTestHook(WorkingCopyTestPoint point,
                                   const QString &path) const;

    mutable WorkingCopyTestPoint m_workingCopyTestPoint =
        WorkingCopyTestPoint::StagingVerifiedBeforePublish;
    mutable bool m_hasWorkingCopyTestHook = false;
    mutable WorkingCopyTestHook m_workingCopyTestHook;
#endif
};

} // namespace xips
