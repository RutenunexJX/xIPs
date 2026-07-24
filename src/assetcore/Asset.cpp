#include "assetcore/Asset.h"

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
    if (normalized == QStringLiteral("code-block")
        || normalized == QStringLiteral("codeblock")) {
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

} // namespace xips
