#include "assetindex/AssetIndex.h"

#include "assetcore/JsonUtil.h"
#include "manifest/ManifestService.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <algorithm>
#include <atomic>

namespace xips {
namespace {

std::atomic<quint64> connectionCounter = 0;

class SqlConnection {
public:
    explicit SqlConnection(const QString &path)
    {
        const quint64 counter = connectionCounter.fetch_add(1, std::memory_order_relaxed);
        const quintptr threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
        m_name = QStringLiteral("xips_%1_%2").arg(threadId).arg(counter);
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_database.setDatabaseName(path);
    }

    ~SqlConnection()
    {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    QSqlDatabase &database()
    {
        return m_database;
    }

private:
    QString m_name;
    QSqlDatabase m_database;
};

bool setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

QStringList searchTrigrams(const QString &normalized)
{
    if (normalized.size() < 3) {
        return json::trigrams(normalized);
    }
    QSet<QString> unique;
    for (qsizetype index = 0; index + 3 <= normalized.size(); ++index) {
        unique.insert(normalized.mid(index, 3));
    }
    QStringList result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

bool execute(QSqlQuery &query, const QString &sql, QString *error)
{
    if (!query.exec(sql)) {
        return setError(error,
                        QStringLiteral("SQLite statement failed: %1; SQL: %2")
                            .arg(query.lastError().text(), sql));
    }
    return true;
}

bool ensureSchema(QSqlDatabase &database, QString *error)
{
    QSqlQuery query(database);
    if (!execute(query, QStringLiteral("PRAGMA foreign_keys=ON"), error)
        || !execute(query, QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !execute(query, QStringLiteral("PRAGMA synchronous=NORMAL"), error)) {
        return false;
    }

    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS metadata ("
            "key TEXT PRIMARY KEY,"
            "value TEXT NOT NULL"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS assets ("
            "id TEXT PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "type TEXT NOT NULL,"
            "version TEXT NOT NULL,"
            "top_name TEXT NOT NULL,"
            "language TEXT NOT NULL,"
            "description TEXT NOT NULL,"
            "root_path TEXT NOT NULL,"
            "manifest_path TEXT NOT NULL,"
            "origin TEXT NOT NULL,"
            "content_hash TEXT NOT NULL,"
            "generation INTEGER NOT NULL,"
            "stale INTEGER NOT NULL DEFAULT 0,"
            "test_status TEXT NOT NULL,"
            "diagnostics_status TEXT NOT NULL,"
            "modified_status TEXT NOT NULL,"
            "source_repository TEXT NOT NULL,"
            "last_used TEXT NOT NULL,"
            "git_commit TEXT NOT NULL DEFAULT '',"
            "git_tag TEXT NOT NULL DEFAULT '',"
            "manifest_json TEXT NOT NULL,"
            "semantic_json TEXT NOT NULL DEFAULT '{}'"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS search_entries ("
            "asset_id TEXT NOT NULL,"
            "field TEXT NOT NULL,"
            "value TEXT NOT NULL,"
            "normalized TEXT NOT NULL,"
            "PRIMARY KEY(asset_id, field, value),"
            "FOREIGN KEY(asset_id) REFERENCES assets(id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS search_grams ("
            "asset_id TEXT NOT NULL,"
            "field TEXT NOT NULL,"
            "gram TEXT NOT NULL,"
            "PRIMARY KEY(asset_id, field, gram),"
            "FOREIGN KEY(asset_id) REFERENCES assets(id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS test_results ("
            "asset_id TEXT NOT NULL,"
            "command_name TEXT NOT NULL,"
            "content_hash TEXT NOT NULL,"
            "git_commit TEXT NOT NULL,"
            "status TEXT NOT NULL,"
            "started_at TEXT NOT NULL,"
            "duration_ms INTEGER NOT NULL,"
            "result_json TEXT NOT NULL,"
            "PRIMARY KEY(asset_id, command_name),"
            "FOREIGN KEY(asset_id) REFERENCES assets(id) ON DELETE CASCADE"
            ")"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assets_type_name "
                       "ON assets(type, name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assets_generation "
                       "ON assets(generation)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_assets_hash "
                       "ON assets(content_hash)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_search_normalized "
                       "ON search_entries(normalized)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_search_gram "
                       "ON search_grams(gram, asset_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_test_results_hash "
                       "ON test_results(asset_id, content_hash)"),
        QStringLiteral("UPDATE metadata SET value='2' "
                       "WHERE key='schema_version' AND CAST(value AS INTEGER)<2"),
    };

    for (const QString &statement : statements) {
        if (!execute(query, statement, error)) {
            return false;
        }
    }

    if (!query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO metadata(key, value) VALUES(?, ?)"))) {
        return setError(error, query.lastError().text());
    }
    const QList<QPair<QString, QString>> defaults{
        {QStringLiteral("schema_version"), QStringLiteral("2")},
        {QStringLiteral("next_generation"), QStringLiteral("0")},
        {QStringLiteral("published_generation"), QStringLiteral("0")},
    };
    for (const auto &[key, value] : defaults) {
        query.bindValue(0, key);
        query.bindValue(1, value);
        if (!query.exec()) {
            return setError(error, query.lastError().text());
        }
    }
    return true;
}

QJsonObject semanticToJson(const SemanticResult &semantic)
{
    QJsonObject object{
        {QStringLiteral("available"), semantic.available},
        {QStringLiteral("success"), semantic.success},
        {QStringLiteral("engineVersion"), semantic.engineVersion},
        {QStringLiteral("generation"), semantic.generation},
        {QStringLiteral("contentHash"), semantic.contentHash},
    };
    QJsonArray units;
    for (const SemanticUnit &unit : semantic.units) {
        QJsonObject unitObject{
            {QStringLiteral("name"), unit.name},
            {QStringLiteral("kind"), unit.kind},
            {QStringLiteral("sourceFile"), unit.sourceFile},
        };
        QJsonArray ports;
        for (const SemanticPort &port : unit.ports) {
            ports.append(QJsonObject{
                {QStringLiteral("name"), port.name},
                {QStringLiteral("direction"), port.direction},
                {QStringLiteral("type"), port.type},
                {QStringLiteral("packedDimensions"), port.packedDimensions},
                {QStringLiteral("unpackedDimensions"), port.unpackedDimensions},
            });
        }
        QJsonArray parameters;
        for (const SemanticParameter &parameter : unit.parameters) {
            parameters.append(QJsonObject{
                {QStringLiteral("name"), parameter.name},
                {QStringLiteral("type"), parameter.type},
                {QStringLiteral("value"), parameter.value},
                {QStringLiteral("local"), parameter.local},
            });
        }
        unitObject.insert(QStringLiteral("ports"), ports);
        unitObject.insert(QStringLiteral("parameters"), parameters);
        unitObject.insert(QStringLiteral("imports"), json::toArray(unit.imports));
        unitObject.insert(QStringLiteral("instances"), json::toArray(unit.instances));
        units.append(unitObject);
    }
    object.insert(QStringLiteral("units"), units);
    object.insert(QStringLiteral("includes"), json::toArray(semantic.includes));
    object.insert(QStringLiteral("defines"), json::toArray(semantic.defines));
    object.insert(QStringLiteral("topCandidates"), json::toArray(semantic.topCandidates));
    QJsonArray diagnostics;
    for (const Diagnostic &diagnostic : semantic.diagnostics) {
        diagnostics.append(QJsonObject{
            {QStringLiteral("severity"), diagnosticSeverityToString(diagnostic.severity)},
            {QStringLiteral("code"), diagnostic.code},
            {QStringLiteral("message"), diagnostic.message},
            {QStringLiteral("file"), diagnostic.file},
            {QStringLiteral("line"), diagnostic.line},
            {QStringLiteral("column"), diagnostic.column},
        });
    }
    object.insert(QStringLiteral("diagnostics"), diagnostics);
    return object;
}

SemanticResult semanticFromJson(const QJsonObject &object)
{
    SemanticResult result;
    result.available = object.value(QStringLiteral("available")).toBool();
    result.success = object.value(QStringLiteral("success")).toBool();
    result.engineVersion = object.value(QStringLiteral("engineVersion")).toString();
    result.generation = object.value(QStringLiteral("generation")).toInteger();
    result.contentHash = object.value(QStringLiteral("contentHash")).toString();
    result.includes = json::stringList(object, QStringLiteral("includes"));
    result.defines = json::stringList(object, QStringLiteral("defines"));
    result.topCandidates = json::stringList(object, QStringLiteral("topCandidates"));
    for (const QJsonValue &diagnosticValue :
         object.value(QStringLiteral("diagnostics")).toArray()) {
        const QJsonObject diagnosticObject = diagnosticValue.toObject();
        const QString severity = diagnosticObject.value(QStringLiteral("severity")).toString();
        Diagnostic::Severity parsedSeverity = Diagnostic::Severity::Info;
        if (severity == QStringLiteral("error")) {
            parsedSeverity = Diagnostic::Severity::Error;
        } else if (severity == QStringLiteral("warning")) {
            parsedSeverity = Diagnostic::Severity::Warning;
        }
        result.diagnostics.append(Diagnostic{
            .severity = parsedSeverity,
            .code = diagnosticObject.value(QStringLiteral("code")).toString(),
            .message = diagnosticObject.value(QStringLiteral("message")).toString(),
            .file = diagnosticObject.value(QStringLiteral("file")).toString(),
            .line = diagnosticObject.value(QStringLiteral("line")).toInt(),
            .column = diagnosticObject.value(QStringLiteral("column")).toInt(),
        });
    }

    for (const QJsonValue &unitValue : object.value(QStringLiteral("units")).toArray()) {
        const QJsonObject unitObject = unitValue.toObject();
        SemanticUnit unit;
        unit.name = unitObject.value(QStringLiteral("name")).toString();
        unit.kind = unitObject.value(QStringLiteral("kind")).toString();
        unit.sourceFile = unitObject.value(QStringLiteral("sourceFile")).toString();
        unit.imports = json::stringList(unitObject, QStringLiteral("imports"));
        unit.instances = json::stringList(unitObject, QStringLiteral("instances"));
        for (const QJsonValue &portValue : unitObject.value(QStringLiteral("ports")).toArray()) {
            const QJsonObject portObject = portValue.toObject();
            unit.ports.append(SemanticPort{
                .name = portObject.value(QStringLiteral("name")).toString(),
                .direction = portObject.value(QStringLiteral("direction")).toString(),
                .type = portObject.value(QStringLiteral("type")).toString(),
                .packedDimensions =
                    portObject.value(QStringLiteral("packedDimensions")).toString(),
                .unpackedDimensions =
                    portObject.value(QStringLiteral("unpackedDimensions")).toString(),
            });
        }
        for (const QJsonValue &parameterValue :
             unitObject.value(QStringLiteral("parameters")).toArray()) {
            const QJsonObject parameterObject = parameterValue.toObject();
            unit.parameters.append(SemanticParameter{
                .name = parameterObject.value(QStringLiteral("name")).toString(),
                .type = parameterObject.value(QStringLiteral("type")).toString(),
                .value = parameterObject.value(QStringLiteral("value")).toString(),
                .local = parameterObject.value(QStringLiteral("local")).toBool(),
            });
        }
        result.units.append(unit);
    }
    return result;
}

AssetRecord recordFromQuery(const QSqlQuery &query)
{
    AssetRecord record;
    ManifestService manifestService;
    const QByteArray manifestJson = query.value(QStringLiteral("manifest_json")).toByteArray();
    const ManifestLoadResult manifestResult =
        manifestService.parse(manifestJson, query.value(QStringLiteral("manifest_path")).toString());
    if (manifestResult.ok()) {
        record.manifest = *manifestResult.manifest;
    }
    record.assetRoot = query.value(QStringLiteral("root_path")).toString();
    record.manifestPath = query.value(QStringLiteral("manifest_path")).toString();
    record.origin = query.value(QStringLiteral("origin")).toString() == QStringLiteral("external")
                        ? AssetOrigin::External
                        : AssetOrigin::Managed;
    record.contentHash = query.value(QStringLiteral("content_hash")).toString();
    record.generation = query.value(QStringLiteral("generation")).toLongLong();
    record.stale = query.value(QStringLiteral("stale")).toBool();
    record.testStatus = query.value(QStringLiteral("test_status")).toString();
    record.diagnosticsStatus = query.value(QStringLiteral("diagnostics_status")).toString();
    record.modifiedStatus = query.value(QStringLiteral("modified_status")).toString();
    record.sourceRepository = query.value(QStringLiteral("source_repository")).toString();
    record.lastUsed = QDateTime::fromString(query.value(QStringLiteral("last_used")).toString(),
                                            Qt::ISODateWithMs);
    record.gitCommit = query.value(QStringLiteral("git_commit")).toString();
    record.gitTag = query.value(QStringLiteral("git_tag")).toString();
    const QJsonDocument semanticDocument =
        QJsonDocument::fromJson(query.value(QStringLiteral("semantic_json")).toByteArray());
    record.semantic = semanticFromJson(semanticDocument.object());
    return record;
}

QList<QPair<QString, QString>> searchEntries(const AssetRecord &record)
{
    QList<QPair<QString, QString>> entries;
    const auto append = [&entries](const QString &field, const QString &value) {
        if (!value.trimmed().isEmpty()) {
            entries.append({field, value});
        }
    };
    append(QStringLiteral("name"), record.manifest.name);
    append(QStringLiteral("id"), record.manifest.id);
    append(QStringLiteral("type"), assetTypeToString(record.manifest.type));
    append(QStringLiteral("version"), record.manifest.version);
    append(QStringLiteral("top"), record.manifest.top);
    append(QStringLiteral("language"), record.manifest.language);
    append(QStringLiteral("description"), record.manifest.description);
    append(QStringLiteral("source-repository"), record.sourceRepository);
    for (const QString &tag : record.manifest.tags) {
        append(QStringLiteral("tag"), tag);
    }
    for (const QString &source : record.manifest.sources) {
        append(QStringLiteral("source"), source);
    }
    for (const QString &define : record.manifest.defines) {
        append(QStringLiteral("define"), define);
    }
    for (const DependencySpec &dependency : record.manifest.dependencies) {
        append(QStringLiteral("dependency"), dependency.id);
    }
    for (const QString &tool : record.manifest.tools.keys()) {
        append(QStringLiteral("tool"), tool + u' '
                                           + record.manifest.tools.value(tool).toVariant().toString());
    }
    for (const SlotDefinition &slot : record.manifest.slotDefinitions) {
        append(QStringLiteral("slot"), slot.name);
    }
    if (!record.stale) {
        for (const SemanticUnit &unit : record.semantic.units) {
            append(unit.kind, unit.name);
            for (const SemanticPort &port : unit.ports) {
                append(QStringLiteral("port"), port.name + u' ' + port.type);
            }
            for (const SemanticParameter &parameter : unit.parameters) {
                append(QStringLiteral("parameter"), parameter.name + u' ' + parameter.type);
            }
            for (const QString &instance : unit.instances) {
                append(QStringLiteral("instance"), instance);
            }
            for (const QString &importName : unit.imports) {
                append(QStringLiteral("import"), importName);
            }
        }
    }
    return entries;
}

bool insertSearchEntries(QSqlDatabase &database,
                         const AssetRecord &record,
                         QString *error)
{
    {
        QSqlQuery deleteQuery(database);
        deleteQuery.prepare(QStringLiteral("DELETE FROM search_entries WHERE asset_id=?"));
        deleteQuery.addBindValue(record.manifest.id);
        if (!deleteQuery.exec()) {
            return setError(error, deleteQuery.lastError().text());
        }
        deleteQuery.prepare(QStringLiteral("DELETE FROM search_grams WHERE asset_id=?"));
        deleteQuery.addBindValue(record.manifest.id);
        if (!deleteQuery.exec()) {
            return setError(error, deleteQuery.lastError().text());
        }
    }

    QSqlQuery entryQuery(database);
    if (!entryQuery.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO search_entries(asset_id, field, value, normalized) "
            "VALUES(?, ?, ?, ?)"))) {
        return setError(error, entryQuery.lastError().text());
    }
    QSqlQuery gramQuery(database);
    if (!gramQuery.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO search_grams(asset_id, field, gram) VALUES(?, ?, ?)"))) {
        return setError(error, gramQuery.lastError().text());
    }

    for (const auto &[field, value] : searchEntries(record)) {
        const QString normalized = json::normalizeSearchText(value);
        entryQuery.bindValue(0, record.manifest.id);
        entryQuery.bindValue(1, field);
        entryQuery.bindValue(2, value);
        entryQuery.bindValue(3, normalized);
        if (!entryQuery.exec()) {
            return setError(error, entryQuery.lastError().text());
        }
        for (const QString &gram : json::trigrams(normalized)) {
            gramQuery.bindValue(0, record.manifest.id);
            gramQuery.bindValue(1, field);
            gramQuery.bindValue(2, gram);
            if (!gramQuery.exec()) {
                return setError(error, gramQuery.lastError().text());
            }
        }
    }
    return true;
}

QString placeholders(const qsizetype count)
{
    QStringList result;
    result.reserve(count);
    for (qsizetype index = 0; index < count; ++index) {
        result.append(QStringLiteral("?"));
    }
    return result.join(u',');
}

QString nonNullString(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

QString diagnosticsStatus(const SemanticResult &semantic)
{
    if (!semantic.available) {
        return QStringLiteral("unavailable");
    }
    bool warning = false;
    for (const Diagnostic &diagnostic : semantic.diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::Error) {
            return QStringLiteral("error");
        }
        warning |= diagnostic.severity == Diagnostic::Severity::Warning;
    }
    return warning ? QStringLiteral("warning") : QStringLiteral("clean");
}

bool upsertRecords(QSqlDatabase &database,
                   const QList<AssetRecord> &assets,
                   const qint64 generation,
                   QString *error)
{
    const QString upsertSql = QStringLiteral(
        "INSERT INTO assets("
        "id,name,type,version,top_name,language,description,root_path,manifest_path,origin,"
        "content_hash,generation,stale,test_status,diagnostics_status,modified_status,"
        "source_repository,last_used,git_commit,git_tag,manifest_json,semantic_json"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "name=excluded.name,type=excluded.type,version=excluded.version,"
        "top_name=excluded.top_name,language=excluded.language,"
        "description=excluded.description,root_path=excluded.root_path,"
        "manifest_path=excluded.manifest_path,origin=excluded.origin,"
        "stale=CASE WHEN assets.content_hash=excluded.content_hash "
        "THEN assets.stale ELSE 1 END,"
        "test_status=CASE WHEN assets.content_hash=excluded.content_hash "
        "THEN assets.test_status ELSE 'stale' END,"
        "diagnostics_status=CASE "
        "WHEN excluded.diagnostics_status='manifest-error' THEN excluded.diagnostics_status "
        "WHEN assets.content_hash=excluded.content_hash THEN assets.diagnostics_status "
        "ELSE 'stale' END,"
        "content_hash=excluded.content_hash,generation=excluded.generation,"
        "modified_status=excluded.modified_status,source_repository=excluded.source_repository,"
        "last_used=CASE WHEN assets.last_used<>'' THEN assets.last_used "
        "ELSE excluded.last_used END,"
        "git_commit=excluded.git_commit,git_tag=excluded.git_tag,"
        "manifest_json=excluded.manifest_json,"
        "semantic_json=CASE WHEN assets.semantic_json='' THEN excluded.semantic_json "
        "ELSE assets.semantic_json END "
        "WHERE excluded.generation >= assets.generation");

    ManifestService manifestService;
    for (AssetRecord record : assets) {
        record.generation = generation;
        QSqlQuery upsert(database);
        if (!upsert.prepare(upsertSql)) {
            return setError(error, upsert.lastError().text());
        }
        const QVariantList values{
            nonNullString(record.manifest.id),
            nonNullString(record.manifest.name),
            assetTypeToString(record.manifest.type),
            nonNullString(record.manifest.version),
            nonNullString(record.manifest.top),
            nonNullString(record.manifest.language),
            nonNullString(record.manifest.description),
            nonNullString(record.assetRoot),
            nonNullString(record.manifestPath),
            assetOriginToString(record.origin),
            nonNullString(record.contentHash),
            generation,
            record.stale,
            nonNullString(record.testStatus),
            nonNullString(record.diagnosticsStatus),
            nonNullString(record.modifiedStatus),
            nonNullString(record.sourceRepository),
            record.lastUsed.isValid() ? record.lastUsed.toString(Qt::ISODateWithMs)
                                      : QStringLiteral(""),
            nonNullString(record.gitCommit),
            nonNullString(record.gitTag),
            QString::fromUtf8(
                QJsonDocument(manifestService.toJson(record.manifest))
                    .toJson(QJsonDocument::Compact)),
            QString::fromUtf8(
                QJsonDocument(semanticToJson(record.semantic))
                    .toJson(QJsonDocument::Compact)),
        };
        for (const QVariant &value : values) {
            upsert.addBindValue(value);
        }
        if (!upsert.exec()) {
            return setError(error, upsert.lastError().text());
        }
        QSqlQuery storedQuery(database);
        storedQuery.prepare(QStringLiteral("SELECT * FROM assets WHERE id=?"));
        storedQuery.addBindValue(record.manifest.id);
        if (!storedQuery.exec() || !storedQuery.next()) {
            return setError(error, storedQuery.lastError().text());
        }
        if (!insertSearchEntries(database, recordFromQuery(storedQuery), error)) {
            return false;
        }
    }
    return true;
}

} // namespace

AssetIndex::AssetIndex(QString databasePath)
    : m_databasePath(std::move(databasePath))
{
}

QString AssetIndex::databasePath() const
{
    return m_databasePath;
}

bool AssetIndex::initialize(QString *error) const
{
    const QFileInfo info(m_databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return setError(error,
                        QStringLiteral("Cannot create index directory: %1").arg(info.absolutePath()));
    }
    SqlConnection connection(m_databasePath);
    if (!connection.database().open()) {
        return setError(error, connection.database().lastError().text());
    }
    return ensureSchema(connection.database(), error);
}

qint64 AssetIndex::reserveGeneration(QString *error) const
{
    if (!initialize(error)) {
        return -1;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        setError(error, database.lastError().text());
        return -1;
    }
    if (!database.transaction()) {
        setError(error, database.lastError().text());
        return -1;
    }

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT value FROM metadata WHERE key='next_generation'"))
        || !query.next()) {
        database.rollback();
        setError(error, query.lastError().text());
        return -1;
    }
    const qint64 generation = query.value(0).toLongLong() + 1;
    query.prepare(QStringLiteral(
        "UPDATE metadata SET value=? WHERE key='next_generation'"));
    query.addBindValue(QString::number(generation));
    if (!query.exec() || !database.commit()) {
        database.rollback();
        setError(error, query.lastError().text());
        return -1;
    }
    return generation;
}

bool AssetIndex::rebuild(const QList<AssetRecord> &assets,
                         const qint64 generation,
                         QString *error) const
{
    if (generation <= 0) {
        return setError(error, QStringLiteral("Index generation must be positive"));
    }
    if (!initialize(error)) {
        return false;
    }

    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        return setError(error, database.lastError().text());
    }
    if (!database.transaction()) {
        return setError(error, database.lastError().text());
    }

    QSqlQuery publishedQuery(database);
    if (!publishedQuery.exec(QStringLiteral(
            "SELECT value FROM metadata WHERE key='published_generation'"))
        || !publishedQuery.next()) {
        database.rollback();
        return setError(error, publishedQuery.lastError().text());
    }
    if (publishedQuery.value(0).toLongLong() > generation) {
        database.rollback();
        return true;
    }

    if (!upsertRecords(database, assets, generation, error)) {
        database.rollback();
        return false;
    }

    QSqlQuery deleteOld(database);
    deleteOld.prepare(QStringLiteral("DELETE FROM assets WHERE generation < ?"));
    deleteOld.addBindValue(generation);
    if (!deleteOld.exec()) {
        database.rollback();
        return setError(error, deleteOld.lastError().text());
    }

    QSqlQuery publish(database);
    publish.prepare(QStringLiteral(
        "UPDATE metadata SET value=? WHERE key='published_generation'"));
    publish.addBindValue(QString::number(generation));
    if (!publish.exec() || !database.commit()) {
        database.rollback();
        return setError(error, publish.lastError().text());
    }
    return true;
}

bool AssetIndex::updateAssets(const QList<AssetRecord> &assets,
                              const qint64 generation,
                              QString *error) const
{
    if (generation <= 0) {
        return setError(error, QStringLiteral("Index generation must be positive"));
    }
    if (!initialize(error)) {
        return false;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open() || !database.transaction()) {
        return setError(error, database.lastError().text());
    }
    QSqlQuery published(database);
    if (!published.exec(QStringLiteral(
            "SELECT value FROM metadata WHERE key='published_generation'"))
        || !published.next()) {
        database.rollback();
        return setError(error, published.lastError().text());
    }
    if (published.value(0).toLongLong() > generation) {
        database.rollback();
        return true;
    }
    if (!upsertRecords(database, assets, generation, error)) {
        database.rollback();
        return false;
    }
    QSqlQuery publish(database);
    publish.prepare(QStringLiteral(
        "UPDATE metadata SET value=? WHERE key='published_generation'"));
    publish.addBindValue(QString::number(generation));
    if (!publish.exec() || !database.commit()) {
        database.rollback();
        return setError(error, publish.lastError().text());
    }
    return true;
}

bool AssetIndex::removeAssets(const QStringList &assetIds,
                              const qint64 generation,
                              QString *error) const
{
    if (generation <= 0) {
        return setError(error, QStringLiteral("Index generation must be positive"));
    }
    if (!initialize(error)) {
        return false;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open() || !database.transaction()) {
        return setError(error, database.lastError().text());
    }
    QSqlQuery published(database);
    if (!published.exec(QStringLiteral(
            "SELECT value FROM metadata WHERE key='published_generation'"))
        || !published.next()) {
        database.rollback();
        return setError(error, published.lastError().text());
    }
    if (published.value(0).toLongLong() > generation) {
        database.rollback();
        return true;
    }
    QSqlQuery remove(database);
    if (!remove.prepare(QStringLiteral("DELETE FROM assets WHERE id=?"))) {
        database.rollback();
        return setError(error, remove.lastError().text());
    }
    QStringList ids = assetIds;
    ids.removeDuplicates();
    std::sort(ids.begin(), ids.end());
    for (const QString &id : ids) {
        remove.bindValue(0, id);
        if (!remove.exec()) {
            database.rollback();
            return setError(error, remove.lastError().text());
        }
    }
    QSqlQuery publish(database);
    publish.prepare(QStringLiteral(
        "UPDATE metadata SET value=? WHERE key='published_generation'"));
    publish.addBindValue(QString::number(generation));
    if (!publish.exec() || !database.commit()) {
        database.rollback();
        return setError(error, publish.lastError().text());
    }
    return true;
}

bool AssetIndex::markUsed(const QString &assetId,
                          const QDateTime &when,
                          QString *error) const
{
    if (assetId.isEmpty() || !when.isValid()) {
        return setError(error,
                        QStringLiteral("Asset ID and usage time are required"));
    }
    if (!initialize(error)) {
        return false;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        return setError(error, database.lastError().text());
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("UPDATE assets SET last_used=? WHERE id=?"));
    query.addBindValue(when.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(assetId);
    if (!query.exec()) {
        return setError(error, query.lastError().text());
    }
    if (query.numRowsAffected() != 1) {
        return setError(error,
                        QStringLiteral("Asset not found in index: %1")
                            .arg(assetId));
    }
    return true;
}

SemanticPublishStatus AssetIndex::publishSemantic(const QString &assetId,
                                                  const QString &expectedContentHash,
                                                  const qint64 expectedGeneration,
                                                  const SemanticResult &semantic,
                                                  QString *error) const
{
    if (!initialize(error)) {
        return SemanticPublishStatus::Error;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        setError(error, database.lastError().text());
        return SemanticPublishStatus::Error;
    }
    if (!database.transaction()) {
        setError(error, database.lastError().text());
        return SemanticPublishStatus::Error;
    }

    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT content_hash,generation FROM assets WHERE id=?"));
    current.addBindValue(assetId);
    if (!current.exec()) {
        database.rollback();
        setError(error, current.lastError().text());
        return SemanticPublishStatus::Error;
    }
    if (!current.next()) {
        database.rollback();
        return SemanticPublishStatus::Missing;
    }
    if (current.value(0).toString() != expectedContentHash
        || current.value(1).toLongLong() != expectedGeneration) {
        database.rollback();
        return SemanticPublishStatus::Stale;
    }

    SemanticResult boundSemantic = semantic;
    boundSemantic.contentHash = expectedContentHash;
    boundSemantic.generation = expectedGeneration;
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE assets SET semantic_json=?,stale=0,diagnostics_status=? "
        "WHERE id=? AND content_hash=? AND generation=?"));
    update.addBindValue(QString::fromUtf8(
        QJsonDocument(semanticToJson(boundSemantic)).toJson(QJsonDocument::Compact)));
    update.addBindValue(diagnosticsStatus(boundSemantic));
    update.addBindValue(assetId);
    update.addBindValue(expectedContentHash);
    update.addBindValue(expectedGeneration);
    if (!update.exec() || update.numRowsAffected() != 1) {
        database.rollback();
        setError(error, update.lastError().text());
        return SemanticPublishStatus::Error;
    }

    QSqlQuery storedQuery(database);
    storedQuery.prepare(QStringLiteral("SELECT * FROM assets WHERE id=?"));
    storedQuery.addBindValue(assetId);
    if (!storedQuery.exec() || !storedQuery.next()) {
        database.rollback();
        setError(error, storedQuery.lastError().text());
        return SemanticPublishStatus::Error;
    }
    const AssetRecord storedRecord = recordFromQuery(storedQuery);
    if (!insertSearchEntries(database, storedRecord, error)) {
        database.rollback();
        return SemanticPublishStatus::Error;
    }
    if (!database.commit()) {
        database.rollback();
        setError(error, database.lastError().text());
        return SemanticPublishStatus::Error;
    }
    return SemanticPublishStatus::Published;
}

TestPublishStatus AssetIndex::publishTestResult(const QString &assetId,
                                                const QString &expectedContentHash,
                                                const qint64 expectedGeneration,
                                                const TestResult &result,
                                                QString *error) const
{
    if (!initialize(error)) {
        return TestPublishStatus::Error;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        setError(error, database.lastError().text());
        return TestPublishStatus::Error;
    }
    if (!database.transaction()) {
        setError(error, database.lastError().text());
        return TestPublishStatus::Error;
    }

    QSqlQuery current(database);
    current.prepare(QStringLiteral(
        "SELECT content_hash,generation FROM assets WHERE id=?"));
    current.addBindValue(assetId);
    if (!current.exec()) {
        database.rollback();
        setError(error, current.lastError().text());
        return TestPublishStatus::Error;
    }
    if (!current.next()) {
        database.rollback();
        return TestPublishStatus::Missing;
    }
    if (current.value(0).toString() != expectedContentHash
        || current.value(1).toLongLong() != expectedGeneration) {
        database.rollback();
        return TestPublishStatus::Stale;
    }

    TestResult bound = result;
    bound.assetId = assetId;
    bound.contentHash = expectedContentHash;
    QSqlQuery upsert(database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO test_results("
        "asset_id,command_name,content_hash,git_commit,status,started_at,duration_ms,result_json"
        ") VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(asset_id,command_name) DO UPDATE SET "
        "content_hash=excluded.content_hash,git_commit=excluded.git_commit,"
        "status=excluded.status,started_at=excluded.started_at,"
        "duration_ms=excluded.duration_ms,result_json=excluded.result_json"));
    upsert.addBindValue(assetId);
    upsert.addBindValue(bound.commandName);
    upsert.addBindValue(expectedContentHash);
    upsert.addBindValue(bound.gitCommit);
    upsert.addBindValue(bound.status);
    upsert.addBindValue(bound.startedAt.toString(Qt::ISODateWithMs));
    upsert.addBindValue(bound.durationMs);
    upsert.addBindValue(QString::fromUtf8(
        QJsonDocument(testResultToJson(bound)).toJson(QJsonDocument::Compact)));
    if (!upsert.exec()) {
        database.rollback();
        setError(error, upsert.lastError().text());
        return TestPublishStatus::Error;
    }

    QSqlQuery update(database);
    update.prepare(QStringLiteral(
        "UPDATE assets SET test_status=? "
        "WHERE id=? AND content_hash=? AND generation=?"));
    update.addBindValue(bound.status);
    update.addBindValue(assetId);
    update.addBindValue(expectedContentHash);
    update.addBindValue(expectedGeneration);
    if (!update.exec() || update.numRowsAffected() != 1) {
        database.rollback();
        setError(error, update.lastError().text());
        return TestPublishStatus::Error;
    }
    if (!database.commit()) {
        database.rollback();
        setError(error, database.lastError().text());
        return TestPublishStatus::Error;
    }
    return TestPublishStatus::Published;
}

QList<TestResult> AssetIndex::testResults(const QString &assetId, QString *error) const
{
    QList<TestResult> results;
    if (!initialize(error)) {
        return results;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        setError(error, database.lastError().text());
        return results;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT result_json FROM test_results WHERE asset_id=? "
        "ORDER BY started_at DESC,command_name"));
    query.addBindValue(assetId);
    if (!query.exec()) {
        setError(error, query.lastError().text());
        return results;
    }
    while (query.next()) {
        const QJsonDocument document =
            QJsonDocument::fromJson(query.value(0).toByteArray());
        if (document.isObject()) {
            results.append(testResultFromJson(document.object()));
        }
    }
    return results;
}

QList<SearchHit> AssetIndex::search(const QString &queryText,
                                    const AssetType typeFilter,
                                    const int limit,
                                    QString *error) const
{
    QList<SearchHit> result;
    if (!initialize(error)) {
        return result;
    }
    SqlConnection connection(m_databasePath);
    QSqlDatabase &database = connection.database();
    if (!database.open()) {
        setError(error, database.lastError().text());
        return result;
    }

    const QString normalized = json::normalizeSearchText(queryText);
    if (normalized.isEmpty()) {
        QString sql = QStringLiteral("SELECT * FROM assets");
        if (typeFilter != AssetType::Unknown) {
            sql += QStringLiteral(" WHERE type=?");
        }
        sql += QStringLiteral(" ORDER BY name COLLATE NOCASE, id LIMIT ?");
        QSqlQuery query(database);
        query.prepare(sql);
        if (typeFilter != AssetType::Unknown) {
            query.addBindValue(assetTypeToString(typeFilter));
        }
        query.addBindValue(limit);
        if (!query.exec()) {
            setError(error, query.lastError().text());
            return result;
        }
        while (query.next()) {
            result.append(SearchHit{
                .asset = recordFromQuery(query),
                .score = 1.0,
            });
        }
        return result;
    }

    const QStringList grams = searchTrigrams(normalized);
    if (grams.isEmpty()) {
        return result;
    }

    QString candidateSql =
        QStringLiteral("SELECT g.asset_id,g.field,COUNT(*) AS hits "
                       "FROM search_grams g JOIN assets a ON a.id=g.asset_id "
                       "WHERE g.gram IN (%1)")
            .arg(placeholders(grams.size()));
    if (typeFilter != AssetType::Unknown) {
        candidateSql += QStringLiteral(" AND a.type=?");
    }
    candidateSql += QStringLiteral(
        " GROUP BY g.asset_id,g.field ORDER BY hits DESC LIMIT ?");

    QSqlQuery candidateQuery(database);
    candidateQuery.prepare(candidateSql);
    for (const QString &gram : grams) {
        candidateQuery.addBindValue(gram);
    }
    if (typeFilter != AssetType::Unknown) {
        candidateQuery.addBindValue(assetTypeToString(typeFilter));
    }
    candidateQuery.addBindValue(std::max(limit * 20, 100));
    if (!candidateQuery.exec()) {
        setError(error, candidateQuery.lastError().text());
        return result;
    }

    struct Candidate {
        int hits = 0;
        QSet<QString> fields;
    };
    QHash<QString, Candidate> candidates;
    while (candidateQuery.next()) {
        Candidate &candidate = candidates[candidateQuery.value(0).toString()];
        candidate.hits = std::max(candidate.hits, candidateQuery.value(2).toInt());
        candidate.fields.insert(candidateQuery.value(1).toString());
    }
    if (candidates.isEmpty()) {
        return result;
    }

    const QStringList ids = candidates.keys();
    QSqlQuery assetQuery(database);
    assetQuery.prepare(
        QStringLiteral("SELECT * FROM assets WHERE id IN (%1)").arg(placeholders(ids.size())));
    for (const QString &id : ids) {
        assetQuery.addBindValue(id);
    }
    if (!assetQuery.exec()) {
        setError(error, assetQuery.lastError().text());
        return result;
    }

    while (assetQuery.next()) {
        const QString id = assetQuery.value(QStringLiteral("id")).toString();
        const Candidate candidate = candidates.value(id);
        const qsizetype minimumHits =
            std::max<qsizetype>(1, (grams.size() * 3 + 9) / 10);
        if (candidate.hits < minimumHits) {
            continue;
        }
        QStringList fields(candidate.fields.begin(), candidate.fields.end());
        std::sort(fields.begin(), fields.end());
        double score = static_cast<double>(candidate.hits) / static_cast<double>(grams.size());
        if (json::normalizeSearchText(assetQuery.value(QStringLiteral("name")).toString())
                .contains(normalized)) {
            score += 0.75;
        }
        if (json::normalizeSearchText(id).contains(normalized)) {
            score += 1.0;
        }
        result.append(SearchHit{
            .asset = recordFromQuery(assetQuery),
            .matchedFields = fields,
            .score = score,
        });
    }

    std::sort(result.begin(), result.end(), [](const SearchHit &left, const SearchHit &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        const int nameOrder = QString::compare(left.asset.manifest.name,
                                               right.asset.manifest.name,
                                               Qt::CaseInsensitive);
        return nameOrder == 0 ? left.asset.manifest.id < right.asset.manifest.id : nameOrder < 0;
    });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

QList<AssetRecord> AssetIndex::allAssets(QString *error) const
{
    QList<AssetRecord> result;
    const QList<SearchHit> hits = search(QString(), AssetType::Unknown, 1000000, error);
    result.reserve(hits.size());
    for (const SearchHit &hit : hits) {
        result.append(hit.asset);
    }
    return result;
}

} // namespace xips
