#include "dependency/DependencyResolver.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace xips {
namespace {

struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    QString prerelease;
    bool valid = false;
};

SemanticVersion parseVersion(QString value)
{
    value = value.trimmed();
    if (value.startsWith(u'v', Qt::CaseInsensitive)) {
        value.remove(0, 1);
    }
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$)"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch()) {
        return {};
    }
    return SemanticVersion{
        .major = match.captured(1).toInt(),
        .minor = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(),
        .patch = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt(),
        .prerelease = match.captured(4),
        .valid = true,
    };
}

int compareVersions(const SemanticVersion &left, const SemanticVersion &right)
{
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    if (left.prerelease == right.prerelease) {
        return 0;
    }
    if (left.prerelease.isEmpty()) {
        return 1;
    }
    if (right.prerelease.isEmpty()) {
        return -1;
    }
    return QString::compare(left.prerelease, right.prerelease, Qt::CaseInsensitive);
}

bool compareOperator(const SemanticVersion &version,
                     const SemanticVersion &required,
                     const QString &operation)
{
    const int comparison = compareVersions(version, required);
    if (operation == QStringLiteral(">=")) {
        return comparison >= 0;
    }
    if (operation == QStringLiteral("<=")) {
        return comparison <= 0;
    }
    if (operation == QStringLiteral(">")) {
        return comparison > 0;
    }
    if (operation == QStringLiteral("<")) {
        return comparison < 0;
    }
    if (operation == QStringLiteral("=") || operation.isEmpty()) {
        return comparison == 0;
    }
    return false;
}

bool matchesSingle(const SemanticVersion &version, QString constraint, QString *error)
{
    constraint = constraint.trimmed();
    if (constraint.isEmpty() || constraint == QStringLiteral("*")
        || constraint.compare(QStringLiteral("latest"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    if (constraint.startsWith(u'^') || constraint.startsWith(u'~')) {
        const QChar kind = constraint.front();
        const SemanticVersion lower = parseVersion(constraint.mid(1));
        if (!lower.valid) {
            if (error) {
                *error = QStringLiteral("Invalid version constraint '%1'").arg(constraint);
            }
            return false;
        }
        SemanticVersion upper = lower;
        if (kind == u'^') {
            if (lower.major > 0) {
                upper.major += 1;
                upper.minor = 0;
                upper.patch = 0;
            } else {
                upper.minor += 1;
                upper.patch = 0;
            }
        } else {
            upper.minor += 1;
            upper.patch = 0;
        }
        return compareVersions(version, lower) >= 0 && compareVersions(version, upper) < 0;
    }

    if (constraint.contains(u'x', Qt::CaseInsensitive) || constraint.contains(u'*')) {
        const QStringList parts = constraint.split(u'.');
        const QStringList versionParts{
            QString::number(version.major),
            QString::number(version.minor),
            QString::number(version.patch),
        };
        for (qsizetype index = 0; index < parts.size() && index < versionParts.size(); ++index) {
            const QString part = parts.at(index);
            if (part.compare(QStringLiteral("x"), Qt::CaseInsensitive) == 0
                || part == QStringLiteral("*")) {
                continue;
            }
            bool integer = false;
            const int expected = part.toInt(&integer);
            if (!integer || expected != versionParts.at(index).toInt()) {
                return false;
            }
        }
        return true;
    }

    QString operation;
    for (const QString &candidate :
         {QStringLiteral(">="),
          QStringLiteral("<="),
          QStringLiteral(">"),
          QStringLiteral("<"),
          QStringLiteral("=")}) {
        if (constraint.startsWith(candidate)) {
            operation = candidate;
            constraint.remove(0, candidate.size());
            break;
        }
    }
    const SemanticVersion required = parseVersion(constraint.trimmed());
    if (!required.valid) {
        if (error) {
            *error = QStringLiteral("Invalid version constraint '%1'").arg(constraint);
        }
        return false;
    }
    return compareOperator(version, required, operation);
}

QStringList splitAndConstraints(QString expression)
{
    expression.replace(u',', u' ');
    return expression.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

bool matchesClause(const SemanticVersion &version, const QString &clause, QString *error)
{
    const QStringList constraints = splitAndConstraints(clause);
    for (const QString &constraint : constraints) {
        if (!matchesSingle(version, constraint, error)) {
            return false;
        }
    }
    return true;
}

QString issueKindName(const DependencyIssueKind kind)
{
    switch (kind) {
    case DependencyIssueKind::Missing:
        return QStringLiteral("missing dependency");
    case DependencyIssueKind::Cycle:
        return QStringLiteral("dependency cycle");
    case DependencyIssueKind::VersionConflict:
        return QStringLiteral("version conflict");
    case DependencyIssueKind::ToolConflict:
        return QStringLiteral("tool compatibility conflict");
    case DependencyIssueKind::DuplicateAsset:
        return QStringLiteral("duplicate asset");
    }
    return QStringLiteral("dependency issue");
}

} // namespace

bool DependencyResolution::hasErrors() const
{
    return std::any_of(issues.cbegin(), issues.cend(), [](const DependencyIssue &issue) {
        return issue.severity == Diagnostic::Severity::Error;
    });
}

QStringList DependencyResolution::orderedIds() const
{
    QStringList result;
    result.reserve(orderedAssets.size());
    for (const AssetRecord &asset : orderedAssets) {
        result.append(asset.manifest.id);
    }
    return result;
}

bool VersionConstraint::matches(const QString &version,
                                const QString &constraint,
                                QString *error)
{
    const SemanticVersion parsedVersion = parseVersion(version);
    if (!parsedVersion.valid) {
        if (error) {
            *error = QStringLiteral("Invalid semantic version '%1'").arg(version);
        }
        return false;
    }

    const QStringList clauses = constraint.split(QStringLiteral("||"), Qt::SkipEmptyParts);
    if (clauses.isEmpty()) {
        return true;
    }
    QString firstError;
    for (const QString &clause : clauses) {
        QString clauseError;
        if (matchesClause(parsedVersion, clause, &clauseError)) {
            return true;
        }
        if (firstError.isEmpty()) {
            firstError = clauseError;
        }
    }
    if (error && !firstError.isEmpty()) {
        *error = firstError;
    }
    return false;
}

int VersionConstraint::compare(const QString &left, const QString &right, bool *valid)
{
    const SemanticVersion parsedLeft = parseVersion(left);
    const SemanticVersion parsedRight = parseVersion(right);
    const bool bothValid = parsedLeft.valid && parsedRight.valid;
    if (valid) {
        *valid = bothValid;
    }
    if (!bothValid) {
        return QString::compare(left, right, Qt::CaseInsensitive);
    }
    return compareVersions(parsedLeft, parsedRight);
}

DependencyResolution DependencyResolver::resolve(const QList<AssetRecord> &catalog,
                                                 const QStringList &rootAssetIds,
                                                 const QJsonObject &targetTools) const
{
    DependencyResolution result;
    QHash<QString, AssetRecord> assets;
    for (const AssetRecord &asset : catalog) {
        const QString id = asset.manifest.id;
        if (assets.contains(id)) {
            result.issues.append(DependencyIssue{
                .kind = DependencyIssueKind::DuplicateAsset,
                .severity = Diagnostic::Severity::Error,
                .assetId = id,
                .dependencyId = id,
                .message = QStringLiteral("Asset id '%1' appears more than once in the catalog")
                               .arg(id),
            });
            continue;
        }
        assets.insert(id, asset);
    }

    QHash<QString, int> state;
    QStringList stack;
    QSet<QString> emittedCycles;

    const auto addIssue = [&result](const DependencyIssueKind kind,
                                    const Diagnostic::Severity severity,
                                    const QString &assetId,
                                    const QString &dependencyId,
                                    const QString &message,
                                    const QStringList &path = {}) {
        result.issues.append(DependencyIssue{
            .kind = kind,
            .severity = severity,
            .assetId = assetId,
            .dependencyId = dependencyId,
            .message = message,
            .path = path,
        });
    };

    std::function<void(const QString &, const QString &, const DependencySpec *)> visit;
    visit = [&](const QString &id, const QString &requester, const DependencySpec *requirement) {
        if (!assets.contains(id)) {
            const bool optional = requirement && requirement->optional;
            addIssue(DependencyIssueKind::Missing,
                     optional ? Diagnostic::Severity::Warning : Diagnostic::Severity::Error,
                     requester,
                     id,
                     QStringLiteral("%1 '%2' required by '%3'")
                         .arg(optional ? QStringLiteral("Optional dependency")
                                       : QStringLiteral("Dependency"),
                              id,
                              requester),
                     stack);
            return;
        }

        const AssetRecord &asset = assets.value(id);
        if (requirement && !requirement->versionConstraint.isEmpty()) {
            QString constraintError;
            if (!VersionConstraint::matches(asset.manifest.version,
                                            requirement->versionConstraint,
                                            &constraintError)) {
                addIssue(
                    DependencyIssueKind::VersionConflict,
                    Diagnostic::Severity::Error,
                    requester,
                    id,
                    QStringLiteral("Asset '%1' version '%2' does not satisfy '%3' requested by '%4'%5")
                        .arg(id,
                             asset.manifest.version,
                             requirement->versionConstraint,
                             requester,
                             constraintError.isEmpty()
                                 ? QString()
                                 : QStringLiteral(": ") + constraintError),
                    stack);
            }
        }

        if (state.value(id) == 1) {
            const qsizetype start = stack.indexOf(id);
            QStringList cycle = start >= 0 ? stack.mid(start) : stack;
            cycle.append(id);
            const QString key = cycle.join(QStringLiteral("->"));
            if (!emittedCycles.contains(key)) {
                emittedCycles.insert(key);
                addIssue(DependencyIssueKind::Cycle,
                         Diagnostic::Severity::Error,
                         requester,
                         id,
                         QStringLiteral("Dependency cycle: %1")
                             .arg(cycle.join(QStringLiteral(" -> "))),
                         cycle);
            }
            return;
        }
        if (state.value(id) == 2) {
            return;
        }

        state.insert(id, 1);
        stack.append(id);

        if (!targetTools.isEmpty()) {
            QStringList tools = asset.manifest.tools.keys();
            std::sort(tools.begin(), tools.end());
            for (const QString &tool : tools) {
                const QString constraint =
                    asset.manifest.tools.value(tool).toVariant().toString();
                if (!targetTools.contains(tool)) {
                    addIssue(DependencyIssueKind::ToolConflict,
                             Diagnostic::Severity::Error,
                             id,
                             tool,
                             QStringLiteral("Target does not declare required tool '%1' (%2)")
                                 .arg(tool, constraint),
                             stack);
                    continue;
                }
                const QString targetVersion = targetTools.value(tool).toVariant().toString();
                if (!constraint.isEmpty()
                    && !VersionConstraint::matches(targetVersion, constraint)) {
                    addIssue(
                        DependencyIssueKind::ToolConflict,
                        Diagnostic::Severity::Error,
                        id,
                        tool,
                        QStringLiteral("Target tool '%1' version '%2' does not satisfy '%3' for '%4'")
                            .arg(tool, targetVersion, constraint, id),
                        stack);
                }
            }
        }

        QList<DependencySpec> dependencies = asset.manifest.dependencies;
        std::sort(dependencies.begin(),
                  dependencies.end(),
                  [](const DependencySpec &left, const DependencySpec &right) {
                      if (left.id != right.id) {
                          return left.id < right.id;
                      }
                      if (left.versionConstraint != right.versionConstraint) {
                          return left.versionConstraint < right.versionConstraint;
                      }
                      return left.optional < right.optional;
                  });
        QStringList graphEdges;
        for (const DependencySpec &dependency : dependencies) {
            graphEdges.append(dependency.id);
            visit(dependency.id, id, &dependency);
        }
        graphEdges.removeDuplicates();
        std::sort(graphEdges.begin(), graphEdges.end());
        result.graph.insert(id, graphEdges);

        stack.removeLast();
        state.insert(id, 2);
        result.orderedAssets.append(asset);
    };

    QStringList roots = rootAssetIds;
    roots.removeDuplicates();
    std::sort(roots.begin(), roots.end());
    for (const QString &root : roots) {
        visit(root, QStringLiteral("<root>"), nullptr);
    }

    std::sort(result.issues.begin(),
              result.issues.end(),
              [](const DependencyIssue &left, const DependencyIssue &right) {
                  if (left.severity != right.severity) {
                      return left.severity > right.severity;
                  }
                  if (left.kind != right.kind) {
                      return issueKindName(left.kind) < issueKindName(right.kind);
                  }
                  if (left.assetId != right.assetId) {
                      return left.assetId < right.assetId;
                  }
                  return left.dependencyId < right.dependencyId;
              });
    return result;
}

} // namespace xips
