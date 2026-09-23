#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace xips {

struct Manifest {
    QString id;
    QString name;
    QString description;
    QString version;
    QStringList tags;
    QJsonObject rawObject;
};

struct AssetRecord {
    Manifest manifest;
    QString assetRoot;
    QString manifestPath;
    QStringList files;
    qsizetype fileCount = 0;
    qint64 totalBytes = 0;
    QDateTime lastModified;
};

struct ScanResult {
    QList<AssetRecord> assets;
    // Every ID from a manifest that passed schema validation, including IDs
    // later excluded from assets because their case-insensitive identity is
    // ambiguous. Mutating callers use this list to avoid creating another
    // colliding asset.
    QStringList discoveredAssetIds;
    QStringList errors;
    bool cancelled = false;
};

struct SearchHit {
    AssetRecord asset;
    double score = 0.0;
    QString matchedFile;
};

} // namespace xips
