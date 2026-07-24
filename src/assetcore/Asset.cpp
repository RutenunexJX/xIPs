#include "assetcore/Asset.h"

#include "assetcore/JsonUtil.h"

namespace xips {

QString assetTypeToString(const AssetType type)
{
    switch (type) {
    case AssetType::CodeBlock:
        return QStringLiteral("code-block");
    case AssetType::Module:
        return QStringLiteral("module");
    case AssetType::Ip:
        return QStringLiteral("ip");
    case AssetType::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

AssetType assetTypeFromString(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("code-block") || normalized == QStringLiteral("codeblock")) {
        return AssetType::CodeBlock;
    }
    if (normalized == QStringLiteral("module")) {
        return AssetType::Module;
    }
    if (normalized == QStringLiteral("ip")) {
        return AssetType::Ip;
    }
    return AssetType::Unknown;
}

QString assetOriginToString(const AssetOrigin origin)
{
    return origin == AssetOrigin::External ? QStringLiteral("external")
                                           : QStringLiteral("managed");
}

QString diagnosticSeverityToString(const Diagnostic::Severity severity)
{
    switch (severity) {
    case Diagnostic::Severity::Info:
        return QStringLiteral("info");
    case Diagnostic::Severity::Warning:
        return QStringLiteral("warning");
    case Diagnostic::Severity::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

bool TestResult::passed() const
{
    return status == QStringLiteral("passed") && exitCode == 0 && !cancelled;
}

bool TestResult::isStale(const QString &currentContentHash,
                         const QString &currentGitCommit) const
{
    if (contentHash != currentContentHash) {
        return true;
    }
    return !currentGitCommit.isEmpty() && !gitCommit.isEmpty()
           && gitCommit != currentGitCommit;
}

QJsonObject testResultToJson(const TestResult &result)
{
    return QJsonObject{
        {QStringLiteral("assetId"), result.assetId},
        {QStringLiteral("commandName"), result.commandName},
        {QStringLiteral("program"), result.program},
        {QStringLiteral("arguments"), json::toArray(result.arguments)},
        {QStringLiteral("workingDirectory"), result.workingDirectory},
        {QStringLiteral("status"), result.status},
        {QStringLiteral("exitCode"), result.exitCode},
        {QStringLiteral("exitStatus"), result.exitStatus},
        {QStringLiteral("startedAt"), result.startedAt.toString(Qt::ISODateWithMs)},
        {QStringLiteral("durationMs"), result.durationMs},
        {QStringLiteral("stdout"), result.standardOutput},
        {QStringLiteral("stderr"), result.standardError},
        {QStringLiteral("gitCommit"), result.gitCommit},
        {QStringLiteral("contentHash"), result.contentHash},
        {QStringLiteral("cancelled"), result.cancelled},
        {QStringLiteral("logTruncated"), result.logTruncated},
    };
}

TestResult testResultFromJson(const QJsonObject &object)
{
    TestResult result;
    result.assetId = object.value(QStringLiteral("assetId")).toString();
    result.commandName = object.value(QStringLiteral("commandName")).toString();
    result.program = object.value(QStringLiteral("program")).toString();
    result.arguments = json::stringList(object, QStringLiteral("arguments"));
    result.workingDirectory =
        object.value(QStringLiteral("workingDirectory")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    result.exitCode = object.value(QStringLiteral("exitCode")).toInt(-1);
    result.exitStatus = object.value(QStringLiteral("exitStatus")).toString();
    result.startedAt = QDateTime::fromString(
        object.value(QStringLiteral("startedAt")).toString(),
        Qt::ISODateWithMs);
    result.durationMs =
        object.value(QStringLiteral("durationMs")).toVariant().toLongLong();
    result.standardOutput = object.value(QStringLiteral("stdout")).toString();
    result.standardError = object.value(QStringLiteral("stderr")).toString();
    result.gitCommit = object.value(QStringLiteral("gitCommit")).toString();
    result.contentHash = object.value(QStringLiteral("contentHash")).toString();
    result.cancelled = object.value(QStringLiteral("cancelled")).toBool();
    result.logTruncated = object.value(QStringLiteral("logTruncated")).toBool();
    return result;
}

} // namespace xips
