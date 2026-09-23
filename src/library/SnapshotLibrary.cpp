#include "SnapshotLibrary.h"

#include "AssetLibraryService.h"
#include "AssetScanner.h"
#include "FileSystemUtil.h"
#include "manifest/ManifestService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <functional>
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
    return QStringList{QStringLiteral("module"), QStringLiteral("ip"), QStringLiteral("artifact"),
                       QStringLiteral("other")}
        .contains(category);
}
Snapshot parseSnapshot(const QJsonObject &object)
{
    Snapshot result{object.value("id").toString(), object.value("note").toString(),
                    QDateTime::fromString(object.value("created").toString(), Qt::ISODateWithMs),
                    object.value("hash").toString(), strings(object.value("files").toArray())};
    bool numeric = false;
    const qlonglong sequence = result.id.toLongLong(&numeric);
    require(numeric && sequence > 0 && QString::number(sequence) == result.id &&
                result.created.isValid() && result.hash.startsWith("sha256:") &&
                !result.files.isEmpty(),
            QStringLiteral("Invalid revision record"));
    QSet<QString> seen;
    for (const QString &file : result.files)
    {
        require(!file.isEmpty() && !QDir::isAbsolutePath(file) && QDir::cleanPath(file) == file &&
                    !file.startsWith("../") && file != ".." && !file.contains('\\') &&
                    !file.contains(':') && !seen.contains(file.toCaseFolded()),
                QStringLiteral("Invalid revision file path: %1").arg(file));
        seen.insert(file.toCaseFolded());
    }
    return result;
}
QJsonObject toJson(const Snapshot &snapshot)
{
    return {{"id", snapshot.id},
            {"note", snapshot.note},
            {"created", snapshot.created.toUTC().toString(Qt::ISODateWithMs)},
            {"hash", snapshot.hash},
            {"files", QJsonArray::fromStringList(snapshot.files)}};
}
CatalogAsset load(const QString &root, const QString &library)
{
    existingDirectory(root);
    require(files::isWithin(root, library) && absolute(root) != absolute(library),
            QStringLiteral("The asset must be inside the library."));
    CatalogAsset asset;
    asset.root = absolute(root);
    asset.library = absolute(library);
    asset.document = readJson(child(root, ".xips.json"));
    const int schema = asset.document.value("schemaVersion").toInt();
    require(schema == 1 || schema == 2, QStringLiteral("Unsupported asset format: %1").arg(root));
    asset.legacy = schema == 1;
    asset.id = asset.document.value("id").toString();
    asset.name = asset.document.value("name").toString();
    asset.description = asset.document.value("description").toString();
    asset.tags = strings(asset.document.value("tags").toArray());
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
        for (const auto &value : asset.document.value("revisions").toArray())
        {
            const auto snapshot = parseSnapshot(value.toObject());
            require(!ids.contains(snapshot.id), QStringLiteral("Duplicate revision number"));
            ids.insert(snapshot.id);
            asset.snapshots.append(snapshot);
        }
        std::sort(asset.snapshots.begin(), asset.snapshots.end(), [](const auto &a, const auto &b)
                  { return a.id.toLongLong() < b.id.toLongLong(); });
        require(!asset.snapshots.isEmpty() && asset.document.value("nextRevision").toInteger() >
                                                  asset.snapshots.last().id.toLongLong(),
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
void enumerate(const QString &root, const QString &directory, QStringList &result, bool source,
               bool artifact)
{
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
    require(payloadFiles(root) == snapshot.files && digest(root, snapshot.files) == snapshot.hash &&
                payloadFiles(root) == snapshot.files,
            QStringLiteral("Revision files were changed or corrupted: %1").arg(root));
}
Snapshot selected(const CatalogAsset &asset, const QString &id)
{
    for (const auto &snapshot : asset.snapshots)
        if (snapshot.id == id)
            return snapshot;
    throw Failure{QStringLiteral("Revision not found: %1").arg(id)};
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
    require(file.commit(), QStringLiteral("Cannot commit metadata: %1").arg(path));
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
Snapshot copySources(const QStringList &sources, const QString &destination, bool artifact,
                     const QString &id, const QString &note)
{
    const auto inputs = sourceFiles(sources, artifact);
    require(QDir().mkpath(destination), QStringLiteral("Cannot create revision directory"));
    Snapshot snapshot{id, note.trimmed(), QDateTime::currentDateTimeUtc(), {}, {}};
    QMap<QString, QByteArray> inputHashes;
    for (const auto &input : inputs)
    {
        const QString target = child(destination, input.relative);
        require(QDir().mkpath(QFileInfo(target).absolutePath()),
                QStringLiteral("Cannot create file directory"));
        safePath(target);
        QFile source(input.source);
        require(source.open(QIODevice::ReadOnly),
                QStringLiteral("Cannot read source: %1").arg(input.source));
        const auto expectedSize = source.size();
        const auto expectedTime = source.fileTime(QFileDevice::FileModificationTime);
        QFile output(target);
        require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                QStringLiteral("Cannot copy to: %1").arg(target));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 count = 0;
        while (!source.atEnd())
        {
            const auto data = source.read(1024 * 1024);
            require(source.error() == QFileDevice::NoError && output.write(data) == data.size(),
                    QStringLiteral("Copy failed: %1").arg(input.source));
            hash.addData(data);
            count += data.size();
        }
        require(output.flush() && count == expectedSize && source.size() == expectedSize &&
                    source.fileTime(QFileDevice::FileModificationTime) == expectedTime,
                QStringLiteral("Source changed while copying: %1").arg(input.source));
        output.close();
        inputHashes.insert(input.source, hash.result());
        snapshot.files.append(input.relative);
    }
    const auto confirmed = sourceFiles(sources, artifact);
    require(confirmed.size() == inputs.size(), QStringLiteral("The source file list has changed."));
    for (qsizetype i = 0; i < inputs.size(); ++i)
    {
        require(inputs[i].source == confirmed[i].source &&
                    inputs[i].relative == confirmed[i].relative,
                QStringLiteral("The source file list has changed."));
        safePath(inputs[i].source);
        QFile file(inputs[i].source);
        require(file.open(QIODevice::ReadOnly),
                QStringLiteral("The source file is no longer available."));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        require(hash.addData(&file) && hash.result() == inputHashes.value(inputs[i].source),
                QStringLiteral("Source changed while copying: %1").arg(inputs[i].source));
        const auto target = child(destination, inputs[i].relative);
        safePath(target);
        QFile copied(target);
        QCryptographicHash copiedHash(QCryptographicHash::Sha256);
        require(copied.open(QIODevice::ReadOnly) && copiedHash.addData(&copied) &&
                    copiedHash.result() == inputHashes.value(inputs[i].source),
                QStringLiteral("Copied file failed verification: %1").arg(target));
    }
    snapshot.hash = digest(destination, snapshot.files);
    verify(destination, snapshot);
    return snapshot;
}
QJsonObject newDocument(const QString &id, const QString &name, const QString &category)
{
    require(!name.trimmed().isEmpty() && validCategory(category),
            QStringLiteral("Enter a name and choose a category."));
    return {{"schemaVersion", 2},   {"id", id},          {"name", name.trimmed()},
            {"category", category}, {"nextRevision", 1}, {"revisions", QJsonArray{}}};
}
void append(QJsonObject &document, const Snapshot &snapshot)
{
    auto revisions = document.value("revisions").toArray();
    revisions.append(toJson(snapshot));
    document.insert("revisions", revisions);
    document.insert("nextRevision", snapshot.id.toLongLong() + 1);
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
        function(result);
        result.ok = true;
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
        std::function<void(const QString &)> visit = [&](const QString &directory)
        {
            for (const auto &entry : QDir(directory).entryInfoList(
                     QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
            {
                if (entry.fileName().startsWith(".xips-"))
                    result.problems.append(
                        QStringLiteral("Retained data: %1").arg(entry.absoluteFilePath()));
                if (entry.fileName().startsWith('.') || files::isLinkLike(entry))
                    continue;
                const QString path = entry.absoluteFilePath();
                if (QFileInfo::exists(child(path, ".xips.json")))
                {
                    try
                    {
                        const auto asset = load(path, library);
                        result.assets.append(asset);
                        if (!asset.legacy)
                        {
                            QSet<QString> recorded;
                            for (const auto &snapshot : asset.snapshots)
                                recorded.insert(snapshot.id);
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
                    visit(path);
            }
        };
        visit(library);
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
    }
    catch (const Failure &failure)
    {
        result.problems.append(failure.message);
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
    if (category == "artifact")
        return QStringLiteral("Artifact");
    return QStringLiteral("Other");
}
SnapshotResult SnapshotLibrary::describe(const CatalogAsset &asset)
{
    return operation(
        [&](auto &result)
        {
            result.asset = load(asset.root, asset.library);
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
            result.asset = load(asset.root, asset.library);
            require(!result.asset.legacy && result.asset.id == asset.id,
                    QStringLiteral("The asset identity has changed."));
            result.snapshot = selected(result.asset, revision);
            verify(revisionRoot(result.asset, result.snapshot), result.snapshot);
        });
}
SnapshotResult SnapshotLibrary::collect(const QString &library, const QStringList &sources,
                                        const QString &name, const QString &category,
                                        const QString &note)
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
            result.snapshot = copySources(sources, child(staging.path(), ".xips/revisions/1"),
                                          category == "artifact", "1", note);
            append(document, result.snapshot);
            writeJson(child(staging.path(), ".xips.json"), document);
            safePath(destination);
            require(!QFileInfo::exists(destination) && QDir().rename(staging.path(), destination),
                    QStringLiteral("Cannot publish asset"));
            staging.setAutoRemove(false);
            result.asset = load(destination, library);
        });
}
SnapshotResult SnapshotLibrary::update(const CatalogAsset &asset, const QStringList &sources,
                                       const QString &note)
{
    return operation(
        [&](auto &result)
        {
            LibraryLock lock(asset.library);
            result.asset = load(asset.root, asset.library);
            require(result.asset.id == asset.id && !result.asset.legacy,
                    QStringLiteral("Convert this legacy asset first."));
            const QByteArray original = bytes(child(asset.root, ".xips.json"));
            auto document = result.asset.document;
            const QString id = QString::number(document.value("nextRevision").toInteger());
            const QString versions = child(asset.root, ".xips/revisions");
            existingDirectory(versions);
            QTemporaryDir staging(child(versions, ".pending-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot create revision"));
            result.snapshot =
                copySources(sources, staging.path(), result.asset.category == "artifact", id, note);
            const auto &latest = result.asset.snapshots.last();
            if (result.snapshot.hash == latest.hash && result.snapshot.files == latest.files)
            {
                verify(revisionRoot(result.asset, latest), latest);
                result.unchanged = true;
                result.snapshot = latest;
                return;
            }
            require(bytes(child(asset.root, ".xips.json")) == original,
                    QStringLiteral("The asset has been updated. Refresh to continue."));
            const QString destination = child(versions, id);
            safePath(destination);
            require(!QFileInfo::exists(destination) && QDir().rename(staging.path(), destination),
                    QStringLiteral("Revision directory exists or cannot be published: %1")
                        .arg(destination));
            staging.setAutoRemove(false);
            result.retainedPath = destination;
            verify(destination, result.snapshot);
            append(document, result.snapshot);
            writeJson(child(asset.root, ".xips.json"), document, &original);
            result.asset = load(asset.root, asset.library);
            result.retainedPath.clear();
        });
}
SnapshotResult SnapshotLibrary::edit(const CatalogAsset &asset, const QString &name,
                                     const QString &category, const QString &description)
{
    return operation(
        [&](auto &result)
        {
            LibraryLock lock(asset.library);
            result.asset = load(asset.root, asset.library);
            require(result.asset.id == asset.id && !result.asset.legacy,
                    QStringLiteral("Convert this legacy asset first."));
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
                                               const QString &destination)
{
    return operation(
        [&](auto &result)
        {
            result.asset = load(asset.root, asset.library);
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
                require(AssetLibraryService().copyVersionPayload(record, oldVersion, destination,
                                                                 &result.exportedPath, &error),
                        error);
                result.snapshot = {revision, {}, {}, plan.payloadFingerprint, plan.files};
                return;
            }
            result.snapshot = selected(result.asset, revision);
            const QString source = revisionRoot(result.asset, result.snapshot);
            verify(source, result.snapshot);
            QTemporaryDir staging(
                child(QFileInfo(destination).absolutePath(), ".xips-export-XXXXXX"));
            require(staging.isValid(), QStringLiteral("Cannot create export directory"));
            const QString payload = child(staging.path(), "payload");
            QDir().mkpath(payload);
            for (const QString &file : result.snapshot.files)
            {
                QDir().mkpath(QFileInfo(child(payload, file)).absolutePath());
                require(QFile::copy(child(source, file), child(payload, file)),
                        QStringLiteral("Copy failed: %1").arg(file));
            }
            verify(payload, result.snapshot);
            verify(source, result.snapshot);
            safePath(destination);
            const QString published = result.snapshot.files.size() == 1
                                          ? child(payload, result.snapshot.files.first())
                                          : payload;
            require(!QFileInfo::exists(destination) && QDir().rename(published, destination),
                    QStringLiteral("Cannot publish exported copy"));
            result.exportedPath = absolute(destination);
        });
}
SnapshotResult SnapshotLibrary::eraseSnapshot(const CatalogAsset &asset, const QString &revision,
                                              bool permanent)
{
    return operation(
        [&](auto &result)
        {
            LibraryLock lock(asset.library);
            result.asset = load(asset.root, asset.library);
            require(!result.asset.legacy && result.asset.id == asset.id,
                    QStringLiteral("Convert this legacy asset first."));
            require(result.asset.snapshots.size() > 1,
                    QStringLiteral("The last revision cannot be deleted on its own. Delete the "
                                   "entire asset instead."));
            const auto snapshot = selected(result.asset, revision);
            const QString source = revisionRoot(result.asset, snapshot);
            const auto original = bytes(child(asset.root, ".xips.json"));
            verify(source, snapshot);
            const QString isolated = child(asset.root, ".xips/" + unique(".removed-"));
            require(QDir().rename(source, isolated),
                    QStringLiteral("Cannot isolate the revision for deletion"));
            result.retainedPath = isolated;
            verify(isolated, snapshot);
            auto document = result.asset.document;
            QJsonArray retained;
            for (const auto &item : document.value("revisions").toArray())
                if (item.toObject().value("id").toString() != revision)
                    retained.append(item);
            document.insert("revisions", retained);
            try
            {
                writeJson(child(asset.root, ".xips.json"), document, &original);
            }
            catch (...)
            {
                if (!QFileInfo::exists(source) && QDir().rename(isolated, source))
                    result.retainedPath.clear();
                throw;
            }
            removeDirectory(isolated, permanent);
            result.retainedPath.clear();
            result.asset = load(asset.root, asset.library);
        });
}
SnapshotResult SnapshotLibrary::eraseAsset(const CatalogAsset &asset, bool permanent)
{
    return operation(
        [&](auto &result)
        {
            LibraryLock lock(asset.library);
            const auto current = load(asset.root, asset.library);
            require(current.id == asset.id, QStringLiteral("The asset identity has changed."));
            if (current.legacy)
            {
                QString error;
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
                verify(revisionRoot(current, snapshot), snapshot);
            const QString isolated =
                child(QFileInfo(asset.root).absolutePath(), unique(".xips-deleted-"));
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
