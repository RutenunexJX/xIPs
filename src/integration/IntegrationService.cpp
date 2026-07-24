#include "integration/IntegrationService.h"

#include "assetcore/JsonUtil.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrlQuery>

#include <algorithm>

namespace xips {
namespace {

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink()) {
        return true;
    }
#ifdef Q_OS_WIN
    return info.isJunction();
#else
    return false;
#endif
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool sameAbsolutePath(const QString &left, const QString &right)
{
    return QFileInfo(left).absoluteFilePath().compare(
               QFileInfo(right).absoluteFilePath(),
               pathCaseSensitivity())
           == 0;
}

bool isWithin(const QString &path, const QString &root)
{
    const QString absolutePath =
        QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QString absoluteRoot =
        QDir::fromNativeSeparators(QFileInfo(root).absoluteFilePath());
    return absolutePath.compare(absoluteRoot, Qt::CaseInsensitive) == 0
           || absolutePath.startsWith(absoluteRoot + u'/',
                                      Qt::CaseInsensitive);
}

QJsonArray issuesToJson(const QList<IntegrationIssue> &issues)
{
    QJsonArray result;
    for (const IntegrationIssue &issue : issues) {
        result.append(QJsonObject{
            {QStringLiteral("severity"), diagnosticSeverityToString(issue.severity)},
            {QStringLiteral("message"), issue.message},
            {QStringLiteral("path"), issue.path},
        });
    }
    return result;
}

bool writeJsonAtomic(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Cannot create handoff directory");
        }
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QByteArray encoded = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        file.cancelWriting();
        return false;
    }
    return true;
}

} // namespace

bool ModuleRegistrationPlan::canExecute() const
{
    return std::none_of(issues.cbegin(), issues.cend(), [](const IntegrationIssue &issue) {
        return issue.severity == Diagnostic::Severity::Error;
    });
}

QJsonObject ModuleRegistrationPlan::toJson() const
{
    return QJsonObject{
        {QStringLiteral("assetRoot"), assetRoot},
        {QStringLiteral("manifestPath"), manifestPath},
        {QStringLiteral("canExecute"), canExecute()},
        {QStringLiteral("manifest"), ManifestService().toJson(manifest)},
        {QStringLiteral("issues"), issuesToJson(issues)},
    };
}

ModuleRegistrationPlan IntegrationService::planModuleRegistration(
    const ModuleRegistrationRequest &request) const
{
    ModuleRegistrationPlan plan;
    QStringList requestedSources = request.sourcePaths;
    if (!request.sourcePath.isEmpty()) {
        requestedSources.prepend(request.sourcePath);
    }
    requestedSources.removeDuplicates();
    const QFileInfo source(requestedSources.value(0));
    plan.assetRoot = request.assetRoot.isEmpty()
                         ? source.absolutePath()
                         : QFileInfo(request.assetRoot).absoluteFilePath();
    plan.manifestPath =
        QDir(plan.assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    if (requestedSources.isEmpty()) {
        plan.issues.append(IntegrationIssue{
            .message = QStringLiteral("At least one module source file is required"),
            .path = plan.assetRoot,
        });
    }
    if (!QFileInfo(plan.assetRoot).isDir()
        || isLinkLike(QFileInfo(plan.assetRoot))) {
        plan.issues.append(IntegrationIssue{
            .message = QStringLiteral(
                "Asset root must be an existing non-link directory"),
            .path = plan.assetRoot,
        });
    }
    if (QFileInfo::exists(plan.manifestPath)
        || isLinkLike(QFileInfo(plan.manifestPath))) {
        plan.issues.append(IntegrationIssue{
            .message = QStringLiteral("Asset root already contains .xips.json"),
            .path = plan.manifestPath,
        });
    }
    if (request.existingAssetIds.contains(request.id)) {
        plan.issues.append(IntegrationIssue{
            .message = QStringLiteral("Asset ID already exists in the registered catalog"),
            .path = request.id,
        });
    }

    plan.manifest.id = request.id;
    plan.manifest.type = AssetType::Module;
    plan.manifest.name = request.name;
    plan.manifest.version = request.version;
    plan.manifest.top = request.top;
    plan.manifest.includeDirs = request.includeDirs;
    plan.manifest.defines = request.defines;
    plan.manifest.dependencies = request.dependencies;
    plan.manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("integration"),
         QJsonObject{
             {QStringLiteral("source"), QStringLiteral("zeroslack")},
             {QStringLiteral("protocolVersion"), 1},
         }},
    };

    for (const QString &sourcePath : requestedSources) {
        const QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.isFile() || isLinkLike(sourceInfo)) {
            plan.issues.append(IntegrationIssue{
                .message = QStringLiteral(
                    "Module source must be a regular non-link file"),
                .path = sourceInfo.absoluteFilePath(),
            });
            continue;
        }
        if (isWithin(sourceInfo.absoluteFilePath(), plan.assetRoot)) {
            QString relative =
                QDir(plan.assetRoot).relativeFilePath(sourceInfo.absoluteFilePath());
            relative = QDir::cleanPath(relative);
            relative.replace(u'\\', u'/');
            plan.manifest.sources.append(relative);
        } else {
            plan.manifest.sources.append(sourceInfo.absoluteFilePath());
            plan.issues.append(IntegrationIssue{
                .severity = Diagnostic::Severity::Warning,
                .message = QStringLiteral("Source is outside the asset root; the manifest uses an absolute path"),
                .path = sourceInfo.absoluteFilePath(),
            });
        }
    }

    const QList<Diagnostic> validation =
        ManifestService().validate(plan.manifest, plan.manifestPath);
    for (const Diagnostic &diagnostic : validation) {
        plan.issues.append(IntegrationIssue{
            .severity = diagnostic.severity,
            .message = diagnostic.message,
            .path = diagnostic.file,
        });
    }
    if (request.top.trimmed().isEmpty()) {
        plan.issues.append(IntegrationIssue{
            .message = QStringLiteral("A top module/interface/package name is required"),
            .path = plan.manifestPath,
        });
    }
    return plan;
}

bool IntegrationService::executeModuleRegistration(
    const ModuleRegistrationPlan &plan,
    const bool confirmed,
    QString *error) const
{
    if (!confirmed) {
        if (error) {
            *error = QStringLiteral("Module registration requires explicit confirmation");
        }
        return false;
    }
    if (!plan.canExecute()) {
        if (error) {
            *error = QStringLiteral("Module registration plan contains errors");
        }
        return false;
    }
    const QFileInfo rootInfo(plan.assetRoot);
    const QString expectedManifest =
        QDir(plan.assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    const QList<Diagnostic> validation =
        ManifestService().validate(plan.manifest, expectedManifest);
    if (!rootInfo.isDir() || isLinkLike(rootInfo)
        || !sameAbsolutePath(plan.manifestPath, expectedManifest)
        || plan.manifest.type != AssetType::Module
        || plan.manifest.top.trimmed().isEmpty()
        || plan.manifest.sources.isEmpty()
        || std::any_of(validation.cbegin(),
                       validation.cend(),
                       [](const Diagnostic &diagnostic) {
                           return diagnostic.severity
                                  == Diagnostic::Severity::Error;
                       })) {
        if (error) {
            *error = QStringLiteral(
                "Unsafe or invalid module registration plan");
        }
        return false;
    }
    for (const QString &source : plan.manifest.sources) {
        const QString absolute =
            QDir::isAbsolutePath(source)
                ? QFileInfo(source).absoluteFilePath()
                : QDir(plan.assetRoot).absoluteFilePath(source);
        const QFileInfo sourceInfo(absolute);
        if (!sourceInfo.isFile() || isLinkLike(sourceInfo)) {
            if (error) {
                *error =
                    QStringLiteral("Module source changed after planning: %1")
                        .arg(absolute);
            }
            return false;
        }
    }
    if (QFileInfo::exists(plan.manifestPath)
        || isLinkLike(QFileInfo(plan.manifestPath))) {
        if (error) {
            *error = QStringLiteral("Manifest appeared after planning");
        }
        return false;
    }
    return ManifestService().write(plan.manifestPath, plan.manifest, error);
}

QUrl IntegrationService::zeroSlackOpenUri(const AssetRecord &asset)
{
    QUrl uri;
    uri.setScheme(QStringLiteral("zeroslack"));
    uri.setHost(QStringLiteral("open"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("protocolVersion"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("assetId"), asset.manifest.id);
    query.addQueryItem(QStringLiteral("manifest"), asset.manifestPath);
    query.addQueryItem(QStringLiteral("top"), asset.manifest.top);
    query.addQueryItem(QStringLiteral("language"), asset.manifest.language);
    query.addQueryItem(QStringLiteral("contentHash"), asset.contentHash);
    if (!asset.manifest.sources.isEmpty()) {
        const QString source = asset.manifest.sources.first();
        query.addQueryItem(
            QStringLiteral("source"),
            QDir::isAbsolutePath(source)
                ? source
                : QDir(asset.assetRoot).absoluteFilePath(source));
    }
    uri.setQuery(query);
    return uri;
}

QJsonObject IntegrationService::codeBlockPayload(const AssetRecord &asset)
{
    QJsonArray slotArray;
    for (const SlotDefinition &slot : asset.manifest.slotDefinitions) {
        slotArray.append(QJsonObject{
            {QStringLiteral("name"), slot.name},
            {QStringLiteral("type"), slot.type},
            {QStringLiteral("default"), slot.defaultValue},
            {QStringLiteral("description"), slot.description},
            {QStringLiteral("required"), slot.required},
        });
    }
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("operation"), QStringLiteral("insert-code-block")},
        {QStringLiteral("assetId"), asset.manifest.id},
        {QStringLiteral("name"), asset.manifest.name},
        {QStringLiteral("version"), asset.manifest.version},
        {QStringLiteral("contentHash"), asset.contentHash},
        {QStringLiteral("template"), asset.manifest.templateText},
        {QStringLiteral("scope"), json::toArray(asset.manifest.scope)},
        {QStringLiteral("slots"), slotArray},
        {QStringLiteral("requiredSymbols"),
         json::toArray(asset.manifest.requiredSymbols)},
        {QStringLiteral("allowWorkspaceOverride"),
         asset.manifest.allowWorkspaceOverride},
        {QStringLiteral("exampleInput"), asset.manifest.exampleInput},
        {QStringLiteral("exampleOutput"), asset.manifest.exampleOutput},
    };
}

bool IntegrationService::writeCodeBlockHandoff(const AssetRecord &asset,
                                               const QString &path,
                                               QString *error)
{
    if (asset.manifest.type != AssetType::CodeBlock) {
        if (error) {
            *error = QStringLiteral("Only Code Block assets can be handed off");
        }
        return false;
    }
    return writeJsonAtomic(path, codeBlockPayload(asset), error);
}

QUrl IntegrationService::zeroSlackCodeBlockUri(const AssetRecord &asset,
                                                const QString &handoffPath)
{
    QUrl uri;
    uri.setScheme(QStringLiteral("zeroslack"));
    uri.setHost(QStringLiteral("insert-code-block"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("protocolVersion"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("assetId"), asset.manifest.id);
    query.addQueryItem(QStringLiteral("contentHash"), asset.contentHash);
    query.addQueryItem(QStringLiteral("handoff"),
                       QFileInfo(handoffPath).absoluteFilePath());
    uri.setQuery(query);
    return uri;
}

} // namespace xips
