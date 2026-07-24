#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace xips {

enum class AssetType {
    CodeBlock,
    Module,
    Ip,
    Unknown
};

enum class AssetOrigin {
    Managed,
    External
};

QString assetTypeToString(AssetType type);
AssetType assetTypeFromString(const QString &value);
QString assetOriginToString(AssetOrigin origin);

struct DependencySpec {
    QString id;
    QString versionConstraint;
    bool optional = false;
    QJsonObject extraFields;
};

struct SlotDefinition {
    QString name;
    QString type;
    QString defaultValue;
    QString description;
    bool required = true;
    QJsonObject extraFields;
};

struct TestCommandSpec {
    QString name;
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QJsonObject extraFields;
};

struct Manifest {
    int schemaVersion = 1;
    QString id;
    AssetType type = AssetType::Unknown;
    QString name;
    QString description;
    QString version;
    QString top;
    QString language = QStringLiteral("SystemVerilog");
    QStringList sources;
    QStringList includeDirs;
    QStringList defines;
    QStringList constraints;
    QList<DependencySpec> dependencies;
    QStringList tests;
    QList<TestCommandSpec> testCommands;
    QStringList tags;
    QStringList examples;
    QStringList documentation;
    QJsonObject tools;

    QString templateText;
    QStringList scope;
    QList<SlotDefinition> slotDefinitions;
    QStringList requiredSymbols;
    bool allowWorkspaceOverride = false;
    QJsonValue exampleInput;
    QJsonValue exampleOutput;

    QJsonObject rawObject;
};

struct Diagnostic {
    enum class Severity {
        Info,
        Warning,
        Error
    };

    Severity severity = Severity::Info;
    QString code;
    QString message;
    QString file;
    int line = 0;
    int column = 0;
};

QString diagnosticSeverityToString(Diagnostic::Severity severity);

struct SemanticPort {
    QString name;
    QString direction;
    QString type;
    QString packedDimensions;
    QString unpackedDimensions;
};

struct SemanticParameter {
    QString name;
    QString type;
    QString value;
    bool local = false;
};

struct SemanticUnit {
    QString name;
    QString kind;
    QString sourceFile;
    QList<SemanticPort> ports;
    QList<SemanticParameter> parameters;
    QStringList imports;
    QStringList instances;
};

struct SemanticResult {
    bool available = false;
    bool success = false;
    QString engineVersion;
    QList<SemanticUnit> units;
    QStringList includes;
    QStringList defines;
    QStringList topCandidates;
    QList<Diagnostic> diagnostics;
    qint64 generation = 0;
    QString contentHash;
};

struct TestResult {
    QString assetId;
    QString commandName;
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QString status = QStringLiteral("not-run");
    int exitCode = -1;
    QString exitStatus = QStringLiteral("not-started");
    QDateTime startedAt;
    qint64 durationMs = 0;
    QString standardOutput;
    QString standardError;
    QString gitCommit;
    QString contentHash;
    bool cancelled = false;
    bool logTruncated = false;

    [[nodiscard]] bool passed() const;
    [[nodiscard]] bool isStale(const QString &currentContentHash,
                               const QString &currentGitCommit = {}) const;
};

QJsonObject testResultToJson(const TestResult &result);
TestResult testResultFromJson(const QJsonObject &object);

struct AssetRecord {
    Manifest manifest;
    QString assetRoot;
    QString manifestPath;
    AssetOrigin origin = AssetOrigin::Managed;
    QString contentHash;
    qint64 generation = 0;
    bool stale = false;

    SemanticResult semantic;
    QString testStatus = QStringLiteral("untested");
    QString diagnosticsStatus = QStringLiteral("not-analyzed");
    QString modifiedStatus = QStringLiteral("unknown");
    QString sourceRepository;
    QDateTime lastUsed;
    QString gitCommit;
    QString gitTag;
};

struct ScanIssue {
    Diagnostic::Severity severity = Diagnostic::Severity::Info;
    QString assetId;
    QString path;
    QString message;
};

struct ScanResult {
    QList<AssetRecord> assets;
    QList<ScanIssue> issues;
    bool cancelled = false;
};

struct SearchHit {
    AssetRecord asset;
    QStringList matchedFields;
    double score = 0.0;
};

struct LibraryRoot {
    QString path;
    AssetOrigin origin = AssetOrigin::Managed;
};

} // namespace xips

Q_DECLARE_METATYPE(xips::AssetRecord)
Q_DECLARE_METATYPE(xips::ScanResult)
Q_DECLARE_METATYPE(QList<xips::AssetRecord>)
Q_DECLARE_METATYPE(QList<xips::SearchHit>)
Q_DECLARE_METATYPE(xips::TestResult)
