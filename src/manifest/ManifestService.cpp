#include "manifest/ManifestService.h"

#include "assetcore/JsonUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace xips {
namespace {

Diagnostic makeDiagnostic(const Diagnostic::Severity severity,
                          const QString &code,
                          const QString &message,
                          const QString &file)
{
    return {.severity = severity, .code = code, .message = message, .file = file};
}

bool hasErrors(const QList<Diagnostic> &diagnostics)
{
    for (const Diagnostic &entry : diagnostics) {
        if (entry.severity == Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

QString readString(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

void readList(const QJsonObject &object,
              const QString &key,
              QStringList &destination,
              QList<Diagnostic> &diagnostics,
              const QString &sourceName)
{
    QString error;
    destination = json::stringList(object, key, &error);
    if (!error.isEmpty()) {
        diagnostics.append(makeDiagnostic(Diagnostic::Severity::Error,
                                          QStringLiteral("manifest.field"),
                                          error,
                                          sourceName));
    }
}

} // namespace

ManifestLoadResult ManifestService::load(const QString &manifestPath) const
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        ManifestLoadResult result;
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.open"),
            QStringLiteral("Cannot open manifest: %1").arg(file.errorString()),
            QFileInfo(manifestPath).absoluteFilePath()));
        return result;
    }
    return parse(file.readAll(), QFileInfo(manifestPath).absoluteFilePath());
}

ManifestLoadResult ManifestService::parse(const QByteArray &contents,
                                          const QString &sourceName) const
{
    ManifestLoadResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.json"),
            QStringLiteral("Invalid JSON object at byte %1: %2")
                .arg(parseError.offset)
                .arg(parseError.errorString()),
            sourceName));
        return result;
    }

    QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schemaVersion"));
    if (!schema.isDouble() || schema.toDouble() != schema.toInt()) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.schema"),
            QStringLiteral("Manifest must contain an integer schemaVersion"),
            sourceName));
        return result;
    }
    const int sourceVersion = schema.toInt();
    if (sourceVersion < 0 || sourceVersion > CurrentSchemaVersion) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.schema"),
            QStringLiteral("Unsupported schemaVersion %1").arg(sourceVersion),
            sourceName));
        return result;
    }
    if (sourceVersion < CurrentSchemaVersion) {
        if (!migrate(object, sourceVersion, result.diagnostics, sourceName)) {
            return result;
        }
        result.migrated = true;
    }

    Manifest manifest;
    manifest.rawObject = object;
    manifest.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt();
    manifest.id = readString(object, QStringLiteral("id"));
    const QString type = readString(object, QStringLiteral("type"));
    manifest.type = type.isEmpty() ? AssetType::Ip : assetTypeFromString(type);
    manifest.name = readString(object, QStringLiteral("name"));
    manifest.description = readString(object, QStringLiteral("description"));
    manifest.version = readString(object, QStringLiteral("version"));
    manifest.top = readString(object, QStringLiteral("top"));
    manifest.language = readString(object, QStringLiteral("language"));
    readList(object, QStringLiteral("sources"), manifest.sources,
             result.diagnostics, sourceName);
    readList(object, QStringLiteral("constraints"), manifest.constraints,
             result.diagnostics, sourceName);
    readList(object, QStringLiteral("tags"), manifest.tags,
             result.diagnostics, sourceName);
    readList(object, QStringLiteral("documentation"), manifest.documentation,
             result.diagnostics, sourceName);
    manifest.tools = object.value(QStringLiteral("tools")).toObject();
    result.diagnostics.append(validate(manifest, sourceName));
    if (!hasErrors(result.diagnostics)) {
        result.manifest = manifest;
    }
    return result;
}

QJsonObject ManifestService::toJson(const Manifest &manifest) const
{
    QJsonObject object = manifest.rawObject;
    object.insert(QStringLiteral("schemaVersion"), CurrentSchemaVersion);
    object.insert(QStringLiteral("id"), manifest.id);
    object.insert(QStringLiteral("type"), assetTypeToString(manifest.type));
    object.insert(QStringLiteral("name"), manifest.name.trimmed());
    const auto setOptional = [&object](const QString &key, const QString &value) {
        if (value.trimmed().isEmpty()) {
            object.remove(key);
        } else {
            object.insert(key, value.trimmed());
        }
    };
    setOptional(QStringLiteral("description"), manifest.description);
    setOptional(QStringLiteral("version"), manifest.version);
    setOptional(QStringLiteral("top"), manifest.top);
    setOptional(QStringLiteral("language"), manifest.language);
    object.insert(QStringLiteral("sources"), json::toArray(manifest.sources));
    object.insert(QStringLiteral("constraints"), json::toArray(manifest.constraints));
    object.insert(QStringLiteral("tags"), json::toArray(manifest.tags));
    if (manifest.documentation.isEmpty()) {
        object.remove(QStringLiteral("documentation"));
    } else {
        object.insert(QStringLiteral("documentation"),
                      json::toArray(manifest.documentation));
    }
    if (manifest.tools.isEmpty()) {
        object.remove(QStringLiteral("tools"));
    } else {
        object.insert(QStringLiteral("tools"), manifest.tools);
    }
    return object;
}

bool ManifestService::write(const QString &manifestPath,
                            const Manifest &manifest,
                            QString *error) const
{
    const QList<Diagnostic> diagnostics = validate(manifest, manifestPath);
    if (hasErrors(diagnostics)) {
        if (error) {
            *error = diagnostics.first().message;
        }
        return false;
    }
    QSaveFile file(manifestPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot open manifest for atomic write: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    const QByteArray data = QJsonDocument(toJson(manifest)).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error) {
            *error = QStringLiteral("Cannot write manifest: %1").arg(file.errorString());
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("Cannot atomically replace manifest: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    return true;
}

bool ManifestService::migrate(QJsonObject &object,
                              const int fromVersion,
                              QList<Diagnostic> &diagnostics,
                              const QString &sourceName) const
{
    if (fromVersion != 0) {
        return false;
    }
    if (!object.contains(QStringLiteral("type"))
        && object.value(QStringLiteral("kind")).isString()) {
        object.insert(QStringLiteral("type"), object.value(QStringLiteral("kind")));
    }
    if (!object.contains(QStringLiteral("name"))
        && object.value(QStringLiteral("displayName")).isString()) {
        object.insert(QStringLiteral("name"), object.value(QStringLiteral("displayName")));
    }
    if (!object.contains(QStringLiteral("sources"))
        && object.value(QStringLiteral("files")).isArray()) {
        object.insert(QStringLiteral("sources"), object.value(QStringLiteral("files")));
    }
    object.insert(QStringLiteral("schemaVersion"), CurrentSchemaVersion);
    diagnostics.append(makeDiagnostic(
        Diagnostic::Severity::Warning,
        QStringLiteral("manifest.schema.migrated"),
        QStringLiteral("Manifest schema migrated in memory from version 0 to 1"),
        sourceName));
    return true;
}

QList<Diagnostic> ManifestService::validate(const Manifest &manifest,
                                            const QString &sourceName) const
{
    QList<Diagnostic> result;
    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!idPattern.match(manifest.id).hasMatch()) {
        result.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.id"),
            QStringLiteral("IP id must start with an alphanumeric character and use only letters, digits, '_', '-' or '.'"),
            sourceName));
    }
    if (manifest.type == AssetType::Unknown) {
        result.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("manifest.type"),
            QStringLiteral("Asset type must be 'ip', 'module', or 'code-block'"),
            sourceName));
    }
    if (manifest.name.trimmed().isEmpty()) {
        result.append(makeDiagnostic(Diagnostic::Severity::Error,
                                     QStringLiteral("manifest.name"),
                                     QStringLiteral("IP name is required"),
                                     sourceName));
    }

    const auto checkPaths = [&result, &sourceName](const QStringList &paths,
                                                   const QString &field) {
        for (const QString &path : paths) {
            if (path.trimmed().isEmpty()) {
                result.append(makeDiagnostic(
                    Diagnostic::Severity::Error,
                    QStringLiteral("manifest.path"),
                    QStringLiteral("Field '%1' contains an empty path").arg(field),
                    sourceName));
            } else if (QDir::isAbsolutePath(path)) {
                result.append(makeDiagnostic(
                    Diagnostic::Severity::Warning,
                    QStringLiteral("manifest.path.absolute"),
                    QStringLiteral("Field '%1' uses a non-portable absolute path: %2")
                        .arg(field, path),
                    sourceName));
            }
        }
    };
    checkPaths(manifest.sources, QStringLiteral("sources"));
    checkPaths(manifest.constraints, QStringLiteral("constraints"));
    checkPaths(manifest.documentation, QStringLiteral("documentation"));
    return result;
}

} // namespace xips
