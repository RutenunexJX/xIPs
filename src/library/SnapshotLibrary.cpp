#include "SnapshotLibrary.h"

#include "AssetLibraryService.h"
#include "AssetScanner.h"
#include "CatalogIndex.h"
#include "FileSystemUtil.h"
#include "OperationControl.h"
#include "manifest/ManifestService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

namespace xips
{
namespace
{
struct Failure
{
    QString message;
};
void require(bool condition, const QString &message)
{
    if (!condition)
        throw Failure{message};
}
QString absolute(const QString &path)
{
    return files::normalizedAbsolute(path);
}
QString child(const QString &root, const QString &relative)
{
    return QDir(root).absoluteFilePath(relative);
}
QString unique(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}
void safePath(const QString &path)
{
    QString current = absolute(path);
    for (;;)
    {
        const QFileInfo info(current);
        require(!files::isLinkLike(info), QStringLiteral("Path contains a link: %1").arg(current));
        const QString parent = info.absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
}
void existingDirectory(const QString &path)
{
    safePath(path);
    require(QFileInfo(path).isDir(), QStringLiteral("Directory unavailable: %1").arg(path));
}
QByteArray bytes(const QString &path)
{
    safePath(path);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read: %1").arg(path));
    const QByteArray value = file.readAll();
    require(file.error() == QFileDevice::NoError, QStringLiteral("Read failed: %1").arg(path));
    return value;
}
QJsonObject readJson(const QString &path)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes(path), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            QStringLiteral("Invalid metadata: %1").arg(path));
    return document.object();
}
QStringList strings(const QJsonArray &array)
{
    QStringList result;
    for (const auto &item : array)
    {
        require(item.isString(), QStringLiteral("Invalid file list"));
        result.append(item.toString());
    }
    return result;
}
bool validCategory(const QString &category)
{
    return QStringList{QStringLiteral("module"), QStringLiteral("ip"), QStringLiteral("project"), QStringLiteral("artifact"),
                       QStringLiteral("other")}
        .contains(category);
}
bool validRevisionLabel(const QString &label)
{
    return !label.isEmpty() && label == label.trimmed() && label.size() <= 128 &&
        std::none_of(label.cbegin(), label.cend(), [](QChar c)
        { return c.category() == QChar::Other_Control || c.category() == QChar::Separator_Line ||
                 c.category() == QChar::Separator_Paragraph; });
}
QString checkedRevisionLabel(const QString &label)
{
    const auto name = label.trimmed();
    require(validRevisionLabel(name), QStringLiteral("Enter a version name of 1 to 128 characters without line breaks or control characters."));
    return name;
}
void setRevisionLabel(QJsonObject &document, const Snapshot &snapshot, const QString &name,
                      const QList<Snapshot> &others = {})
{
    for (const auto &other : others)
        require(other.id == snapshot.id || SnapshotLibrary::revisionLabel(other).compare(name, Qt::CaseInsensitive) != 0,
                QStringLiteral("Another version already uses this name."));
    auto labels = document.value("revisionLabels").toObject();
    auto defaultSnapshot = snapshot; defaultSnapshot.label.clear();
    if (name == SnapshotLibrary::revisionLabel(defaultSnapshot)) labels.remove(snapshot.id);
    else labels.insert(snapshot.id, name);
    if (labels.isEmpty()) document.remove("revisionLabels");
    else document.insert("revisionLabels", labels);
}
Snapshot parseSnapshot(const QJsonObject &object)
{
    Snapshot result{object.value("id").toString(), object.value("note").toString(),
                    QDateTime::fromString(object.value("created").toString(), Qt::ISODateWithMs),
                    object.value("hash").toString(), strings(object.value("files").toArray())};
    bool numeric = false;
    const qlonglong sequence = result.id.toLongLong(&numeric);
    const bool contentAddressed = object.contains("objects");
    result.sequence = contentAddressed ? object.value("sequence").toInteger() : sequence;
    result.parents = strings(object.value("parents").toArray());
    require((contentAddressed ? QUuid(result.id).toString(QUuid::WithoutBraces) == result.id &&
                                  !QUuid(result.id).isNull() && result.sequence > 0
                             : numeric && sequence > 0 && QString::number(sequence) == result.id) &&
                result.created.isValid() && result.hash.startsWith("sha256:") &&
                !result.files.isEmpty(),
            QStringLiteral("Invalid revision record"));
    QSet<QString> seen;
    for (const QString &file : result.files)
    {
        require(!file.isEmpty() && !QDir::isAbsolutePath(file) && QDir::cleanPath(file) == file &&
                    !file.startsWith("../") && file != ".." && file != "." && !file.contains('\\') &&
                    !file.contains(':') && !seen.contains(file.toCaseFolded()),
                QStringLiteral("Invalid revision file path: %1").arg(file));
        seen.insert(file.toCaseFolded());
    }
    if (contentAddressed)
    {
        const auto objects = object.value("objects").toObject();
        require(objects.size() == result.files.size(), QStringLiteral("Incomplete content map"));
        for (const auto &file : result.files)
        {
            const auto record = objects.value(file).toObject();
            ContentObject ref{record.value("hash").toString(), record.value("size").toInteger(-1)};
            require(ContentStore::validHash(ref.hash) && ref.size >= 0,
                    QStringLiteral("Invalid content reference: %1").arg(file));
            result.objects.insert(file, ref);
        }
        require(result.objects.keys() == result.files && ContentStore::treeHash(result.objects) == result.hash,
                QStringLiteral("Version manifest hash mismatch"));
    }
    return result;
}
QJsonObject toJson(const Snapshot &snapshot)
{
    QJsonObject result{{"id", snapshot.id},
            {"note", snapshot.note},
            {"created", snapshot.created.toUTC().toString(Qt::ISODateWithMs)},
            {"hash", snapshot.hash},
            {"files", QJsonArray::fromStringList(snapshot.files)}};
    if (!snapshot.objects.isEmpty())
    {
        QJsonObject objects;
        for (auto it = snapshot.objects.cbegin(); it != snapshot.objects.cend(); ++it)
            objects.insert(it.key(), QJsonObject{{"hash", it->hash}, {"size", it->size}});
        result.insert("objects", objects);
        result.insert("sequence", snapshot.sequence);
        result.insert("parents", QJsonArray::fromStringList(snapshot.parents));
    }
    return result;
}
CatalogAsset load(const QString &root, const QString &library)
{
    OperationScope::checkpoint(QStringLiteral("Reading asset metadata"));
    existingDirectory(root);
    require(files::isWithin(root, library) && absolute(root) != absolute(library),
            QStringLiteral("The asset must be inside the library."));
    CatalogAsset asset;
    asset.root = absolute(root);
    asset.library = absolute(library);
    asset.document = readJson(child(root, ".xips.json"));
    const int schema = asset.document.value("schemaVersion").toInt();
    require(schema == 1 || schema == 2 || schema == 3, QStringLiteral("Unsupported asset format: %1").arg(root));
    asset.legacy = schema == 1;
    asset.id = asset.document.value("id").toString();
    asset.name = asset.document.value("name").toString();
    asset.description = asset.document.value("description").toString();
    asset.tags = strings(asset.document.value("tags").toArray());
    const auto indexes = asset.document.value("indexes").toObject();
    for (auto it = indexes.begin(); it != indexes.end(); ++it)
        asset.indexes.insert(it.key(), strings(it.value().toArray()));
    asset.category = asset.document.value("category").toString();
    if (asset.legacy)
    {
        const auto legacy = ManifestService().parse(QJsonDocument(asset.document).toJson());
        require(legacy.ok(), QStringLiteral("Invalid legacy asset metadata: %1").arg(root));
        if (!validCategory(asset.category))
            asset.category = "other";
    }
    else
    {
        require(!asset.id.trimmed().isEmpty() && !asset.name.trimmed().isEmpty() &&
                    validCategory(asset.category),
                QStringLiteral("Incomplete asset metadata: %1").arg(root));
        QSet<QString> ids;
        asset.nextSequence = asset.document.value("nextRevision").toInteger(1);
        const auto appendRevision = [&](const Snapshot &snapshot)
        {
            require(!ids.contains(snapshot.id), QStringLiteral("Duplicate revision number"));
            ids.insert(snapshot.id);
            asset.nextSequence = qMax(asset.nextSequence, snapshot.sequence + 1);
            const auto tombstone = child(root, ".xips/deleted-revisions/" + snapshot.id + ".json");
            if (QFileInfo::exists(tombstone))
            {
                const auto deleted = readJson(tombstone);
                require(deleted.value("assetId").toString() == asset.id &&
                            deleted.value("revision").toString() == snapshot.id &&
                            (!deleted.contains("hash") || deleted.value("hash").toString() == snapshot.hash) &&
                            (!deleted.contains("sequence") || deleted.value("sequence").toInteger() == snapshot.sequence) &&
                            (!deleted.contains("parents") || strings(deleted.value("parents").toArray()) == snapshot.parents),
                        QStringLiteral("Invalid revision deletion record: %1").arg(tombstone));
                asset.deletedRevisionParents.insert(snapshot.id, snapshot.parents);
            }
            else asset.snapshots.append(snapshot);
        };
        for (const auto &value : asset.document.value("revisions").toArray())
            appendRevision(parseSnapshot(value.toObject()));
        if (schema == 3)
        {
            const auto directory = child(root, ".xips/revisions");
            safePath(directory);
            for (const auto &entry : QDir(directory).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
            {
                OperationScope::checkpoint();
                try
                {
                const auto record = readJson(entry.absoluteFilePath());
                const auto snapshot = parseSnapshot(record);
                require(record.value("schemaVersion").toInt() == 1 &&
                            record.value("assetId").toString() == asset.id && !snapshot.objects.isEmpty() &&
                            entry.fileName() == snapshot.id + ".json" && !ids.contains(snapshot.id),
                        QStringLiteral("Invalid independent version manifest: %1").arg(entry.absoluteFilePath()));
                appendRevision(snapshot);
                }
                catch (const Failure &failure)
                {
                    asset.historyIncomplete = true;
                    asset.problems.append(entry.absoluteFilePath() + ": " + failure.message);
                }
                catch (const std::exception &failure)
                {
                    asset.historyIncomplete = true;
                    asset.problems.append(entry.absoluteFilePath() + ": " + QString::fromUtf8(failure.what()));
                }
            }
        }
        const auto deletions = child(root, ".xips/deleted-revisions");
        safePath(deletions);
        for (const auto &entry : QDir(deletions).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
            if (!ids.contains(entry.completeBaseName()))
            {
                asset.historyIncomplete = true;
                asset.problems.append(QStringLiteral("Deleted revision manifest unavailable: %1").arg(entry.absoluteFilePath()));
            }
        std::sort(asset.snapshots.begin(), asset.snapshots.end(), [](const auto &a, const auto &b)
                  { return a.sequence != b.sequence ? a.sequence < b.sequence
                       : a.created != b.created ? a.created < b.created : a.id < b.id; });
        const auto labels = asset.document.value("revisionLabels");
        if (!labels.isUndefined() && !labels.isObject())
        {
            asset.historyIncomplete = true;
            asset.problems.append(QStringLiteral("Invalid version labels: %1").arg(child(root, ".xips.json")));
        }
        const auto customLabels = labels.toObject();
        for (auto &snapshot : asset.snapshots)
        {
            const auto label = customLabels.value(snapshot.id);
            if (label.isUndefined()) continue;
            if (label.isString() && validRevisionLabel(label.toString())) snapshot.label = label.toString();
            else
            {
                asset.historyIncomplete = true;
                asset.problems.append(QStringLiteral("Invalid version label for %1: %2")
                    .arg(snapshot.id, child(root, ".xips.json")));
            }
        }
        const auto workingArea = asset.document.value("workingArea").toString();
        const bool emptyWorkspace = asset.document.contains("source") &&
            (workingArea == "managed" || workingArea == "linked");
        const bool emptyHistory = emptyWorkspace || !asset.deletedRevisionParents.isEmpty();
        if (asset.snapshots.isEmpty() && schema == 3 && !emptyHistory)
        {
            asset.historyIncomplete = true;
            asset.problems.append(QStringLiteral("No healthy saved revisions: %1").arg(root));
        }
        require(asset.snapshots.isEmpty() ? (asset.historyIncomplete || (emptyHistory && asset.nextSequence > 0))
                                          : asset.nextSequence > asset.snapshots.last().sequence,
                QStringLiteral("Invalid revision counter: %1").arg(root));
    }
    return asset;
}
AssetRecord legacyRecord(const CatalogAsset &asset)
{
    const auto manifest = ManifestService().load(child(asset.root, ".xips.json"));
    require(manifest.ok() && manifest.manifest->id == asset.id,
            QStringLiteral("The legacy asset identity has changed."));
    AssetRecord result;
    result.manifest = *manifest.manifest;
    result.assetRoot = asset.root;
    result.manifestPath = child(asset.root, ".xips.json");
    result.files = AssetScanner::assetFiles(asset.root);
    return result;
}
bool validImportPath(const QString &path)
{
    const auto parts = path.toCaseFolded().split('/');
    return !path.isEmpty() && path != "." && path != ".." && QDir::cleanPath(path) == path &&
        !QDir::isAbsolutePath(path) && !path.startsWith("../") && !path.contains(':') && !path.contains('\\') &&
        !parts.contains(".xips") && !parts.contains(".git") && !parts.contains(".xips.json") &&
        !parts.contains(".snapshot.json") &&
        std::none_of(parts.cbegin(), parts.cend(), [](const auto &part) { return part.startsWith(".xips-"); });
}
QStringList includedWorkingFiles(const CatalogAsset &asset)
{
    const auto stored = asset.document.value("includedWorkingFiles");
    if (stored.isUndefined()) return {};
    require(stored.isArray(), QStringLiteral("Invalid imported working file selection."));
    const auto paths = strings(stored.toArray());
    for (const auto &path : paths)
        require(validImportPath(path), QStringLiteral("Invalid imported working file path: %1").arg(path));
    return paths;
}
void enumerate(const QString &root, const QString &directory, QStringList &result, bool source,
               bool artifact)
{
    OperationScope::checkpoint(QStringLiteral("Listing %1").arg(QFileInfo(directory).fileName()));
    existingDirectory(directory);
    for (const QFileInfo &entry : QDir(directory).entryInfoList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name))
    {
        require(!files::isLinkLike(entry),
                QStringLiteral("File set contains a link: %1").arg(entry.absoluteFilePath()));
        if (source && (entry.fileName() == ".xips.json" || entry.fileName() == ".snapshot.json"))
            continue;
        if (entry.isDir())
        {
            const QString lower = entry.fileName().toLower();
            if (source && (lower == ".git" || lower == ".xips" || lower.startsWith(".xips-")))
                continue;
            if (source && !artifact && files::isIgnoredDirectory(lower))
                continue;
            enumerate(root, entry.absoluteFilePath(), result, source, artifact);
        }
        else
        {
            require(entry.isFile(),
                    QStringLiteral("Unsupported file: %1").arg(entry.absoluteFilePath()));
            result.append(QDir(root).relativeFilePath(entry.absoluteFilePath()));
        }
    }
}
QStringList payloadFiles(const QString &root)
{
    QStringList result;
    enumerate(root, root, result, false, true);
    std::sort(result.begin(), result.end());
    return result;
}
QString digest(const QString &root, const QStringList &files)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &relative : files)
    {
        const QString path = child(root, relative);
        safePath(path);
        QFile file(path);
        require(file.open(QIODevice::ReadOnly),
                QStringLiteral("Cannot read revision file: %1").arg(path));
        const auto size = file.size();
        const auto modified = file.fileTime(QFileDevice::FileModificationTime);
        const auto name = relative.toUtf8();
        hash.addData(QByteArray::number(name.size()) + ':' + name + ':' + QByteArray::number(size) +
                     ':');
        qint64 count = 0;
        while (!file.atEnd())
        {
            OperationScope::checkpoint(QStringLiteral("Verifying %1").arg(relative), count, size);
            const auto data = file.read(1024 * 1024);
            require(file.error() == QFileDevice::NoError,
                    QStringLiteral("Cannot read revision: %1").arg(path));
            hash.addData(data);
            count += data.size();
        }
        require(count == size && file.size() == size &&
                    file.fileTime(QFileDevice::FileModificationTime) == modified,
                QStringLiteral("File changed while reading: %1").arg(path));
    }
    return "sha256:" + QString::fromLatin1(hash.result().toHex());
}
QString revisionRoot(const CatalogAsset &asset, const Snapshot &snapshot)
{
    return child(asset.root, ".xips/revisions/" + snapshot.id);
}
void verify(const QString &root, const Snapshot &snapshot)
{
    if (!snapshot.objects.isEmpty())
    {
        require(payloadFiles(root) == snapshot.files, QStringLiteral("Exported file list changed"));
        for (const auto &file : snapshot.files)
            require(ContentStore::fingerprint(child(root, file)) == snapshot.objects.value(file),
                    QStringLiteral("Exported content mismatch: %1").arg(file));
        return;
    }
    require(payloadFiles(root) == snapshot.files && digest(root, snapshot.files) == snapshot.hash &&
                payloadFiles(root) == snapshot.files,
            QStringLiteral("Revision files were changed or corrupted: %1").arg(root));
}
Snapshot selected(const CatalogAsset &asset, const QString &id)
{
    const auto matches = SnapshotLibrary::matchingRevisions(asset, id);
    require(matches.size() <= 1, QStringLiteral("Ambiguous revision number; choose its unique ID."));
    if (matches.size() == 1)
        return matches.first();
    throw Failure{QStringLiteral("Revision not found: %1").arg(id)};
}
void verifySaved(const CatalogAsset &asset, const Snapshot &snapshot)
{
    if (snapshot.objects.isEmpty())
        verify(revisionRoot(asset, snapshot), snapshot);
    else
    {
        ContentStore store(asset.library);
        QSet<QString> checked;
        for (const auto &object : snapshot.objects)
            if (!checked.contains(object.hash + ':' + QString::number(object.size)))
            {
                store.verify(object);
                checked.insert(object.hash + ':' + QString::number(object.size));
            }
    }
}
void publishSnapshot(const QString &root, const QString &assetId, const Snapshot &snapshot)
{
    auto record = toJson(snapshot);
    record.insert("schemaVersion", 1);
    record.insert("assetId", assetId);
    ContentStore::publishJson(child(root, ".xips/revisions/" + snapshot.id + ".json"), record);
}
void writeJson(const QString &path, const QJsonObject &document,
               const QByteArray *expected = nullptr)
{
    safePath(path);
    if (expected)
        require(bytes(path) == *expected,
                QStringLiteral("Another application updated this asset. Refresh and retry."));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), QStringLiteral("Cannot write metadata: %1").arg(path));
    const auto data = QJsonDocument(document).toJson(QJsonDocument::Indented);
    require(file.write(data) == data.size(), QStringLiteral("Metadata write failed"));
    if (expected)
        require(bytes(path) == *expected,
                QStringLiteral("Another application updated this asset. Refresh and retry."));
    safePath(path);
    OperationScope::publish();
    require(file.commit(), QStringLiteral("Cannot commit metadata: %1").arg(path));
}
bool skipSource(const QFileInfo &entry)
{
    return entry.fileName().startsWith('.') || files::isLinkLike(entry) ||
           (entry.isDir() && files::isIgnoredDirectory(entry.fileName()));
}
QString sourceId(const QString &root, bool directory)
{
    QString path = absolute(root);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    const QByteArray key = (directory ? QByteArray("directory:") : QByteArray("file:")) +
                           path.toUtf8();
    return "local-" + QString::fromLatin1(
                          QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}
CatalogAsset discoveredAsset(const QString &root, const QString &library,
                             const QString &category, QStringList files, bool directory)
{
    CatalogAsset asset;
    asset.root = absolute(root);
    asset.library = absolute(library);
    asset.discovered = true;
    asset.sourceIsDirectory = directory;
    asset.id = sourceId(asset.root, directory);
    asset.name = directory ? QFileInfo(root).fileName() : QFileInfo(root).completeBaseName();
    asset.category = category;
    asset.description = QDir(library).relativeFilePath(root);
    std::sort(files.begin(), files.end());
    asset.workingFiles = files;
    if (!files.isEmpty()) asset.snapshots.append({"current", {}, {}, {}, files});
    return asset;
}
QStringList directorySources(const QString &directory)
{
    OperationScope::checkpoint(QStringLiteral("Listing %1").arg(QFileInfo(directory).fileName()));
    existingDirectory(directory);
    require(QFileInfo(directory).isReadable(),
            QStringLiteral("Cannot read directory: %1").arg(directory));
    QStringList result;
    for (const auto &entry : QDir(directory).entryInfoList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name))
    {
        if (skipSource(entry))
            continue;
        if (entry.isDir())
        {
            if (QFileInfo::exists(child(entry.absoluteFilePath(), ".xips.json")))
                continue;
            for (const auto &file : directorySources(entry.absoluteFilePath()))
                result.append(entry.fileName() + '/' + file);
        }
        else if (entry.isFile())
            result.append(entry.fileName());
    }
    std::sort(result.begin(), result.end());
    return result;
}
CatalogAsset attachHistory(const CatalogAsset &history, const CatalogAsset *source = nullptr)
{
    const auto location = history.document.value("source").toObject();
    const auto relative = location.value("path").toString();
    require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
                QDir::cleanPath(relative) == relative && relative != ".." && !relative.startsWith("../") &&
                !relative.contains('\\') && !relative.contains(':'), QStringLiteral("Invalid source location"));
    CatalogAsset result = history;
    result.historyRoot = history.root;
    result.root = absolute(child(history.library, relative));
    result.discovered = true;
    result.sourceIsDirectory = location.value("directory").toBool();
    require(files::isWithin(result.root, history.library), QStringLiteral("Source is outside the library"));
    if (source)
    {
        require(result.root == source->root && result.sourceIsDirectory == source->sourceIsDirectory,
                QStringLiteral("Saved source location has changed"));
        result.workingFiles = source->workingFiles;
        if (!source->snapshots.isEmpty()) result.snapshots.append(source->snapshots.last());
    }
    return result;
}
CatalogAsset readReference(const QString &path)
{
    const auto record = readJson(path);
    require(record.value("schema").toString() == "xips.reference/v1", QStringLiteral("Invalid IP reference"));
    const QString base = QFileInfo(path).absolutePath();
    const QString owner = absolute(child(base, record.value("library").toString()));
    const QString definition = absolute(child(base, record.value("definition").toString()));
    require(!record.value("library").toString().isEmpty() && !record.value("definition").toString().isEmpty(),
            QStringLiteral("Incomplete IP reference"));
    auto asset = load(definition, owner);
    require(asset.id == record.value("assetId").toString(), QStringLiteral("Referenced IP identity changed"));
    if (asset.document.contains("source"))
        asset = attachHistory(asset);
    const auto pinned = selected(asset, record.value("revision").toString());
    require(!pinned.objects.isEmpty(), QStringLiteral("An IP reference requires a saved revision"));
    asset.snapshots = {pinned};
    asset.referencePath = absolute(path);
    asset.referenceLibrary = absolute(child(base, "../.."));
    asset.referenceRecord = record;
    asset.pinnedRevision = pinned.id;
    return asset;
}
QStringList inactiveWorkingFiles(const CatalogAsset &history)
{
    if (history.document.value("workingArea") != "managed") return {};
    const auto stored = history.document.value("inactiveWorkingFiles");
    if (!stored.isUndefined())
    {
        require(stored.isArray(), QStringLiteral("Invalid working file selection."));
        const auto paths = strings(stored.toArray());
        for (const auto &path : paths)
            require(!path.isEmpty() && !QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path &&
                        path != "." && path != ".." && !path.startsWith("../") &&
                        !path.contains('\\') && !path.contains(':'),
                    QStringLiteral("Invalid working file selection."));
        return paths;
    }
    // Older folder imports kept each version under a separate top-level folder.
    if (history.historyIncomplete || SnapshotLibrary::heads(history).size() != 1) return {};
    QSet<QString> roots;
    QString latest;
    for (const auto &snapshot : history.snapshots)
    {
        if (snapshot.id == "current" || snapshot.id == "working") continue;
        QSet<QString> folders;
        for (const auto &path : snapshot.files)
        {
            if (!path.contains('/')) return {};
            folders.insert(path.section('/', 0, 0));
        }
        if (folders.size() != 1) return {};
        latest = *folders.cbegin();
        roots.insert(latest);
    }
    if (roots.size() < 2) return {};
    QSet<QString> retired;
    for (const auto &snapshot : history.snapshots)
        if (snapshot.id != "current" && snapshot.id != "working")
            for (const auto &path : snapshot.files)
                if (path.section('/', 0, 0) != latest) retired.insert(path);
    auto paths = retired.values(); paths.sort();
    return paths;
}
CatalogAsset currentSource(const CatalogAsset &asset, const CatalogAsset *storedHistory = nullptr)
{
    existingDirectory(asset.library);
    safePath(asset.root);
    require(files::isWithin(asset.root, asset.library),
            QStringLiteral("The source is outside the selected folder."));
    const QFileInfo info(asset.root);
    require(asset.sourceIsDirectory ? info.isDir() : info.isFile(),
            QStringLiteral("Source unavailable: %1").arg(asset.root));
    QString parent = asset.sourceIsDirectory ? asset.root : info.absolutePath();
    for (;;)
    {
        require(!QFileInfo::exists(child(parent, ".xips.json")),
                QStringLiteral("The source is now a managed asset. Rescan the folder."));
        if (files::isWithin(asset.library, parent))
            break;
        parent = QFileInfo(parent).absolutePath();
    }
    const auto history = storedHistory ? *storedHistory
        : asset.historyRoot.isEmpty() ? CatalogAsset{} : load(asset.historyRoot, asset.library);
    QStringList working;
    if (!asset.sourceIsDirectory) working = {info.fileName()};
    else if (history.document.value("workingArea") == "managed")
    {
        enumerate(asset.root, asset.root, working, true, true);
        QSet<QString> inactive;
        for (const auto &path : inactiveWorkingFiles(history)) inactive.insert(path.toCaseFolded());
        working.removeIf([&](const auto &path) { return inactive.contains(path.toCaseFolded()); });
        working.sort();
    }
    else
    {
        working = directorySources(asset.root);
        QSet<QString> present;
        for (const auto &path : working) present.insert(path.toCaseFolded());
        for (const auto &path : includedWorkingFiles(history))
        {
            OperationScope::checkpoint();
            const auto location = child(asset.root, path);
            safePath(location);
            const QFileInfo imported(location);
            if (!imported.exists()) continue;
            require(imported.isFile(), QStringLiteral("Imported file is no longer a file: %1").arg(path));
            if (!present.contains(path.toCaseFolded()))
            {
                working.append(path);
                present.insert(path.toCaseFolded());
            }
        }
        working.sort();
    }
    auto result = discoveredAsset(asset.root, asset.library, asset.category, working, asset.sourceIsDirectory);
    if (!asset.historyRoot.isEmpty())
        result = attachHistory(history, &result);
    require(result.id == asset.id && (!result.workingFiles.isEmpty() || !asset.historyRoot.isEmpty()),
            QStringLiteral("The source has changed. Rescan the folder."));
    return result;
}
CatalogAsset resolveAsset(const CatalogAsset &asset)
{
    if (!asset.referencePath.isEmpty())
        return readReference(asset.referencePath);
    if (!asset.discovered)
        return load(asset.root, asset.library);
    if (asset.historyRoot.isEmpty())
        return currentSource(asset);
    const auto history = load(asset.historyRoot, asset.library);
    require(history.id == asset.id, QStringLiteral("The asset identity has changed."));
    auto retained = attachHistory(history);
    try { return currentSource(asset); }
    catch (const OperationCancelled &) { throw; }
    catch (const Failure &failure) { retained.sourceProblem = failure.message; }
    catch (const std::exception &failure) { retained.sourceProblem = QString::fromUtf8(failure.what()); }
    retained.problems.append(QStringLiteral("Working files unavailable; saved history retained: %1").arg(retained.sourceProblem));
    return retained;
}
QString sourceRoot(const CatalogAsset &asset)
{
    return asset.sourceIsDirectory ? asset.root : QFileInfo(asset.root).absolutePath();
}
void verifyCurrentSource(const CatalogAsset &asset, const Snapshot &snapshot)
{
    const auto current = currentSource(asset);
    require(current.snapshots.last().files == snapshot.files &&
                digest(sourceRoot(current), snapshot.files) == snapshot.hash &&
                currentSource(asset).snapshots.last().files == snapshot.files,
            QStringLiteral("Source changed while copying. Rescan and retry."));
}
class LibraryLock
{
    QLockFile lock;

  public:
    explicit LibraryLock(const QString &root) : lock(child(root, ".xips-library.lock"))
    {
        existingDirectory(root);
        safePath(child(root, ".xips-library.lock"));
        lock.setStaleLockTime(0);
        require(lock.tryLock(0),
                QStringLiteral("Another library operation is running. Try again shortly."));
    }
};
struct InputFile
{
    QString source;
    QString relative;
};
QList<InputFile> selectedFiles(const CatalogAsset &asset, QStringList files)
{
    require(!files.isEmpty(), QStringLiteral("Select at least one working file."));
    files.sort();
    require(std::adjacent_find(files.cbegin(), files.cend()) == files.cend(), QStringLiteral("Duplicate selected file."));
    QList<InputFile> inputs;
    for (const auto &file : files)
    {
        require(asset.workingFiles.contains(file), QStringLiteral("Selected file is no longer available: %1. Refresh and select again.").arg(file));
        inputs.append({child(sourceRoot(asset), file), file});
    }
    return inputs;
}
QList<InputFile> sourceFiles(const QStringList &sources, bool artifact)
{
    require(!sources.isEmpty(), QStringLiteral("Choose files or a folder"));
    QList<InputFile> result;
    QSet<QString> names;
    for (const auto &source : sources)
    {
        safePath(source);
        const QFileInfo info(source);
        require(info.exists(), QStringLiteral("Source unavailable: %1").arg(source));
        QStringList relatives;
        if (info.isDir())
            enumerate(source, source, relatives, true, artifact);
        else
        {
            require(info.isFile(), QStringLiteral("Source is not a regular file: %1").arg(source));
            relatives.append(info.fileName());
        }
        for (const QString &relative : relatives)
        {
            const QString destination =
                info.isDir() && sources.size() > 1 ? info.fileName() + '/' + relative : relative;
            require(!names.contains(destination.toCaseFolded()),
                    QStringLiteral("Conflicting file name: %1").arg(destination));
            names.insert(destination.toCaseFolded());
            result.append({info.isDir() ? child(source, relative) : source, destination});
        }
    }
    require(!result.isEmpty(), QStringLiteral("The source contains no files to collect."));
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.relative < b.relative; });
    return result;
}
QStringList parentRevisions(const CatalogAsset &asset)
{
    auto ancestry = asset.deletedRevisionParents;
    for (const auto &snapshot : asset.snapshots) ancestry.insert(snapshot.id, snapshot.parents);
    QSet<QString> referenced;
    QStringList pending;
    for (const auto &snapshot : asset.snapshots)
        pending.append(snapshot.parents);
    while (!pending.isEmpty())
    {
        const auto parent = pending.takeLast();
        if (referenced.contains(parent)) continue;
        referenced.insert(parent);
        pending.append(ancestry.value(parent));
    }
    QStringList result;
    for (const auto &snapshot : asset.snapshots)
        if (snapshot.id != "current" && !referenced.contains(snapshot.id))
            result.append(snapshot.id);
    return result;
}
void healthyHistory(const CatalogAsset &asset)
{
    require(!asset.historyIncomplete,
            QStringLiteral("Repair unavailable revision metadata before changing this asset. Healthy revisions can still be exported."));
}
void matchesPreview(const Snapshot &snapshot, const QString &assetId, const QStringList &parents,
                    const PayloadPreview *expected)
{
    if (!expected) return;
    require(expected->assetId == assetId && expected->files == snapshot.files &&
                expected->objects == snapshot.objects && expected->heads == parents,
            QStringLiteral("Files or revision heads changed after preview. Review the operation again."));
}
Snapshot storeInputs(const QString &library, const QList<InputFile> &inputs, qint64 sequence,
                     const QString &note, const QStringList &parents)
{
    require(!inputs.isEmpty(), QStringLiteral("The source contains no files to save."));
    Snapshot snapshot{unique({}), note.trimmed(), QDateTime::currentDateTimeUtc(), {}, {}};
    snapshot.sequence = sequence;
    snapshot.parents = parents;
    ContentStore store(library);
    for (const auto &input : inputs)
        snapshot.objects.insert(input.relative, store.putFile(input.source));
    for (const auto &input : inputs)
        require(ContentStore::fingerprint(input.source) == snapshot.objects.value(input.relative),
                QStringLiteral("Source changed while saving: %1").arg(input.source));
    snapshot.files = snapshot.objects.keys();
    snapshot.hash = ContentStore::treeHash(snapshot.objects);
    return snapshot;
}
bool sameContent(const CatalogAsset &asset, const Snapshot &previous, const Snapshot &next)
{
    if (previous.files != next.files)
        return false;
    if (!previous.objects.isEmpty())
        return previous.hash == next.hash;
    verifySaved(asset, previous);
    QMap<QString, ContentObject> objects;
    for (const auto &file : previous.files)
        objects.insert(file, ContentStore::fingerprint(child(revisionRoot(asset, previous), file)));
    return ContentStore::treeHash(objects) == next.hash;
}
QJsonObject newDocument(const QString &id, const QString &name, const QString &category)
{
    require(!name.trimmed().isEmpty() && validCategory(category),
            QStringLiteral("Enter a name and choose a category."));
    return {{"schemaVersion", 3}, {"id", id}, {"name", name.trimmed()}, {"category", category}};
}
void append(QJsonObject &document, const Snapshot &snapshot)
{
    auto revisions = document.value("revisions").toArray();
    revisions.append(toJson(snapshot));
    document.insert("revisions", revisions);
    document.insert("nextRevision", snapshot.id.toLongLong() + 1);
}
QString sourceHistoryRoot(const QString &library, const QString &source, bool directory)
{
    const auto relative = QDir(library).relativeFilePath(source);
    auto identity = relative;
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    const auto key = QUuid::createUuidV5(QUuid("89a7fef0-e3aa-4d2a-9334-62d3bb4f3e1f"),
        ((directory ? "directory:" : "file:") + identity).toUtf8()).toString(QUuid::WithoutBraces);
    const auto histories = child(library, ".xips/assets");
    safePath(histories);
    QString active;
    QMap<QString, QString> retired;
    QSet<QString> replaced;
    for (const auto &entry : QDir(histories).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        OperationScope::checkpoint();
        QJsonObject document;
        try { document = readJson(child(entry.absoluteFilePath(), ".xips.json")); }
        catch (const OperationCancelled &) { throw; }
        catch (...) { continue; }
        const auto predecessor = document.value("replacesSourceId").toString();
        if (!predecessor.isEmpty()) replaced.insert(predecessor);
        const auto location = document.value("source").toObject();
        auto registered = location.value("path").toString();
#ifdef Q_OS_WIN
        registered = registered.toCaseFolded();
#endif
        if (registered != identity || location.value("directory").toBool() != directory) continue;
        if (document.value("registered") == false)
            retired.insert(entry.absoluteFilePath(), document.value("id").toString());
        else
        {
            require(active.isEmpty(), QStringLiteral("Conflicting source histories: %1").arg(source));
            active = entry.absoluteFilePath();
        }
    }
    if (!active.isEmpty()) return active;
    QString found;
    for (auto it = retired.cbegin(); it != retired.cend(); ++it)
    {
        // A new entry can reuse a deleted working path without reviving its predecessor.
        if (replaced.contains(it.value())) continue;
        require(found.isEmpty(), QStringLiteral("Conflicting source histories: %1").arg(source));
        found = it.key();
    }
    if (!found.isEmpty()) return found;
    const auto original = child(histories, key);
    if (!QFileInfo::exists(original)) return original;
    // A renamed source keeps this history location so external references stay valid.
    const auto retained = load(original, library);
    require(retained.document.contains("source"), QStringLiteral("Source history location is occupied."));
    return child(histories, unique({}));
}
QString availableRoot(const QString &library, const QString &name)
{
    QString base = AssetLibraryService::suggestedId(name);
    if (base.isEmpty())
        base = "asset";
    QString path = child(library, base);
    for (int index = 2; QFileInfo::exists(path); ++index)
        path = child(library, base + '_' + QString::number(index));
    safePath(path);
    return path;
}
template <class Function> SnapshotResult operation(Function function)
{
    SnapshotResult result;
    try
    {
        OperationScope::checkpoint();
        function(result);
        result.ok = true;
    }
    catch (const OperationCancelled &failure)
    {
        result.cancelled = true;
        result.error = QString::fromUtf8(failure.what());
    }
    catch (const Failure &failure)
    {
        result.error = failure.message;
    }
    catch (const std::exception &failure)
    {
        result.error = QString::fromUtf8(failure.what());
    }
    return result;
}
void removeDirectory(const QString &path, bool permanent)
{
    safePath(path);
    require(permanent ? QDir(path).removeRecursively() : QFile::moveToTrash(path),
            QStringLiteral("Cannot remove; files retained at: %1").arg(path));
}
} // namespace

CatalogResult SnapshotLibrary::scan(const QString &library)
{
    CatalogResult result;
    try
    {
        existingDirectory(library);
        if (QFileInfo::exists(child(library, ".xips.json")))
        {
            result.problems.append(QStringLiteral("This folder is a saved xIPs asset. Choose its parent folder."));
            return result;
        }
        std::function<QStringList(const QString &, bool)> visit =
            [&](const QString &directory, bool discover) -> QStringList
        {
            OperationScope::checkpoint(QStringLiteral("Scanning %1").arg(QFileInfo(directory).fileName()));
            QStringList sourceFiles;
            if (!QFileInfo(directory).isReadable())
            {
                result.problems.append(QStringLiteral("Cannot read directory: %1").arg(directory));
                return sourceFiles;
            }
            const auto entries = QDir(directory).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            const bool package = discover && std::any_of(
                entries.cbegin(), entries.cend(), [](const QFileInfo &entry)
                { return entry.isFile() && !skipSource(entry) &&
                         entry.fileName().compare("component.xml", Qt::CaseInsensitive) == 0; });
            for (const auto &entry : entries)
            {
                if (!discover) break;
                if (!entry.isFile() || skipSource(entry))
                    continue;
                sourceFiles.append(entry.fileName());
                const QString category = suggestedCategory({entry.absoluteFilePath()});
                if (discover && !package && category != "other")
                    result.assets.append(discoveredAsset(entry.absoluteFilePath(), library,
                                                          category, {entry.fileName()}, false));
            }
            for (const auto &entry : entries)
            {
                if (!entry.isDir())
                    continue;
                if (entry.fileName().startsWith(".xips-"))
                    result.problems.append(
                        QStringLiteral("Retained data: %1").arg(entry.absoluteFilePath()));
                if (skipSource(entry))
                    continue;
                const QString path = entry.absoluteFilePath();
                if (QFileInfo::exists(child(path, ".xips.json")))
                {
                    try
                    {
                        const auto asset = load(path, library);
                        result.assets.append(asset);
                        result.problems.append(asset.problems);
                        if (!asset.legacy)
                        {
                            QSet<QString> recorded;
                            for (const auto &snapshot : asset.snapshots)
                                recorded.insert(snapshot.id);
                            for (auto it = asset.deletedRevisionParents.cbegin(); it != asset.deletedRevisionParents.cend(); ++it)
                                recorded.insert(it.key());
                            for (const auto &revision :
                                 QDir(child(path, ".xips/revisions"))
                                     .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                                    QDir::Hidden))
                                if (!recorded.contains(revision.fileName()))
                                    result.problems.append(
                                        QStringLiteral("Unreferenced revision data: %1")
                                            .arg(revision.absoluteFilePath()));
                            for (const auto &retained :
                                 QDir(child(path, ".xips"))
                                     .entryInfoList({".removed-*"}, QDir::Dirs |
                                                                        QDir::NoDotAndDotDot |
                                                                        QDir::Hidden))
                                result.problems.append(QStringLiteral("Retained revision data: %1")
                                                           .arg(retained.absoluteFilePath()));
                        }
                    }
                    catch (const Failure &failure)
                    {
                        result.problems.append(failure.message);
                    }
                }
                else
                    for (const auto &file : visit(path, discover && !package))
                        sourceFiles.append(entry.fileName() + '/' + file);
            }
            if (package)
                result.assets.append(discoveredAsset(directory, library, "ip", sourceFiles, true));
            return sourceFiles;
        };
        visit(library, false);
        // Source histories live outside the working tree layout and travel with the library.
        const QString histories = child(library, ".xips/assets");
        safePath(histories);
        QSet<QString> attached;
        for (const auto &entry : QDir(histories).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            try
            {
                const auto history = load(entry.absoluteFilePath(), library);
                if (history.document.value("registered") == false) continue;
                auto saved = attachHistory(history);
                require(!attached.contains(saved.root), QStringLiteral("Conflicting source histories: %1").arg(saved.root));
                attached.insert(saved.root);
                saved = resolveAsset(saved);
                result.problems.append(saved.problems);
                result.assets.append(saved);
            }
            catch (const OperationCancelled &) { throw; }
            catch (const Failure &failure) { result.problems.append(failure.message); }
            catch (const std::exception &failure) { result.problems.append(QString::fromUtf8(failure.what())); }
        }
        const auto references = child(library, ".xips/references");
        safePath(references);
        for (const auto &entry : QDir(references).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        {
            QString problem;
            try {
                auto reference = readReference(entry.absoluteFilePath());
                result.problems.append(reference.problems);
                result.assets.append(reference);
            }
            catch (const OperationCancelled &) { throw; }
            catch (const Failure &failure) { problem = failure.message; }
            catch (const std::exception &failure) { problem = QString::fromUtf8(failure.what()); }
            if (!problem.isEmpty())
            {
                result.problems.append(entry.absoluteFilePath() + ": " + problem);
                try
                {
                    const auto record = readJson(entry.absoluteFilePath());
                    if (record.value("schema") != "xips.reference/v1" || record.value("assetId").toString().isEmpty()) continue;
                    CatalogAsset broken;
                    broken.id = record.value("assetId").toString();
                    broken.name = QStringLiteral("Unavailable reference · %1").arg(broken.id.left(8));
                    broken.category = "other";
                    broken.root = entry.absoluteFilePath();
                    broken.library = library;
                    broken.referencePath = entry.absoluteFilePath();
                    broken.referenceLibrary = library;
                    broken.referenceRecord = record;
                    broken.pinnedRevision = record.value("revision").toString();
                    broken.problems = {problem};
                    result.assets.append(broken);
                }
                catch (...) { /* Keep malformed records in Issues, never delete them. */ }
            }
        }
        QHash<QString, int> counts;
        for (const auto &asset : result.assets)
            ++counts[asset.id.toCaseFolded()];
        result.assets.erase(
            std::remove_if(
                result.assets.begin(), result.assets.end(),
                [&](const auto &asset)
                {
                    if (counts.value(asset.id.toCaseFolded()) < 2)
                        return false;
                    result.problems.append(
                        QStringLiteral("Duplicate asset id '%1': %2").arg(asset.id, asset.root));
                    return true;
                }),
            result.assets.end());
        std::sort(result.assets.begin(), result.assets.end(), [](const auto &a, const auto &b)
                  { return a.name.localeAwareCompare(b.name) < 0; });
        const auto groups = CatalogGroups::scan(library);
        result.groups = groups.groups;
        result.problems.append(groups.problems);
        OperationScope::checkpoint(QStringLiteral("Indexing catalog"));
        CatalogIndex::rebuild(library, result.assets);
        OperationScope::checkpoint();
    }
    catch (const OperationCancelled &)
    {
        result.cancelled = true;
        result.assets.clear();
    }
    catch (const Failure &failure)
    {
        result.problems.append(failure.message);
    }
    catch (const std::exception &failure)
    {
        result.problems.append(QString::fromUtf8(failure.what()));
    }
    return result;
}
QString SnapshotLibrary::suggestedCategory(const QStringList &sources)
{
    for (const auto &source : sources)
    {
        const QString suffix = QFileInfo(source).suffix().toLower();
        if (QStringList{"bit", "bin", "mcs", "dcp", "zip", "7z", "sof", "pof"}.contains(suffix))
            return "artifact";
        if (QStringList{"xci", "xcix", "qip", "ip", "tcl"}.contains(suffix))
            return "ip";
        if (QStringList{"v", "sv", "vhd", "vhdl", "svh", "vh"}.contains(suffix))
            return "module";
    }
    return "other";
}
QString SnapshotLibrary::categoryLabel(const QString &category)
{
    if (category == "ip")
        return "IP";
    if (category == "module")
        return "Module";
    if (category == "project")
        return "Project";
    if (category == "artifact")
        return QStringLiteral("Artifact");
    return QStringLiteral("Other");
}
SnapshotResult SnapshotLibrary::describe(const CatalogAsset &asset)
{
    return operation(
        [&](auto &result)
        {
            if (asset.discovered)
            {
                result.asset = resolveAsset(asset);
                return;
            }
            result.asset = resolveAsset(asset);
            require(result.asset.id == asset.id, QStringLiteral("The asset identity has changed."));
            if (result.asset.legacy)
            {
                const auto record = legacyRecord(result.asset);
                AssetLibraryService service;
                const auto inventory = service.versionInventory(asset.root);
                require(inventory.usable(), inventory.fatalError);
                for (const auto &version : inventory.validVersions)
                {
                    const auto plan = service.copyPlan(record, version.version);
                    if (plan.ok())
                        result.asset.snapshots.append(
                            {version.version, QStringLiteral("Legacy saved version"),
                             version.createdAt, plan.payloadFingerprint, plan.files});
                }
                const auto plan = service.copyPlan(record, {});
                require(plan.ok(), plan.error);
                result.asset.snapshots.append({"working",
                                               QStringLiteral("Legacy working copy"),
                                               {},
                                               plan.payloadFingerprint,
                                               plan.files});
            }
        });
}
SnapshotResult SnapshotLibrary::verifySnapshot(const CatalogAsset &asset, const QString &revision)
{
    return operation(
        [&](auto &result)
        {
            if (asset.discovered)
            {
                result.asset = resolveAsset(asset);
                result.snapshot = selected(result.asset, revision);
                if (result.snapshot.id == "current")
                {
                    result.snapshot.hash = digest(sourceRoot(result.asset), result.snapshot.files);
                    verifyCurrentSource(result.asset, result.snapshot);
                }
                else
                    verifySaved(result.asset, result.snapshot);
                return;
            }
            result.asset = resolveAsset(asset);
            require(!result.asset.legacy && result.asset.id == asset.id,
                    QStringLiteral("The asset identity has changed."));
            result.snapshot = selected(result.asset, revision);
            verifySaved(result.asset, result.snapshot);
        });
}
SnapshotResult SnapshotLibrary::prepareFile(const CatalogAsset &asset, const QString &revision,
                                            const QString &relative, const QString &cacheRoot)
{
    return operation([&](auto &result)
    {
        require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
                    QDir::cleanPath(relative) == relative && relative != "." && relative != ".." &&
                    !relative.startsWith("../") && !relative.contains('\\') && !relative.contains(':'),
                QStringLiteral("Invalid file path: %1").arg(relative));
        result.asset = resolveAsset(asset);
        require(result.asset.id == asset.id, QStringLiteral("The asset identity has changed."));
        QString source;
        if (result.asset.legacy)
        {
            const auto plan = AssetLibraryService().copyPlan(legacyRecord(result.asset),
                revision == "working" ? QString() : revision);
            require(plan.ok(), plan.error);
            result.snapshot = {revision, {}, {}, plan.payloadFingerprint, plan.files};
            source = child(plan.sourceRoot, relative);
        }
        else
        {
            result.snapshot = selected(result.asset, revision);
            source = child(revision == "current" ? sourceRoot(result.asset)
                                                   : revisionRoot(result.asset, result.snapshot), relative);
        }
        require(result.snapshot.files.contains(relative),
                QStringLiteral("File is no longer in this version. Refresh the catalog: %1").arg(relative));
        if (revision == "current" || (result.asset.legacy && revision == "working"))
        {
            safePath(source);
            require(QFileInfo(source).isFile() && QFileInfo(source).isReadable(),
                    QStringLiteral("File unavailable: %1").arg(source));
            result.exportedPath = absolute(source);
            return;
        }
        ContentObject object;
        if (!result.snapshot.objects.isEmpty())
            object = result.snapshot.objects.value(relative);
        else
        {
            if (!result.asset.legacy) verifySaved(result.asset, result.snapshot);
            object = ContentStore::fingerprint(source);
        }
        require(!cacheRoot.isEmpty() && QDir::isAbsolutePath(cacheRoot) &&
                    !files::isWithin(cacheRoot, result.asset.library),
                QStringLiteral("File previews require a local cache outside the library."));
        const auto key = QCryptographicHash::hash((result.asset.library + '\n' + asset.id + '\n' +
            result.snapshot.id + '\n' + relative + '\n' + object.hash).toUtf8(), QCryptographicHash::Sha256).toHex();
        const auto directory = child(cacheRoot, QString::fromLatin1(key));
        ContentStore::makeDirectory(directory);
        const auto destination = child(directory, QFileInfo(relative).fileName());
        safePath(destination);
        QLockFile lock(child(directory, ".open.lock"));
        require(lock.tryLock(1000), QStringLiteral("This file is already being prepared. Try again."));
        bool cached = QFileInfo::exists(destination);
        if (cached && ContentStore::fingerprint(destination) != object)
        {
            require(QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner) &&
                        QFile::remove(destination), QStringLiteral("Cannot replace cached file: %1").arg(destination));
            cached = false;
        }
        if (!cached)
        {
            QTemporaryDir staging(child(directory, ".pending-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot prepare file preview"));
            const auto staged = child(staging.path(), QFileInfo(relative).fileName());
            if (!result.snapshot.objects.isEmpty())
                ContentStore(result.asset.library).materialize(object, staged);
            else
                require(QFile::copy(source, staged), QStringLiteral("Cannot copy saved file: %1").arg(relative));
            require(ContentStore::fingerprint(staged) == object,
                    QStringLiteral("Saved file content changed: %1").arg(relative));
            OperationScope::checkpoint();
            require(QFile::rename(staged, destination), QStringLiteral("Cannot publish file preview"));
        }
        require(QFile::setPermissions(destination, QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther),
                QStringLiteral("Cannot make the saved file read-only: %1").arg(destination));
        result.exportedPath = absolute(destination);
    });
}
SnapshotResult SnapshotLibrary::collect(const QString &library, const QStringList &sources,
                                        const QString &name, const QString &category,
                                        const QString &note, const PayloadPreview *expected)
{
    return operation(
        [&](auto &result)
        {
            LibraryLock lock(library);
            const QString destination = availableRoot(library, name);
            QTemporaryDir staging(child(library, ".xips-import-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot create collection directory"));
            auto document =
                newDocument(QUuid::createUuid().toString(QUuid::WithoutBraces), name, category);
            const auto inputs = sourceFiles(sources, category == "artifact");
            result.snapshot = storeInputs(library, inputs, 1, note, {});
            matchesPreview(result.snapshot, {}, {}, expected);
            const auto confirmed = sourceFiles(sources, category == "artifact");
            require(inputs.size() == confirmed.size(), QStringLiteral("Source file list changed while saving"));
            for (qsizetype i = 0; i < inputs.size(); ++i)
                require(inputs[i].source == confirmed[i].source && inputs[i].relative == confirmed[i].relative,
                        QStringLiteral("Source file list changed while saving"));
            publishSnapshot(staging.path(), document.value("id").toString(), result.snapshot);
            writeJson(child(staging.path(), ".xips.json"), document);
            safePath(destination);
            require(!QFileInfo::exists(destination) && QDir().rename(staging.path(), destination),
                    QStringLiteral("Cannot publish asset"));
            staging.setAutoRemove(false);
            result.asset = load(destination, library);
        });
}
SnapshotResult SnapshotLibrary::update(const CatalogAsset &asset, const QStringList &sources,
                                       const QString &note, const PayloadPreview *expected)
{
    return operation(
        [&](auto &result)
        {
            require(!asset.discovered, QStringLiteral("Original files are updated in their source folder."));
            require(asset.referencePath.isEmpty(), QStringLiteral("Open the owning library to modify a referenced IP."));
            LibraryLock lock(asset.library);
            result.asset = load(asset.root, asset.library);
            require(result.asset.id == asset.id && result.asset.document.value("schemaVersion").toInt() == 3,
                    QStringLiteral("Saving revisions requires a new-format asset."));
            healthyHistory(result.asset);
            const QByteArray original = bytes(child(asset.root, ".xips.json"));
            const auto inputs = sourceFiles(sources, result.asset.category == "artifact");
            const auto parents = parentRevisions(result.asset);
            result.snapshot = storeInputs(asset.library, inputs, result.asset.nextSequence, note, parents);
            matchesPreview(result.snapshot, asset.id, parents, expected);
            const auto confirmed = sourceFiles(sources, result.asset.category == "artifact");
            require(inputs.size() == confirmed.size(), QStringLiteral("Source file list changed while saving"));
            for (qsizetype i = 0; i < inputs.size(); ++i)
                require(inputs[i].source == confirmed[i].source && inputs[i].relative == confirmed[i].relative,
                        QStringLiteral("Source file list changed while saving"));
            if (parents.size() == 1 && sameContent(result.asset, result.asset.snapshots.last(), result.snapshot))
            {
                const auto &latest = result.asset.snapshots.last();
                verifySaved(result.asset, latest);
                result.unchanged = true;
                result.snapshot = latest;
                return;
            }
            require(bytes(child(asset.root, ".xips.json")) == original,
                    QStringLiteral("The asset has been updated. Refresh to continue."));
            require(parentRevisions(load(asset.root, asset.library)) == parents,
                    QStringLiteral("Versions changed during saving. Refresh and retry."));
            publishSnapshot(asset.root, asset.id, result.snapshot);
            result.asset = load(asset.root, asset.library);
        });
}
QList<Snapshot> SnapshotLibrary::matchingRevisions(const CatalogAsset &asset, const QString &selector)
{
    for (const auto &snapshot : asset.snapshots)
        if (snapshot.id == selector)
            return {snapshot};
    QList<Snapshot> matches;
    for (const auto &snapshot : asset.snapshots)
        if (QString::number(snapshot.sequence) == selector)
            matches.append(snapshot);
    return matches;
}
QString SnapshotLibrary::revisionLabel(const Snapshot &snapshot)
{
    if (!snapshot.label.isEmpty()) return snapshot.label;
    return snapshot.sequence > 0 ? QStringLiteral("rev%1").arg(snapshot.sequence)
                                 : QStringLiteral("rev%1").arg(snapshot.id);
}
namespace
{
QJsonObject definitionFields(const CatalogDefinition &definition)
{
    require(!definition.name.trimmed().isEmpty() && validCategory(definition.category),
            QStringLiteral("Enter a name and choose a type."));
    QJsonObject indexes;
    for (auto it = definition.indexes.cbegin(); it != definition.indexes.cend(); ++it)
    {
        require(QStringList{"category", "tag", "interface", "purpose"}.contains(it.key()),
                QStringLiteral("Unknown index type"));
        QStringList values;
        for (const auto &value : it.value())
            if (!value.trimmed().isEmpty() && !values.contains(value.trimmed(), Qt::CaseInsensitive))
                values.append(value.trimmed());
        indexes.insert(it.key(), QJsonArray::fromStringList(values));
    }
    return {{"name", definition.name.trimmed()}, {"category", definition.category},
            {"description", definition.description.trimmed()}, {"indexes", indexes}};
}
void saveSourceRevision(const CatalogAsset &asset, const QString &note, SnapshotResult &result,
                        const QJsonObject &definition = {}, const PayloadPreview *expected = nullptr,
                        const QStringList *selection = nullptr)
{
        require(asset.discovered, QStringLiteral("Select original files to save a revision."));
        require(asset.referencePath.isEmpty(), QStringLiteral("Open the owning library to modify a referenced IP."));
        auto current = currentSource(asset);
        healthyHistory(current);
        const QString relative = QDir(asset.library).relativeFilePath(asset.root);
        const auto destination = asset.historyRoot.isEmpty()
            ? sourceHistoryRoot(asset.library, asset.root, asset.sourceIsDirectory) : asset.historyRoot;
        const bool existing = QFileInfo::exists(destination);
        const auto id = existing ? load(destination, asset.library).id : unique({});
        const auto previous = existing ? load(destination, asset.library) : CatalogAsset{};
        const bool reactivating = existing && previous.document.value("registered") == false;
        require(definition.isEmpty() || !existing || reactivating, QStringLiteral("This source is already registered in the catalog."));
        if (existing && current.historyRoot.isEmpty())
        {
            current.historyRoot = destination;
            current.id = load(destination, asset.library).id;
            current = currentSource(current);
            healthyHistory(current);
        }
        QList<InputFile> inputs;
        for (const auto &file : current.workingFiles)
            inputs.append({child(sourceRoot(current), file), file});
        if (selection) inputs = selectedFiles(current, *selection);
        const auto parents = parentRevisions(current);
        result.snapshot = storeInputs(asset.library, inputs, current.nextSequence, note, parents);
        matchesPreview(result.snapshot, current.id, parents, expected);
        const auto fresh = currentSource(current);
        if (selection) selectedFiles(fresh, *selection);
        else require(fresh.workingFiles == result.snapshot.files, QStringLiteral("Source file list changed while saving"));
        for (const auto &input : inputs)
            require(ContentStore::fingerprint(input.source) == result.snapshot.objects.value(input.relative),
                    QStringLiteral("Working file changed while saving: %1. Review again.").arg(input.relative));
        if (existing)
        {
            const auto history = load(destination, asset.library);
            require(parentRevisions(history) == parents, QStringLiteral("Versions changed during saving. Rescan and retry."));
            if (parents.size() == 1 && sameContent(history, history.snapshots.last(), result.snapshot))
            {
                verifySaved(history, history.snapshots.last());
                result.unchanged = true;
                result.snapshot = history.snapshots.last();
            }
            else
                publishSnapshot(destination, history.id, result.snapshot);
        }
        else
        {
            ContentStore::makeDirectory(QFileInfo(destination).absolutePath());
            QTemporaryDir staging(child(QFileInfo(destination).absolutePath(), ".pending-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot stage source history"));
            auto document = newDocument(id, current.name, current.category);
            for (auto it = definition.begin(); it != definition.end(); ++it)
                document.insert(it.key(), it.value());
            document.insert("source", QJsonObject{{"path", relative}, {"directory", current.sourceIsDirectory}});
            writeJson(child(staging.path(), ".xips.json"), document);
            publishSnapshot(staging.path(), id, result.snapshot);
            safePath(destination);
            require(!QFileInfo::exists(destination) && QDir().rename(staging.path(), destination),
                    QStringLiteral("Source history changed during saving. Rescan and retry."));
            staging.setAutoRemove(false);
        }
        if (reactivating)
        {
            const auto original = bytes(child(destination, ".xips.json"));
            auto document = load(destination, asset.library).document;
            require(document == previous.document, QStringLiteral("Registration changed elsewhere. Refresh and retry."));
            document.insert("registered", true);
            for (auto it = definition.begin(); it != definition.end(); ++it) document.insert(it.key(), it.value());
            writeJson(child(destination, ".xips.json"), document, &original);
        }
        current.historyRoot = destination;
        current.id = load(destination, asset.library).id;
        result.asset = currentSource(current);
}
}
SnapshotResult SnapshotLibrary::saveCurrent(const CatalogAsset &asset, const QString &note, const PayloadPreview *expected)
{
    return operation([&](auto &result)
    {
        LibraryLock lock(asset.library);
        saveSourceRevision(asset, note, result, {}, expected);
    });
}
SnapshotResult SnapshotLibrary::saveSelected(const CatalogAsset &asset, const QStringList &files,
                                            const QString &note, const PayloadPreview *expected)
{
    return operation([&](auto &result)
    {
        LibraryLock lock(asset.library);
        saveSourceRevision(asset, note, result, {}, expected, &files);
    });
}
namespace
{
QMap<QString, InputFile> importInputs(const QStringList &sources, const QString &targetRoot = {},
                                      bool strict = false)
{
    require(!sources.isEmpty(), QStringLiteral("Choose files or a folder to add."));
    QMap<QString, InputFile> planned;
    for (const auto &source : sources)
    {
        safePath(source);
        const QFileInfo info(source);
        require(info.isDir() || info.isFile(), QStringLiteral("Source unavailable: %1").arg(source));
        if (!targetRoot.isEmpty())
            require(info.isDir() ? !files::isWithin(targetRoot, source) && !files::isWithin(source, targetRoot)
                                 : (!strict || !files::isWithin(source, targetRoot)),
                    QStringLiteral("Choose sources outside the destination working folder."));
        QStringList relatives;
        if (info.isDir()) enumerate(source, source, relatives, true, true);
        else relatives = {info.fileName()};
        for (const auto &relative : relatives)
        {
            const auto target = info.isDir() ? info.fileName() + '/' + relative : relative;
            require(validImportPath(target), QStringLiteral("Reserved or invalid working file path: %1").arg(target));
            const InputFile input{info.isDir() ? child(source, relative) : absolute(source), target};
            const auto key = target.toCaseFolded();
            if (planned.contains(key))
                require((!strict || absolute(planned.value(key).source) == absolute(input.source)) &&
                            ContentStore::fingerprint(planned.value(key).source) == ContentStore::fingerprint(input.source),
                        QStringLiteral("Conflicting imports: %1. Rename one source and retry.").arg(target));
            else planned.insert(key, input);
        }
    }
    require(!planned.isEmpty(), QStringLiteral("The selected sources contain no files to add."));
    return planned;
}
void selectImports(QMap<QString, InputFile> &planned, const PayloadPreview &selection)
{
    require(!selection.files.isEmpty() && selection.files == selection.objects.keys(),
            QStringLiteral("Select at least one file to import and archive."));
    QMap<QString, InputFile> selected;
    for (const auto &file : selection.files)
    {
        const auto key = file.toCaseFolded();
        require(planned.contains(key) && planned.value(key).relative == file && !selected.contains(key),
                QStringLiteral("Selected source is no longer available: %1. Review again.").arg(file));
        selected.insert(key, planned.value(key));
    }
    planned = selected;
}
QMap<QString, ContentObject> stageImports(const QString &targetRoot, const QString &stageRoot,
                                          const QMap<QString, InputFile> &planned,
                                          const PayloadPreview *selection)
{
    QMap<QString, ContentObject> staged;
    for (const auto &input : planned)
    {
        const auto destination = child(targetRoot, input.relative);
        safePath(destination);
        require(files::isWithin(destination, targetRoot), QStringLiteral("Invalid import destination."));
        const auto expected = ContentStore::fingerprint(input.source);
        require(!selection || selection->objects.value(input.relative) == expected,
                QStringLiteral("Source changed after review: %1. Review the import again.").arg(input.source));
        if (QFileInfo::exists(destination))
        {
            require(QFileInfo(destination).isFile() && ContentStore::fingerprint(destination) == expected,
                    QStringLiteral("Already exists with different content: %1. Rename the incoming file or folder and retry; nothing was overwritten.").arg(input.relative));
            continue;
        }
        auto parent = QFileInfo(destination).absolutePath();
        while (parent != absolute(targetRoot))
        {
            require(!QFileInfo::exists(parent) || QFileInfo(parent).isDir(),
                    QStringLiteral("A file blocks the destination folder: %1").arg(parent));
            const auto relativeParent = QDir(targetRoot).relativeFilePath(parent).toCaseFolded();
            require(!planned.contains(relativeParent), QStringLiteral("Conflicting file and folder names: %1").arg(relativeParent));
            parent = QFileInfo(parent).absolutePath();
        }
        const auto stagedPath = child(stageRoot, input.relative);
        ContentStore::makeDirectory(QFileInfo(stagedPath).absolutePath());
        QFile in(input.source), out(stagedPath);
        require(in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                QStringLiteral("Cannot copy %1").arg(input.source));
        while (!in.atEnd())
        {
            OperationScope::checkpoint(QStringLiteral("Adding %1").arg(input.relative), in.pos(), expected.size);
            const auto block = in.read(1024 * 1024);
            require(in.error() == QFileDevice::NoError && out.write(block) == block.size(), QStringLiteral("Import failed: %1").arg(input.relative));
        }
        require(out.flush(), QStringLiteral("Cannot flush imported file: %1").arg(input.relative));
        in.close(); out.close();
        require(ContentStore::fingerprint(input.source) == expected && ContentStore::fingerprint(stagedPath) == expected,
                QStringLiteral("Source changed while adding files: %1").arg(input.source));
        staged.insert(input.relative, expected);
    }
    return staged;
}
void finishImport(const ImportRequest &request, const QMap<QString, InputFile> &planned, SnapshotResult &result)
{
    if (!request.move) return;
    // The immutable version and destination must both verify before any source is removed.
    verifySaved(result.asset, result.snapshot);
    for (const auto &input : planned)
        require(ContentStore::fingerprint(child(result.asset.root, input.relative)) == result.snapshot.objects.value(input.relative),
                QStringLiteral("Imported destination changed; source files retained: %1").arg(input.relative));
    QSet<QString> processedSources;
    for (const auto &input : planned)
    {
        auto sourceKey = absolute(input.source);
#ifdef Q_OS_WIN
        sourceKey = sourceKey.toCaseFolded();
#endif
        if (processedSources.contains(sourceKey)) continue;
        processedSources.insert(sourceKey);
        try
        {
            safePath(input.source);
            require(!files::isWithin(input.source, result.asset.root) &&
                        ContentStore::fingerprint(input.source) == result.snapshot.objects.value(input.relative) &&
                        QFile::remove(input.source),
                    QStringLiteral("Cannot remove source"));
        }
        catch (...)
        {
            result.retainedSources.append(input.source);
            continue;
        }
        // Only remove empty parents inside a dropped folder; never its ancestors.
        for (const auto &source : request.sources)
        {
            if (!QFileInfo(source).isDir() || !files::isWithin(input.source, source)) continue;
            auto parent = QFileInfo(input.source).absolutePath();
            while (files::isWithin(parent, source))
            {
                if (!QDir().rmdir(parent) || absolute(parent) == absolute(source)) break;
                parent = QFileInfo(parent).absolutePath();
            }
        }
    }
}
}
SnapshotResult SnapshotLibrary::create(const QString &library, const CatalogDefinition &definition,
                                       const ImportRequest *request)
{
    return operation([&](auto &result)
    {
        const auto metadata = definitionFields(definition);
        require(QStringList{"module", "ip", "project"}.contains(definition.category), QStringLiteral("Choose Module, IP or Project."));
        require(!request || definition.source.isEmpty(), QStringLiteral("Imported files need a new working folder."));
        const auto version = request && !request->version.trimmed().isEmpty()
            ? checkedRevisionLabel(request->version) : QString();
        LibraryLock lock(library);
        const bool managed = definition.source.isEmpty();
        QString source = managed ? availableRoot(library, definition.name) : absolute(definition.source);
        std::unique_ptr<QTemporaryDir> sourceStage;
        if (managed)
        {
            sourceStage = std::make_unique<QTemporaryDir>(child(library, ".xips-create-XXXXXX"));
            require(sourceStage->isValid(), QStringLiteral("Cannot create working folder"));
        }
        safePath(source);
        require(files::isWithin(source, library) && absolute(source) != absolute(library),
                QStringLiteral("Choose a source file or subfolder inside the library root."));
        const QFileInfo info(source);
        require(managed || info.isFile() || info.isDir(), QStringLiteral("Source unavailable"));
        const bool directory = managed || info.isDir();
        if (!managed)
        {
            auto parent = directory ? source : info.absolutePath();
            for (;;)
            {
                require(!QFileInfo::exists(child(parent, ".xips.json")),
                        QStringLiteral("This source belongs to a managed asset; choose an original working folder."));
                if (files::isWithin(library, parent)) break;
                parent = QFileInfo(parent).absolutePath();
            }
        }
        const auto relative = QDir(library).relativeFilePath(source);
        require(!relative.toCaseFolded().split('/').contains(".xips"), QStringLiteral("Choose source files outside catalog metadata."));
        auto destination = sourceHistoryRoot(library, source, directory);
        const auto previous = QFileInfo::exists(destination) ? load(destination, library) : CatalogAsset{};
        require(previous.id.isEmpty() || previous.document.value("registered") == false,
                QStringLiteral("This source is already registered in the catalog."));
        const bool existing = !managed && !previous.id.isEmpty();
        if (managed && !previous.id.isEmpty())
            destination = child(QFileInfo(destination).absolutePath(), unique({}));
        auto document = existing ? previous.document : newDocument(unique({}), definition.name, definition.category);
        const auto originalDocument = document;
        if (existing) healthyHistory(previous);
        if (managed && !previous.id.isEmpty()) document.insert("replacesSourceId", previous.id);
        for (auto it = metadata.begin(); it != metadata.end(); ++it) document.insert(it.key(), it.value());
        document.insert("source", QJsonObject{{"path", relative}, {"directory", directory}});
        if (!existing) document.insert("workingArea", managed ? "managed" : "linked");
        if (!existing && managed) document.insert("inactiveWorkingFiles", QJsonArray{});
        document.insert("registered", true);
        QMap<QString, InputFile> planned;
        if (request)
        {
            planned = importInputs(request->sources, source, true);
            selectImports(planned, request->selection);
            stageImports(source, sourceStage->path(), planned, &request->selection);
            QList<InputFile> inputs;
            for (const auto &input : planned) inputs.append({child(sourceStage->path(), input.relative), input.relative});
            result.snapshot = storeInputs(library, inputs, 1, request->note, {});
            require(result.snapshot.objects == request->selection.objects, QStringLiteral("Imported files changed. Review again."));
            if (!version.isEmpty())
            {
                setRevisionLabel(document, result.snapshot, version);
                result.snapshot.label = version;
            }
        }
        // Stage working files, the definition and optional revision before publication.
        ContentStore::makeDirectory(QFileInfo(destination).absolutePath());
        QTemporaryDir historyStage(child(QFileInfo(destination).absolutePath(), ".pending-XXXXXX"));
        require(historyStage.isValid(), QStringLiteral("Cannot stage catalog entry"));
        QFile staged(child(historyStage.path(), ".xips.json"));
        const auto data = QJsonDocument(document).toJson();
        require(staged.open(QIODevice::WriteOnly) && staged.write(data) == data.size() && staged.flush(),
                QStringLiteral("Cannot stage catalog metadata"));
        staged.close();
        if (request) publishSnapshot(historyStage.path(), document.value("id").toString(), result.snapshot);
        OperationScope::publish();
        if (managed)
        {
            require(!QFileInfo::exists(source) && QDir().rename(sourceStage->path(), source), QStringLiteral("Cannot publish working folder"));
            sourceStage->setAutoRemove(false);
            result.retainedPath = source;
        }
        if (existing)
        {
            const auto original = bytes(child(destination, ".xips.json"));
            require(readJson(child(destination, ".xips.json")) == originalDocument,
                    QStringLiteral("Registration changed elsewhere. Refresh and retry."));
            writeJson(child(destination, ".xips.json"), document, &original);
        }
        else if (!QDir().rename(historyStage.path(), destination))
        {
            if (managed && QDir().rmdir(source)) result.retainedPath.clear();
            throw Failure{QStringLiteral("Cannot publish catalog entry")};
        }
        else historyStage.setAutoRemove(false);
        result.asset = currentSource(attachHistory(load(destination, library)));
        if (request) finishImport(*request, planned, result);
        result.retainedPath.clear();
    });
}
namespace
{
SnapshotResult importWorking(const CatalogAsset &asset, const QStringList &sources, const ImportRequest *request)
{
    return operation([&](auto &result)
    {
        require(asset.discovered && asset.sourceIsDirectory && asset.referencePath.isEmpty(),
                QStringLiteral("Choose an IP with a writable working folder."));
        require(!sources.isEmpty(), QStringLiteral("Choose files or a folder to add."));
        const auto version = request && !request->version.trimmed().isEmpty()
            ? checkedRevisionLabel(request->version) : QString();
        LibraryLock lock(asset.library);
        auto current = currentSource(asset);
        healthyHistory(current);
        if (request)
            require(!current.historyRoot.isEmpty() && current.document == asset.document &&
                        parentRevisions(current) == parentRevisions(asset),
                    QStringLiteral("The destination changed. Refresh and review again."));
        auto planned = importInputs(sources, asset.root, request != nullptr);
        if (request) selectImports(planned, request->selection);
        ContentStore::makeDirectory(child(asset.library, ".xips/imports"));
        QTemporaryDir stage(child(asset.library, ".xips/imports/.pending-XXXXXX"));
        require(stage.isValid(), QStringLiteral("Cannot stage imported files."));
        const auto staged = stageImports(asset.root, stage.path(), planned, request ? &request->selection : nullptr);
        auto document = current.document;
        if (document.value("workingArea") == "managed")
        {
            QMap<QString, QString> inactive;
            for (const auto &path : inactiveWorkingFiles(current)) inactive.insert(path.toCaseFolded(), path);
            if (request)
                for (const auto &snapshot : current.snapshots)
                    if (snapshot.id != "current" && snapshot.id != "working")
                        for (const auto &path : snapshot.files) inactive.insert(path.toCaseFolded(), path);
            for (auto it = planned.cbegin(); it != planned.cend(); ++it) inactive.remove(it.key().toCaseFolded());
            auto paths = inactive.values(); paths.sort();
            document.insert("inactiveWorkingFiles", QJsonArray::fromStringList(paths));
        }
        else
        {
            QMap<QString, QString> included;
            for (const auto &path : includedWorkingFiles(current)) included.insert(path.toCaseFolded(), path);
            for (const auto &input : planned)
            {
                const auto parts = input.relative.toCaseFolded().split('/');
                for (int i = 0; i < parts.size(); ++i)
                    if (parts[i].startsWith('.') || (i < parts.size() - 1 && files::isIgnoredDirectory(parts[i])))
                    {
                        included.insert(input.relative.toCaseFolded(), input.relative);
                        break;
                    }
            }
            if (!included.isEmpty())
            {
                auto paths = included.values(); paths.sort();
                document.insert("includedWorkingFiles", QJsonArray::fromStringList(paths));
            }
        }
        bool reuseVersion = false;
        if (request)
        {
            QList<InputFile> inputs;
            for (const auto &input : planned)
                inputs.append({child(staged.contains(input.relative) ? stage.path() : asset.root, input.relative), input.relative});
            result.snapshot = storeInputs(asset.library, inputs, current.nextSequence, request->note, parentRevisions(current));
            require(result.snapshot.objects == request->selection.objects, QStringLiteral("Imported files changed. Review again."));
            const auto history = load(current.historyRoot, asset.library);
            if (result.snapshot.parents.size() == 1 && !history.snapshots.isEmpty() &&
                sameContent(history, history.snapshots.last(), result.snapshot) &&
                (version.isEmpty() || version == SnapshotLibrary::revisionLabel(history.snapshots.last())))
            {
                verifySaved(history, history.snapshots.last());
                result.snapshot = history.snapshots.last();
                reuseVersion = true;
            }
            if (!version.isEmpty())
            {
                setRevisionLabel(document, result.snapshot, version, history.snapshots);
                result.snapshot.label = version;
            }
        }
        // No original is modified. A failed batch removes only the new files it published.
        QStringList published, directories;
        try
        {
            const auto fresh = currentSource(current);
            require(fresh.document == current.document, QStringLiteral("IP definition changed. Refresh and retry."));
            OperationScope::publish();
            for (auto it = staged.cbegin(); it != staged.cend(); ++it)
            {
                const auto target = child(asset.root, it.key());
                safePath(target);
                auto parent = QFileInfo(target).absolutePath();
                QStringList missing;
                while (!QFileInfo::exists(parent)) { missing.prepend(parent); parent = QFileInfo(parent).absolutePath(); }
                for (const auto &directory : missing)
                {
                    require(QDir().mkdir(directory), QStringLiteral("Cannot create import folder: %1").arg(directory));
                    directories.append(directory);
                }
                require(!QFileInfo::exists(target) && QFile::rename(child(stage.path(), it.key()), target),
                        QStringLiteral("Destination changed or cannot be written: %1").arg(it.key()));
                published.append(it.key());
            }
            if (request)
            {
                for (const auto &input : planned)
                    require(ContentStore::fingerprint(child(asset.root, input.relative)) == result.snapshot.objects.value(input.relative),
                            QStringLiteral("Imported files changed before archival: %1").arg(input.relative));
                const auto history = load(current.historyRoot, asset.library);
                require(history.document == current.document && parentRevisions(history) == parentRevisions(current),
                        QStringLiteral("Versions changed during import. Refresh and retry."));
            }
            const auto path = child(current.historyRoot, ".xips.json");
            const bool changed = document != current.document;
            if (changed)
            {
                const auto original = bytes(path);
                require(QJsonDocument::fromJson(original).object() == current.document,
                        QStringLiteral("Version details changed. Refresh and retry."));
                writeJson(path, document, &original);
            }
            try
            {
                if (request && !reuseVersion) publishSnapshot(current.historyRoot, current.id, result.snapshot);
            }
            catch (...)
            {
                if (changed)
                {
                    const auto expected = QJsonDocument(document).toJson(QJsonDocument::Indented);
                    try { writeJson(path, current.document, &expected); }
                    catch (...) { result.retainedPath = current.historyRoot; }
                }
                throw;
            }
        }
        catch (...)
        {
            for (const auto &file : published)
            {
                const auto path = child(asset.root, file);
                try
                {
                    if (ContentStore::fingerprint(path) != staged.value(file) || !QFile::remove(path)) result.retainedPath = asset.root;
                }
                catch (...) { result.retainedPath = asset.root; }
            }
            for (auto it = directories.crbegin(); it != directories.crend(); ++it) QDir().rmdir(*it);
            throw;
        }
        result.unchanged = staged.isEmpty() && document == current.document && (!request ||
            std::any_of(current.snapshots.cbegin(), current.snapshots.cend(),
                [&](const auto &snapshot) { return snapshot.id == result.snapshot.id; }));
        result.preview.files = staged.keys();
        if (!staged.isEmpty()) result.retainedPath = asset.root;
        result.asset = currentSource(current);
        if (request) finishImport(*request, planned, result);
        result.retainedPath.clear();
    });
}
}
SnapshotResult SnapshotLibrary::importFiles(const CatalogAsset &asset, const QStringList &sources)
{
    return importWorking(asset, sources, nullptr);
}
SnapshotResult SnapshotLibrary::importAndSave(const CatalogAsset &asset, const ImportRequest &request)
{
    return importWorking(asset, request.sources, &request);
}
SnapshotResult SnapshotLibrary::previewImport(const QStringList &sources)
{
    return operation([&](auto &result)
    {
        const auto planned = importInputs(sources, {}, true);
        for (const auto &input : planned)
        {
            const auto object = ContentStore::fingerprint(input.source);
            result.preview.objects.insert(input.relative, object);
            result.preview.bytes += object.size;
        }
        result.preview.files = result.preview.objects.keys();
    });
}
SnapshotResult SnapshotLibrary::setDefinition(const CatalogAsset &asset, const CatalogDefinition &definition)
{
    return operation([&](auto &result)
    {
        require(asset.referencePath.isEmpty(), QStringLiteral("Edit the IP definition in its owning library."));
        require(!asset.discovered || !asset.historyRoot.isEmpty(), QStringLiteral("Register this source first."));
        const auto fields = definitionFields(definition);
        LibraryLock lock(asset.library);
        const auto root = asset.historyRoot.isEmpty() ? asset.root : asset.historyRoot;
        const auto current = load(root, asset.library);
        healthyHistory(current);
        require(current.id == asset.id && current.document == asset.document, QStringLiteral("IP details changed. Refresh and retry."));
        const auto original = bytes(child(root, ".xips.json"));
        auto document = current.document;
        for (auto it = fields.begin(); it != fields.end(); ++it)
            document.insert(it.key(), it.value());
        auto updated = asset;
        const auto name = fields.value("name").toString();
        const bool renameFolder = asset.discovered && asset.sourceIsDirectory &&
            QFileInfo(asset.root).fileName() != name &&
            (name != current.name || document.value("workingArea") == "managed");
        QString location = asset.root;
        if (renameFolder)
        {
            const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
            const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
                                               QRegularExpression::CaseInsensitiveOption);
            require(name != "." && name != ".." && name.compare(".xips", Qt::CaseInsensitive) != 0 && !name.endsWith('.') &&
                        !invalid.match(name).hasMatch() && !reserved.match(name).hasMatch(),
                    QStringLiteral("Choose a valid working folder name."));
            existingDirectory(asset.root);
            require(files::isWithin(asset.root, asset.library) && absolute(asset.root) != absolute(asset.library),
                    QStringLiteral("Cannot rename the library root or a source outside it."));
            for (const auto &entry : QDir(child(asset.library, ".xips/assets")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            {
                OperationScope::checkpoint();
                if (absolute(entry.absoluteFilePath()) == absolute(root)) continue;
                QJsonObject other;
                try { other = readJson(child(entry.absoluteFilePath(), ".xips.json")); }
                catch (const OperationCancelled &) { throw; }
                catch (...) { continue; }
                if (other.value("registered") == false) continue;
                const auto relative = other.value("source").toObject().value("path").toString();
                require(relative.isEmpty() || !files::isWithin(child(asset.library, relative), asset.root),
                        QStringLiteral("This folder contains another registered source."));
            }
            updated.root = child(QFileInfo(asset.root).absolutePath(), name);
            safePath(updated.root);
            require(files::isWithin(updated.root, asset.library) && absolute(updated.root) != absolute(asset.library),
                    QStringLiteral("Working folder must remain inside the library."));
            bool caseOnly = false;
#ifdef Q_OS_WIN
            caseOnly = asset.root.compare(updated.root, Qt::CaseInsensitive) == 0 && asset.root != updated.root;
#endif
            require(caseOnly || !QFileInfo::exists(updated.root), QStringLiteral("A folder with this name already exists."));
            const auto otherHistory = sourceHistoryRoot(asset.library, updated.root, true);
            require(!QFileInfo::exists(otherHistory) || absolute(otherHistory) == absolute(root),
                    QStringLiteral("Another entry already uses this working folder name."));
            auto source = document.value("source").toObject();
            source.insert("path", QDir(asset.library).relativeFilePath(updated.root));
            document.insert("source", source);
            require(bytes(child(root, ".xips.json")) == original, QStringLiteral("IP details changed. Refresh and retry."));
            OperationScope::publish();
            if (caseOnly)
            {
                const auto temporary = child(QFileInfo(asset.root).absolutePath(), unique(".xips-rename-"));
                safePath(temporary);
                require(files::isWithin(temporary, asset.library) && !QFileInfo::exists(temporary) &&
                            QDir().rename(location, temporary), QStringLiteral("Cannot rename the working folder."));
                location = temporary;
            }
        }
        try
        {
            if (renameFolder)
            {
                require(QDir().rename(location, updated.root),
                        QStringLiteral("Cannot rename the working folder. Close files that are in use and retry."));
                location = updated.root;
            }
            writeJson(child(root, ".xips.json"), document, &original);
        }
        catch (...)
        {
            if (location != asset.root)
            {
#ifdef Q_OS_WIN
                if (location.compare(asset.root, Qt::CaseInsensitive) == 0)
                {
                    const auto temporary = child(QFileInfo(asset.root).absolutePath(), unique(".xips-rename-"));
                    if (QDir().rename(location, temporary)) location = temporary;
                }
#endif
                if (!QDir().rename(location, asset.root)) result.retainedPath = location;
            }
            throw;
        }
        result.asset = resolveAsset(updated);
    });
}
SnapshotResult SnapshotLibrary::addReference(const CatalogAsset &asset, const QString &revision,
                                             const QString &destinationLibrary)
{
    return operation([&](auto &result)
    {
        const auto current = resolveAsset(asset);
        require(current.id == asset.id, QStringLiteral("The IP identity changed."));
        result.snapshot = selected(current, revision);
        require(!result.snapshot.objects.isEmpty(), QStringLiteral("Choose a saved revision to reference."));
        verifySaved(current, result.snapshot);
        LibraryLock lock(destinationLibrary);
        require(absolute(destinationLibrary) != absolute(current.library), QStringLiteral("This IP already belongs to this library."));
        const auto folder = child(destinationLibrary, ".xips/references");
        const auto path = child(folder, current.id + ".json");
        require(!QFileInfo::exists(path), QStringLiteral("This destination already references the IP."));
        const auto definition = current.historyRoot.isEmpty() ? current.root : current.historyRoot;
        ContentStore::publishJson(path, {{"schema", "xips.reference/v1"}, {"assetId", current.id},
            {"revision", result.snapshot.id}, {"library", QDir(folder).relativeFilePath(current.library)},
            {"definition", QDir(folder).relativeFilePath(definition)}});
        result.asset = readReference(path);
        result.exportedPath = path;
    });
}
SnapshotResult SnapshotLibrary::edit(const CatalogAsset &asset, const QString &name,
                                     const QString &category, const QString &description)
{
    return operation(
        [&](auto &result)
        {
            require(!asset.discovered, QStringLiteral("Scanned files have no saved asset metadata."));
            require(asset.referencePath.isEmpty(), QStringLiteral("Edit the IP definition in its owning library."));
            LibraryLock lock(asset.library);
            result.asset = load(asset.root, asset.library);
            require(result.asset.id == asset.id && !result.asset.legacy,
                    QStringLiteral("Convert this legacy asset first."));
            healthyHistory(result.asset);
            require(!name.trimmed().isEmpty() && validCategory(category),
                    QStringLiteral("Invalid name or category"));
            const auto original = bytes(child(asset.root, ".xips.json"));
            require(result.asset.document == asset.document,
                    QStringLiteral("Asset details have changed. Refresh and retry."));
            auto document = result.asset.document;
            document.insert("name", name.trimmed());
            document.insert("category", category);
            document.insert("description", description.trimmed());
            writeJson(child(asset.root, ".xips.json"), document, &original);
            result.asset = load(asset.root, asset.library);
        });
}
SnapshotResult SnapshotLibrary::exportSnapshot(const CatalogAsset &asset, const QString &revision,
                                               const QString &destination, const PayloadPreview *expected)
{
    return operation(
        [&](auto &result)
        {
            result.asset = resolveAsset(asset);
            require(result.asset.id == asset.id, QStringLiteral("The asset identity has changed."));
            safePath(destination);
            existingDirectory(QFileInfo(destination).absolutePath());
            require(!QFileInfo::exists(destination) && !files::isWithin(destination, asset.library),
                    QStringLiteral("Choose a new destination outside the library."));
            if (result.asset.legacy)
            {
                const auto record = legacyRecord(result.asset);
                const QString oldVersion = revision == "working" ? QString() : revision;
                const auto plan = AssetLibraryService().copyPlan(record, oldVersion);
                require(plan.ok(), plan.error);
                QString error;
                OperationScope::publish(); // The legacy service does not support interruption.
                require(AssetLibraryService().copyVersionPayload(record, oldVersion, destination,
                                                                 &result.exportedPath, &error),
                        error);
                result.snapshot = {revision, {}, {}, plan.payloadFingerprint, plan.files};
                return;
            }
            result.snapshot = selected(result.asset, revision);
            if (expected)
                require(expected->assetId == asset.id && expected->revision == result.snapshot.id &&
                            expected->files == result.snapshot.files,
                        QStringLiteral("Export file set changed after preview. Review the destination type again."));
            const bool currentFiles = result.snapshot.id == "current";
            const QString source = currentFiles ? sourceRoot(result.asset)
                                                    : revisionRoot(result.asset, result.snapshot);
            const auto verifySource = [&]
            {
                if (currentFiles)
                    verifyCurrentSource(result.asset, result.snapshot);
                else
                    verifySaved(result.asset, result.snapshot);
            };
            if (currentFiles)
                result.snapshot.hash = digest(source, result.snapshot.files);
            if (result.snapshot.objects.isEmpty())
                verifySource();
            QTemporaryDir staging(
                child(QFileInfo(destination).absolutePath(), ".xips-export-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot create export directory"));
            const QString payload = child(staging.path(), "payload");
            QDir().mkpath(payload);
            for (const QString &file : result.snapshot.files)
            {
                QDir().mkpath(QFileInfo(child(payload, file)).absolutePath());
                if (!result.snapshot.objects.isEmpty())
                    ContentStore(asset.library).materialize(result.snapshot.objects.value(file), child(payload, file));
                else
                    require(QFile::copy(child(source, file), child(payload, file)),
                            QStringLiteral("Copy failed: %1").arg(file));
            }
            verify(payload, result.snapshot);
            if (expected)
            {
                for (const auto &file : expected->files)
                    require(ContentStore::fingerprint(child(payload, file)) == expected->objects.value(file),
                            QStringLiteral("Export content changed after preview: %1").arg(file));
            }
            if (result.snapshot.objects.isEmpty())
                verifySource();
            safePath(destination);
            const QString published = result.snapshot.files.size() == 1
                                          ? child(payload, result.snapshot.files.first())
                                          : payload;
            OperationScope::publish();
            require(!QFileInfo::exists(destination) && QDir().rename(published, destination),
                    QStringLiteral("Cannot publish exported copy"));
            result.exportedPath = absolute(destination);
        });
}
namespace
{
void previewFiles(PayloadPreview &preview, const QList<InputFile> &inputs)
{
    for (const auto &input : inputs)
    {
        const auto object = ContentStore::fingerprint(input.source);
        preview.objects.insert(input.relative, object);
        preview.bytes += object.size;
    }
    preview.files = preview.objects.keys();
}
void excludedFiles(const QString &directory, const QString &prefix, bool registered, bool artifact,
                   QStringList &excluded, const QStringList &included = {})
{
    OperationScope::checkpoint(QStringLiteral("Reviewing excluded paths"));
    for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name))
    {
        const auto relative = prefix + entry.fileName();
        QString reason;
        if (files::isLinkLike(entry)) reason = "linked path";
        else if (registered && entry.fileName().startsWith('.')) reason = "hidden working-source path";
        else if (entry.fileName() == ".xips.json" || entry.fileName() == ".snapshot.json" ||
                 entry.fileName() == ".git" || entry.fileName() == ".xips" || entry.fileName().startsWith(".xips-")) reason = "internal metadata";
        else if (entry.isDir() && !artifact && files::isIgnoredDirectory(entry.fileName())) reason = "generated directory policy";
        else if (registered && entry.isDir() && QFileInfo::exists(child(entry.absoluteFilePath(), ".xips.json"))) reason = "nested managed asset";
        if (!reason.isEmpty())
        {
            if (included.contains(relative, Qt::CaseInsensitive)) continue;
            const bool partial = entry.isDir() && std::any_of(included.cbegin(), included.cend(),
                [&](const auto &file) { return file.startsWith(relative + '/', Qt::CaseInsensitive); });
            excluded.append(relative + (entry.isDir() ? "/" : "") + " — " + reason +
                (partial ? " (except explicitly imported files)" : ""));
        }
        else if (entry.isDir()) excludedFiles(entry.absoluteFilePath(), relative + '/', registered, artifact, excluded, included);
    }
}
void comparePreview(PayloadPreview &preview, const CatalogAsset &asset)
{
    QMap<QString, ContentObject> previous;
    for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
        if (it->id != "current")
        {
            preview.comparisonRevision = SnapshotLibrary::revisionLabel(*it) + " (" + it->id.left(8) + ')';
            previous = it->objects;
            if (previous.isEmpty())
            {
                verifySaved(asset, *it);
                for (const auto &file : it->files) previous.insert(file, ContentStore::fingerprint(child(revisionRoot(asset, *it), file)));
            }
            break;
        }
    for (auto it = preview.objects.cbegin(); it != preview.objects.cend(); ++it)
        if (!previous.contains(it.key())) preview.added.append(it.key());
        else if (previous.value(it.key()) != it.value()) preview.modified.append(it.key());
        else ++preview.unchanged;
    for (auto it = previous.cbegin(); it != previous.cend(); ++it)
        if (!preview.objects.contains(it.key())) preview.removed.append(it.key());
}
SnapshotResult previewWorkingCopy(const CatalogAsset &asset, const QStringList *selection)
{
    return operation([&](auto &result)
    {
        require(asset.discovered && asset.referencePath.isEmpty(), QStringLiteral("Choose an original working copy."));
        result.asset = currentSource(asset);
        healthyHistory(result.asset);
        result.preview.assetId = result.asset.id;
        result.preview.heads = parentRevisions(result.asset);
        QList<InputFile> inputs;
        if (selection) inputs = selectedFiles(result.asset, *selection);
        else
            for (const auto &file : result.asset.workingFiles)
                inputs.append({child(sourceRoot(result.asset), file), file});
        previewFiles(result.preview, inputs);
        comparePreview(result.preview, result.asset);
    });
}
QString referencePath(const CatalogAsset &reference)
{
    require(!reference.referencePath.isEmpty() && !reference.referenceLibrary.isEmpty(), QStringLiteral("Select a local reference."));
    const auto path = absolute(reference.referencePath);
    require(QFileInfo(path).absolutePath() == absolute(child(reference.referenceLibrary, ".xips/references")),
            QStringLiteral("Reference is outside the receiving catalog."));
    safePath(path);
    return path;
}
}
QStringList SnapshotLibrary::heads(const CatalogAsset &asset) { return parentRevisions(asset); }
SnapshotResult SnapshotLibrary::previewCollect(const QStringList &sources, const QString &category)
{
    return operation([&](auto &result)
    {
        require(validCategory(category), QStringLiteral("Choose an asset type."));
        previewFiles(result.preview, sourceFiles(sources, category == "artifact"));
        result.preview.added = result.preview.files;
        for (const auto &source : sources)
            if (QFileInfo(source).isDir()) excludedFiles(source, sources.size() > 1 ? QFileInfo(source).fileName() + '/' : QString(), false, category == "artifact", result.preview.excluded);
    });
}
SnapshotResult SnapshotLibrary::previewSave(const CatalogAsset &asset, const QStringList &sources)
{
    return operation([&](auto &result)
    {
        result.asset = resolveAsset(asset);
        healthyHistory(result.asset);
        require(result.asset.referencePath.isEmpty() && !result.asset.legacy, QStringLiteral("Choose an original new-format asset."));
        auto &preview = result.preview;
        preview.assetId = asset.id;
        preview.heads = parentRevisions(result.asset);
        if (asset.discovered)
        {
            result.asset = currentSource(result.asset);
            QList<InputFile> inputs;
            for (const auto &file : result.asset.workingFiles) inputs.append({child(sourceRoot(result.asset), file), file});
            previewFiles(preview, inputs);
            if (asset.sourceIsDirectory)
            {
                const bool managed = result.asset.document.value("workingArea") == "managed";
                excludedFiles(asset.root, {}, !managed, managed, preview.excluded, includedWorkingFiles(result.asset));
            }
        }
        else
        {
            previewFiles(preview, sourceFiles(sources, asset.category == "artifact"));
            for (const auto &source : sources)
                if (QFileInfo(source).isDir()) excludedFiles(source, sources.size() > 1 ? QFileInfo(source).fileName() + '/' : QString(), false, asset.category == "artifact", preview.excluded);
        }
        comparePreview(preview, result.asset);
    });
}
SnapshotResult SnapshotLibrary::previewWorking(const CatalogAsset &asset)
{
    return previewWorkingCopy(asset, nullptr);
}
SnapshotResult SnapshotLibrary::previewSelected(const CatalogAsset &asset, const QStringList &files)
{
    return previewWorkingCopy(asset, &files);
}
SnapshotResult SnapshotLibrary::previewExport(const CatalogAsset &asset, const QString &revision)
{
    return operation([&](auto &result)
    {
        result.asset = resolveAsset(asset);
        require(!result.asset.legacy, QStringLiteral("Convert this legacy asset before previewing."));
        result.snapshot = selected(result.asset, revision);
        auto &preview = result.preview;
        preview.assetId = asset.id;
        preview.revision = result.snapshot.id;
        if (!result.snapshot.objects.isEmpty())
        {
            preview.objects = result.snapshot.objects;
            preview.files = result.snapshot.files;
            for (const auto &object : preview.objects) preview.bytes += object.size;
        }
        else
        {
            QList<InputFile> inputs;
            const auto root = revision == "current" ? sourceRoot(result.asset) : revisionRoot(result.asset, result.snapshot);
            for (const auto &file : result.snapshot.files) inputs.append({child(root, file), file});
            previewFiles(preview, inputs);
        }
        if (revision == "current" && asset.sourceIsDirectory) excludedFiles(asset.root, {}, true, false, preview.excluded);
    });
}
SnapshotResult SnapshotLibrary::relocateSource(const CatalogAsset &asset, const QString &source)
{
    return operation([&](auto &result)
    {
        require(asset.discovered && asset.referencePath.isEmpty() && !asset.historyRoot.isEmpty(),
                QStringLiteral("Select a registered source."));
        LibraryLock lock(asset.library);
        auto history = load(asset.historyRoot, asset.library);
        healthyHistory(history);
        require(history.id == asset.id && history.document == asset.document &&
                    history.document.value("registered") != false,
                QStringLiteral("Source details changed. Refresh and retry."));
        const auto location = absolute(source);
        safePath(location);
        require(files::isWithin(location, asset.library) && location != absolute(asset.library),
                QStringLiteral("Choose a source inside this library."));
        const auto relative = QDir(asset.library).relativeFilePath(location);
        require(!relative.toCaseFolded().split('/').contains(".xips"),
                QStringLiteral("Choose source files outside catalog metadata."));
        const QFileInfo info(location);
        require(asset.sourceIsDirectory ? info.isDir() : info.isFile(),
                QStringLiteral("Choose an available source of the same type."));
        // A registered or retired source already owns its history at this location.
        const auto occupied = sourceHistoryRoot(asset.library, location, asset.sourceIsDirectory);
        require(!QFileInfo::exists(occupied) || absolute(occupied) == absolute(asset.historyRoot),
                QStringLiteral("Another asset already uses this source location."));
        const auto path = child(asset.historyRoot, ".xips.json");
        const auto original = bytes(path);
        require(QJsonDocument::fromJson(original).object() == history.document,
                QStringLiteral("Source details changed. Refresh and retry."));
        auto descriptor = history.document.value("source").toObject();
        descriptor.insert("path", relative);
        history.document.insert("source", descriptor);
        // Validate the new source before publishing, retaining the existing ID and history.
        result.asset = currentSource(attachHistory(history), &history);
        writeJson(path, history.document, &original);
    });
}
SnapshotResult SnapshotLibrary::unregisterSource(const CatalogAsset &asset)
{
    return operation([&](auto &result)
    {
        require(asset.discovered && asset.referencePath.isEmpty() && !asset.historyRoot.isEmpty(), QStringLiteral("Select a registered source."));
        LibraryLock lock(asset.library);
        const auto current = load(asset.historyRoot, asset.library);
        require(current.id == asset.id && current.document.value("source") == asset.document.value("source"), QStringLiteral("Source identity changed."));
        healthyHistory(current);
        result.asset = asset;
        if (current.document.value("registered") == false) { result.unchanged = true; return; }
        require(current.document == asset.document, QStringLiteral("Definition changed. Refresh and retry."));
        const auto path = child(asset.historyRoot, ".xips.json");
        const auto original = bytes(path);
        auto document = current.document;
        document.insert("registered", false);
        writeJson(path, document, &original);
        result.retainedPath = asset.historyRoot;
    });
}
SnapshotResult SnapshotLibrary::removeReference(const CatalogAsset &asset)
{
    return operation([&](auto &result)
    {
        const auto path = referencePath(asset);
        LibraryLock lock(asset.referenceLibrary);
        if (!QFileInfo::exists(path)) { result.unchanged = true; return; }
        const auto original = bytes(path);
        require(QJsonDocument::fromJson(original).object() == asset.referenceRecord, QStringLiteral("Reference changed. Refresh and retry."));
        const auto destination = child(asset.referenceLibrary, ".xips/removed-references/" + unique({}) + ".json");
        ContentStore::makeDirectory(QFileInfo(destination).absolutePath());
        OperationScope::publish();
        require(QFile::rename(path, destination), QStringLiteral("Cannot remove local reference."));
        result.retainedPath = destination;
        if (bytes(destination) != original)
        {
            if (!QFileInfo::exists(path)) QFile::rename(destination, path);
            throw Failure{QStringLiteral("Reference changed during removal; metadata retained.")};
        }
    });
}
SnapshotResult SnapshotLibrary::referenceTarget(const CatalogAsset &reference, const QString &ownerLibrary)
{
    return operation([&](auto &result)
    {
        referencePath(reference);
        const auto catalog = scan(ownerLibrary);
        if (catalog.cancelled) throw OperationCancelled();
        QList<CatalogAsset> candidates;
        for (const auto &asset : catalog.assets)
            if (asset.id == reference.id && asset.referencePath.isEmpty()) candidates.append(asset);
        // An unregistered owner remains a valid pinned history at its existing definition.
        if (candidates.isEmpty())
        {
            const auto base = QFileInfo(reference.referencePath).absolutePath();
            const auto oldOwner = absolute(child(base, reference.referenceRecord.value("library").toString()));
            const auto oldDefinition = absolute(child(base, reference.referenceRecord.value("definition").toString()));
            const auto relative = QDir(oldOwner).relativeFilePath(oldDefinition);
            require(!relative.startsWith("../") && !QDir::isAbsolutePath(relative), QStringLiteral("Invalid owner definition path."));
            const auto loaded = load(child(ownerLibrary, relative), ownerLibrary);
            require(loaded.id == reference.id, QStringLiteral("The selected library does not contain this asset ID."));
            candidates.append(loaded.document.contains("source") ? attachHistory(loaded) : loaded);
        }
        require(candidates.size() == 1, QStringLiteral("Choose the owning library containing this exact asset ID."));
        result.asset = candidates.first();
    });
}
SnapshotResult SnapshotLibrary::changeReference(const CatalogAsset &reference, const QString &ownerLibrary, const QString &revision)
{
    return operation([&](auto &result)
    {
        const auto target = referenceTarget(reference, ownerLibrary);
        if (target.cancelled) throw OperationCancelled();
        require(target.ok, target.error);
        result.snapshot = selected(target.asset, revision);
        require(!result.snapshot.objects.isEmpty(), QStringLiteral("Choose a saved revision."));
        verifySaved(target.asset, result.snapshot);
        const auto path = referencePath(reference);
        LibraryLock lock(reference.referenceLibrary);
        const auto original = bytes(path);
        require(QJsonDocument::fromJson(original).object() == reference.referenceRecord, QStringLiteral("Reference changed. Refresh and retry."));
        const auto base = QFileInfo(path).absolutePath();
        auto record = reference.referenceRecord;
        record.insert("library", QDir(base).relativeFilePath(target.asset.library));
        record.insert("definition", QDir(base).relativeFilePath(target.asset.historyRoot.isEmpty() ? target.asset.root : target.asset.historyRoot));
        record.insert("revision", result.snapshot.id);
        if (record == reference.referenceRecord) result.unchanged = true;
        else writeJson(path, record, &original);
        result.asset = readReference(path);
    });
}
SnapshotResult SnapshotLibrary::saveReceipt(const SnapshotResult &exported, const QString &destination)
{
    return operation([&](auto &result)
    {
        require(exported.ok && !exported.exportedPath.isEmpty() && QFileInfo::exists(exported.exportedPath), QStringLiteral("Export a copy before saving its origin."));
        require(!files::isWithin(destination, exported.asset.library) && absolute(destination) != absolute(exported.exportedPath) &&
                    !files::isWithin(destination, exported.exportedPath), QStringLiteral("Save the receipt outside the library and exported payload."));
        existingDirectory(QFileInfo(destination).absolutePath());
        const QJsonObject receipt{{"schema", "xips.use/v1"}, {"assetId", exported.asset.id}, {"name", exported.asset.name},
            {"revision", exported.snapshot.id}, {"contentHash", exported.snapshot.hash},
            {"sourceImmutable", exported.snapshot.id != "current" && exported.snapshot.id != "working"},
            {"path", QDir(QFileInfo(destination).absolutePath()).relativeFilePath(exported.exportedPath)},
            {"files", QJsonArray::fromStringList(exported.snapshot.files)}, {"category", exported.asset.category}};
        if (QFileInfo::exists(destination))
        {
            require(readJson(destination) == receipt, QStringLiteral("Receipt destination already exists with different content."));
            result.unchanged = true;
        }
        else ContentStore::publishJson(destination, receipt);
        result.exportedPath = absolute(destination);
    });
}
SnapshotResult SnapshotLibrary::renameSnapshot(const CatalogAsset &asset, const QString &revision,
                                               const QString &label)
{
    return operation([&](auto &result)
    {
        require(asset.referencePath.isEmpty(), QStringLiteral("Edit the version in its owning library."));
        require(!asset.legacy && revision != "current" && revision != "working",
                QStringLiteral("Choose an archived version."));
        const auto name = checkedRevisionLabel(label);
        LibraryLock lock(asset.library);
        const auto root = asset.discovered ? asset.historyRoot : asset.root;
        require(!root.isEmpty(), QStringLiteral("This asset has no saved history."));
        const auto path = child(root, ".xips.json");
        const auto original = bytes(path);
        const auto current = load(root, asset.library);
        healthyHistory(current);
        require(!current.legacy && current.id == asset.id && current.document == asset.document &&
                    QJsonDocument::fromJson(original).object() == current.document,
                QStringLiteral("Version details changed. Refresh and retry."));
        const auto snapshot = selected(current, revision);
        require(toJson(snapshot) == toJson(selected(asset, revision)),
                QStringLiteral("The selected version changed. Refresh and retry."));
        if (revisionLabel(snapshot) == name) result.unchanged = true;
        else
        {
            auto document = current.document;
            setRevisionLabel(document, snapshot, name, current.snapshots);
            writeJson(path, document, &original);
        }
        result.asset = resolveAsset(asset);
        result.snapshot = selected(result.asset, snapshot.id);
    });
}
SnapshotResult SnapshotLibrary::eraseSnapshot(const CatalogAsset &asset, const QString &revision,
                                              bool /*permanent*/)
{
    return operation(
        [&](auto &result)
        {
            require(asset.referencePath.isEmpty(), QStringLiteral("A reference cannot delete its source IP."));
            LibraryLock lock(asset.library);
            const auto definitionRoot = asset.discovered ? asset.historyRoot : asset.root;
            require(!definitionRoot.isEmpty(), QStringLiteral("This IP has no saved history."));
            result.asset = load(definitionRoot, asset.library);
            require(!result.asset.legacy && result.asset.id == asset.id,
                    QStringLiteral("The asset identity or format has changed. Refresh and retry."));
            if (asset.discovered)
            {
                const auto source = attachHistory(result.asset);
                require(source.root == absolute(asset.root) && source.sourceIsDirectory == asset.sourceIsDirectory,
                        QStringLiteral("Saved source location has changed. Refresh and retry."));
            }
            healthyHistory(result.asset);
            const auto snapshot = selected(result.asset, revision);
            const auto original = bytes(child(definitionRoot, ".xips.json"));
            verifySaved(result.asset, snapshot);
            const auto fresh = load(definitionRoot, asset.library);
            healthyHistory(fresh);
            require(fresh.id == asset.id && toJson(selected(fresh, snapshot.id)) == toJson(snapshot) &&
                        bytes(child(definitionRoot, ".xips.json")) == original,
                    QStringLiteral("The version changed during deletion. Refresh and retry."));
            // Preserve immutable manifests, ancestry and shared content for synchronization.
            ContentStore::publishJson(child(definitionRoot, ".xips/deleted-revisions/" + snapshot.id + ".json"),
                {{"assetId", asset.id}, {"revision", snapshot.id}, {"sequence", snapshot.sequence},
                 {"hash", snapshot.hash}, {"parents", QJsonArray::fromStringList(snapshot.parents)},
                 {"deleted", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
            result.asset = resolveAsset(asset);
        });
}
SnapshotResult SnapshotLibrary::eraseAsset(const CatalogAsset &asset, bool permanent)
{
    return operation(
        [&](auto &result)
        {
            require(!asset.discovered, QStringLiteral("Scanned files cannot be deleted from xIPs."));
            require(asset.referencePath.isEmpty(), QStringLiteral("A reference cannot delete its source IP."));
            LibraryLock lock(asset.library);
            const auto current = load(asset.root, asset.library);
            require(current.id == asset.id, QStringLiteral("The asset identity has changed."));
            healthyHistory(current);
            if (current.legacy)
            {
                QString error;
                OperationScope::publish();
                require(AssetLibraryService().deleteAsset(asset.library, legacyRecord(current),
                                                          permanent ? RemovalMode::Permanent
                                                                    : RemovalMode::MoveToTrash,
                                                          &result.retainedPath, &error),
                        error);
                return;
            }
            const auto files = payloadFiles(asset.root);
            const auto proof = digest(asset.root, files);
            for (const auto &snapshot : current.snapshots)
                verifySaved(current, snapshot);
            const QString isolated =
                child(QFileInfo(asset.root).absolutePath(), unique(".xips-deleted-"));
            OperationScope::publish();
            require(QDir().rename(asset.root, isolated),
                    QStringLiteral("Cannot isolate the asset"));
            result.retainedPath = isolated;
            require(payloadFiles(isolated) == files && digest(isolated, files) == proof,
                    QStringLiteral(
                        "The asset changed during deletion. The isolated directory was retained."));
            removeDirectory(isolated, permanent);
            result.retainedPath.clear();
        });
}
SnapshotResult SnapshotLibrary::migrate(const CatalogAsset &asset)
{
    return operation(
        [&](auto &result)
        {
            require(!asset.discovered, QStringLiteral("Scanned files are already available without conversion."));
            LibraryLock lock(asset.library);
            const auto current = load(asset.root, asset.library);
            require(
                current.legacy && current.id == asset.id,
                QStringLiteral("The asset does not need conversion, or its identity has changed."));
            const auto record = legacyRecord(current);
            AssetLibraryService service;
            const auto proof = service.assetDeletionProof(record);
            require(proof.ok(), proof.error);
            const auto inventory = service.versionInventory(asset.root);
            require(
                inventory.usable() && inventory.invalidVersions.isEmpty(),
                QStringLiteral(
                    "Legacy versions have issues. Check the original files before converting."));
            QTemporaryDir staging(
                child(QFileInfo(asset.root).absolutePath(), ".xips-convert-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot create conversion directory"));
            auto document = newDocument(current.id, current.name, current.category);
            document.insert("schemaVersion", 2);
            document.insert("description", current.description);
            document.insert("tags", QJsonArray::fromStringList(current.tags));
            QStringList versions;
            for (const auto &version : inventory.validVersions)
                versions.append(version.version);
            versions.append(QString());
            Snapshot previous;
            int sequence = 1;
            for (const QString &version : versions)
            {
                const auto plan = service.copyPlan(record, version);
                require(plan.ok(), plan.error);
                const QString revision = QString::number(sequence);
                const QString target = child(staging.path(), ".xips/revisions/" + revision);
                const QString exportPath =
                    plan.files.size() == 1 ? child(target, plan.files.first()) : target;
                require(QDir().mkpath(QFileInfo(exportPath).absolutePath()),
                        QStringLiteral("Cannot create conversion directory"));
                QString error;
                require(service.copyVersionPayload(record, version, exportPath, nullptr, &error),
                        error);
                Snapshot snapshot{revision,
                                  version.isEmpty()
                                      ? QStringLiteral("Legacy working copy")
                                      : QStringLiteral("Legacy version %1").arg(version),
                                  QDateTime::currentDateTimeUtc(),
                                  {},
                                  payloadFiles(target)};
                for (const auto &saved : inventory.validVersions)
                    if (saved.version == version && saved.createdAt.isValid())
                        snapshot.created = saved.createdAt;
                snapshot.hash = digest(target, snapshot.files);
                if (version.isEmpty() && snapshot.hash == previous.hash &&
                    snapshot.files == previous.files)
                {
                    removeDirectory(target, true);
                    continue;
                }
                append(document, snapshot);
                previous = snapshot;
                ++sequence;
            }
            writeJson(child(staging.path(), ".xips.json"), document);
            const auto confirmed = service.assetDeletionProof(record);
            require(confirmed.ok() && confirmed.fingerprint == proof.fingerprint,
                    QStringLiteral("The legacy asset changed during conversion."));
            const QString backup =
                child(QFileInfo(asset.root).absolutePath(), unique(".xips-legacy-"));
            require(QDir().rename(asset.root, backup),
                    QStringLiteral("Cannot retain the legacy asset directory"));
            result.retainedPath = backup;
            if (!QDir().rename(staging.path(), asset.root))
            {
                QDir().rename(backup, asset.root);
                throw Failure{QStringLiteral("Cannot publish the converted asset")};
            }
            staging.setAutoRemove(false);
            result.asset = load(asset.root, asset.library);
            result.snapshot = result.asset.snapshots.last();
        });
}
} // namespace xips
