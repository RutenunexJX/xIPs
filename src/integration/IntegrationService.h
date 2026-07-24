#pragma once

#include "assetcore/Asset.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

namespace xips {

struct IntegrationIssue {
    Diagnostic::Severity severity = Diagnostic::Severity::Error;
    QString message;
    QString path;
};

struct ModuleRegistrationRequest {
    QString assetRoot;
    QString sourcePath;
    QStringList sourcePaths;
    QString id;
    QString name;
    QString top;
    QString version;
    QStringList includeDirs;
    QStringList defines;
    QList<DependencySpec> dependencies;
    QStringList existingAssetIds;
};

struct ModuleRegistrationPlan {
    QString assetRoot;
    QString manifestPath;
    Manifest manifest;
    QList<IntegrationIssue> issues;

    [[nodiscard]] bool canExecute() const;
    [[nodiscard]] QJsonObject toJson() const;
};

class IntegrationService {
public:
    [[nodiscard]] ModuleRegistrationPlan planModuleRegistration(
        const ModuleRegistrationRequest &request) const;
    bool executeModuleRegistration(const ModuleRegistrationPlan &plan,
                                   bool confirmed,
                                   QString *error = nullptr) const;

    [[nodiscard]] static QUrl zeroSlackOpenUri(const AssetRecord &asset);
    [[nodiscard]] static QJsonObject codeBlockPayload(const AssetRecord &asset);
    static bool writeCodeBlockHandoff(const AssetRecord &asset,
                                      const QString &path,
                                      QString *error = nullptr);
    [[nodiscard]] static QUrl zeroSlackCodeBlockUri(const AssetRecord &asset,
                                                   const QString &handoffPath);
};

} // namespace xips
