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

Diagnostic errorDiagnostic(const QString &code,
                           const QString &message,
                           const QString &sourceName)
{
    return Diagnostic{
        .severity = Diagnostic::Severity::Error,
        .code = code,
        .message = message,
        .file = sourceName,
    };
}

Diagnostic warningDiagnostic(const QString &code,
                             const QString &message,
                             const QString &sourceName)
{
    return Diagnostic{
        .severity = Diagnostic::Severity::Warning,
        .code = code,
        .message = message,
        .file = sourceName,
    };
}

bool hasErrors(const QList<Diagnostic> &diagnostics)
{
    for (const Diagnostic &diagnostic : diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

QString readString(const QJsonObject &object, const QString &key)
{
    return object.value(key).isString() ? object.value(key).toString() : QString();
}

QList<DependencySpec> readDependencies(const QJsonObject &object,
                                       QList<Diagnostic> &diagnostics,
                                       const QString &sourceName)
{
    QList<DependencySpec> result;
    const QJsonValue value = object.value(QStringLiteral("dependencies"));
    if (value.isUndefined() || value.isNull()) {
        return result;
    }
    if (!value.isArray()) {
        diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.dependencies.type"),
            QStringLiteral("Field 'dependencies' must be an array"),
            sourceName));
        return result;
    }

    for (qsizetype index = 0; index < value.toArray().size(); ++index) {
        const QJsonValue entry = value.toArray().at(index);
        DependencySpec dependency;
        if (entry.isString()) {
            dependency.id = entry.toString();
        } else if (entry.isObject()) {
            dependency.extraFields = entry.toObject();
            dependency.id = readString(dependency.extraFields, QStringLiteral("id"));
            dependency.versionConstraint =
                readString(dependency.extraFields, QStringLiteral("version"));
            if (dependency.versionConstraint.isEmpty()) {
                dependency.versionConstraint =
                    readString(dependency.extraFields, QStringLiteral("constraint"));
            }
            dependency.optional =
                dependency.extraFields.value(QStringLiteral("optional")).toBool(false);
        } else {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.dependencies.entry"),
                QStringLiteral("Dependency entry %1 must be a string or object").arg(index),
                sourceName));
            continue;
        }
        if (dependency.id.trimmed().isEmpty()) {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.dependencies.id"),
                QStringLiteral("Dependency entry %1 has no id").arg(index),
                sourceName));
            continue;
        }
        result.append(dependency);
    }
    return result;
}

QList<SlotDefinition> readSlots(const QJsonObject &object,
                                QList<Diagnostic> &diagnostics,
                                const QString &sourceName)
{
    QList<SlotDefinition> result;
    const QJsonValue value = object.value(QStringLiteral("slots"));
    if (value.isUndefined() || value.isNull()) {
        return result;
    }
    if (!value.isArray()) {
        diagnostics.append(errorDiagnostic(QStringLiteral("manifest.slots.type"),
                                           QStringLiteral("Field 'slots' must be an array"),
                                           sourceName));
        return result;
    }

    for (qsizetype index = 0; index < value.toArray().size(); ++index) {
        const QJsonValue entry = value.toArray().at(index);
        SlotDefinition slot;
        if (entry.isString()) {
            slot.name = entry.toString();
        } else if (entry.isObject()) {
            slot.extraFields = entry.toObject();
            slot.name = readString(slot.extraFields, QStringLiteral("name"));
            slot.type = readString(slot.extraFields, QStringLiteral("type"));
            slot.defaultValue = readString(slot.extraFields, QStringLiteral("default"));
            slot.description = readString(slot.extraFields, QStringLiteral("description"));
            slot.required = slot.extraFields.value(QStringLiteral("required")).toBool(true);
        } else {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.slots.entry"),
                QStringLiteral("Slot entry %1 must be a string or object").arg(index),
                sourceName));
            continue;
        }
        if (slot.name.trimmed().isEmpty()) {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.slots.name"),
                QStringLiteral("Slot entry %1 has no name").arg(index),
                sourceName));
            continue;
        }
        result.append(slot);
    }
    return result;
}

QList<TestCommandSpec> readTestCommands(const QJsonObject &object,
                                        QList<Diagnostic> &diagnostics,
                                        const QString &sourceName)
{
    QList<TestCommandSpec> result;
    const QJsonValue value = object.value(QStringLiteral("testCommands"));
    if (value.isUndefined() || value.isNull()) {
        return result;
    }
    if (!value.isArray()) {
        diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.testCommands.type"),
            QStringLiteral("Field 'testCommands' must be an array"),
            sourceName));
        return result;
    }
    for (qsizetype index = 0; index < value.toArray().size(); ++index) {
        const QJsonValue entry = value.toArray().at(index);
        if (!entry.isObject()) {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.testCommands.entry"),
                QStringLiteral("Test command entry %1 must be an object").arg(index),
                sourceName));
            continue;
        }
        TestCommandSpec command;
        command.extraFields = entry.toObject();
        command.name = readString(command.extraFields, QStringLiteral("name"));
        command.program = readString(command.extraFields, QStringLiteral("program"));
        command.workingDirectory =
            readString(command.extraFields, QStringLiteral("workingDirectory"));
        QString argumentsError;
        command.arguments =
            json::stringList(command.extraFields, QStringLiteral("arguments"), &argumentsError);
        if (!argumentsError.isEmpty()) {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.testCommands.arguments"),
                QStringLiteral("Test command entry %1: %2").arg(index).arg(argumentsError),
                sourceName));
        }
        if (command.name.isEmpty()) {
            command.name = command.program;
        }
        if (command.program.isEmpty()) {
            diagnostics.append(errorDiagnostic(
                QStringLiteral("manifest.testCommands.program"),
                QStringLiteral("Test command entry %1 has no program").arg(index),
                sourceName));
            continue;
        }
        result.append(command);
    }
    return result;
}

void appendStringList(const QJsonObject &object,
                      const QString &key,
                      QStringList &target,
                      QList<Diagnostic> &diagnostics,
                      const QString &sourceName)
{
    QString error;
    target = json::stringList(object, key, &error);
    if (!error.isEmpty()) {
        diagnostics.append(errorDiagnostic(QStringLiteral("manifest.array.type"),
                                           error,
                                           sourceName));
    }
}

QJsonArray dependenciesToJson(const QList<DependencySpec> &dependencies)
{
    QJsonArray result;
    for (const DependencySpec &dependency : dependencies) {
        QJsonObject object = dependency.extraFields;
        object.insert(QStringLiteral("id"), dependency.id);
        if (!dependency.versionConstraint.isEmpty()) {
            object.insert(QStringLiteral("version"), dependency.versionConstraint);
        }
        if (dependency.optional) {
            object.insert(QStringLiteral("optional"), true);
        }
        result.append(object);
    }
    return result;
}

QJsonArray slotsToJson(const QList<SlotDefinition> &slotDefinitions)
{
    QJsonArray result;
    for (const SlotDefinition &slot : slotDefinitions) {
        if (slot.extraFields.isEmpty() && slot.type.isEmpty() && slot.defaultValue.isEmpty()
            && slot.description.isEmpty() && slot.required) {
            result.append(slot.name);
            continue;
        }
        QJsonObject object = slot.extraFields;
        object.insert(QStringLiteral("name"), slot.name);
        if (!slot.type.isEmpty()) {
            object.insert(QStringLiteral("type"), slot.type);
        }
        if (!slot.defaultValue.isEmpty()) {
            object.insert(QStringLiteral("default"), slot.defaultValue);
        }
        if (!slot.description.isEmpty()) {
            object.insert(QStringLiteral("description"), slot.description);
        }
        object.insert(QStringLiteral("required"), slot.required);
        result.append(object);
    }
    return result;
}

QJsonArray testCommandsToJson(const QList<TestCommandSpec> &commands)
{
    QJsonArray result;
    for (const TestCommandSpec &command : commands) {
        QJsonObject object = command.extraFields;
        object.insert(QStringLiteral("name"), command.name);
        object.insert(QStringLiteral("program"), command.program);
        object.insert(QStringLiteral("arguments"), json::toArray(command.arguments));
        if (command.workingDirectory.isEmpty()) {
            object.remove(QStringLiteral("workingDirectory"));
        } else {
            object.insert(QStringLiteral("workingDirectory"), command.workingDirectory);
        }
        result.append(object);
    }
    return result;
}

} // namespace

ManifestLoadResult ManifestService::load(const QString &manifestPath) const
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        ManifestLoadResult result;
        result.diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.open"),
            QStringLiteral("Cannot open manifest: %1").arg(file.errorString()),
            manifestPath));
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
        result.diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.json"),
            QStringLiteral("Invalid JSON object at byte %1: %2")
                .arg(parseError.offset)
                .arg(parseError.errorString()),
            sourceName));
        return result;
    }

    QJsonObject object = document.object();
    const QJsonValue schemaValue = object.value(QStringLiteral("schemaVersion"));
    if (!schemaValue.isDouble() || schemaValue.toDouble() != schemaValue.toInt()) {
        result.diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.schema.missing"),
            QStringLiteral("Manifest must contain an integer schemaVersion"),
            sourceName));
        return result;
    }

    const int sourceVersion = schemaValue.toInt();
    if (sourceVersion < 0 || sourceVersion > CurrentSchemaVersion) {
        result.diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.schema.unsupported"),
            QStringLiteral("Unsupported schemaVersion %1; this build supports versions 0 through %2")
                .arg(sourceVersion)
                .arg(CurrentSchemaVersion),
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
    manifest.type = assetTypeFromString(readString(object, QStringLiteral("type")));
    manifest.name = readString(object, QStringLiteral("name"));
    manifest.description = readString(object, QStringLiteral("description"));
    manifest.version = readString(object, QStringLiteral("version"));
    manifest.top = readString(object, QStringLiteral("top"));
    const QString language = readString(object, QStringLiteral("language"));
    if (!language.isEmpty()) {
        manifest.language = language;
    }

    appendStringList(object,
                     QStringLiteral("sources"),
                     manifest.sources,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("includeDirs"),
                     manifest.includeDirs,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("defines"),
                     manifest.defines,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("constraints"),
                     manifest.constraints,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("tests"),
                     manifest.tests,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("tags"),
                     manifest.tags,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("examples"),
                     manifest.examples,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("documentation"),
                     manifest.documentation,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("scope"),
                     manifest.scope,
                     result.diagnostics,
                     sourceName);
    appendStringList(object,
                     QStringLiteral("requiredSymbols"),
                     manifest.requiredSymbols,
                     result.diagnostics,
                     sourceName);

    manifest.dependencies = readDependencies(object, result.diagnostics, sourceName);
    manifest.slotDefinitions = readSlots(object, result.diagnostics, sourceName);
    manifest.testCommands = readTestCommands(object, result.diagnostics, sourceName);
    manifest.tools = object.value(QStringLiteral("tools")).toObject();
    manifest.templateText = readString(object, QStringLiteral("template"));
    manifest.allowWorkspaceOverride =
        object.value(QStringLiteral("allowWorkspaceOverride")).toBool(false);
    manifest.exampleInput = object.value(QStringLiteral("exampleInput"));
    manifest.exampleOutput = object.value(QStringLiteral("exampleOutput"));

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
    object.insert(QStringLiteral("name"), manifest.name);

    const auto setOptionalString = [&object](const QString &key, const QString &value) {
        if (value.isEmpty()) {
            object.remove(key);
        } else {
            object.insert(key, value);
        }
    };
    setOptionalString(QStringLiteral("description"), manifest.description);
    setOptionalString(QStringLiteral("version"), manifest.version);
    setOptionalString(QStringLiteral("top"), manifest.top);
    setOptionalString(QStringLiteral("language"), manifest.language);

    object.insert(QStringLiteral("sources"), json::toArray(manifest.sources));
    object.insert(QStringLiteral("includeDirs"), json::toArray(manifest.includeDirs));
    object.insert(QStringLiteral("defines"), json::toArray(manifest.defines));
    object.insert(QStringLiteral("constraints"), json::toArray(manifest.constraints));
    object.insert(QStringLiteral("dependencies"), dependenciesToJson(manifest.dependencies));
    object.insert(QStringLiteral("tests"), json::toArray(manifest.tests));
    if (!manifest.testCommands.isEmpty()) {
        object.insert(QStringLiteral("testCommands"),
                      testCommandsToJson(manifest.testCommands));
    } else {
        object.remove(QStringLiteral("testCommands"));
    }
    object.insert(QStringLiteral("tags"), json::toArray(manifest.tags));
    if (!manifest.examples.isEmpty()) {
        object.insert(QStringLiteral("examples"), json::toArray(manifest.examples));
    }
    if (!manifest.documentation.isEmpty()) {
        object.insert(QStringLiteral("documentation"), json::toArray(manifest.documentation));
    }
    if (!manifest.tools.isEmpty()) {
        object.insert(QStringLiteral("tools"), manifest.tools);
    }

    if (manifest.type == AssetType::CodeBlock) {
        object.insert(QStringLiteral("template"), manifest.templateText);
        object.insert(QStringLiteral("scope"), json::toArray(manifest.scope));
        object.insert(QStringLiteral("slots"), slotsToJson(manifest.slotDefinitions));
        if (!manifest.requiredSymbols.isEmpty()) {
            object.insert(QStringLiteral("requiredSymbols"),
                          json::toArray(manifest.requiredSymbols));
        }
        object.insert(QStringLiteral("allowWorkspaceOverride"),
                      manifest.allowWorkspaceOverride);
        if (manifest.exampleInput.isUndefined()) {
            object.remove(QStringLiteral("exampleInput"));
        } else {
            object.insert(QStringLiteral("exampleInput"), manifest.exampleInput);
        }
        if (manifest.exampleOutput.isUndefined()) {
            object.remove(QStringLiteral("exampleOutput"));
        } else {
            object.insert(QStringLiteral("exampleOutput"), manifest.exampleOutput);
        }
    }
    return object;
}

bool ManifestService::write(const QString &manifestPath,
                            const Manifest &manifest,
                            QString *error) const
{
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
    int version = fromVersion;
    while (version < CurrentSchemaVersion) {
        if (version == 0) {
            if (!object.contains(QStringLiteral("type"))
                && object.value(QStringLiteral("kind")).isString()) {
                object.insert(QStringLiteral("type"), object.value(QStringLiteral("kind")));
            }
            if (!object.contains(QStringLiteral("name"))
                && object.value(QStringLiteral("displayName")).isString()) {
                object.insert(QStringLiteral("name"),
                              object.value(QStringLiteral("displayName")));
            }
            if (!object.contains(QStringLiteral("sources"))
                && object.value(QStringLiteral("files")).isArray()) {
                object.insert(QStringLiteral("sources"),
                              object.value(QStringLiteral("files")));
            }
            version = 1;
            object.insert(QStringLiteral("schemaVersion"), version);
            diagnostics.append(warningDiagnostic(
                QStringLiteral("manifest.schema.migrated"),
                QStringLiteral("Manifest schema migrated in memory from version 0 to 1"),
                sourceName));
            continue;
        }

        diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.schema.migration"),
            QStringLiteral("No migration path from schema version %1").arg(version),
            sourceName));
        return false;
    }
    return true;
}

QList<Diagnostic> ManifestService::validate(const Manifest &manifest,
                                            const QString &sourceName) const
{
    QList<Diagnostic> diagnostics;
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));

    if (manifest.id.isEmpty() || !idPattern.match(manifest.id).hasMatch()) {
        diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.id"),
            QStringLiteral("Asset id must start with an alphanumeric character and contain only "
                           "letters, digits, '_', '-' or '.'"),
            sourceName));
    }
    if (manifest.type == AssetType::Unknown) {
        diagnostics.append(errorDiagnostic(
            QStringLiteral("manifest.type"),
            QStringLiteral("Asset type must be 'code-block', 'module', or 'ip'"),
            sourceName));
    }
    if (manifest.name.trimmed().isEmpty()) {
        diagnostics.append(errorDiagnostic(QStringLiteral("manifest.name"),
                                           QStringLiteral("Asset name is required"),
                                           sourceName));
    }
    if (manifest.type == AssetType::CodeBlock && manifest.templateText.isEmpty()) {
        diagnostics.append(errorDiagnostic(QStringLiteral("manifest.template"),
                                           QStringLiteral("Code Block assets require a template"),
                                           sourceName));
    }

    const auto checkPaths = [&diagnostics, &sourceName](const QStringList &paths,
                                                        const QString &field) {
        for (const QString &path : paths) {
            if (QDir::isAbsolutePath(path)) {
                diagnostics.append(warningDiagnostic(
                    QStringLiteral("manifest.path.absolute"),
                    QStringLiteral("Field '%1' uses an absolute path and will not relocate with the "
                                   "asset: %2")
                        .arg(field, path),
                    sourceName));
            }
        }
    };
    checkPaths(manifest.sources, QStringLiteral("sources"));
    checkPaths(manifest.includeDirs, QStringLiteral("includeDirs"));
    checkPaths(manifest.constraints, QStringLiteral("constraints"));
    checkPaths(manifest.tests, QStringLiteral("tests"));
    return diagnostics;
}

} // namespace xips
