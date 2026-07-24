#include "semantic/SlangService.h"

#include "assetcore/JsonUtil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

namespace xips {
namespace {

constexpr qsizetype MaximumProcessOutput = 512 * 1024;

bool isCancelled(const std::atomic_bool *cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

Diagnostic makeDiagnostic(const Diagnostic::Severity severity,
                          const QString &code,
                          const QString &message,
                          const QString &file = {},
                          const int line = 0,
                          const int column = 0)
{
    return Diagnostic{
        .severity = severity,
        .code = code,
        .message = message,
        .file = file,
        .line = line,
        .column = column,
    };
}

QString valueText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 16);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isNull() || value.isUndefined()) {
        return {};
    }
    return QString::fromUtf8(json::canonicalJson(value));
}

QString firstNonEmpty(const QJsonObject &object, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString text = valueText(object.value(key));
        if (!text.isEmpty()) {
            return text;
        }
    }
    return {};
}

QString typeText(const QJsonValue &type)
{
    if (type.isString()) {
        return type.toString();
    }
    if (!type.isObject()) {
        return valueText(type);
    }
    const QJsonObject object = type.toObject();
    const QString name = firstNonEmpty(
        object,
        {QStringLiteral("name"),
         QStringLiteral("typeName"),
         QStringLiteral("scalarKind"),
         QStringLiteral("keyword")});
    if (!name.isEmpty()) {
        return name;
    }
    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (!kind.isEmpty() && object.size() == 1) {
        return kind;
    }
    return QString::fromUtf8(json::canonicalJson(object));
}

QString rangeText(const QJsonValue &range)
{
    if (range.isString()) {
        return range.toString();
    }
    if (range.isArray()) {
        QStringList parts;
        for (const QJsonValue &entry : range.toArray()) {
            parts.append(rangeText(entry));
        }
        return parts.join(QStringLiteral(" "));
    }
    if (!range.isObject()) {
        return valueText(range);
    }

    const QJsonObject object = range.toObject();
    const QString left =
        firstNonEmpty(object,
                      {QStringLiteral("left"), QStringLiteral("msb"), QStringLiteral("upper")});
    const QString right =
        firstNonEmpty(object,
                      {QStringLiteral("right"), QStringLiteral("lsb"), QStringLiteral("lower")});
    if (!left.isEmpty() || !right.isEmpty()) {
        return QStringLiteral("[%1:%2]").arg(left, right);
    }
    return QString::fromUtf8(json::canonicalJson(object));
}

void collectDimensions(const QJsonValue &value,
                       QStringList &packed,
                       QStringList &unpacked,
                       const QString &parentKind = {})
{
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            collectDimensions(entry, packed, unpacked, parentKind);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    const QString effectiveKind = kind.isEmpty() ? parentKind : kind;
    const bool isPacked = effectiveKind.contains(QStringLiteral("Packed"), Qt::CaseInsensitive);
    const bool isUnpacked =
        effectiveKind.contains(QStringLiteral("Unpacked"), Qt::CaseInsensitive)
        || effectiveKind.contains(QStringLiteral("FixedSize"), Qt::CaseInsensitive)
        || effectiveKind.contains(QStringLiteral("DynamicArray"), Qt::CaseInsensitive);

    for (const QString &key : {QStringLiteral("range"), QStringLiteral("dimensions")}) {
        if (!object.contains(key)) {
            continue;
        }
        const QString text = rangeText(object.value(key));
        if (text.isEmpty()) {
            continue;
        }
        if (isUnpacked) {
            unpacked.append(text);
        } else if (isPacked) {
            packed.append(text);
        }
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (iterator.key() != QStringLiteral("range")
            && iterator.key() != QStringLiteral("dimensions")) {
            collectDimensions(iterator.value(), packed, unpacked, effectiveKind);
        }
    }
}

QString definitionNameForInstance(const QJsonObject &instance)
{
    const QJsonObject body = instance.value(QStringLiteral("body")).toObject();
    QString name = firstNonEmpty(
        body,
        {QStringLiteral("definitionName"),
         QStringLiteral("definition"),
         QStringLiteral("name")});
    if (name.isEmpty()) {
        name = firstNonEmpty(instance,
                             {QStringLiteral("definitionName"),
                              QStringLiteral("definition"),
                              QStringLiteral("type")});
    }
    if (name.isEmpty()) {
        name = instance.value(QStringLiteral("name")).toString();
    }
    return name;
}

struct AstCollector {
    QHash<QString, SemanticUnit> units;
    QHash<QString, QString> definitionKinds;
    QSet<QString> instantiatedDefinitions;
    QSet<QString> rootInstances;

    SemanticUnit &unit(const QString &name, const QString &kind = {})
    {
        SemanticUnit &result = units[name];
        if (result.name.isEmpty()) {
            result.name = name;
        }
        if (!kind.isEmpty() && (result.kind.isEmpty() || result.kind == QStringLiteral("unknown"))) {
            result.kind = kind.toLower();
        }
        return result;
    }

    void visit(const QJsonValue &value,
               const QString &currentUnit = {},
               const bool rootMember = false)
    {
        if (value.isArray()) {
            for (const QJsonValue &entry : value.toArray()) {
                visit(entry, currentUnit, rootMember);
            }
            return;
        }
        if (!value.isObject()) {
            return;
        }

        const QJsonObject object = value.toObject();
        const QString kind = object.value(QStringLiteral("kind")).toString();
        const QString name = object.value(QStringLiteral("name")).toString();
        QString activeUnit = currentUnit;

        if (kind == QStringLiteral("Definition")) {
            const QString definitionKind =
                object.value(QStringLiteral("definitionKind")).toString().toLower();
            SemanticUnit &definition = unit(name, definitionKind);
            definitionKinds.insert(name, definitionKind);
            if (definition.sourceFile.isEmpty()) {
                definition.sourceFile =
                    object.value(QStringLiteral("source_file")).toString();
            }
        } else if (kind == QStringLiteral("Package")) {
            activeUnit = name;
            SemanticUnit &package = unit(name, QStringLiteral("package"));
            if (package.sourceFile.isEmpty()) {
                package.sourceFile = object.value(QStringLiteral("source_file")).toString();
            }
        } else if (kind == QStringLiteral("Instance")) {
            const QString definitionName = definitionNameForInstance(object);
            if (rootMember) {
                rootInstances.insert(definitionName);
            }
            if (!currentUnit.isEmpty() && !definitionName.isEmpty()
                && definitionName != currentUnit) {
                SemanticUnit &parent = unit(currentUnit);
                if (!parent.instances.contains(definitionName)) {
                    parent.instances.append(definitionName);
                }
                instantiatedDefinitions.insert(definitionName);
            }
            if (!definitionName.isEmpty()) {
                activeUnit = definitionName;
                SemanticUnit &instanceUnit =
                    unit(definitionName, definitionKinds.value(definitionName));
                const QJsonObject body = object.value(QStringLiteral("body")).toObject();
                if (instanceUnit.sourceFile.isEmpty()) {
                    instanceUnit.sourceFile =
                        firstNonEmpty(body,
                                      {QStringLiteral("source_file"),
                                       QStringLiteral("source_file_start")});
                }
            }
        } else if (!activeUnit.isEmpty() && kind == QStringLiteral("Port")) {
            SemanticPort port;
            port.name = name;
            port.direction = object.value(QStringLiteral("direction")).toString().toLower();
            port.type = typeText(object.value(QStringLiteral("type")));
            QStringList packed;
            QStringList unpacked;
            collectDimensions(object.value(QStringLiteral("type")), packed, unpacked);
            port.packedDimensions = packed.join(u' ');
            port.unpackedDimensions = unpacked.join(u' ');
            SemanticUnit &target = unit(activeUnit);
            const auto existing = std::find_if(target.ports.cbegin(),
                                               target.ports.cend(),
                                               [&port](const SemanticPort &candidate) {
                                                   return candidate.name == port.name;
                                               });
            if (existing == target.ports.cend()) {
                target.ports.append(port);
            }
        } else if (!activeUnit.isEmpty()
                   && (kind == QStringLiteral("Parameter")
                       || kind == QStringLiteral("TypeParameter")
                       || kind == QStringLiteral("Specparam"))) {
            SemanticParameter parameter;
            parameter.name = name;
            parameter.type = typeText(object.value(QStringLiteral("type")));
            parameter.value = valueText(object.value(QStringLiteral("value")));
            parameter.local = object.value(QStringLiteral("isLocal")).toBool(false)
                              || kind == QStringLiteral("Specparam");
            SemanticUnit &target = unit(activeUnit);
            const auto existing = std::find_if(
                target.parameters.cbegin(),
                target.parameters.cend(),
                [&parameter](const SemanticParameter &candidate) {
                    return candidate.name == parameter.name;
                });
            if (existing == target.parameters.cend()) {
                target.parameters.append(parameter);
            }
        } else if (!activeUnit.isEmpty()
                   && (kind == QStringLiteral("ExplicitImport")
                       || kind == QStringLiteral("WildcardImport")
                       || kind.endsWith(QStringLiteral("Import")))) {
            const QString packageName =
                firstNonEmpty(object,
                              {QStringLiteral("packageName"),
                               QStringLiteral("package"),
                               QStringLiteral("name")});
            const QString importedName =
                firstNonEmpty(object,
                              {QStringLiteral("importedName"),
                               QStringLiteral("member"),
                               QStringLiteral("symbol")});
            QString importText = packageName;
            if (!importedName.isEmpty()) {
                importText += QStringLiteral("::") + importedName;
            } else if (kind == QStringLiteral("WildcardImport") && !packageName.isEmpty()) {
                importText += QStringLiteral("::*");
            }
            if (!importText.isEmpty()) {
                SemanticUnit &target = unit(activeUnit);
                if (!target.imports.contains(importText)) {
                    target.imports.append(importText);
                }
            }
        }

        if (kind == QStringLiteral("Root")) {
            const QJsonArray members = object.value(QStringLiteral("members")).toArray();
            for (const QJsonValue &member : members) {
                visit(member, {}, true);
            }
            for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
                if (iterator.key() != QStringLiteral("members")
                    && iterator.key() != QStringLiteral("kind")
                    && iterator.key() != QStringLiteral("name")) {
                    visit(iterator.value(), {}, false);
                }
            }
            return;
        }

        for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
            if (iterator.key() != QStringLiteral("kind")
                && iterator.key() != QStringLiteral("name")
                && iterator.key() != QStringLiteral("definitionKind")
                && iterator.key() != QStringLiteral("source_file")) {
                visit(iterator.value(), activeUnit, false);
            }
        }
    }
};

QString directiveValue(const QJsonObject &object,
                       const QStringList &directKeys,
                       const QStringList &nestedKeys)
{
    const QString direct = firstNonEmpty(object, directKeys);
    if (!direct.isEmpty()) {
        return direct;
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (!iterator.value().isObject()) {
            continue;
        }
        const QString nested = firstNonEmpty(iterator.value().toObject(), nestedKeys);
        if (!nested.isEmpty()) {
            return nested;
        }
    }
    return {};
}

void collectCstFacts(const QJsonValue &value, QSet<QString> &includes, QSet<QString> &defines)
{
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            collectCstFacts(entry, includes, defines);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    if (kind.contains(QStringLiteral("IncludeDirective"), Qt::CaseInsensitive)) {
        const QString include = directiveValue(
            object,
            {QStringLiteral("path"),
             QStringLiteral("file"),
             QStringLiteral("fileName"),
             QStringLiteral("value")},
            {QStringLiteral("valueText"),
             QStringLiteral("text"),
             QStringLiteral("value")});
        if (!include.isEmpty()) {
            includes.insert(include);
        }
    } else if (kind.contains(QStringLiteral("DefineDirective"), Qt::CaseInsensitive)
               || kind.contains(QStringLiteral("MacroDefinition"), Qt::CaseInsensitive)) {
        const QString define = directiveValue(
            object,
            {QStringLiteral("name"),
             QStringLiteral("macroName"),
             QStringLiteral("identifier")},
            {QStringLiteral("valueText"),
             QStringLiteral("text"),
             QStringLiteral("value")});
        if (!define.isEmpty()) {
            defines.insert(define);
        }
    }

    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        collectCstFacts(iterator.value(), includes, defines);
    }
}

Diagnostic::Severity severityFromText(const QString &severity)
{
    const QString normalized = severity.toLower();
    if (normalized.contains(QStringLiteral("fatal"))
        || normalized.contains(QStringLiteral("error"))) {
        return Diagnostic::Severity::Error;
    }
    if (normalized.contains(QStringLiteral("warn"))) {
        return Diagnostic::Severity::Warning;
    }
    return Diagnostic::Severity::Info;
}

void collectDiagnostics(const QJsonValue &value, QList<Diagnostic> &diagnostics)
{
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            collectDiagnostics(entry, diagnostics);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    const QString severity =
        firstNonEmpty(object,
                      {QStringLiteral("severity"),
                       QStringLiteral("level"),
                       QStringLiteral("kind")});
    const QString message =
        firstNonEmpty(object,
                      {QStringLiteral("message"),
                       QStringLiteral("formattedMessage"),
                       QStringLiteral("text")});
    if (!severity.isEmpty() && !message.isEmpty()) {
        const QJsonObject location = object.value(QStringLiteral("location")).toObject();
        diagnostics.append(makeDiagnostic(
            severityFromText(severity),
            firstNonEmpty(object,
                          {QStringLiteral("code"),
                           QStringLiteral("optionName"),
                           QStringLiteral("option")}),
            message,
            firstNonEmpty(location,
                          {QStringLiteral("fileName"),
                           QStringLiteral("file"),
                           QStringLiteral("path")})
                .isEmpty()
                ? firstNonEmpty(object,
                                {QStringLiteral("fileName"),
                                 QStringLiteral("file"),
                                 QStringLiteral("path")})
                : firstNonEmpty(location,
                                {QStringLiteral("fileName"),
                                 QStringLiteral("file"),
                                 QStringLiteral("path")}),
            location.value(QStringLiteral("line")).toInt(
                object.value(QStringLiteral("line")).toInt()),
            location.value(QStringLiteral("column")).toInt(
                object.value(QStringLiteral("column")).toInt())));
        return;
    }

    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        collectDiagnostics(iterator.value(), diagnostics);
    }
}

bool parseJson(const QByteArray &contents,
               const QString &label,
               QJsonDocument &document,
               QList<Diagnostic> &diagnostics,
               const bool allowEmpty = false)
{
    if (contents.trimmed().isEmpty() && allowEmpty) {
        document = QJsonDocument(QJsonArray{});
        return true;
    }
    QJsonParseError error;
    document = QJsonDocument::fromJson(contents, &error);
    if (error.error == QJsonParseError::NoError
        && (document.isObject() || document.isArray())) {
        return true;
    }
    diagnostics.append(makeDiagnostic(
        Diagnostic::Severity::Error,
        QStringLiteral("semantic.slang.json"),
        QStringLiteral("Invalid %1 JSON at byte %2: %3")
            .arg(label)
            .arg(error.offset)
            .arg(error.errorString())));
    return false;
}

QStringList sortedSet(const QSet<QString> &values)
{
    QStringList result(values.begin(), values.end());
    std::sort(result.begin(), result.end());
    return result;
}

QString absoluteDeclaredPath(const QString &root, const QString &path)
{
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path)
                                      : QDir(root).absoluteFilePath(path);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList readDependencyFile(const QString &path, const QString &assetRoot)
{
    QStringList result;
    const QString text = QString::fromUtf8(readFile(path));
    for (QString line : text.split(u'\n', Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QString normalized = QDir::cleanPath(line);
        const QString relative = QDir(assetRoot).relativeFilePath(normalized);
        result.append(relative.startsWith(QStringLiteral("../")) ? normalized : relative);
    }
    result.removeDuplicates();
    std::sort(result.begin(), result.end());
    return result;
}

bool waitForProcess(QProcess &process,
                    const int timeoutMs,
                    const std::atomic_bool *cancelled,
                    bool &timedOut,
                    QByteArray &standardOutput,
                    QByteArray &standardError,
                    bool &outputTruncated)
{
    const auto drain = [&] {
        const auto appendBounded =
            [&outputTruncated](QByteArray &target, const QByteArray &bytes) {
                const qsizetype remaining =
                    qMax<qsizetype>(0, MaximumProcessOutput - target.size());
                target.append(bytes.first(qMin(bytes.size(), remaining)));
                outputTruncated |= bytes.size() > remaining;
            };
        appendBounded(standardOutput, process.readAllStandardOutput());
        appendBounded(standardError, process.readAllStandardError());
    };
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
        if (isCancelled(cancelled)) {
            process.kill();
            process.waitForFinished(3000);
            drain();
            return false;
        }
        if (timeoutMs > 0 && timer.elapsed() >= timeoutMs) {
            timedOut = true;
            process.kill();
            process.waitForFinished(3000);
            drain();
            return false;
        }
        process.waitForFinished(40);
        drain();
    }
    drain();
    return true;
}

} // namespace

SemanticResult SlangService::analyze(const Request &request,
                                     const std::atomic_bool *cancelled) const
{
    SemanticResult result;
    result.generation = request.generation;
    result.contentHash = request.asset.contentHash;

    if (isCancelled(cancelled)) {
        result.diagnostics.append(makeDiagnostic(Diagnostic::Severity::Info,
                                                 QStringLiteral("semantic.cancelled"),
                                                 QStringLiteral("Semantic analysis cancelled")));
        return result;
    }
    if (!supports(request.asset)) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Info,
            QStringLiteral("semantic.not-applicable"),
            QStringLiteral("Slang analysis is not applicable to this asset")));
        return result;
    }

    const QString executable = locateExecutable(request.executable);
    if (executable.isEmpty()) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("semantic.slang.unavailable"),
            QStringLiteral("Slang executable was not found. Set XIPS_SLANG or install slang in PATH.")));
        return result;
    }
    result.available = true;
    result.engineVersion = queryVersion(executable, std::min(request.timeoutMs, 10000));

    QTemporaryDir temporary(QStringLiteral("xips-slang-XXXXXX"));
    if (!temporary.isValid()) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("semantic.temp"),
            QStringLiteral("Cannot create a temporary directory for Slang output")));
        return result;
    }

    const QString astPath = temporary.filePath(QStringLiteral("ast.json"));
    const QString cstPath = temporary.filePath(QStringLiteral("cst.json"));
    const QString diagnosticsPath = temporary.filePath(QStringLiteral("diagnostics.json"));
    const QString dependenciesPath = temporary.filePath(QStringLiteral("includes.d"));

    QStringList arguments{
        QStringLiteral("--quiet"),
        QStringLiteral("--ast-json"),
        astPath,
        QStringLiteral("--ast-json-source-info"),
        QStringLiteral("--ast-json-detailed-types"),
        QStringLiteral("--cst-json"),
        cstPath,
        QStringLiteral("--cst-json-mode"),
        QStringLiteral("no-whitespace"),
        QStringLiteral("--diag-json"),
        diagnosticsPath,
        QStringLiteral("--include-deps"),
        dependenciesPath,
        QStringLiteral("--depfile-sort"),
    };
    for (const QString &includeDirectory : request.asset.manifest.includeDirs) {
        arguments.append(QStringLiteral("-I"));
        arguments.append(absoluteDeclaredPath(request.asset.assetRoot, includeDirectory));
    }
    for (const QString &define : request.asset.manifest.defines) {
        arguments.append(QStringLiteral("-D"));
        arguments.append(define);
    }
    for (const QString &source : request.asset.manifest.sources) {
        const QString suffix = QFileInfo(source).suffix().toLower();
        if (suffix == QStringLiteral("sv") || suffix == QStringLiteral("svh")
            || suffix == QStringLiteral("v") || suffix == QStringLiteral("vh")) {
            arguments.append(absoluteDeclaredPath(request.asset.assetRoot, source));
        }
    }

    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setWorkingDirectory(request.asset.assetRoot);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Error,
            QStringLiteral("semantic.slang.start"),
            QStringLiteral("Cannot start Slang: %1").arg(process.errorString())));
        return result;
    }

    bool timedOut = false;
    bool outputTruncated = false;
    QByteArray standardOutput;
    QByteArray standardError;
    if (!waitForProcess(process,
                        request.timeoutMs,
                        cancelled,
                        timedOut,
                        standardOutput,
                        standardError,
                        outputTruncated)) {
        result.diagnostics.append(makeDiagnostic(
            timedOut ? Diagnostic::Severity::Error : Diagnostic::Severity::Info,
            timedOut ? QStringLiteral("semantic.slang.timeout")
                     : QStringLiteral("semantic.cancelled"),
            timedOut ? QStringLiteral("Slang analysis timed out after %1 ms").arg(request.timeoutMs)
                     : QStringLiteral("Slang analysis cancelled")));
        return result;
    }

    result = parseArtifacts(readFile(astPath),
                            readFile(cstPath),
                            readFile(diagnosticsPath),
                            readDependencyFile(dependenciesPath, request.asset.assetRoot),
                            request.asset.manifest.defines,
                            result.engineVersion,
                            request.generation,
                            request.asset.contentHash);
    result.available = true;
    if (outputTruncated) {
        result.diagnostics.append(makeDiagnostic(
            Diagnostic::Severity::Warning,
            QStringLiteral("semantic.slang.output-truncated"),
            QStringLiteral("Slang console output exceeded the 512 KiB capture limit")));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.success = false;
        if (result.diagnostics.isEmpty()) {
            result.diagnostics.append(makeDiagnostic(
                Diagnostic::Severity::Error,
                QStringLiteral("semantic.slang.exit"),
                QStringLiteral("Slang exited with code %1: %2")
                    .arg(process.exitCode())
                    .arg(QString::fromUtf8(standardError).trimmed())));
        }
    }
    return result;
}

SemanticResult SlangService::parseArtifacts(const QByteArray &astJson,
                                            const QByteArray &cstJson,
                                            const QByteArray &diagnosticsJson,
                                            const QStringList &includeDependencies,
                                            const QStringList &commandLineDefines,
                                            const QString &engineVersion,
                                            const qint64 generation,
                                            const QString &contentHash)
{
    SemanticResult result;
    result.available = true;
    result.engineVersion = engineVersion;
    result.generation = generation;
    result.contentHash = contentHash;

    QJsonDocument astDocument;
    QJsonDocument cstDocument;
    QJsonDocument diagnosticsDocument;
    const bool astValid =
        parseJson(astJson, QStringLiteral("Slang AST"), astDocument, result.diagnostics);
    const bool cstValid =
        parseJson(cstJson, QStringLiteral("Slang CST"), cstDocument, result.diagnostics, true);
    const bool diagnosticsValid = parseJson(diagnosticsJson,
                                            QStringLiteral("Slang diagnostics"),
                                            diagnosticsDocument,
                                            result.diagnostics,
                                            true);

    if (astValid) {
        AstCollector collector;
        collector.visit(astDocument.isObject() ? QJsonValue(astDocument.object())
                                               : QJsonValue(astDocument.array()));
        QStringList unitNames = collector.units.keys();
        std::sort(unitNames.begin(), unitNames.end());
        for (const QString &unitName : unitNames) {
            SemanticUnit unit = collector.units.value(unitName);
            if (unit.kind.isEmpty()) {
                unit.kind = collector.definitionKinds.value(unitName, QStringLiteral("module"));
            }
            std::sort(unit.ports.begin(),
                      unit.ports.end(),
                      [](const SemanticPort &left, const SemanticPort &right) {
                          return left.name < right.name;
                      });
            std::sort(unit.parameters.begin(),
                      unit.parameters.end(),
                      [](const SemanticParameter &left, const SemanticParameter &right) {
                          return left.name < right.name;
                      });
            std::sort(unit.imports.begin(), unit.imports.end());
            std::sort(unit.instances.begin(), unit.instances.end());
            result.units.append(unit);
        }

        QSet<QString> tops = collector.rootInstances;
        if (tops.isEmpty()) {
            for (auto iterator = collector.definitionKinds.constBegin();
                 iterator != collector.definitionKinds.constEnd();
                 ++iterator) {
                if (!collector.instantiatedDefinitions.contains(iterator.key())) {
                    tops.insert(iterator.key());
                }
            }
        }
        result.topCandidates = sortedSet(tops);
    }

    QSet<QString> includes(includeDependencies.begin(), includeDependencies.end());
    QSet<QString> defines(commandLineDefines.begin(), commandLineDefines.end());
    if (cstValid) {
        collectCstFacts(cstDocument.isObject() ? QJsonValue(cstDocument.object())
                                              : QJsonValue(cstDocument.array()),
                        includes,
                        defines);
    }
    result.includes = sortedSet(includes);
    result.defines = sortedSet(defines);

    if (diagnosticsValid) {
        collectDiagnostics(diagnosticsDocument.isObject()
                               ? QJsonValue(diagnosticsDocument.object())
                               : QJsonValue(diagnosticsDocument.array()),
                           result.diagnostics);
    }
    result.success = astValid && cstValid && diagnosticsValid;
    for (const Diagnostic &diagnostic : result.diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::Error) {
            result.success = false;
            break;
        }
    }
    return result;
}

QString SlangService::locateExecutable(const QString &explicitPath)
{
    if (!explicitPath.isEmpty() && QFileInfo(explicitPath).isFile()) {
        return QFileInfo(explicitPath).absoluteFilePath();
    }
    const QString environment = qEnvironmentVariable("XIPS_SLANG");
    if (!environment.isEmpty() && QFileInfo(environment).isFile()) {
        return QFileInfo(environment).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("slang"));
}

bool SlangService::supports(const AssetRecord &asset)
{
    if (asset.manifest.type == AssetType::CodeBlock) {
        return false;
    }
    for (const QString &source : asset.manifest.sources) {
        const QString suffix = QFileInfo(source).suffix().toLower();
        if (suffix == QStringLiteral("sv") || suffix == QStringLiteral("svh")
            || suffix == QStringLiteral("v") || suffix == QStringLiteral("vh")) {
            return true;
        }
    }
    return false;
}

QString SlangService::queryVersion(const QString &executable, const int timeoutMs)
{
    QProcess process;
    process.start(executable, {QStringLiteral("--version")});
    if (!process.waitForStarted(std::min(timeoutMs, 5000))
        || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return QStringLiteral("unknown");
    }
    QString version = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (version.isEmpty()) {
        version = QString::fromUtf8(process.readAllStandardError()).trimmed();
    }
    return version.isEmpty() ? QStringLiteral("unknown") : version;
}

} // namespace xips
