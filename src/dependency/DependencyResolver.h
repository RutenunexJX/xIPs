#pragma once

#include "assetcore/Asset.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace xips {

enum class DependencyIssueKind {
    Missing,
    Cycle,
    VersionConflict,
    ToolConflict,
    DuplicateAsset
};

struct DependencyIssue {
    DependencyIssueKind kind = DependencyIssueKind::Missing;
    Diagnostic::Severity severity = Diagnostic::Severity::Error;
    QString assetId;
    QString dependencyId;
    QString message;
    QStringList path;
};

struct DependencyResolution {
    QList<AssetRecord> orderedAssets;
    QMap<QString, QStringList> graph;
    QList<DependencyIssue> issues;

    [[nodiscard]] bool hasErrors() const;
    [[nodiscard]] QStringList orderedIds() const;
};

class VersionConstraint {
public:
    [[nodiscard]] static bool matches(const QString &version,
                                      const QString &constraint,
                                      QString *error = nullptr);
    [[nodiscard]] static int compare(const QString &left,
                                     const QString &right,
                                     bool *valid = nullptr);
};

class DependencyResolver {
public:
    [[nodiscard]] DependencyResolution resolve(
        const QList<AssetRecord> &catalog,
        const QStringList &rootAssetIds,
        const QJsonObject &targetTools = {}) const;
};

} // namespace xips
