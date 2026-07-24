#pragma once

#include "assetcore/Asset.h"
#include "dependency/DependencyResolver.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <atomic>

namespace xips {

enum class ImportMode {
    Reference,
    Vendor
};

enum class PlannedFileAction {
    Add,
    Overwrite,
    Remove,
    Conflict,
    Skip
};

QString plannedFileActionToString(PlannedFileAction action);

struct PlannedFile {
    QString assetId;
    QString sourcePath;
    QString destinationPath;
    QString sourceHash;
    QString existingHash;
    QString previouslyOwnedHash;
    PlannedFileAction action = PlannedFileAction::Add;
};

struct ImportIssue {
    Diagnostic::Severity severity = Diagnostic::Severity::Info;
    QString message;
    QString path;
};

struct ImportPlan {
    ImportMode mode = ImportMode::Reference;
    QString targetRoot;
    QString outputPath;
    DependencyResolution dependencies;
    QList<PlannedFile> files;
    QList<ImportIssue> issues;
    QJsonObject outputDocument;
    bool outputDocumentExisted = false;
    QString expectedOutputHash;

    [[nodiscard]] bool hasErrors() const;
    [[nodiscard]] bool hasConflicts() const;
    [[nodiscard]] bool canExecute() const;
    [[nodiscard]] QMap<PlannedFileAction, int> actionCounts() const;
};

struct ImportExecutionOptions {
    bool confirmed = false;
    int failAfterFileOperations = -1;
    const std::atomic_bool *cancelled = nullptr;
};

struct ImportExecutionResult {
    bool success = false;
    bool rolledBack = false;
    bool cancelled = false;
    int added = 0;
    int overwritten = 0;
    int removed = 0;
    int skipped = 0;
    QString error;
};

struct ReferenceState {
    QString assetId;
    QString resolvedSourcePath;
    bool available = false;
    bool hashMatches = false;
    bool repairableById = false;
};

enum class AssetUpgradeKind {
    Added,
    Removed,
    Changed,
    Unchanged
};

struct AssetUpgrade {
    QString assetId;
    QString beforeVersion;
    QString afterVersion;
    QString beforeContentHash;
    QString afterContentHash;
    AssetUpgradeKind kind = AssetUpgradeKind::Unchanged;
};

struct VendorUpgradePlan {
    ImportPlan importPlan;
    QList<AssetUpgrade> assets;

    [[nodiscard]] bool hasChanges() const;
    [[nodiscard]] bool canExecute() const;
};

class ImportService {
public:
    [[nodiscard]] ImportPlan planReference(
        const QList<AssetRecord> &catalog,
        const QStringList &rootAssetIds,
        const QString &targetRoot,
        const QJsonObject &targetTools = {}) const;
    bool writeReference(const ImportPlan &plan, QString *error = nullptr) const;

    [[nodiscard]] QList<ReferenceState> inspectReferences(
        const QJsonObject &referenceDocument,
        const QList<AssetRecord> &catalog,
        const QString &targetRoot) const;
    bool repairReferences(QJsonObject &referenceDocument,
                          const QList<AssetRecord> &catalog,
                          const QString &targetRoot,
                          QString *error = nullptr) const;
    bool writeReferenceDocument(const QString &path,
                                const QJsonObject &referenceDocument,
                                QString *error = nullptr) const;
    bool writeReferenceDocumentIfUnchanged(
        const QString &path,
        const QJsonObject &referenceDocument,
        const QString &expectedContentHash,
        QString *error = nullptr) const;

    [[nodiscard]] ImportPlan planVendor(
        const QList<AssetRecord> &catalog,
        const QStringList &rootAssetIds,
        const QString &targetRoot,
        const QJsonObject &targetTools = {}) const;
    [[nodiscard]] VendorUpgradePlan planVendorUpgrade(
        const QList<AssetRecord> &catalog,
        const QStringList &rootAssetIds,
        const QString &targetRoot,
        const QJsonObject &targetTools = {}) const;
    [[nodiscard]] ImportExecutionResult executeVendor(
        const ImportPlan &plan,
        const ImportExecutionOptions &options = {}) const;
    bool recoverVendorTransactions(const QString &targetRoot,
                                   QStringList *recovered = nullptr,
                                   QString *error = nullptr) const;

    [[nodiscard]] static QString fileHash(const QString &path);
};

} // namespace xips
