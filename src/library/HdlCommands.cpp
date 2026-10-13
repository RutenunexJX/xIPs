#include "HdlCommands.h"
#include "FileSystemUtil.h"
#include "OperationControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QResource>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <stdexcept>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <dlfcn.h>
#endif

static void initializeHdlResources() { Q_INIT_RESOURCE(xips_hdl); }
namespace xips {
namespace {
const QString manifest = QStringLiteral("xips-commands.json");
const QStringList bundleFiles{"tb_signal_package.sv", "tb_reset_package.sv", "tb_sequence_package.sv",
                              "tb_random_package.sv", "xips-commands.json"};
void require(bool ok, const QString &message) { if (!ok) throw std::runtime_error(message.toStdString()); }
QByteArray read(const QString &path, qint64 maximum = 1024 * 1024)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot read " + path + ": " + file.errorString());
    const auto bytes = file.read(maximum + 1);
    require(bytes.size() <= maximum, "File exceeds metadata limit: " + path); return bytes;
}
QJsonObject document(const QByteArray &bytes)
{
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "Invalid command metadata: " + error.errorString());
    auto result = doc.object();
    require(result.value("schema") == "xips.hdl-commands/v1", "Unsupported HDL command metadata schema.");
    require(result.value("commands").isArray(), "Command metadata requires a commands array.");
    return result;
}
QString relativePath(const QString &value)
{
    require(!value.isEmpty() && !QDir::isAbsolutePath(value) && !value.contains('\\') && !value.contains(':') &&
        !value.split('/').contains("..") && QDir::cleanPath(value) == value && !value.startsWith('.'), "Invalid asset-relative path: " + value);
    return value;
}
QString child(const QString &root, const QString &relative)
{
    auto path = QDir(root).filePath(relativePath(relative)); ContentStore::safePath(path);
    require(files::isWithin(path, root), "Source escapes the asset."); return path;
}
bool identifier(const QString &text)
{
    static const QRegularExpression pattern("^[A-Za-z_][A-Za-z0-9_$]*$"); return pattern.match(text).hasMatch();
}
QJsonObject analyze(const QString &root, const QStringList &files, const QJsonArray &commands, const QStringList &payload)
{
    QProcess process; const auto helper = HdlCommands::helperPath();
    require(QFileInfo::exists(helper), "HDL semantic helper unavailable: " + helper);
    process.setProgram(helper); process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", QCoreApplication::applicationDirPath() + QDir::listSeparator() + environment.value("PATH"));
    process.setProcessEnvironment(environment);
    process.start(); require(process.waitForStarted(5000), "Cannot start HDL semantic helper.");
    const auto cleanup = qScopeGuard([&] { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); } });
    process.write(QJsonDocument(QJsonObject{{"root",root},{"files",QJsonArray::fromStringList(files)},
        {"payload",QJsonArray::fromStringList(payload)}, {"commands",commands}}).toJson(QJsonDocument::Compact));
    process.closeWriteChannel(); QElapsedTimer timer; timer.start();
    while (!process.waitForFinished(50))
    {
        OperationScope::checkpoint("Checking HDL source semantics…");
        require(timer.elapsed() < 30000, "HDL analysis exceeded 30 seconds. Source was retained.");
    }
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "HDL semantic helper failed: " + QString::fromUtf8(process.readAllStandardError()));
    QJsonParseError error; const auto result = QJsonDocument::fromJson(process.readAllStandardOutput(), &error).object();
    require(error.error == QJsonParseError::NoError && result.value("schema") == "xips.hdl-analysis/v1", "Invalid HDL analysis response.");
    return result;
}
QStringList inputs(const QJsonObject &command)
{
    QStringList result;
    for (const auto &file : command.value("files").toArray())
    {
        const auto path = relativePath(file.toString());
        require(path.endsWith(".sv", Qt::CaseInsensitive), "Compile inputs must be SystemVerilog .sv files.");
        require(!result.contains(path, Qt::CaseInsensitive), "Duplicate dependency file: " + path);
        result.append(path);
    }
    const auto file = relativePath(command.value("file").toString());
    require(file.endsWith("_package.sv") && result.contains(file), "Select a xx_package.sv file included in the dependency list.");
    require(!result.isEmpty() && result.size() <= 256, "Choose 1 to 256 dependency files.");
    return result;
}
QVariantList entries(const CatalogAsset &asset, const QString &revision, const QString &root,
                     const QMap<QString, ContentObject> &objects, const QVariantList &previous, bool builtin = false)
{
    const auto doc = document(read(QDir(root).filePath(manifest)));
    const auto identity = ContentStore::treeHash(objects);
    QVariantList result;
    QSet<QString> ids;
    QMap<QString, QJsonObject> analyses;
    for (const auto &item : doc.value("commands").toArray())
    {
        const auto command = item.toObject();
        const auto id = command.value("id").toString();
        QVariantMap entry{{"mappingId",id}, {"commandId",builtin ? "builtin:" + id : asset.id + '/' + id},
            {"name",command.value("name").toString()}, {"aliases",command.value("aliases").toArray().toVariantList()},
            {"description",command.value("description").toString()}, {"origin",doc.value("origin").toString("user")},
            {"assetId",builtin ? QString("builtin:tb-behaviors") : asset.id}, {"assetName",asset.name},
            {"revision",revision}, {"contentHash",revision == "current" ? QString() : identity},
            {"sourceIdentity",identity}, {"sourceFile",command.value("file").toString()},
            {"package",command.value("package").toString()}, {"symbol",command.value("symbol").toString()},
            {"includeDirectories",QStringList{"."}}, {"bundle",doc.value("bundle").toString()}};
        QStringList problems;
        try
        {
            require(!QUuid(id).isNull() && !ids.contains(id), "Missing or duplicate command UUID."); ids.insert(id);
            require(!entry.value("name").toString().trimmed().isEmpty(), "A command search name is required.");
            const auto dependencies = inputs(command);
            for (const auto &file : dependencies) require(objects.contains(file), "Missing dependency: " + file);
            entry.insert("compileOrder", dependencies);
            auto cached = std::find_if(previous.cbegin(), previous.cend(), [&](const auto &candidate)
            {
                const auto old = candidate.toMap();
                return old.value("commandId") == entry.value("commandId") && old.value("revision") == revision &&
                    old.value("sourceIdentity") == identity && old.value("availability") != "invalid";
            });
            if (cached != previous.cend())
            {
                const auto old = cached->toMap();
                for (const auto *field : {"kind","returnType","parameters","typeParameters","availability","diagnostics"})
                    entry.insert(field, old.value(field));
                result.append(entry); continue;
            }
            const auto key = dependencies.join('\n');
            if (!analyses.contains(key))
            {
                QJsonArray related;
                for (const auto &other : doc.value("commands").toArray())
                    if (other.toObject().value("files") == command.value("files")) related.append(other);
                analyses.insert(key, analyze(root, dependencies, related, objects.keys()));
            }
            const auto analysis = analyses.value(key);
            for (const auto &error : analysis.value("diagnostics").toArray()) problems.append(error.toString());
            bool found = false;
            for (const auto &checked : analysis.value("entries").toArray())
                if (checked.toObject().value("id").toString() == id)
                {
                    found = true;
                    const auto semantic = checked.toObject();
                    for (const auto *field : {"kind", "returnType", "parameters", "typeParameters"})
                        entry.insert(field, semantic.value(field).toVariant());
                    for (const auto &error : semantic.value("diagnostics").toArray()) problems.append(error.toString());
                }
            require(found, "The semantic checker did not return this command.");
        }
        catch (const OperationCancelled &) { throw; }
        catch (const std::exception &error) { problems.append(QString::fromUtf8(error.what())); }
        entry.insert("availability", !problems.isEmpty() ? "invalid" : revision == "current" ? "draft" : "available");
        entry.insert("diagnostics", problems); result.append(entry);
    }
    return result;
}
QMap<QString, ContentObject> workingObjects(const CatalogAsset &asset)
{
    QMap<QString, ContentObject> result;
    // Only command source material affects semantic invalidation, not unrelated payloads.
    for (const auto &file : asset.workingFiles)
        if (file == manifest || file.endsWith(".sv", Qt::CaseInsensitive) || file.endsWith(".svh", Qt::CaseInsensitive))
        {
            OperationScope::checkpoint("Reading HDL source identity…");
            result.insert(file, ContentStore::fingerprint(child(asset.root, file)));
        }
    return result;
}
QMap<QString, ContentObject> commandObjects(const Snapshot &snapshot)
{
    auto result = snapshot.objects;
    for (auto it = result.begin(); it != result.end();)
        if (it.key() != manifest && !it.key().endsWith(".sv", Qt::CaseInsensitive) && !it.key().endsWith(".svh", Qt::CaseInsensitive)) it = result.erase(it);
        else ++it;
    return result;
}
void materialize(const CatalogAsset &asset, const QMap<QString, ContentObject> &objects, const QString &root)
{
    for (auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        OperationScope::checkpoint("Reading saved HDL sources…");
        const auto target = child(root, it.key()); ContentStore::makeDirectory(QFileInfo(target).absolutePath());
        ContentStore(asset.library).materialize(it.value(), target);
    }
}
void builtinSources(const QString &root)
{
    static const bool initialized = [] { initializeHdlResources(); return true; }(); Q_UNUSED(initialized);
    for (const auto &file : bundleFiles)
    {
        QFile out(QDir(root).filePath(file)); const auto bytes = read(":/xips/hdl/" + file);
        require(out.open(QIODevice::WriteOnly | QIODevice::NewOnly) && out.write(bytes) == bytes.size(), "Cannot stage builtin source.");
    }
}
template<class Work> SnapshotResult operation(Work work)
{
    SnapshotResult result;
    try { OperationScope::checkpoint(); work(result); result.ok = true; }
    catch (const OperationCancelled &error) { result.cancelled = true; result.error = QString::fromUtf8(error.what()); }
    catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
    return result;
}
}
QString HdlCommands::manifestName() { return manifest; }
QString HdlCommands::digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QString HdlCommands::helperPath()
{
    QString directory = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&HdlCommands::helperPath), &module))
    {
        wchar_t path[32768]; const auto size = GetModuleFileNameW(module, path, 32768);
        if (size && size < 32768) directory = QFileInfo(QString::fromWCharArray(path, int(size))).absolutePath();
    }
    return directory + "/xips-hdl-check.exe";
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&HdlCommands::helperPath), &info) && info.dli_fname)
        directory = QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath();
    return directory + "/xips-hdl-check";
#endif
}
QByteArray HdlCommands::workingManifest(const CatalogAsset &asset)
{
    const auto path = child(asset.root, manifest); return QFileInfo::exists(path) ? read(path) : QByteArray{};
}
HdlCatalogResult HdlCommands::catalog(const QList<CatalogAsset> &assets, const QVariantList &previous)
{
    HdlCatalogResult result;
    QSet<QString> installed;
    for (const auto &asset : assets)
    {
        OperationScope::checkpoint();
        try
        {
            const Snapshot *saved = nullptr;
            for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
                if (it->id != "current" && it->id != "working") { saved = &*it; break; }
            QMap<QString, ContentObject> archived;
            if (saved && saved->files.contains(manifest))
            {
                archived = commandObjects(*saved);
                QTemporaryDir tmp; require(tmp.isValid(), "Cannot stage HDL analysis.");
                materialize(asset, archived, tmp.path());
                auto list = entries(asset, saved->id, tmp.path(), archived, previous);
                for (auto &item : list)
                {
                    auto entry = item.toMap(); entry.insert("contentHash", saved->hash); item = entry;
                    if (entry.value("origin") == "builtin" && entry.value("bundle") == "xips.tb-behaviors/v1")
                        installed.insert(entry.value("mappingId").toString());
                }
                result.entries.append(list);
            }
            if (asset.discovered && asset.referencePath.isEmpty() && asset.sourceProblem.isEmpty() && asset.workingFiles.contains(manifest))
            {
                const auto objects = workingObjects(asset);
                if (objects != archived) result.entries.append(entries(asset, "current", asset.root, objects, previous));
            }
        }
        catch (const OperationCancelled &) { throw; }
        catch (const std::exception &error)
        { result.errors.append(QVariantMap{{"code","invalid_asset_commands"},{"assetId",asset.id},{"message",QString::fromUtf8(error.what())}}); }
    }
    try
    {
        QTemporaryDir tmp; require(tmp.isValid(), "Cannot stage builtins."); builtinSources(tmp.path());
        CatalogAsset asset; asset.name = "TB behavior packages"; asset.root = tmp.path(); asset.workingFiles = bundleFiles;
        const auto objects = workingObjects(asset);
        const auto revision = "builtin:" + ContentStore::treeHash(objects);
        for (const auto &item : entries(asset, revision, tmp.path(), objects, previous, true))
            if (!installed.contains(item.toMap().value("mappingId").toString())) result.entries.append(item);
    }
    catch (const OperationCancelled &) { throw; }
    catch (const std::exception &error)
    { result.errors.append(QVariantMap{{"code","builtin_unavailable"},{"message",QString::fromUtf8(error.what())}}); }
    return result;
}
SnapshotResult HdlCommands::installBuiltins(const QString &library)
{
    return operation([&](auto &result)
    {
        QTemporaryDir tmp; require(tmp.isValid(), "Cannot stage builtins."); builtinSources(tmp.path());
        QStringList sources; for (const auto &file : bundleFiles) sources.append(tmp.filePath(file));
        auto preview = SnapshotLibrary::previewImport(sources); require(preview.ok, preview.error);
        ImportRequest request; request.sources = sources; request.selection = preview.preview; request.note = "xIPs builtin HDL commands";
        CatalogDefinition definition; definition.name = "TB behavior packages"; definition.category = "module";
        auto created = SnapshotLibrary::create(library, definition, &request); require(created.ok, created.error); result = created;
    });
}
SnapshotResult HdlCommands::edit(const CatalogAsset &asset, QJsonObject command, const QString &createKind, const QString &expectedHash)
{
    return operation([&](auto &result)
    {
        require(asset.discovered && asset.sourceIsDirectory && asset.referencePath.isEmpty(), "Choose an editable asset working folder.");
        QLockFile lock(QDir(asset.library).filePath(".xips-library.lock")); lock.setStaleLockTime(30000);
        require(lock.tryLock(0), "The library is busy. Retry after its current operation.");
        const auto current = SnapshotLibrary::describe(asset); require(current.ok, current.error);
        require(current.asset.id == asset.id && current.asset.root == asset.root && current.asset.sourceProblem.isEmpty(), "Asset source changed. Refresh and retry.");
        const auto before = workingManifest(current.asset);
        require(digest(before) == expectedHash, "Command mappings changed. Reopen the editor.");
        auto doc = before.isEmpty() ? QJsonObject{{"schema","xips.hdl-commands/v1"},{"origin","user"},{"commands",QJsonArray{}}} : document(before);
        const auto dependencies = inputs(command); Q_UNUSED(dependencies);
        const auto package = command.value("package").toString();
        const auto qualified = command.value("symbol").toString().split("::");
        require(identifier(package) && (qualified.size() == 2 || qualified.size() == 3) && qualified.first() == package,
                "Use an actual package::symbol identity.");
        for (const auto &part : qualified) require(identifier(part), "Invalid HDL identifier.");
        require(!command.value("name").toString().trimmed().isEmpty(), "Enter a command search name.");
        QString id = command.value("id").toString();
        const bool creating = id.isEmpty();
        if (creating) { id = QUuid::createUuid().toString(QUuid::WithoutBraces); command.insert("id",id); }
        require(!QUuid(id).isNull(), "Invalid command identity.");
        auto commands = doc.value("commands").toArray(); bool found = false;
        for (int i = 0; i < commands.size(); ++i)
            if (commands[i].toObject().value("id").toString() == id) { commands[i] = command; found = true; break; }
        require(creating || found, "This command was removed. Refresh and retry.");
        if (!found) commands.append(command);
        QString createdPath; QByteArray createdBytes;
        const auto rollback = qScopeGuard([&] {
            if (!createdPath.isEmpty()) { QFile file(createdPath); if (file.open(QIODevice::ReadOnly) && file.readAll() == createdBytes) { file.close(); QFile::remove(createdPath); } }
        });
        if (!createKind.isEmpty())
        {
            require(creating && qualified.size() == 2 && (createKind == "task" || createKind == "function"), "Choose a new package-level task or function.");
            const auto path = child(asset.root, command.value("file").toString());
            require(!QFileInfo::exists(path), "Source file already exists. Bind its existing symbol or choose a new package file.");
            const auto symbol = qualified.last();
            const auto body = createKind == "task" ? QString("  task automatic %1();\n    // Implement this behavior in the ordinary source editor.\n  endtask\n").arg(symbol)
                : QString("  function automatic int %1(input int value);\n    return value;\n  endfunction\n").arg(symbol);
            createdBytes = QString("package %1;\n  timeunit 1ns;\n  timeprecision 1ps;\n%2endpackage\n").arg(package,body).toUtf8();
            ContentStore::makeDirectory(QFileInfo(path).absolutePath()); QFile file(path);
            require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create package source."); createdPath = path;
            require(file.write(createdBytes) == createdBytes.size() && file.flush(), "Cannot write package source.");
        }
        else require(QFileInfo(child(asset.root, command.value("file").toString())).isFile(), "Choose an existing package source to bind.");
        doc.insert("commands",commands);
        const auto path = child(asset.root,manifest); QSaveFile file(path);
        const auto bytes = QJsonDocument(doc).toJson();
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot stage command mappings.");
        OperationScope::publish(); require(file.commit(), "Cannot save command mappings."); createdPath.clear();
        const auto fresh = SnapshotLibrary::describe(current.asset); require(fresh.ok, fresh.error); result = fresh;
    });
}
SnapshotResult HdlCommands::saveVersion(const CatalogAsset &asset)
{
    return operation([&](auto &result)
    {
        auto preview = SnapshotLibrary::previewWorking(asset); require(preview.ok, preview.error);
        const auto objects = workingObjects(preview.asset);
        const auto commands = entries(preview.asset, "current", preview.asset.root, objects, {});
        require(!commands.isEmpty(), "Create or bind a command first.");
        for (const auto &item : commands) require(item.toMap().value("availability") != "invalid", item.toMap().value("diagnostics").toStringList().join('\n'));
        auto saved = SnapshotLibrary::saveCurrent(preview.asset, "HDL command source and mapping", &preview.preview);
        require(saved.ok, saved.error); result = saved;
    });
}
SnapshotResult HdlCommands::prepareUse(const CatalogAsset &asset, const QVariantMap &entry)
{
    return operation([&](auto &result)
    {
        const auto revision = entry.value("revision").toString();
        require(revision != "current" && revision != "working", "Save a version before using this command.");
        auto preview = SnapshotLibrary::previewExport(asset, revision); require(preview.ok, preview.error);
        require(preview.snapshot.hash == entry.value("contentHash").toString(), "The selected revision identity changed.");
        QTemporaryDir tmp; require(tmp.isValid(), "Cannot stage selected command.");
        const auto objects = commandObjects(preview.snapshot); materialize(preview.asset, objects, tmp.path());
        require(ContentStore::treeHash(objects) == entry.value("sourceIdentity").toString(), "The selected command source identity changed.");
        bool valid = false;
        for (const auto &item : entries(preview.asset, revision, tmp.path(), objects, {}))
            if (item.toMap().value("mappingId") == entry.value("mappingId"))
            {
                require(item.toMap().value("availability") == "available", item.toMap().value("diagnostics").toStringList().join('\n')); valid = true;
            }
        require(valid, "The selected saved version no longer contains the command."); result = preview;
    });
}
QVariantMap HdlCommands::receiptDetails(const QVariantMap &entry)
{
    QVariantMap result{{"schema","xips.hdl-use/v1"}};
    for (const auto *key : {"commandId","mappingId","symbol","kind","sourceFile","compileOrder","includeDirectories","sourceIdentity"})
        result.insert(key,entry.value(key));
    return result;
}
}
