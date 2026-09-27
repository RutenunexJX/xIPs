#include "CatalogIndex.h"
#include "FileSystemUtil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <stdexcept>

namespace xips
{
namespace
{
QString indexKind(const QString &term)
{
    const auto key = term.section(':', 0, 0).toCaseFolded();
    return term.contains(':') && QStringList{"category", "tag", "interface", "purpose"}.contains(key)
        ? key : QString();
}
class Connection
{
    QString name = QUuid::createUuid().toString();
  public:
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    ~Connection()
    {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(name);
    }
    bool open(const QString &path, bool readOnly = false)
    {
        db.setDatabaseName(path);
        db.setConnectOptions(readOnly ? "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0"
                                     : "QSQLITE_BUSY_TIMEOUT=1000");
        return db.open();
    }
};
bool initialize(QSqlDatabase &db)
{
    QSqlQuery query(db);
    return query.exec("PRAGMA journal_mode=DELETE") &&
           query.exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)") &&
           query.exec("CREATE TABLE IF NOT EXISTS assets (id TEXT PRIMARY KEY, root TEXT NOT NULL, "
                      "name TEXT NOT NULL, category TEXT NOT NULL, search TEXT NOT NULL)") &&
           query.exec("CREATE INDEX IF NOT EXISTS assets_category ON assets(category)") &&
           query.exec("CREATE TABLE IF NOT EXISTS revisions (asset_id TEXT NOT NULL, id TEXT NOT NULL, "
                      "sequence INTEGER, created TEXT, note TEXT, hash TEXT, "
                      "PRIMARY KEY(asset_id, id))") &&
           query.exec("CREATE TABLE IF NOT EXISTS facets (asset_id TEXT NOT NULL, kind TEXT NOT NULL, "
                      "value TEXT NOT NULL, PRIMARY KEY(asset_id, kind, value))") &&
           query.exec("CREATE INDEX IF NOT EXISTS facets_lookup ON facets(kind, value, asset_id)");
}
}
QString CatalogIndex::path(const QString &library)
{
    auto key = files::normalizedAbsolute(library);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    QString root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
#ifdef XIPS_ENABLE_TEST_HOOKS
    if (qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT"))
        root = qEnvironmentVariable("XIPS_TEST_CACHE_ROOT");
#endif
    if (root.isEmpty())
        return {};
    const QString result = QDir(root).filePath("xIPs/catalog/" + hash + ".sqlite");
    return files::isWithin(result, library) ? QString() : result;
}
QString CatalogIndex::searchText(const CatalogAsset &asset)
{
    QString text = asset.name + ' ' + asset.description + ' ' + asset.tags.join(' ');
    for (const auto &values : asset.indexes)
        text += ' ' + values.join(' ');
    if (!asset.snapshots.isEmpty())
        text += ' ' + asset.snapshots.last().files.join(' ');
    return text.toCaseFolded();
}
bool CatalogIndex::matches(const CatalogAsset &asset, const QStringList &terms)
{
    const auto text = searchText(asset);
    for (const auto &term : terms)
    {
        const auto kind = indexKind(term);
        if (kind.isEmpty())
        {
            if (!text.contains(term.toCaseFolded())) return false;
            continue;
        }
        const auto wanted = term.mid(term.indexOf(':') + 1).toCaseFolded();
        bool found = false;
        for (const auto &value : asset.indexes.value(kind))
            if (value.toCaseFolded() == wanted || (kind == "category" && value.toCaseFolded().startsWith(wanted + '/')))
                found = true;
        if (!found) return false;
    }
    return true;
}
QString CatalogIndex::generation(const QList<CatalogAsset> &assets)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &asset : assets)
        for (const auto &part : {asset.id, asset.root, asset.category, searchText(asset),
                                QString::fromUtf8(QJsonDocument(asset.document.value("indexes").toObject()).toJson(QJsonDocument::Compact))})
        {
            const auto bytes = part.toUtf8();
            hash.addData(QByteArray::number(bytes.size()) + ':' + bytes);
        }
    return QString::fromLatin1(hash.result().toHex());
}
bool CatalogIndex::rebuild(const QString &library, const QList<CatalogAsset> &assets)
{
    const auto target = path(library);
    if (target.isEmpty())
        return false;
    try { ContentStore::makeDirectory(QFileInfo(target).absolutePath()); ContentStore::safePath(target); }
    catch (const std::exception &) { return false; }
    QLockFile lock(target + ".lock");
    if (!lock.tryLock(0))
        return false;
    Connection connection;
    if (!connection.open(target))
        return false;
    // A corrupt cache can be discarded, but a busy database must never be removed.
    QSqlQuery health(connection.db);
    const bool checked = health.exec("PRAGMA quick_check(1)");
    const QString code = health.lastError().nativeErrorCode();
    const bool corrupt = (!checked && (code == "11" || code == "26")) ||
                         (checked && health.next() && health.value(0).toString() != "ok");
    health.finish();
    if (corrupt)
    {
        connection.db.close();
        if (!QFile::remove(target) || !connection.db.open())
            return false;
    }
    else if (!checked)
        return false;
    if (!initialize(connection.db) || !connection.db.transaction())
        return false;
    QSqlQuery assetQuery(connection.db), revisionQuery(connection.db), facets(connection.db), meta(connection.db);
    if (!assetQuery.exec("DELETE FROM assets") || !revisionQuery.exec("DELETE FROM revisions") ||
        !facets.exec("DELETE FROM facets") ||
        !facets.prepare("INSERT OR IGNORE INTO facets VALUES (?, ?, ?)") ||
        !assetQuery.prepare("INSERT INTO assets VALUES (?, ?, ?, ?, ?)") ||
        !revisionQuery.prepare("INSERT INTO revisions VALUES (?, ?, ?, ?, ?, ?)"))
        return false;
    for (const auto &asset : assets)
    {
        assetQuery.bindValue(0, asset.id);
        assetQuery.bindValue(1, asset.root);
        assetQuery.bindValue(2, asset.name);
        assetQuery.bindValue(3, asset.category);
        assetQuery.bindValue(4, searchText(asset));
        if (!assetQuery.exec())
            return false;
        for (auto it = asset.indexes.cbegin(); it != asset.indexes.cend(); ++it)
            for (const auto &value : it.value())
            {
                facets.bindValue(0, asset.id);
                facets.bindValue(1, it.key());
                facets.bindValue(2, value.toCaseFolded());
                if (!facets.exec()) return false;
            }
        for (const auto &revision : asset.snapshots)
        {
            revisionQuery.bindValue(0, asset.id);
            revisionQuery.bindValue(1, revision.id);
            revisionQuery.bindValue(2, revision.sequence);
            revisionQuery.bindValue(3, revision.created.toString(Qt::ISODateWithMs));
            revisionQuery.bindValue(4, revision.note);
            revisionQuery.bindValue(5, revision.hash);
            if (!revisionQuery.exec())
                return false;
        }
    }
    meta.prepare("INSERT OR REPLACE INTO meta VALUES ('generation', ?)");
    meta.addBindValue(generation(assets));
    return meta.exec() && connection.db.commit();
}
std::optional<QSet<QString>> CatalogIndex::matchingRoots(const QString &library,
                                                       const QString &expected,
                                                       const QString &category,
                                                       const QStringList &terms)
{
    const auto target = path(library);
    if (target.isEmpty() || !QFileInfo::exists(target))
        return std::nullopt;
    try { ContentStore::safePath(target); } catch (const std::exception &) { return std::nullopt; }
    Connection connection;
    if (!connection.open(target, true))
        return std::nullopt;
    QSqlQuery query(connection.db);
    if (!query.exec("SELECT value FROM meta WHERE key='generation'") || !query.next() ||
        query.value(0).toString() != expected)
        return std::nullopt;
    QString sql = "SELECT root FROM assets WHERE 1=1";
    if (!category.isEmpty())
        sql += " AND category=?";
    for (const auto &term : terms)
    {
        const auto kind = indexKind(term);
        if (kind.isEmpty()) sql += " AND instr(search, ?) > 0";
        else sql += kind == "category"
            ? " AND EXISTS (SELECT 1 FROM facets f WHERE f.asset_id=assets.id AND f.kind=? AND (f.value=? OR instr(f.value, ?)=1))"
            : " AND EXISTS (SELECT 1 FROM facets f WHERE f.asset_id=assets.id AND f.kind=? AND f.value=?)";
    }
    if (!query.prepare(sql))
        return std::nullopt;
    if (!category.isEmpty())
        query.addBindValue(category);
    for (const auto &term : terms)
    {
        const auto kind = indexKind(term);
        if (kind.isEmpty()) query.addBindValue(term.toCaseFolded());
        else
        {
            const auto value = term.mid(term.indexOf(':') + 1).toCaseFolded();
            query.addBindValue(kind);
            query.addBindValue(value);
            if (kind == "category") query.addBindValue(value + '/');
        }
    }
    if (!query.exec())
        return std::nullopt;
    QSet<QString> roots;
    while (query.next())
        roots.insert(query.value(0).toString());
    return roots;
}
}
