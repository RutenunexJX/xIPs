#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace xips {

// Older manifests may still contain code-block or module. The desktop
// application presents every entry as an IP, while preserving the original
// value when an existing manifest is read and written.
enum class AssetType {
    CodeBlock,
    Module,
    Ip,
    Unknown
};

QString assetTypeToString(AssetType type);
AssetType assetTypeFromString(const QString &value);

struct Manifest {
    int schemaVersion = 1;
    QString id;
    AssetType type = AssetType::Ip;
    QString name;
    QString description;
    QString version;
    QString top;
    QString language;
    QStringList sources;
    QStringList constraints;
    QStringList tags;
    QStringList documentation;
    QJsonObject tools;
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

struct AssetRecord {
    Manifest manifest;
    QString assetRoot;
    QString manifestPath;
    QString contentHash;
    qsizetype fileCount = 0;
    qint64 totalBytes = 0;
    QDateTime lastModified;
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

} // namespace xips

Q_DECLARE_METATYPE(xips::AssetRecord)
Q_DECLARE_METATYPE(xips::ScanResult)
Q_DECLARE_METATYPE(QList<xips::AssetRecord>)
Q_DECLARE_METATYPE(QList<xips::SearchHit>)
