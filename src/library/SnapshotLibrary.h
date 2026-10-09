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
    // Display-only override; immutable revision manifests never contain this field.
    QString label;
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
    QStringList problems;
    bool historyIncomplete = false;
    QString sourceProblem;
    QStringList workingFiles;
    QString referenceLibrary;
    QJsonObject referenceRecord;
    // Retain ancestry through removed revisions without exposing them as versions.
    QMap<QString, QStringList> deletedRevisionParents;
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
    bool cancelled = false;
    QList<CatalogAsset> assets;
    QStringList problems;
    QList<CatalogGroup> groups;
};

struct PayloadPreview
{
    QString assetId;
    QString revision;
    QString comparisonRevision;
    QStringList files;
    QMap<QString, ContentObject> objects;
    QStringList added;
    QStringList modified;
    QStringList removed;
    QStringList excluded;
    QStringList heads;
    qint64 bytes = 0;
    qsizetype unchanged = 0;
};

struct ImportRequest
{
    QStringList sources;
    PayloadPreview selection;
    bool move = false;
    QString note;
    QString version;
};

struct SnapshotResult
{
    bool ok = false;
    bool cancelled = false;
    bool unchanged = false;
    QString error;
    QString retainedPath;
    QStringList retainedSources;
    CatalogAsset asset;
    Snapshot snapshot;
    QString exportedPath;
    PayloadPreview preview;
};

class SnapshotLibrary
{
  public:
    static CatalogResult scan(const QString &library);
    static QString suggestedCategory(const QStringList &sources);
    static QString categoryLabel(const QString &category);
    static QString revisionLabel(const Snapshot &snapshot);
    // Cached metadata only: exact IDs take precedence over decimal sequence aliases.
    static QList<Snapshot> matchingRevisions(const CatalogAsset &asset, const QString &selector);
    static SnapshotResult create(const QString &library, const CatalogDefinition &definition,
                                 const ImportRequest *request = nullptr);
    static SnapshotResult setDefinition(const CatalogAsset &asset, const CatalogDefinition &definition);
    static SnapshotResult addReference(const CatalogAsset &asset, const QString &revision,
                                       const QString &destinationLibrary);
    static SnapshotResult saveCurrent(const CatalogAsset &asset, const QString &note = {},
                                      const PayloadPreview *expected = nullptr);
    static SnapshotResult previewSave(const CatalogAsset &asset, const QStringList &sources = {});
    static SnapshotResult importFiles(const CatalogAsset &asset, const QStringList &sources);
    static SnapshotResult previewImport(const QStringList &sources);
    static SnapshotResult importAndSave(const CatalogAsset &asset, const ImportRequest &request);
    // Compare every current working file, including a valid empty working set.
    static SnapshotResult previewWorking(const CatalogAsset &asset);
    static SnapshotResult previewSelected(const CatalogAsset &asset, const QStringList &files);
    static SnapshotResult saveSelected(const CatalogAsset &asset, const QStringList &files,
                                       const QString &note = {}, const PayloadPreview *expected = nullptr);
    static SnapshotResult previewCollect(const QStringList &sources, const QString &category);
    static SnapshotResult previewExport(const CatalogAsset &asset, const QString &revision);
    static QStringList heads(const CatalogAsset &asset);
    static SnapshotResult relocateSource(const CatalogAsset &asset, const QString &source);
    static SnapshotResult unregisterSource(const CatalogAsset &asset);
    static SnapshotResult removeReference(const CatalogAsset &asset);
    static SnapshotResult referenceTarget(const CatalogAsset &reference, const QString &ownerLibrary);
    static SnapshotResult changeReference(const CatalogAsset &reference, const QString &ownerLibrary,
                                          const QString &revision);
    static SnapshotResult saveReceipt(const SnapshotResult &exported, const QString &destination);
    static SnapshotResult describe(const CatalogAsset &asset);
    static SnapshotResult verifySnapshot(const CatalogAsset &asset, const QString &revision);
    // Current files remain in place; saved files are verified into a read-only local cache.
    static SnapshotResult prepareFile(const CatalogAsset &asset, const QString &revision,
                                      const QString &relative, const QString &cacheRoot);
    static SnapshotResult collect(const QString &library, const QStringList &sources,
                                  const QString &name, const QString &category,
                                  const QString &note = {}, const PayloadPreview *expected = nullptr);
    static SnapshotResult update(const CatalogAsset &asset, const QStringList &sources,
                                 const QString &note = {}, const PayloadPreview *expected = nullptr);
    static SnapshotResult edit(const CatalogAsset &asset, const QString &name,
                               const QString &category, const QString &description);
    static SnapshotResult exportSnapshot(const CatalogAsset &asset, const QString &revision,
                                         const QString &destination, const PayloadPreview *expected = nullptr);
    static SnapshotResult renameSnapshot(const CatalogAsset &asset, const QString &revision,
                                         const QString &label);
    static SnapshotResult eraseSnapshot(const CatalogAsset &asset, const QString &revision,
                                        bool permanent = false);
    static SnapshotResult eraseAsset(const CatalogAsset &asset, bool permanent = false);
    static SnapshotResult migrate(const CatalogAsset &asset);
};

} // namespace xips
