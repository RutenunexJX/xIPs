#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QStringList>
#include "ContentStore.h"
#include "CatalogGroups.h"

namespace xips
{

struct Snapshot
{
    Snapshot() = default;
    Snapshot(const QString &id, const QString &note, const QDateTime &created,
             const QString &hash, const QStringList &files)
        : id(id), note(note), created(created), hash(hash), files(files) {}
    QString id;
    QString note;
    QDateTime created;
    QString hash;
    QStringList files;
    QMap<QString, ContentObject> objects;
    qint64 sequence = 0;
    QStringList parents;
};

struct CatalogAsset
{
    QString id;
    QString name;
    QString category;
    QString description;
    QStringList tags;
    QString root;
    QString library;
    bool legacy = false;
    QList<Snapshot> snapshots;
    QJsonObject document;
    bool discovered = false;
    bool sourceIsDirectory = false;
    QString historyRoot;
    qint64 nextSequence = 1;
    QMap<QString, QStringList> indexes;
    QString referencePath;
    QString pinnedRevision;
};

struct CatalogDefinition
{
    QString name;
    QString category = "module";
    QString source;
    QString description;
    QMap<QString, QStringList> indexes;
};

struct CatalogResult
{
    QList<CatalogAsset> assets;
    QStringList problems;
    QList<CatalogGroup> groups;
};

struct SnapshotResult
{
    bool ok = false;
    bool unchanged = false;
    QString error;
    QString retainedPath;
    CatalogAsset asset;
    Snapshot snapshot;
    QString exportedPath;
};

class SnapshotLibrary
{
  public:
    static CatalogResult scan(const QString &library);
    static QString suggestedCategory(const QStringList &sources);
    static QString categoryLabel(const QString &category);
    static QString revisionLabel(const Snapshot &snapshot);
    static SnapshotResult create(const QString &library, const CatalogDefinition &definition);
    static SnapshotResult setDefinition(const CatalogAsset &asset, const CatalogDefinition &definition);
    static SnapshotResult addReference(const CatalogAsset &asset, const QString &revision,
                                       const QString &destinationLibrary);
    static SnapshotResult saveCurrent(const CatalogAsset &asset, const QString &note = {});
    static SnapshotResult describe(const CatalogAsset &asset);
    static SnapshotResult verifySnapshot(const CatalogAsset &asset, const QString &revision);
    static SnapshotResult collect(const QString &library, const QStringList &sources,
                                  const QString &name, const QString &category,
                                  const QString &note = {});
    static SnapshotResult update(const CatalogAsset &asset, const QStringList &sources,
                                 const QString &note = {});
    static SnapshotResult edit(const CatalogAsset &asset, const QString &name,
                               const QString &category, const QString &description);
    static SnapshotResult exportSnapshot(const CatalogAsset &asset, const QString &revision,
                                         const QString &destination);
    static SnapshotResult eraseSnapshot(const CatalogAsset &asset, const QString &revision,
                                        bool permanent = false);
    static SnapshotResult eraseAsset(const CatalogAsset &asset, bool permanent = false);
    static SnapshotResult migrate(const CatalogAsset &asset);
};

} // namespace xips
