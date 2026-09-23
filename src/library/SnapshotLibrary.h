#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QStringList>

namespace xips
{

struct Snapshot
{
    QString id;
    QString note;
    QDateTime created;
    QString hash;
    QStringList files;
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
};

struct CatalogResult
{
    QList<CatalogAsset> assets;
    QStringList problems;
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
