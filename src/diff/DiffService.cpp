#include "diff/DiffService.h"

#include "assetcore/JsonUtil.h"
#include "manifest/ManifestService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QSet>

#include <algorithm>

namespace xips {
namespace {

QString hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("<missing>");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

void collectDirectory(const QString &directory, QStringList &files)
{
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        if (entry.isDir()) {
            collectDirectory(entry.absoluteFilePath(), files);
        } else if (entry.isFile()) {
            files.append(entry.absoluteFilePath());
        }
    }
}

QString absolutePath(const AssetRecord &asset, const QString &path)
{
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path)
                                      : QDir(asset.assetRoot).absoluteFilePath(path);
}

QString displayPath(const AssetRecord &asset, const QString &absolute)
{
    QString relative = QDir(asset.assetRoot).relativeFilePath(absolute);
    relative = QDir::cleanPath(relative);
    relative.replace(u'\\', u'/');
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))
        || QDir::isAbsolutePath(relative)) {
        return QFileInfo(absolute).absoluteFilePath();
    }
    return relative;
}

void flattenJson(const QJsonObject &object,
                 const QString &prefix,
                 QMap<QString, QString> &values)
{
    QStringList keys = object.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        const QString path = prefix.isEmpty() ? key : prefix + u'.' + key;
        if (value.isObject()) {
            flattenJson(value.toObject(), path, values);
        } else {
            values.insert(path, QString::fromUtf8(json::canonicalJson(value)));
        }
    }
}

QMap<QString, QString> manifestValues(const Manifest &manifest)
{
    QMap<QString, QString> values;
    flattenJson(ManifestService().toJson(manifest), {}, values);
    values.remove(QStringLiteral("dependencies"));
    return values;
}

QMap<QString, QString> dependencyValues(const Manifest &manifest)
{
    QMap<QString, QString> values;
    for (const DependencySpec &dependency : manifest.dependencies) {
        values.insert(
            dependency.id,
            QStringLiteral("version=%1;optional=%2")
                .arg(dependency.versionConstraint,
                     dependency.optional ? QStringLiteral("true")
                                         : QStringLiteral("false")));
    }
    return values;
}

QMap<QString, QString> semanticValues(const SemanticResult &semantic)
{
    QMap<QString, QString> values;
    for (const SemanticUnit &unit : semantic.units) {
        const QString unitKey =
            QStringLiteral("unit/%1/%2").arg(unit.kind, unit.name);
        values.insert(unitKey, unit.sourceFile);
        for (const SemanticPort &port : unit.ports) {
            values.insert(
                QStringLiteral("port/%1/%2").arg(unit.name, port.name),
                QStringLiteral("%1;%2;%3;%4")
                    .arg(port.direction,
                         port.type,
                         port.packedDimensions,
                         port.unpackedDimensions));
        }
        for (const SemanticParameter &parameter : unit.parameters) {
            values.insert(
                QStringLiteral("parameter/%1/%2").arg(unit.name, parameter.name),
                QStringLiteral("%1;%2;%3")
                    .arg(parameter.local ? QStringLiteral("localparam")
                                         : QStringLiteral("parameter"),
                         parameter.type,
                         parameter.value));
        }
        QStringList imports = unit.imports;
        QStringList instances = unit.instances;
        std::sort(imports.begin(), imports.end());
        std::sort(instances.begin(), instances.end());
        values.insert(QStringLiteral("imports/%1").arg(unit.name), imports.join(u';'));
        values.insert(QStringLiteral("instances/%1").arg(unit.name), instances.join(u';'));
    }
    QStringList includes = semantic.includes;
    QStringList defines = semantic.defines;
    QStringList tops = semantic.topCandidates;
    std::sort(includes.begin(), includes.end());
    std::sort(defines.begin(), defines.end());
    std::sort(tops.begin(), tops.end());
    values.insert(QStringLiteral("includes"), includes.join(u';'));
    values.insert(QStringLiteral("defines"), defines.join(u';'));
    values.insert(QStringLiteral("topCandidates"), tops.join(u';'));
    return values;
}

void appendDifferences(const QMap<QString, QString> &before,
                       const QMap<QString, QString> &after,
                       const DifferenceCategory category,
                       QList<DifferenceEntry> &result)
{
    QSet<QString> keys(before.keyBegin(), before.keyEnd());
    keys.unite(QSet<QString>(after.keyBegin(), after.keyEnd()));
    QStringList ordered(keys.begin(), keys.end());
    std::sort(ordered.begin(), ordered.end());
    for (const QString &key : ordered) {
        if (!before.contains(key)) {
            result.append(DifferenceEntry{
                .category = category,
                .kind = DifferenceKind::Added,
                .key = key,
                .after = after.value(key),
            });
        } else if (!after.contains(key)) {
            result.append(DifferenceEntry{
                .category = category,
                .kind = DifferenceKind::Removed,
                .key = key,
                .before = before.value(key),
            });
        } else if (before.value(key) != after.value(key)) {
            result.append(DifferenceEntry{
                .category = category,
                .kind = DifferenceKind::Modified,
                .key = key,
                .before = before.value(key),
                .after = after.value(key),
            });
        }
    }
}

} // namespace

QString differenceCategoryToString(const DifferenceCategory category)
{
    switch (category) {
    case DifferenceCategory::File:
        return QStringLiteral("file");
    case DifferenceCategory::Manifest:
        return QStringLiteral("manifest");
    case DifferenceCategory::Semantic:
        return QStringLiteral("semantic");
    case DifferenceCategory::Dependency:
        return QStringLiteral("dependency");
    }
    return QStringLiteral("file");
}

QString differenceKindToString(const DifferenceKind kind)
{
    switch (kind) {
    case DifferenceKind::Added:
        return QStringLiteral("added");
    case DifferenceKind::Removed:
        return QStringLiteral("removed");
    case DifferenceKind::Modified:
        return QStringLiteral("modified");
    }
    return QStringLiteral("modified");
}

bool AssetDifference::identical() const
{
    return entries.isEmpty();
}

QMap<DifferenceCategory, int> AssetDifference::counts() const
{
    QMap<DifferenceCategory, int> result;
    for (const DifferenceEntry &entry : entries) {
        result[entry.category] += 1;
    }
    return result;
}

QMap<QString, QString> DiffService::fileHashes(const AssetRecord &asset)
{
    QStringList paths = asset.manifest.sources;
    paths.append(asset.manifest.constraints);
    paths.append(asset.manifest.tests);
    paths.append(asset.manifest.examples);
    paths.append(asset.manifest.documentation);

    QStringList absoluteFiles;
    if (!asset.manifestPath.isEmpty()) {
        absoluteFiles.append(QFileInfo(asset.manifestPath).absoluteFilePath());
    }
    for (const QString &path : paths) {
        absoluteFiles.append(absolutePath(asset, path));
    }
    for (const QString &includeDirectory : asset.manifest.includeDirs) {
        const QString directory = absolutePath(asset, includeDirectory);
        if (QFileInfo(directory).isDir()) {
            collectDirectory(directory, absoluteFiles);
        }
    }
    absoluteFiles.removeDuplicates();
    std::sort(absoluteFiles.begin(), absoluteFiles.end());

    QMap<QString, QString> result;
    for (const QString &absolute : absoluteFiles) {
        result.insert(displayPath(asset, absolute), hashFile(absolute));
    }
    return result;
}

AssetDifference DiffService::compare(const AssetRecord &before,
                                     const AssetRecord &after) const
{
    AssetDifference result;
    result.beforeId = before.manifest.id;
    result.afterId = after.manifest.id;
    result.beforeContentHash = before.contentHash;
    result.afterContentHash = after.contentHash;
    appendDifferences(fileHashes(before),
                      fileHashes(after),
                      DifferenceCategory::File,
                      result.entries);
    appendDifferences(manifestValues(before.manifest),
                      manifestValues(after.manifest),
                      DifferenceCategory::Manifest,
                      result.entries);
    appendDifferences(semanticValues(before.semantic),
                      semanticValues(after.semantic),
                      DifferenceCategory::Semantic,
                      result.entries);
    appendDifferences(dependencyValues(before.manifest),
                      dependencyValues(after.manifest),
                      DifferenceCategory::Dependency,
                      result.entries);
    return result;
}

} // namespace xips
