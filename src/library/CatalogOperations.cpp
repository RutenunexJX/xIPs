#include "CatalogOperations.h"
#include "CatalogIndex.h"
#include "FileSystemUtil.h"
#include "LibraryTransaction.h"
#include "OperationControl.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <memory>
#include <vector>
namespace xips
{
OperationError::OperationError(QString c, QString message, int e, QJsonObject d)
    : std::runtime_error(message.toUtf8().constData()), code(std::move(c)), exitCode(e), details(std::move(d))
{
}
namespace
{
const QMap<QString, QStringList> fields = {
    {"list", {"query", "category"}},
    {"history", {"asset"}},
    {"working", {"asset"}},
    {"verify", {"asset", "revision"}},
    {"groups", {}},
    {"references", {}},
    {"create",
     {"name", "category", "source", "description", "indexes", "sources", "files", "move", "note", "version"}},
    {"collect", {"name", "category", "sources", "note"}},
    {"import", {"asset", "sources", "files", "move", "note", "version"}},
    {"save", {"asset", "sources", "files", "note"}},
    {"metadata", {"asset", "name", "category", "description", "indexes"}},
    {"version-rename", {"asset", "revision", "name"}},
    {"delete-version", {"asset", "revision"}},
    {"restore-version", {"asset", "revision"}},
    {"delete-asset", {"asset"}},
    {"unregister", {"asset"}},
    {"relocate", {"asset", "source"}},
    {"migrate", {"asset"}},
    {"export", {"asset", "revision", "destination"}},
    {"group-create", {"name"}},
    {"group-rename", {"group", "name"}},
    {"group-delete", {"group"}},
    {"group-member", {"group", "asset", "included"}},
    {"group-move", {"group", "targetGroup", "asset"}},
    {"reference-add", {"asset", "revision", "destinationLibrary"}},
    {"reference-verify", {"asset", "ownerLibrary", "revision"}},
    {"reference-update", {"asset", "ownerLibrary", "revision"}},
    {"reference-remove", {"asset"}},
    {"receipt-create", {"asset", "revision", "payload", "receipt"}},
    {"receipt-verify", {"receipt"}},
    {"receipt-recover", {"receipt", "destination"}}};
const QSet<QString> reads = {"list",   "history",    "working",          "verify",
                             "groups", "references", "reference-verify", "receipt-verify"};
[[noreturn]] void invalid(const QString &message)
{
    throw OperationError("invalid_request", message, 2);
}
void require(bool condition, const QString &message)
{
    if (!condition)
        invalid(message);
}
QString digest(const QJsonObject &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
QString string(const QJsonObject &request, const QString &key, bool required = false)
{
    const auto value = request.value(key);
    require(value.isUndefined() || value.isString(), key + " must be a string.");
    const auto text = value.toString();
    require(!required || !text.trimmed().isEmpty(), key + " is required.");
    return text;
}
QStringList strings(const QJsonObject &request, const QString &key)
{
    const auto value = request.value(key);
    require(value.isUndefined() || value.isArray(), key + " must be an array of strings.");
    QStringList result;
    for (const auto &item : value.toArray())
    {
        require(item.isString() && !item.toString().isEmpty(), key + " contains an invalid value.");
        result.append(item.toString());
    }
    require(result.size() <= 100000, "Too many entries in " + key);
    require(QSet<QString>(result.cbegin(), result.cend()).size() == result.size(),
            key + " contains duplicates.");
    return result;
}
void check(const SnapshotResult &result)
{
    if (result.ok)
        return;
    throw OperationError(result.cancelled ? "cancelled" : "operation_failed", result.error,
                         result.cancelled ? 5 : 3,
                         {{"retainedPath", result.retainedPath},
                          {"retainedSources", QJsonArray::fromStringList(result.retainedSources)},
                          {"asset", CatalogOperations::assetJson(result.asset)},
                          {"revision", result.snapshot.id}});
}
CatalogResult catalog(const QString &library)
{
    require(QFileInfo(library).isDir(), "library must be an existing directory.");
    const auto result = SnapshotLibrary::scan(library);
    if (result.cancelled)
        throw OperationError("cancelled", "Catalog scan cancelled.", 5);
    return result;
}
CatalogAsset find(const CatalogResult &catalog, const QString &id)
{
    QList<CatalogAsset> matches;
    for (const auto &asset : catalog.assets)
        if (asset.id.compare(id, Qt::CaseInsensitive) == 0)
            matches.append(asset);
    if (matches.size() != 1)
        throw OperationError(matches.isEmpty() ? "not_found" : "ambiguous_id",
                             "Asset ID is missing or ambiguous: " + id, matches.isEmpty() ? 4 : 3);
    return matches.first();
}
QJsonArray groupJson(QList<CatalogGroup> groups)
{
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    QJsonArray result;
    for (auto group : groups)
    {
        group.members.sort();
        result.append(QJsonObject{
            {"id", group.id}, {"name", group.name}, {"members", QJsonArray::fromStringList(group.members)}});
    }
    return result;
}
QJsonObject payloadJson(const PayloadPreview &p)
{
    QJsonObject objects;
    for (auto it = p.objects.cbegin(); it != p.objects.cend(); ++it)
        objects.insert(it.key(), QJsonObject{{"hash", it->hash}, {"size", it->size}});
    return {{"assetId", p.assetId},
            {"revision", p.revision},
            {"comparisonRevision", p.comparisonRevision},
            {"files", QJsonArray::fromStringList(p.files)},
            {"objects", objects},
            {"added", QJsonArray::fromStringList(p.added)},
            {"modified", QJsonArray::fromStringList(p.modified)},
            {"removed", QJsonArray::fromStringList(p.removed)},
            {"excluded", QJsonArray::fromStringList(p.excluded)},
            {"heads", QJsonArray::fromStringList(p.heads)},
            {"bytes", p.bytes},
            {"unchanged", qint64(p.unchanged)}};
}
PayloadPreview payloadFromJson(const QJsonObject &json)
{
    PayloadPreview p;
    p.assetId = json.value("assetId").toString();
    p.revision = json.value("revision").toString();
    p.comparisonRevision = json.value("comparisonRevision").toString();
    p.files = strings(json, "files");
    p.heads = strings(json, "heads");
    p.added = strings(json, "added");
    p.modified = strings(json, "modified");
    p.removed = strings(json, "removed");
    p.excluded = strings(json, "excluded");
    p.bytes = json.value("bytes").toInteger();
    p.unchanged = json.value("unchanged").toInteger();
    const auto objects = json.value("objects").toObject();
    for (auto it = objects.begin(); it != objects.end(); ++it)
    {
        const auto value = it.value().toObject();
        p.objects.insert(it.key(), {value.value("hash").toString(), value.value("size").toInteger()});
    }
    return p;
}
CatalogDefinition definition(const QJsonObject &r, const CatalogAsset &asset = {})
{
    CatalogDefinition d;
    d.name = r.contains("name") ? string(r, "name", true) : asset.name;
    d.category = string(r, "category");
    if (d.category.isEmpty())
        d.category = asset.id.isEmpty() ? "module" : asset.category;
    d.source = string(r, "source");
    d.description = r.contains("description") ? string(r, "description") : asset.description;
    d.indexes = asset.indexes;
    if (r.contains("indexes"))
    {
        require(r.value("indexes").isObject(), "indexes must be an object of string arrays.");
        d.indexes.clear();
        const auto indexes = r.value("indexes").toObject();
        for (auto it = indexes.begin(); it != indexes.end(); ++it)
            d.indexes.insert(it.key(), strings(indexes, it.key()));
    }
    require(!d.name.trimmed().isEmpty(), "name is required.");
    require(QStringList{"module", "ip", "project", "artifact", "other"}.contains(d.category),
            "Unsupported category.");
    return d;
}
QString exactRevision(const CatalogAsset &asset, const QJsonObject &r)
{
    const auto selector = string(r, "revision", true);
    const auto revisions = SnapshotLibrary::matchingRevisions(asset, selector);
    if (revisions.size() != 1)
        throw OperationError("revision_not_found",
                             "Version selector is missing or ambiguous. Use an exact revision ID.", 4);
    return revisions.first().id;
}
PayloadPreview importPreview(const QJsonObject &r)
{
    const auto sources = strings(r, "sources");
    require(!sources.isEmpty(), "sources is required.");
    const auto result = SnapshotLibrary::previewImport(sources);
    check(result);
    auto p = result.preview;
    if (r.contains("files"))
    {
        const auto selected = strings(r, "files");
        require(!selected.isEmpty(), "Choose at least one import file.");
        QMap<QString, ContentObject> objects;
        p.bytes = 0;
        for (const auto &file : selected)
        {
            require(p.objects.contains(file), "Unknown import file: " + file);
            objects.insert(file, p.objects.value(file));
            p.bytes += p.objects.value(file).size;
        }
        p.objects = objects;
        p.files = objects.keys();
    }
    return p;
}
PayloadPreview payload(const QJsonObject &r, const CatalogAsset &asset)
{
    const auto action = r.value("action").toString();
    SnapshotResult result;
    if (action == "import" || (action == "create" && r.contains("sources")))
        return importPreview(r);
    if (action == "collect")
        result =
            SnapshotLibrary::previewCollect(strings(r, "sources"), r.value("category").toString("module"));
    else if (action == "save")
    {
        require(asset.discovered ? !r.contains("sources") : !r.contains("files"),
                asset.discovered ? "Working-copy saves use files, not external sources."
                                 : "Archived-only updates select their input sources; files "
                                   "applies to a working copy.");
        if (asset.discovered)
            result = r.contains("files") ? SnapshotLibrary::previewSelected(asset, strings(r, "files"))
                                         : SnapshotLibrary::previewWorking(asset);
        else
        {
            require(!strings(r, "sources").isEmpty(),
                    "An archived-only asset requires sources for a new revision.");
            result = SnapshotLibrary::previewSave(asset, strings(r, "sources"));
        }
    }
    else if (action == "export" || action == "receipt-create" || action == "reference-add")
        result = SnapshotLibrary::previewExport(asset, exactRevision(asset, r));
    else
        return {};
    check(result);
    return result.preview;
}
QStringList libraries(const QJsonObject &r)
{
    QStringList result{r.value("library").toString()};
    for (const auto &key : {"destinationLibrary", "ownerLibrary"})
        if (r.contains(key))
            result.append(r.value(key).toString());
    result.removeDuplicates();
    std::sort(result.begin(), result.end(),
              [](const QString &a, const QString &b) { return a.toCaseFolded() < b.toCaseFolded(); });
    return result;
}
QJsonObject snapshotResult(const SnapshotResult &result)
{
    check(result);
    return {{"asset", CatalogOperations::assetJson(result.asset)},
            {"snapshot", CatalogOperations::snapshotJson(result.snapshot)},
            {"unchanged", result.unchanged},
            {"path", result.exportedPath},
            {"retainedPath", result.retainedPath},
            {"retainedSources", QJsonArray::fromStringList(result.retainedSources)}};
}
SnapshotResult receiptPayload(const QJsonObject &r, const CatalogAsset &asset)
{
    auto verified = SnapshotLibrary::verifySnapshot(asset, exactRevision(asset, r));
    check(verified);
    verified.exportedPath = string(r, "payload", true);
    const bool singleFile = verified.snapshot.files.size() == 1 && QFileInfo(verified.exportedPath).isFile();
    require(singleFile || QFileInfo(verified.exportedPath).isDir(),
            "payload must name an existing export file or directory.");
    for (const auto &file : verified.snapshot.files)
        if (ContentStore::fingerprint(singleFile ? verified.exportedPath
                                                 : QDir(verified.exportedPath).filePath(file)) !=
            verified.snapshot.objects.value(file))
            throw OperationError("payload_mismatch", "Exported payload differs: " + file);
    return verified;
}
} // namespace
QJsonObject CatalogOperations::capabilities()
{
    QJsonArray actions;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        actions.append(QJsonObject{{"action", it.key()},
                                   {"mutation", isMutation(it.key())},
                                   {"fields", QJsonArray::fromStringList(it.value())}});
    return {{"schema", "xips.operations/v1"},
            {"actions", actions},
            {"required", QJsonArray{"action", "library"}},
            {"writeProtocol", "--request request.json --dry-run; save the "
                              "response, then --apply preview.json"},
            {"relativePaths", "request file directory; normalized absolute paths "
                              "are bound into the preview"},
            {"versionSelectors", "exact UUID preferred; existing unique "
                                 "revision/sequence aliases remain supported"},
            {"deletion", "versions are recoverable tombstones; delete-asset uses the OS "
                         "recycle bin, recovered there; discovered sources use unregister"},
            {"exitCodes", QJsonObject{{"success", 0},
                                      {"invalidRequest", 2},
                                      {"conflictOrFailure", 3},
                                      {"notFound", 4},
                                      {"cancelled", 5},
                                      {"guiPending", 6}}}};
}
bool CatalogOperations::isMutation(const QString &action)
{
    return fields.contains(action) && !reads.contains(action);
}
QJsonObject CatalogOperations::normalize(QJsonObject r, const QString &baseDirectory)
{
    const auto action = string(r, "action", true);
    require(fields.contains(action), "Unknown action: " + action);
    const auto allowed = fields.value(action);
    for (auto it = r.begin(); it != r.end(); ++it)
        require(it.key() == "action" || it.key() == "library" || allowed.contains(it.key()),
                "Unknown field for " + action + ": " + it.key());
    string(r, "library", true);
    for (const auto &key :
         {"library", "source", "destination", "destinationLibrary", "ownerLibrary", "receipt", "payload"})
        if (r.contains(key))
        {
            const auto value = string(r, key, true);
            r.insert(key, files::normalizedAbsolute(QDir(baseDirectory).absoluteFilePath(value)));
            ContentStore::safePath(r.value(key).toString());
        }
    if (r.contains("sources"))
    {
        const auto sources = strings(r, "sources");
        require(sources.size() <= 4096, "Too many sources.");
        QJsonArray normalized;
        for (const auto &path : sources)
        {
            const auto value = files::normalizedAbsolute(QDir(baseDirectory).absoluteFilePath(path));
            ContentStore::safePath(value);
            normalized.append(value);
        }
        r.insert("sources", normalized);
    }
    if (r.contains("files"))
        strings(r, "files");
    for (const auto &key : {"move", "included"})
        require(!r.contains(key) || r.value(key).isBool(), QString(key) + " must be a boolean.");
    for (const auto &key : allowed)
        if (key != "indexes" && key != "sources" && key != "files" && key != "move" && key != "included")
            string(r, key);
    if (action == "create" || action == "collect" || action == "group-create" || action == "group-rename" ||
        action == "version-rename")
        string(r, "name", true);
    if (allowed.contains("asset"))
        string(r, "asset", true);
    if (allowed.contains("group"))
        string(r, "group", true);
    if (action == "group-move")
        string(r, "targetGroup", true);
    if (action == "group-member")
        require(r.contains("included"), "included is required.");
    if (action == "collect" || action == "import")
        require(!strings(r, "sources").isEmpty(), "sources is required.");
    if (action == "create")
        require(!(r.contains("source") && r.contains("sources")), "Choose registration or import, not both.");
    if (action == "relocate")
        string(r, "source", true);
    if (action == "reference-add")
        string(r, "destinationLibrary", true);
    if (action == "reference-update" || action == "reference-verify")
        string(r, "ownerLibrary", true);
    if (action.startsWith("receipt-"))
        string(r, "receipt", true);
    if (action == "export" || action == "receipt-recover")
    {
        const auto destination = string(r, "destination", true);
        require(!QFileInfo::exists(destination) && QFileInfo(QFileInfo(destination).absolutePath()).isDir(),
                "destination must be new, with an existing parent directory.");
    }
    return r;
}
QJsonObject CatalogOperations::snapshotJson(const Snapshot &s)
{
    return {{"id", s.id},
            {"label", (s.id.isEmpty() || s.id == "current" || s.id == "working")
                          ? s.id
                          : SnapshotLibrary::revisionLabel(s)},
            {"sequence", s.sequence},
            {"hash", s.hash},
            {"immutable", !s.id.isEmpty() && s.id != "current" && s.id != "working"},
            {"note", s.note},
            {"created", s.created.toString(Qt::ISODateWithMs)},
            {"parents", QJsonArray::fromStringList(s.parents)},
            {"files", QJsonArray::fromStringList(s.files)}};
}
QJsonObject CatalogOperations::assetJson(const CatalogAsset &a)
{
    QJsonObject indexes;
    for (auto it = a.indexes.cbegin(); it != a.indexes.cend(); ++it)
        indexes.insert(it.key(), QJsonArray::fromStringList(it.value()));
    return {{"id", a.id},
            {"name", a.name},
            {"category", a.category},
            {"description", a.description},
            {"indexes", indexes},
            {"tags", QJsonArray::fromStringList(a.tags)},
            {"workingRoot", a.discovered ? a.root : QString()},
            {"hasWorkingCopy", a.discovered},
            {"legacy", a.legacy},
            {"heads", QJsonArray::fromStringList(SnapshotLibrary::heads(a))},
            {"reference", a.referencePath},
            {"pinnedRevision", a.pinnedRevision},
            {"problems", QJsonArray::fromStringList(a.problems)}};
}
QString CatalogOperations::viewToken(const QList<CatalogAsset> &assets, const QList<CatalogGroup> &groups)
{
    auto sorted = assets;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.id != b.id ? a.id < b.id : a.root < b.root; });
    QJsonArray state;
    for (const auto &a : sorted)
    {
        QJsonArray revisions;
        for (const auto &s : a.snapshots)
            revisions.append(snapshotJson(s));
        state.append(QJsonObject{{"asset", assetJson(a)},
                                 {"definition", a.document},
                                 {"reference", a.referenceRecord},
                                 {"revisions", revisions},
                                 {"deleted", QJsonArray::fromStringList(a.deletedRevisionParents.keys())}});
    }
    return digest({{"assets", state}, {"groups", groupJson(groups)}});
}
QJsonObject CatalogOperations::read(const QJsonObject &r)
{
    const auto action = r.value("action").toString(), library = r.value("library").toString();
    require(reads.contains(action), "This action requires a preview and apply.");
    const auto c = catalog(library);
    QJsonObject data{{"library", library},
                     {"viewToken", viewToken(c.assets, c.groups)},
                     {"problems", QJsonArray::fromStringList(c.problems)}};
    if (action == "groups")
    {
        data.insert("groups", groupJson(c.groups));
        return data;
    }
    if (action == "list" || action == "references")
    {
        QString error;
        const auto terms = CatalogIndex::queryTerms(r.value("query").toString(), &error);
        require(error.isEmpty(), error);
        QJsonArray values;
        for (const auto &a : c.assets)
        {
            if (action == "references" && a.referencePath.isEmpty())
                continue;
            if (!r.value("category").toString().isEmpty() && a.category != r.value("category").toString())
                continue;
            if (CatalogIndex::matches(a, terms))
                values.append(assetJson(a));
        }
        data.insert("assets", values);
        return data;
    }
    if (action == "receipt-verify")
    {
        data.insert("result",
                    snapshotResult(SnapshotLibrary::verifyReceipt(r.value("receipt").toString(), library)));
        return data;
    }
    const auto a = find(c, string(r, "asset", true));
    data.insert("asset", assetJson(a));
    if (action == "history")
    {
        QJsonArray versions;
        for (const auto &s : a.snapshots)
            if (s.id != "current" && s.id != "working")
                versions.append(snapshotJson(s));
        data.insert("versions", versions);
        data.insert("deletedRevisionIds", QJsonArray::fromStringList(a.deletedRevisionParents.keys()));
    }
    else if (action == "working")
    {
        if (a.discovered && a.referencePath.isEmpty())
        {
            const auto p = SnapshotLibrary::previewWorking(a);
            check(p);
            data.insert("working", payloadJson(p.preview));
            data.insert("changed", !p.preview.added.isEmpty() || !p.preview.modified.isEmpty() ||
                                       !p.preview.removed.isEmpty());
        }
        else
        {
            data.insert("working", QJsonValue::Null);
            data.insert("changed", false);
        }
    }
    else if (action == "verify")
        data.insert("result", snapshotResult(SnapshotLibrary::verifySnapshot(a, exactRevision(a, r))));
    else if (action == "reference-verify")
    {
        const auto target = SnapshotLibrary::referenceTarget(a, r.value("ownerLibrary").toString());
        check(target);
        const auto revision = r.value("revision").toString(a.pinnedRevision);
        data.insert("result", snapshotResult(SnapshotLibrary::verifySnapshot(target.asset, revision)));
    }
    return data;
}
QJsonObject CatalogOperations::preview(const QJsonObject &r)
{
    const auto action = r.value("action").toString();
    require(isMutation(action), "This action is read-only.");
    const auto c = catalog(r.value("library").toString());
    const auto a = r.contains("asset") ? find(c, r.value("asset").toString()) : CatalogAsset{};
    QJsonObject guards;
    for (const auto &library : libraries(r))
    {
        const auto state = catalog(library);
        guards.insert(library, viewToken(state.assets, state.groups));
    }
    const auto p = payload(r, a);
    QJsonObject detail;
    if (action == "export")
        detail.insert("destinationKind", p.files.size() == 1 ? "file" : "directory");
    if (action == "create" || action == "collect" || action == "metadata")
        definition(r, a);
    if (action == "restore-version")
        detail = snapshotResult(SnapshotLibrary::deletedSnapshot(a, string(r, "revision", true)));
    if (action == "delete-version" || action == "version-rename")
        detail = snapshotResult(SnapshotLibrary::verifySnapshot(a, exactRevision(a, r)));
    if (action == "reference-update")
    {
        const auto target = SnapshotLibrary::referenceTarget(a, string(r, "ownerLibrary", true));
        check(target);
        detail =
            snapshotResult(SnapshotLibrary::verifySnapshot(target.asset, exactRevision(target.asset, r)));
    }
    if (action == "receipt-create")
        detail = snapshotResult(receiptPayload(r, a));
    if (action == "receipt-recover")
        detail = snapshotResult(SnapshotLibrary::verifyReceipt(r.value("receipt").toString(),
                                                               r.value("library").toString(), false));
    if (action.startsWith("receipt-"))
    {
        const auto receipt = r.value("receipt").toString();
        if (QFileInfo::exists(receipt))
        {
            const auto hash = ContentStore::fingerprint(receipt);
            detail.insert("receiptHash", hash.hash);
        }
    }
    QJsonObject plan{{"schema", "xips.operation-plan/v1"},
                     {"request", r},
                     {"guards", guards},
                     {"payload", payloadJson(p)},
                     {"detail", detail}};
    plan.insert("token", digest(plan));
    return plan;
}
QJsonObject CatalogOperations::apply(const QJsonObject &submitted)
{
    auto original = submitted;
    const auto token = original.take("token").toString();
    require(submitted.value("schema") == "xips.operation-plan/v1" && !token.isEmpty() &&
                digest(original) == token,
            "Invalid or altered preview plan.");
    QJsonObject r;
    try
    {
        r = normalize(submitted.value("request").toObject(), QDir::currentPath());
    }
    catch (const std::exception &error)
    {
        throw OperationError("preview_conflict",
                             "Preview input is no longer valid: " + QString::fromUtf8(error.what()));
    }
    require(r == submitted.value("request").toObject(), "Preview request was not normalized.");
    std::vector<std::unique_ptr<LibraryTransaction>> locks;
    for (const auto &library : libraries(r))
        locks.push_back(std::make_unique<LibraryTransaction>(library));
    QJsonObject current;
    try
    {
        current = preview(r);
    }
    catch (const OperationError &error)
    {
        if (error.exitCode == 5)
            throw;
        throw OperationError("preview_conflict",
                             "Preview no longer applies: " + QString::fromUtf8(error.what()), 3,
                             error.details);
    }
    if (current != submitted)
        throw OperationError("preview_conflict", "Library, selected files, or destination changed "
                                                 "after preview. Review a fresh preview.");
    OperationScope::checkpoint();
    const auto library = r.value("library").toString(), action = r.value("action").toString();
    const auto c = catalog(library);
    const auto a = r.contains("asset") ? find(c, r.value("asset").toString()) : CatalogAsset{};
    const auto p = payloadFromJson(current.value("payload").toObject());
    SnapshotResult result;
    QJsonObject data;
    if (action.startsWith("group-"))
    {
        OperationScope::publish();
        GroupResult group;
        const auto id = r.value("group").toString();
        if (action != "group-create")
        {
            bool found = false;
            for (const auto &g : c.groups)
                found |= g.id == id;
            require(found, "Group ID not found.");
        }
        if (action == "group-create")
            group = CatalogGroups::create(library, r.value("name").toString());
        else if (action == "group-rename")
            group = CatalogGroups::rename(library, id, r.value("name").toString());
        else if (action == "group-delete")
            group = CatalogGroups::erase(library, id);
        else if (action == "group-member")
            group = CatalogGroups::setMember(library, id, a.id, r.value("included").toBool());
        else
            group = CatalogGroups::moveMember(library, id, r.value("targetGroup").toString(), a.id);
        if (!group.ok)
            throw OperationError("operation_failed", group.error);
        data.insert("groups", groupJson(CatalogGroups::scan(library).groups));
        data.insert("groupId", group.group.id);
    }
    else
    {
        const ImportRequest imported{strings(r, "sources"), p, r.value("move").toBool(),
                                     r.value("note").toString(), r.value("version").toString()};
        if (action == "create")
            result =
                SnapshotLibrary::create(library, definition(r), r.contains("sources") ? &imported : nullptr);
        else if (action == "collect")
            result = SnapshotLibrary::collect(library, strings(r, "sources"), r.value("name").toString(),
                                              r.value("category").toString("module"),
                                              r.value("note").toString(), &p);
        else if (action == "import")
            result = SnapshotLibrary::importAndSave(a, imported);
        else if (action == "save")
            result = a.discovered
                         ? SnapshotLibrary::saveSelected(a, p.files, r.value("note").toString(), &p)
                         : SnapshotLibrary::update(a, strings(r, "sources"), r.value("note").toString(), &p);
        else if (action == "metadata")
            result = SnapshotLibrary::setDefinition(a, definition(r, a));
        else if (action == "version-rename")
            result = SnapshotLibrary::renameSnapshot(a, exactRevision(a, r), r.value("name").toString());
        else if (action == "delete-version")
            result = SnapshotLibrary::eraseSnapshot(a, exactRevision(a, r));
        else if (action == "restore-version")
            result = SnapshotLibrary::restoreSnapshot(a, r.value("revision").toString());
        else if (action == "delete-asset")
            result = SnapshotLibrary::eraseAsset(a);
        else if (action == "unregister")
            result = SnapshotLibrary::unregisterSource(a);
        else if (action == "relocate")
            result = SnapshotLibrary::relocateSource(a, r.value("source").toString());
        else if (action == "migrate")
            result = SnapshotLibrary::migrate(a);
        else if (action == "export")
            result = SnapshotLibrary::exportSnapshot(a, exactRevision(a, r),
                                                     r.value("destination").toString(), &p);
        else if (action == "reference-add")
            result = SnapshotLibrary::addReference(a, exactRevision(a, r),
                                                   r.value("destinationLibrary").toString());
        else if (action == "reference-update")
            result = SnapshotLibrary::changeReference(a, r.value("ownerLibrary").toString(),
                                                      r.value("revision").toString());
        else if (action == "reference-remove")
            result = SnapshotLibrary::removeReference(a);
        else if (action == "receipt-create")
            result = SnapshotLibrary::saveReceipt(receiptPayload(r, a), r.value("receipt").toString());
        else if (action == "receipt-recover")
        {
            const auto verified =
                SnapshotLibrary::verifyReceipt(r.value("receipt").toString(), library, false);
            check(verified);
            result = SnapshotLibrary::exportSnapshot(verified.asset, verified.snapshot.id,
                                                     r.value("destination").toString());
        }
        else
            invalid("Unimplemented action.");
        data = snapshotResult(result);
    }
    QJsonObject observed;
    for (const auto &root : libraries(r))
    {
        const auto state = catalog(root);
        observed.insert(root, viewToken(state.assets, state.groups));
    }
    data.insert("disk", "committed");
    data.insert("viewTokens", observed);
    return data;
}
} // namespace xips
