#pragma once

#include "assetcore/Asset.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <atomic>

namespace xips {

enum class ManagedAssetSeed {
    EmptyModule,
    SourceFile,
    ExistingDirectory
};

struct ManagedAssetRequest {
    QString libraryRoot;
    QString id;
    QString name;
    QString top;
    QString version;
    ManagedAssetSeed seed = ManagedAssetSeed::EmptyModule;
    QString sourceFile;
    QString existingDirectory;
    QStringList existingAssetIds;
};

struct ManagedPlannedFile {
    QString sourcePath;
    QString destinationPath;
    QString sourceHash;
    QByteArray generatedContents;

    [[nodiscard]] bool generated() const;
};

struct ManagedAssetIssue {
    Diagnostic::Severity severity = Diagnostic::Severity::Error;
    QString message;
    QString path;
};

struct ManagedAssetPlan {
    QString libraryRoot;
    QString targetRoot;
    QString manifestPath;
    Manifest manifest;
    QList<ManagedPlannedFile> files;
    QList<ManagedAssetIssue> issues;

    [[nodiscard]] bool canExecute() const;
    [[nodiscard]] QJsonObject toJson() const;
};

struct ManagedAssetExecutionOptions {
    bool confirmed = false;
    int failAfterFileOperations = -1;
    const std::atomic_bool *cancelled = nullptr;
};

struct ManagedAssetExecutionResult {
    bool success = false;
    bool rolledBack = false;
    int filesCreated = 0;
    QString targetRoot;
    QString error;
};

class ManagedAssetService {
public:
    [[nodiscard]] ManagedAssetPlan plan(const ManagedAssetRequest &request) const;
    [[nodiscard]] ManagedAssetExecutionResult execute(
        const ManagedAssetPlan &plan,
        const ManagedAssetExecutionOptions &options = {}) const;
};

} // namespace xips
