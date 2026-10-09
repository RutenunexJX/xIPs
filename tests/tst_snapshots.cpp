#include "CatalogFixture.h"
#include "library/AssetLibraryService.h"
#include "library/SnapshotLibrary.h"
#include "library/CatalogIndex.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <QScopeGuard>
#endif
using namespace xips;
namespace
{
void put(const QString &path, const QByteArray &value)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size())
        qFatal("fixture write failed");
}
QByteArray get(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
int objectCount(const QString &library)
{
    int count = 0;
    QDirIterator it(library + "/.xips/objects", {"*.obj"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); ++count; }
    return count;
}
} // namespace
class SnapshotTest : public QObject
{
    Q_OBJECT
  private slots:
    void linearVersionsAndPinnedCopies();
    void artifactsKeepBuildOutputs();
    void rejectsCorruptionAndConflicts();
    void deletionNeverReusesVersionNumbers();
    void legacyMigrationPreservesHistory();
    void existingFoldersAreIndexedWithoutImport();
    void ipPackagesStayTogetherAndSkipGeneratedFiles();
    void originalFilesRemainReadOnlyUntilExplicitExport();
    void objectsAreSharedCompressedAndManifestsStayImmutable();
    void sourceHistorySurvivesRelocationAndMissingOriginal();
    void partialSyncAndConcurrentRevisionsAreSafe();
    void localIndexCanBeRebuilt();
    void localIndexUpdatesOnlyOneAssetAndRejectsStaleWriters();
    void catalogDefinitionsSupportMultipleIndexes();
    void referencesPinOneDefinitionAcrossLibrariesAndProjects();
    void groupsPersistWithoutChangingSourcesOrHistory();
    void movingGroupMembershipIsRecoverable();
    void relocatingSourceRetainsHistoryAndRejectsConflicts();
    void opensOriginalsAndReadOnlySavedFiles();
    void workingPreviewDistinguishesEmptySetsAndExplicitSelection();
};
void SnapshotTest::workingPreviewDistinguishesEmptySetsAndExplicitSelection()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "working_preview";
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    const auto empty = SnapshotLibrary::previewWorking(created.asset);
    QVERIFY2(empty.ok, qPrintable(empty.error));
    QVERIFY(empty.preview.files.isEmpty()); QVERIFY(empty.preview.comparisonRevision.isEmpty());
    QVERIFY(empty.preview.added.isEmpty()); QVERIFY(empty.preview.modified.isEmpty());
    QVERIFY(empty.preview.removed.isEmpty());
    QVERIFY(!SnapshotLibrary::previewSelected(created.asset, {}).ok);
    QVERIFY(!SnapshotLibrary::saveSelected(created.asset, {}).ok);
    QVERIFY(!SnapshotLibrary::saveCurrent(created.asset).ok);

    put(created.asset.root + "/a.sv", "module a; endmodule");
    put(created.asset.root + "/b.sv", "module b; endmodule");
    const auto saved = SnapshotLibrary::saveCurrent(created.asset);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const auto manifest = saved.asset.historyRoot + "/.xips/revisions/" + saved.snapshot.id + ".json";
    const auto metadata = saved.asset.historyRoot + "/.xips.json";
    const auto manifestBytes = get(manifest), metadataBytes = get(metadata);
    QVERIFY(!manifestBytes.isEmpty()); QVERIFY(!metadataBytes.isEmpty());
    auto preview = SnapshotLibrary::previewWorking(saved.asset);
    QVERIFY2(preview.ok, qPrintable(preview.error));
    QCOMPARE(preview.preview.unchanged, 2);
    QVERIFY(!preview.preview.comparisonRevision.isEmpty());
    QVERIFY(preview.preview.added.isEmpty()); QVERIFY(preview.preview.modified.isEmpty());
    QVERIFY(preview.preview.removed.isEmpty());
    QVERIFY(!SnapshotLibrary::previewSelected(saved.asset, {}).ok);
    QVERIFY(!SnapshotLibrary::saveSelected(saved.asset, {}).ok);

    // Reuse the stale catalog value: the comparison must enumerate current source files.
    QVERIFY(QFile::remove(saved.asset.root + "/a.sv"));
    preview = SnapshotLibrary::previewWorking(saved.asset);
    QVERIFY2(preview.ok, qPrintable(preview.error));
    QCOMPARE(preview.preview.files, QStringList{"b.sv"});
    QCOMPARE(preview.preview.removed, QStringList{"a.sv"});
    QCOMPARE(preview.preview.unchanged, 1);
    QVERIFY(QFile::remove(saved.asset.root + "/b.sv"));
    QVERIFY(QDir(saved.asset.root).exists());
    preview = SnapshotLibrary::previewWorking(saved.asset);
    QVERIFY2(preview.ok, qPrintable(preview.error));
    QVERIFY(preview.preview.files.isEmpty()); QVERIFY(preview.preview.objects.isEmpty());
    QCOMPARE(preview.preview.removed, (QStringList{"a.sv", "b.sv"}));
    QCOMPARE(preview.preview.unchanged, 0);
    QVERIFY(!preview.preview.comparisonRevision.isEmpty());
    QVERIFY(preview.preview.added.isEmpty()); QVERIFY(preview.preview.modified.isEmpty());
    QVERIFY(!SnapshotLibrary::previewSelected(saved.asset, {}).ok);
    QVERIFY(!SnapshotLibrary::saveSelected(saved.asset, {}).ok);
    QVERIFY(!SnapshotLibrary::saveCurrent(saved.asset).ok);
    QCOMPARE(get(manifest), manifestBytes); QCOMPARE(get(metadata), metadataBytes);
    const auto verified = SnapshotLibrary::verifySnapshot(saved.asset, saved.snapshot.id);
    QVERIFY2(verified.ok, qPrintable(verified.error));
    QCOMPARE(verified.snapshot.files, saved.snapshot.files);
    const auto exported = SnapshotLibrary::exportSnapshot(saved.asset, saved.snapshot.id, tmp.filePath("restored"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("restored/a.sv")), QByteArray("module a; endmodule"));
    QCOMPARE(get(tmp.filePath("restored/b.sv")), QByteArray("module b; endmodule"));
    put(saved.asset.root + "/a.sv", "module a; endmodule");
    put(saved.asset.root + "/b.sv", "module b; endmodule");
    preview = SnapshotLibrary::previewWorking(saved.asset);
    QVERIFY2(preview.ok, qPrintable(preview.error));
    QCOMPARE(preview.preview.unchanged, 2);
    QVERIFY(preview.preview.added.isEmpty()); QVERIFY(preview.preview.modified.isEmpty());
    QVERIFY(preview.preview.removed.isEmpty());
    QCOMPARE(get(manifest), manifestBytes); QCOMPARE(get(metadata), metadataBytes);
}
void SnapshotTest::opensOriginalsAndReadOnlySavedFiles()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), cache = tmp.filePath("cache");
    const auto folder = library + QString::fromUtf8("/原始 source");
    const auto original = folder + "/rtl/serial port.sv";
    put(original, "module original; endmodule\n");
    CatalogDefinition definition;
    definition.name = "serial";
    definition.source = folder;
    const auto created = savedCatalogFixture(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    const auto file = QStringLiteral("rtl/serial port.sv");
    put(original, "module changed; endmodule\n");
    const auto current = SnapshotLibrary::prepareFile(created.asset, "current", file, cache);
    QVERIFY2(current.ok, qPrintable(current.error));
    QCOMPARE(current.exportedPath, QFileInfo(original).absoluteFilePath());
    QVERIFY(!QFileInfo::exists(cache));
    const auto saved = SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, file, cache);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QVERIFY(saved.exportedPath.startsWith(cache + '/'));
    QCOMPARE(get(saved.exportedPath), QByteArray("module original; endmodule\n"));
    QVERIFY(!QFileInfo(saved.exportedPath).permissions().testFlag(QFile::WriteOwner));
    QCOMPARE(get(original), QByteArray("module changed; endmodule\n"));
    QVERIFY(QFile::setPermissions(saved.exportedPath, QFile::ReadOwner | QFile::WriteOwner));
    put(saved.exportedPath, "modified cache");
    const auto reopened = SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, file, cache);
    QVERIFY2(reopened.ok, qPrintable(reopened.error));
    QCOMPARE(reopened.exportedPath, saved.exportedPath);
    QCOMPARE(get(reopened.exportedPath), QByteArray("module original; endmodule\n"));
    QVERIFY(!SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, "../outside.sv", cache).ok);
    QVERIFY(!SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, ".xips.json", cache).ok);
    QVERIFY(!SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, file, library + "/cache").ok);
    QVERIFY(QDir().mkpath(tmp.filePath("receiver")));
    const auto reference = SnapshotLibrary::addReference(created.asset, created.snapshot.id, tmp.filePath("receiver"));
    QVERIFY2(reference.ok, qPrintable(reference.error));
    const auto references = SnapshotLibrary::scan(tmp.filePath("receiver"));
    QCOMPARE(references.assets.size(), 1);
    const auto pinned = SnapshotLibrary::prepareFile(references.assets.first(), created.snapshot.id, file, cache);
    QVERIFY2(pinned.ok, qPrintable(pinned.error));
    QCOMPARE(get(pinned.exportedPath), QByteArray("module original; endmodule\n"));
    QVERIFY(QFile::remove(original));
    QVERIFY(!SnapshotLibrary::prepareFile(created.asset, "current", file, cache).ok);
    QVERIFY(SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, file, cache).ok);
    const auto object = created.snapshot.objects.value(file);
    put(ContentStore(library).objectPath(object.hash), "damaged object");
    QVERIFY(!SnapshotLibrary::prepareFile(created.asset, created.snapshot.id, file, tmp.filePath("fresh-cache")).ok);
}
void SnapshotTest::linearVersionsAndPinnedCopies()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source/uart.sv");
    QDir().mkpath(lib);
    put(src, "module uart; endmodule\n");
    auto first = SnapshotLibrary::collect(lib, {src}, "UART", "module");
    QVERIFY2(first.ok, qPrintable(first.error));
    QCOMPARE(first.snapshot.sequence, 1);
    QVERIFY(!QUuid(first.snapshot.id).isNull());
    QVERIFY(!QFileInfo::exists(first.asset.root + "/uart.sv"));
    const auto same = SnapshotLibrary::update(first.asset, {src});
    QVERIFY2(same.ok, qPrintable(same.error));
    QVERIFY(same.unchanged);
    const auto metadata = SnapshotLibrary::edit(first.asset, "UART RX", "module", "receive data");
    QVERIFY2(metadata.ok, qPrintable(metadata.error));
    QCOMPARE(metadata.asset.snapshots.size(), 1);
    const auto exported =
        SnapshotLibrary::exportSnapshot(metadata.asset, "1", tmp.filePath("project.sv"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    put(src, "module uart; wire valid; endmodule\n");
    auto second = SnapshotLibrary::update(metadata.asset, {src}, "board verified");
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(second.snapshot.sequence, 2);
    QCOMPARE(second.asset.snapshots.size(), 2);
    QCOMPARE(get(tmp.filePath("project.sv")), QByteArray("module uart; endmodule\n"));
    QVERIFY(SnapshotLibrary::exportSnapshot(second.asset, "1", tmp.filePath("old.sv")).ok);
    QCOMPARE(get(tmp.filePath("old.sv")), get(tmp.filePath("project.sv")));
    QVERIFY(!SnapshotLibrary::exportSnapshot(second.asset, "2", tmp.filePath("project.sv")).ok);
    QCOMPARE(SnapshotLibrary::scan(lib).assets.size(), 1);
}
void SnapshotTest::artifactsKeepBuildOutputs()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source");
    QDir().mkpath(lib);
    put(src + "/rtl/top.sv", "module top;endmodule");
    put(src + "/build/top.bit", "bitstream");
    put(src + "/ip_user_files/core.dcp", "checkpoint");
    put(src + "/.git/config", "excluded");
    auto module = SnapshotLibrary::collect(lib, {src}, "module", "module");
    QVERIFY2(module.ok, qPrintable(module.error));
    QCOMPARE(module.snapshot.files, QStringList{"rtl/top.sv"});
    auto artifact = SnapshotLibrary::collect(lib, {src}, "output", "artifact");
    QVERIFY2(artifact.ok, qPrintable(artifact.error));
    QCOMPARE(artifact.snapshot.files.size(), 3);
    QVERIFY(artifact.snapshot.files.contains("build/top.bit"));
    QVERIFY(artifact.snapshot.files.contains("ip_user_files/core.dcp"));
    auto exported = SnapshotLibrary::exportSnapshot(artifact.asset, "1", tmp.filePath("export"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("export/build/top.bit")), QByteArray("bitstream"));
}
void SnapshotTest::rejectsCorruptionAndConflicts()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "original");
    auto first = SnapshotLibrary::collect(lib, {src}, "example", "module");
    QVERIFY(first.ok);
    QVERIFY(!SnapshotLibrary::exportSnapshot(first.asset, "1", lib + "/inside.sv").ok);
    QLockFile lock(lib + "/.xips-library.lock");
    QVERIFY(lock.tryLock());
    QVERIFY(!SnapshotLibrary::update(first.asset, {src}).ok);
    lock.unlock();
    auto edit = SnapshotLibrary::edit(first.asset, "new name", "module", {});
    QVERIFY(edit.ok);
    QVERIFY(!SnapshotLibrary::edit(first.asset, "stale", "module", {}).ok);
    put(ContentStore(lib).objectPath(first.snapshot.objects.value("source.sv").hash), "corrupt");
    auto exportResult = SnapshotLibrary::exportSnapshot(edit.asset, "1", tmp.filePath("bad.sv"));
    QVERIFY(!exportResult.ok);
    QVERIFY(!QFileInfo::exists(tmp.filePath("bad.sv")));
    QVERIFY(!SnapshotLibrary::eraseSnapshot(edit.asset, "1", true).ok);
}
void SnapshotTest::deletionNeverReusesVersionNumbers()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "1");
    auto asset = SnapshotLibrary::collect(lib, {src}, "example", "module");
    QVERIFY(asset.ok);
    put(src, "2");
    asset = SnapshotLibrary::update(asset.asset, {src});
    QVERIFY(asset.ok);
    auto removed = SnapshotLibrary::eraseSnapshot(asset.asset, "2", true);
    QVERIFY2(removed.ok, qPrintable(removed.error));
    put(src, "3");
    auto third = SnapshotLibrary::update(removed.asset, {src});
    QVERIFY2(third.ok, qPrintable(third.error));
    QCOMPARE(third.snapshot.sequence, 3);
    QVERIFY(SnapshotLibrary::eraseAsset(third.asset, true).ok);
    QVERIFY(QFileInfo::exists(src));
    QVERIFY(SnapshotLibrary::scan(lib).assets.isEmpty());
}
void SnapshotTest::legacyMigrationPreservesHistory()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "old");
    AssetRecord old;
    QString error;
    AssetLibraryService service;
    QVERIFY2(
        service.importAsset({lib, src, AssetLibraryService::suggestedMetadata(src)}, &old, &error),
        qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(old, "1.0.0", &saved, &error), qPrintable(error));
    put(old.assetRoot + "/source.sv", "current");
    const auto before = SnapshotLibrary::scan(lib);
    QCOMPARE(before.assets.size(), 1);
    QVERIFY(before.assets.first().legacy);
    auto migrated = SnapshotLibrary::migrate(before.assets.first());
    QVERIFY2(migrated.ok, qPrintable(migrated.error));
    QVERIFY(!migrated.asset.legacy);
    QCOMPARE(migrated.asset.id, old.manifest.id);
    QCOMPARE(migrated.asset.snapshots.size(), 2);
    QVERIFY(QFileInfo::exists(migrated.retainedPath + "/source.sv"));
    QVERIFY(SnapshotLibrary::exportSnapshot(migrated.asset, "1", tmp.filePath("old.sv")).ok);
    QCOMPARE(get(tmp.filePath("old.sv")), QByteArray("old"));
    QVERIFY(SnapshotLibrary::exportSnapshot(migrated.asset, "2", tmp.filePath("current.sv")).ok);
    QCOMPARE(get(tmp.filePath("current.sv")), QByteArray("current"));
}
void SnapshotTest::existingFoldersAreIndexedWithoutImport()
{
    QTemporaryDir tmp;
    const QString library = tmp.filePath("library");
    put(library + "/rtl/uart.sv", "module uart; endmodule\n");
    put(library + "/vendor/fifo.xci", "ip configuration");
    put(library + "/counter.VHD", "entity counter is end counter;");
    put(library + "/build/generated.sv", "skip");
    put(library + "/build-debug/generated.sv", "skip");
    put(library + "/.git/ignored.sv", "skip");
    put(library + "/ip_user_files/generated.sv", "skip");
    put(library + "/notes.txt", "not an asset");
    put(tmp.filePath("managed.sv"), "saved source");
    const auto saved = SnapshotLibrary::collect(library, {tmp.filePath("managed.sv")},
                                                 "Saved module", "module");
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const auto savedRoot = SnapshotLibrary::scan(saved.asset.root);
    QVERIFY(savedRoot.assets.isEmpty());
    QVERIFY(!savedRoot.problems.isEmpty());
    const auto first = SnapshotLibrary::scan(library);
    QVERIFY(first.problems.isEmpty());
    QCOMPARE(first.assets.size(), 1);
    QMap<QString, QString> ids;
    for (const auto &asset : first.assets)
    {
        ids.insert(asset.root, asset.id);
        if (asset.id == saved.asset.id)
        {
            QVERIFY(!asset.discovered);
            continue;
        }
        QVERIFY(asset.discovered);
        QVERIFY(!asset.sourceIsDirectory);
        QVERIFY(asset.document.isEmpty());
        QCOMPARE(asset.snapshots.size(), 1);
        QCOMPARE(asset.snapshots.first().id, QString("current"));
        QVERIFY(asset.snapshots.first().hash.isEmpty());
    }
    const auto repeated = SnapshotLibrary::scan(library);
    QCOMPARE(repeated.assets.size(), first.assets.size());
    for (const auto &asset : repeated.assets)
        QCOMPARE(asset.id, ids.value(asset.root));
    QVERIFY(!QFileInfo::exists(library + "/rtl/.xips.json"));
    QVERIFY(!QFileInfo::exists(library + "/rtl/.xips"));
    QCOMPARE(get(library + "/rtl/uart.sv"), QByteArray("module uart; endmodule\n"));
    put(tmp.filePath("another-library/rtl/uart.sv"), "different library");
    const auto other = SnapshotLibrary::scan(tmp.filePath("another-library"));
    QCOMPARE(other.assets.size(), 0);
    put(library + "/new.sv", "new module");
    QVERIFY(QFile::remove(library + "/counter.VHD"));
    const auto refreshed = SnapshotLibrary::scan(library);
    QCOMPARE(refreshed.assets.size(), 1);
    bool newFile = false;
    for (const auto &asset : refreshed.assets)
    {
        QVERIFY(asset.root != library + "/counter.VHD");
        if (asset.root == library + "/new.sv")
            newFile = true;
        else
            QCOMPARE(asset.id, ids.value(asset.root));
    }
    QVERIFY(!newFile);
}
void SnapshotTest::ipPackagesStayTogetherAndSkipGeneratedFiles()
{
    QTemporaryDir tmp;
    const QString library = tmp.filePath("library"), package = library + "/vendor/UART";
    put(package + "/component.xml", "<component/>");
    put(package + "/rtl/uart.sv", "module uart; endmodule");
    put(package + "/include/defs.svh", "`define WIDTH 8");
    put(package + "/README.md", "package notes");
    put(package + "/.git/config", "private");
    put(package + "/build/generated.v", "generated");
    CatalogDefinition definition;
    definition.name = "UART";
    definition.category = "ip";
    definition.source = package;
    QVERIFY(savedCatalogFixture(library, definition).ok);
    const auto scanned = SnapshotLibrary::scan(library);
    QVERIFY(scanned.problems.isEmpty());
    QCOMPARE(scanned.assets.size(), 1);
    const auto asset = scanned.assets.first();
    QVERIFY(asset.discovered);
    QVERIFY(asset.sourceIsDirectory);
    QCOMPARE(asset.name, QString("UART"));
    QCOMPARE(asset.category, QString("ip"));
    QCOMPARE(asset.snapshots.first().files,
             QStringList({"README.md", "component.xml", "include/defs.svh", "rtl/uart.sv"}));
    const auto directly = SnapshotLibrary::scan(package);
    QCOMPARE(directly.assets.size(), 0);
    QVERIFY(SnapshotLibrary::verifySnapshot(asset, "current").ok);
    const auto exported = SnapshotLibrary::exportSnapshot(asset, "current", tmp.filePath("copy"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("copy/rtl/uart.sv")), get(package + "/rtl/uart.sv"));
    QVERIFY(!QFileInfo::exists(tmp.filePath("copy/build")));
    QVERIFY(!QFileInfo::exists(package + "/.xips.json"));
}
void SnapshotTest::originalFilesRemainReadOnlyUntilExplicitExport()
{
    QTemporaryDir tmp;
    const QString library = tmp.filePath("library"), source = library + "/source.sv";
    put(source, "first source");
    CatalogDefinition definition;
    definition.name = "source";
    definition.source = source;
    QVERIFY(savedCatalogFixture(library, definition).ok);
    const auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.size(), 1);
    const auto asset = catalog.assets.first();
    QVERIFY(SnapshotLibrary::describe(asset).ok);
    QVERIFY(!SnapshotLibrary::update(asset, {source}).ok);
    QVERIFY(!SnapshotLibrary::edit(asset, "renamed", "module", {}).ok);
    QVERIFY(!SnapshotLibrary::eraseAsset(asset, true).ok);
    QVERIFY(!SnapshotLibrary::eraseSnapshot(asset, "current", true).ok);
    QVERIFY(!SnapshotLibrary::migrate(asset).ok);
    QCOMPARE(QDir(library).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({".xips", "source.sv"}));
    QCOMPARE(get(source), QByteArray("first source"));
    QVERIFY(!SnapshotLibrary::verifySnapshot(asset, "99").ok);
    const auto firstProof = SnapshotLibrary::verifySnapshot(asset, "current");
    QVERIFY2(firstProof.ok, qPrintable(firstProof.error));
    put(source, "current source");
    const auto exported = SnapshotLibrary::exportSnapshot(asset, "current", tmp.filePath("copy.sv"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QVERIFY(exported.snapshot.hash != firstProof.snapshot.hash);
    QCOMPARE(get(tmp.filePath("copy.sv")), QByteArray("current source"));
    QVERIFY(!SnapshotLibrary::exportSnapshot(asset, "current", tmp.filePath("copy.sv")).ok);
    QVERIFY(!SnapshotLibrary::exportSnapshot(asset, "current", library + "/inside.sv").ok);
    QVERIFY(QFile::remove(source));
    QVERIFY(!SnapshotLibrary::exportSnapshot(asset, "current", tmp.filePath("missing.sv")).ok);
    QVERIFY(!QFileInfo::exists(tmp.filePath("missing.sv")));
}
void SnapshotTest::objectsAreSharedCompressedAndManifestsStayImmutable()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), source = tmp.filePath("sources");
    QVERIFY(QDir().mkpath(library));
    put(source + "/a.sv", "shared");
    put(source + "/b.sv", "shared");
    put(source + "/empty.sv", {});
    const QByteArray large(3 * 1024 * 1024 + 71, 'x');
    put(source + "/large.bin", large);
    auto first = SnapshotLibrary::collect(library, {source}, "first", "artifact");
    QVERIFY2(first.ok, qPrintable(first.error));
    QCOMPARE(objectCount(library), 3);
    QCOMPARE(first.snapshot.objects.value("a.sv"), first.snapshot.objects.value("b.sv"));
    QVERIFY(QFileInfo(ContentStore(library).objectPath(first.snapshot.objects.value("large.bin").hash)).size() < large.size() / 10);
    const auto manifest = first.asset.root + "/.xips/revisions/" + first.snapshot.id + ".json";
    const auto originalManifest = get(manifest), originalMetadata = get(first.asset.root + "/.xips.json");
    auto another = SnapshotLibrary::collect(library, {source}, "another", "artifact");
    QVERIFY2(another.ok, qPrintable(another.error));
    QCOMPARE(objectCount(library), 3);
    put(source + "/a.sv", "changed");
    auto second = SnapshotLibrary::update(first.asset, {source});
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(objectCount(library), 4);
    QCOMPARE(get(manifest), originalManifest);
    QCOMPARE(get(first.asset.root + "/.xips.json"), originalMetadata);
    QVERIFY(!QFileInfo::exists(first.asset.root + "/.xips/revisions/1/a.sv"));
    QVERIFY(SnapshotLibrary::eraseAsset(second.asset, true).ok);
    const auto exported = SnapshotLibrary::exportSnapshot(another.asset, another.snapshot.id, tmp.filePath("export"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("export/a.sv")), QByteArray("shared"));
    QCOMPARE(get(tmp.filePath("export/large.bin")), large);
    QCOMPARE(QFileInfo(tmp.filePath("export/empty.sv")).size(), 0);
}
void SnapshotTest::sourceHistorySurvivesRelocationAndMissingOriginal()
{
    QTemporaryDir tmp;
    auto library = tmp.filePath("library");
    put(library + "/rtl/source.sv", "first");
    CatalogDefinition definition;
    definition.name = "source";
    definition.source = library + "/rtl/source.sv";
    auto saved = savedCatalogFixture(library, definition);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QVERIFY(!QFileInfo::exists(library + "/rtl/.xips.json"));
    QCOMPARE(get(library + "/rtl/source.sv"), QByteArray("first"));
    QCOMPARE(SnapshotLibrary::scan(library).assets.size(), 1);
    const auto unchanged = SnapshotLibrary::saveCurrent(saved.asset);
    QVERIFY2(unchanged.ok, qPrintable(unchanged.error));
    QVERIFY(unchanged.unchanged);
    put(library + "/rtl/source.sv", "second");
    const auto second = SnapshotLibrary::saveCurrent(saved.asset);
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(second.snapshot.sequence, 2);
    QCOMPARE(second.asset.snapshots.size(), 3);
    const auto moved = tmp.filePath("other-machine-root");
    QVERIFY(QDir().rename(library, moved));
    library = moved;
    const auto relocated = SnapshotLibrary::scan(library);
    QVERIFY2(relocated.problems.isEmpty(), qPrintable(relocated.problems.join('\n')));
    QCOMPARE(relocated.assets.size(), 1);
    QCOMPARE(relocated.assets.first().id, saved.asset.id);
    QVERIFY(QFile::remove(library + "/rtl/source.sv"));
    const auto history = SnapshotLibrary::scan(library);
    QCOMPARE(history.assets.size(), 1);
    QCOMPARE(history.assets.first().snapshots.size(), 2);
    const auto exported = SnapshotLibrary::exportSnapshot(history.assets.first(), saved.snapshot.id, tmp.filePath("old.sv"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("old.sv")), QByteArray("first"));
}
void SnapshotTest::partialSyncAndConcurrentRevisionsAreSafe()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), source = tmp.filePath("source.sv");
    QVERIFY(QDir().mkpath(library));
    put(source, "first");
    auto first = SnapshotLibrary::collect(library, {source}, "asset", "module");
    QVERIFY(first.ok);
    put(source, "second");
    auto second = SnapshotLibrary::update(first.asset, {source});
    QVERIFY(second.ok);
    auto branch = QJsonDocument::fromJson(get(second.asset.root + "/.xips/revisions/" + second.snapshot.id + ".json")).object();
    const auto branchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    branch.insert("id", branchId);
    branch.insert("note", "another machine");
    ContentStore::publishJson(second.asset.root + "/.xips/revisions/" + branchId + ".json", branch);
    const auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.first().snapshots.size(), 3);
    QVERIFY(!SnapshotLibrary::verifySnapshot(catalog.assets.first(), "2").ok);
    QVERIFY(SnapshotLibrary::verifySnapshot(catalog.assets.first(), branchId).ok);
    auto joined = SnapshotLibrary::update(catalog.assets.first(), {source});
    QVERIFY2(joined.ok, qPrintable(joined.error));
    QCOMPARE(joined.snapshot.sequence, 3);
    QCOMPARE(joined.snapshot.parents.size(), 2);
    const auto object = ContentStore(library).objectPath(joined.snapshot.objects.value("source.sv").hash);
    const auto content = get(object);
    QVERIFY(QFile::remove(object));
    const auto missing = SnapshotLibrary::exportSnapshot(joined.asset, joined.snapshot.id, tmp.filePath("unavailable.sv"));
    QVERIFY(!missing.ok);
    QVERIFY(!QFileInfo::exists(tmp.filePath("unavailable.sv")));
    put(object, content);
    QVERIFY(SnapshotLibrary::exportSnapshot(joined.asset, joined.snapshot.id, tmp.filePath("restored.sv")).ok);
    QCOMPARE(get(tmp.filePath("restored.sv")), QByteArray("second"));
}
void SnapshotTest::localIndexCanBeRebuilt()
{
    QTemporaryDir tmp;
    const auto oldCache = qgetenv("XIPS_TEST_CACHE_ROOT");
    const bool hadCache = qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT");
    const auto restore = qScopeGuard([&]
    {
        if (hadCache) qputenv("XIPS_TEST_CACHE_ROOT", oldCache);
        else qunsetenv("XIPS_TEST_CACHE_ROOT");
    });
    qputenv("XIPS_TEST_CACHE_ROOT", tmp.filePath("local-cache").toUtf8());
    const auto library = tmp.filePath("library");
    put(library + "/uart.sv", "module uart; endmodule");
    CatalogDefinition definition;
    definition.name = "UART";
    definition.source = library + "/uart.sv";
    QVERIFY(savedCatalogFixture(library, definition).ok);
    auto catalog = SnapshotLibrary::scan(library);
    const auto index = CatalogIndex::path(library);
    QVERIFY(!index.isEmpty() && !index.startsWith(library));
    QVERIFY2(QFileInfo::exists(index), qPrintable(index));
    auto matches = CatalogIndex::matchingRoots(library, CatalogIndex::generation(catalog.assets), "module", {"uart"});
    QVERIFY(matches.has_value());
    QCOMPARE(matches->size(), 1);
    const auto all = CatalogIndex::matchingRoots(library, CatalogIndex::generation(catalog.assets), {}, {});
    QVERIFY(all.has_value());
    QCOMPARE(all->size(), 1);
    const auto missing = CatalogIndex::matchingRoots(library, CatalogIndex::generation(catalog.assets), {}, {"absent"});
    QVERIFY(missing.has_value());
    QVERIFY(missing->isEmpty());
    QVERIFY(QFile::remove(index));
    catalog = SnapshotLibrary::scan(library);
    QVERIFY(QFileInfo::exists(index));
    put(index, "damaged cache");
    catalog = SnapshotLibrary::scan(library);
    matches = CatalogIndex::matchingRoots(library, CatalogIndex::generation(catalog.assets), "module", {"uart"});
    QVERIFY(matches.has_value());
    QCOMPARE(matches->size(), 1);
    QCOMPARE(QDir(library).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList({".xips", "uart.sv"}));
}
void SnapshotTest::localIndexUpdatesOnlyOneAssetAndRejectsStaleWriters()
{
    QTemporaryDir tmp;
    const auto oldCache = qgetenv("XIPS_TEST_CACHE_ROOT");
    const bool hadCache = qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT");
    const auto restore = qScopeGuard([&]
    {
        if (hadCache) qputenv("XIPS_TEST_CACHE_ROOT", oldCache); else qunsetenv("XIPS_TEST_CACHE_ROOT");
    });
    qputenv("XIPS_TEST_CACHE_ROOT", tmp.filePath("cache").toUtf8());
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "alpha";
    const auto first = savedCatalogFixture(library, definition); QVERIFY(first.ok);
    definition.name = "beta";
    const auto second = savedCatalogFixture(library, definition); QVERIFY(second.ok);
    auto assets = SnapshotLibrary::scan(library).assets;
    const auto before = CatalogIndex::generation(assets);
    const auto path = CatalogIndex::path(library);
    const auto connection = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery query(db);
        // Fail any implementation that replaces unrelated rows during an update.
        for (const auto &table : {QString("assets"), QString("facets"), QString("revisions")})
            QVERIFY(query.exec(QString("CREATE TRIGGER keep_%1 BEFORE DELETE ON %1 WHEN OLD.%2='%3' "
                "BEGIN SELECT RAISE(ABORT, 'unrelated asset touched'); END")
                .arg(table, table == "assets" ? "id" : "asset_id", second.asset.id)));
    }
    QSqlDatabase::removeDatabase(connection);
    put(first.asset.root + "/added.sv", "new file");
    const auto changed = SnapshotLibrary::saveCurrent(first.asset, "new revision"); QVERIFY(changed.ok);
    for (auto &asset : assets) if (asset.id == first.asset.id) asset = changed.asset;
    const auto after = CatalogIndex::generation(assets);
    QVERIFY(before != after);
    QVERIFY(CatalogIndex::updateAsset(library, changed.asset, before, after));
    const auto all = CatalogIndex::matchingRoots(library, after, {}, {});
    QVERIFY(all.has_value()); QCOMPARE(all->size(), 2);
    const auto added = CatalogIndex::matchingRoots(library, after, {}, {"added.sv"});
    QVERIFY(added.has_value()); QCOMPARE(*added, QSet<QString>{changed.asset.root});
    QVERIFY(!CatalogIndex::updateAsset(library, first.asset, before, "stale"));
    QVERIFY(CatalogIndex::matchingRoots(library, after, {}, {}).has_value());
    // A same-path content revision must also change the cache generation.
    put(first.asset.root + "/added.sv", "changed contents");
    const auto newer = SnapshotLibrary::saveCurrent(changed.asset); QVERIFY(newer.ok);
    for (auto &asset : assets) if (asset.id == first.asset.id) asset = newer.asset;
    const auto newest = CatalogIndex::generation(assets); QVERIFY(newest != after);
    QLockFile lock(path + ".lock"); QVERIFY(lock.tryLock(0));
    QVERIFY(!CatalogIndex::updateAsset(library, newer.asset, after, newest));
    lock.unlock();
    QVERIFY(CatalogIndex::updateAsset(library, newer.asset, after, newest));
    // One failing row must roll back an entire coalesced external refresh.
    auto batch = assets;
    for (auto &asset : batch) asset.description = "batch-only change";
    const auto batchGeneration = CatalogIndex::generation(batch);
    QVERIFY(!CatalogIndex::updateAssets(library, batch, newest, batchGeneration));
    const auto rolledBack = CatalogIndex::matchingRoots(library, newest, {}, {"batch-only"});
    QVERIFY(rolledBack.has_value()); QVERIFY(rolledBack->isEmpty());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(path); QVERIFY(db.open());
        QSqlQuery query(db);
        for (const auto &table : {QString("assets"), QString("facets"), QString("revisions")})
            QVERIFY(query.exec("DROP TRIGGER keep_" + table));
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(CatalogIndex::updateAssets(library, batch, newest, batchGeneration));
    const auto combined = CatalogIndex::matchingRoots(library, batchGeneration, {}, {"batch-only"});
    QVERIFY(combined.has_value()); QCOMPARE(combined->size(), 2);
    QVERIFY(QFile::remove(path));
    QVERIFY(!CatalogIndex::updateAsset(library, newer.asset, newest, newest));
    QVERIFY(!QFileInfo::exists(path));
}
void SnapshotTest::catalogDefinitionsSupportMultipleIndexes()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    put(library + "/unregistered.sv", "not in catalog");
    CatalogDefinition definition;
    definition.name = "uart_core";
    definition.category = "ip";
    definition.indexes = {{"category", {"Communication/UART", "Control"}},
                          {"tag", {"serial", "debug"}}, {"interface", {"AXI", "UART"}},
                          {"purpose", {"telemetry"}}};
    auto created = savedCatalogFixture(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    QVERIFY(get(created.asset.root + "/rtl/uart_core.sv").contains("module uart_core"));
    auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.size(), 1);
    QCOMPARE(catalog.assets.first().name, definition.name);
    QVERIFY(CatalogIndex::matches(catalog.assets.first(), {"category:Communication", "tag:SERIAL", "interface:axi", "purpose:telemetry"}));
    QVERIFY(CatalogIndex::matches(catalog.assets.first(), {"category:Control"}));
    QVERIFY(!CatalogIndex::matches(catalog.assets.first(), {"tag:absent"}));
    const auto oldCache = qgetenv("XIPS_TEST_CACHE_ROOT");
    const bool hadCache = qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT");
    const auto restore = qScopeGuard([&] { if (hadCache) qputenv("XIPS_TEST_CACHE_ROOT", oldCache); else qunsetenv("XIPS_TEST_CACHE_ROOT"); });
    qputenv("XIPS_TEST_CACHE_ROOT", tmp.filePath("cache").toUtf8());
    QVERIFY(CatalogIndex::rebuild(library, catalog.assets));
    const auto matches = CatalogIndex::matchingRoots(library, CatalogIndex::generation(catalog.assets), "ip",
        {"category:Communication", "tag:serial", "interface:AXI", "purpose:telemetry"});
    QVERIFY(matches.has_value());
    QCOMPARE(matches->size(), 1);
    definition.source = created.asset.root;
    QVERIFY(!savedCatalogFixture(library, definition).ok);
    definition.indexes["category"] = {"Updated"};
    const auto edited = SnapshotLibrary::setDefinition(created.asset, definition);
    QVERIFY2(edited.ok, qPrintable(edited.error));
    QVERIFY(CatalogIndex::matches(edited.asset, {"category:Updated"}));
    QCOMPARE(edited.asset.snapshots.first().id, created.snapshot.id);
}
void SnapshotTest::referencesPinOneDefinitionAcrossLibrariesAndProjects()
{
    QTemporaryDir tmp;
    const auto owner = tmp.filePath("libraries/owner"), other = tmp.filePath("libraries/other"), project = tmp.filePath("project");
    QVERIFY(QDir().mkpath(owner));
    QVERIFY(QDir().mkpath(other));
    QVERIFY(QDir().mkpath(project));
    CatalogDefinition definition;
    definition.name = "timer_core";
    definition.indexes = {{"category", {"Timing", "Control"}}, {"interface", {"AXI"}}};
    const auto created = savedCatalogFixture(owner, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    const auto independent = savedCatalogFixture(other, definition);
    QVERIFY2(independent.ok, qPrintable(independent.error));
    QVERIFY(independent.asset.id != created.asset.id);
    const auto referenced = SnapshotLibrary::addReference(created.asset, created.snapshot.id, other);
    QVERIFY2(referenced.ok, qPrintable(referenced.error));
    const auto projectRef = SnapshotLibrary::addReference(referenced.asset, created.snapshot.id, project);
    QVERIFY2(projectRef.ok, qPrintable(projectRef.error));
    QCOMPARE(QDir(project).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{".xips"});
    QVERIFY(!QFileInfo::exists(project + "/.xips/objects"));
    auto catalog = SnapshotLibrary::scan(project);
    QCOMPARE(catalog.assets.size(), 1);
    QCOMPARE(catalog.assets.first().id, created.asset.id);
    QCOMPARE(catalog.assets.first().snapshots.size(), 1);
    QCOMPARE(catalog.assets.first().snapshots.first().id, created.snapshot.id);
    QVERIFY(!SnapshotLibrary::setDefinition(catalog.assets.first(), definition).ok);
    QVERIFY(!SnapshotLibrary::saveCurrent(catalog.assets.first()).ok);
    put(created.asset.root + "/rtl/timer_core.sv", "module timer_core; wire newer; endmodule");
    const auto changed = SnapshotLibrary::saveCurrent(created.asset);
    QVERIFY(changed.ok);
    catalog = SnapshotLibrary::scan(project);
    QCOMPARE(catalog.assets.first().snapshots.first().id, created.snapshot.id);
    const auto exported = SnapshotLibrary::exportSnapshot(catalog.assets.first(), created.snapshot.id, tmp.filePath("pinned.sv"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QVERIFY(!get(tmp.filePath("pinned.sv")).contains("newer"));
    QVERIFY(!SnapshotLibrary::addReference(created.asset, changed.snapshot.id, project).ok);
    const auto record = QJsonDocument::fromJson(get(projectRef.exportedPath)).object();
    QVERIFY(!QDir::isAbsolutePath(record.value("definition").toString()));
    QCOMPARE(record.value("revision").toString(), created.snapshot.id);
}
void SnapshotTest::groupsPersistWithoutChangingSourcesOrHistory()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    QVERIFY(CatalogGroups::scan(library).groups.isEmpty());
    QVERIFY(!QFileInfo::exists(library + "/.xips"));
    const auto first = CatalogGroups::create(library, "Bus");
    QVERIFY2(first.ok, qPrintable(first.error));
    QVERIFY(!CatalogGroups::create(library, "bus").ok);
    QVERIFY(!CatalogGroups::create(library, "   ").ok);
    const auto second = CatalogGroups::create(library, "Empty");
    QVERIFY(second.ok);
    QCOMPARE(SnapshotLibrary::scan(library).groups.size(), 2);
    CatalogDefinition definition;
    definition.name = "axi_bridge";
    const auto asset = savedCatalogFixture(library, definition);
    QVERIFY(asset.ok);
    const auto source = library + "/axi_bridge/rtl/axi_bridge.sv";
    const auto original = get(source);
    const auto metadata = get(asset.asset.historyRoot + "/.xips.json");
    const int objects = objectCount(library);
    QVERIFY(CatalogGroups::setMember(library, first.group.id, asset.asset.id, true).ok);
    QVERIFY(CatalogGroups::setMember(library, first.group.id, asset.asset.id.toUpper(), true).ok);
    QVERIFY(CatalogGroups::setMember(library, second.group.id, asset.asset.id, true).ok);
    const auto renamed = CatalogGroups::rename(library, first.group.id, "AXI");
    QVERIFY(renamed.ok);
    QCOMPARE(renamed.group.id, first.group.id);
    QCOMPARE(renamed.group.members.size(), 1);
    QVERIFY(renamed.group.members.contains(asset.asset.id, Qt::CaseInsensitive));
    QVERIFY(!CatalogGroups::rename(library, first.group.id, "Empty").ok);
    const auto groupFile = library + "/.xips/groups/" + first.group.id + ".json";
    const auto record = get(groupFile);
    QLockFile lock(library + "/.xips/groups/.groups.lock");
    QVERIFY(lock.tryLock());
    QVERIFY(!CatalogGroups::rename(library, first.group.id, "Locked").ok);
    QCOMPARE(get(groupFile), record);
    lock.unlock();
    QVERIFY(!CatalogGroups::erase(library, "../../axi_bridge").ok);
    put(groupFile, "{broken");
    QVERIFY(!CatalogGroups::rename(library, first.group.id, "Broken").ok);
    QCOMPARE(get(groupFile), QByteArray("{broken"));
    const auto partial = SnapshotLibrary::scan(library);
    QCOMPARE(partial.groups.size(), 1);
    QVERIFY(!partial.problems.isEmpty());
    QCOMPARE(partial.assets.size(), 1);
    put(groupFile, record);
    QVERIFY(CatalogGroups::erase(library, first.group.id).ok);
    auto groups = CatalogGroups::scan(library).groups;
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().members, QStringList{asset.asset.id});
    QVERIFY(CatalogGroups::setMember(library, second.group.id, asset.asset.id, false).ok);
    groups = CatalogGroups::scan(library).groups;
    QCOMPARE(groups.size(), 1);
    QVERIFY(groups.first().members.isEmpty());
    QCOMPARE(SnapshotLibrary::scan(library).assets.size(), 1);
    QCOMPARE(get(source), original);
    QCOMPARE(get(asset.asset.historyRoot + "/.xips.json"), metadata);
    QCOMPARE(objectCount(library), objects);
    QVERIFY(SnapshotLibrary::verifySnapshot(asset.asset, asset.snapshot.id).ok);
}
void SnapshotTest::movingGroupMembershipIsRecoverable()
{
    QTemporaryDir tmp; const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    const auto a = CatalogGroups::create(library, "A").group;
    const auto b = CatalogGroups::create(library, "B").group;
    const auto c = CatalogGroups::create(library, "C").group;
    const auto path = [&](const CatalogGroup &group) { return library + "/.xips/groups/" + group.id + ".json"; };
    QVERIFY(CatalogGroups::setMember(library, a.id, "asset", true).ok);
    QVERIFY(CatalogGroups::setMember(library, c.id, "asset", true).ok);
    const auto originalA = get(path(a)), originalB = get(path(b)), originalC = get(path(c));
    QLockFile lock(library + "/.xips/groups/.groups.lock"); QVERIFY(lock.tryLock(0));
    QVERIFY(!CatalogGroups::moveMember(library, a.id, b.id, "asset").ok);
    QCOMPARE(get(path(a)), originalA); QCOMPARE(get(path(b)), originalB); lock.unlock();
    QVERIFY(!CatalogGroups::moveMember(library, a.id, a.id, "asset").ok);
    QVERIFY(!CatalogGroups::moveMember(library, a.id, "../invalid", "asset").ok);
    put(path(b), "{broken"); QVERIFY(!CatalogGroups::moveMember(library, a.id, b.id, "asset").ok);
    QCOMPARE(get(path(a)), originalA); put(path(b), originalB);
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path(a));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    const auto close = qScopeGuard([&] { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); });
    const auto failed = CatalogGroups::moveMember(library, a.id, b.id, "asset");
    QVERIFY(!failed.ok); QCOMPARE(get(path(a)), originalA); QCOMPARE(get(path(b)), originalB);
    CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
#endif
    auto moved = CatalogGroups::moveMember(library, a.id, b.id, "ASSET"); QVERIFY2(moved.ok, qPrintable(moved.error));
    QCOMPARE(moved.updatedGroups.size(), 2);
    QCOMPARE(QJsonDocument::fromJson(get(path(a))).object()["members"].toArray().size(), 0);
    QCOMPARE(QJsonDocument::fromJson(get(path(b))).object()["members"].toArray().size(), 1);
    QCOMPARE(get(path(c)), originalC);
    QVERIFY(!CatalogGroups::moveMember(library, a.id, c.id, "asset").ok); // stale drag
    moved = CatalogGroups::moveMember(library, b.id, c.id, "asset"); QVERIFY(moved.ok); // already at destination
    QCOMPARE(QJsonDocument::fromJson(get(path(b))).object()["members"].toArray().size(), 0);
    QCOMPARE(get(path(c)), originalC);
    QVERIFY(CatalogGroups::moveMember(library, {}, a.id, "asset").ok); // ungrouped entry
}

void SnapshotTest::relocatingSourceRetainsHistoryAndRejectsConflicts()
{
    QTemporaryDir tmp; const auto library = tmp.filePath("library"), receiver = tmp.filePath("receiver");
    QVERIFY(QDir().mkpath(library) && QDir().mkpath(receiver));
    CatalogDefinition definition; definition.name = "uart";
    const auto saved = savedCatalogFixture(library, definition); QVERIFY(saved.ok);
    const auto group = CatalogGroups::create(library, "Bus"); QVERIFY(group.ok);
    QVERIFY(CatalogGroups::setMember(library, group.group.id, saved.asset.id, true).ok);
    QVERIFY(SnapshotLibrary::addReference(saved.asset, saved.snapshot.id, receiver).ok);
    const auto manifest = saved.asset.historyRoot + "/.xips/revisions/" + saved.snapshot.id + ".json";
    const auto manifestBytes = get(manifest), metadata = get(saved.asset.historyRoot + "/.xips.json");
    const auto newRoot = library + "/relocated uart";
    QVERIFY(QDir().rename(saved.asset.root, newRoot));
    const auto missing = SnapshotLibrary::describe(saved.asset); QVERIFY(missing.ok);
    QVERIFY(!missing.asset.sourceProblem.isEmpty());
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, receiver).ok);
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, library + "/.xips").ok);
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, newRoot + "/rtl/uart.sv").ok);
    definition.name = "occupied"; const auto occupied = savedCatalogFixture(library, definition); QVERIFY(occupied.ok);
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, occupied.asset.root).ok);
    QCOMPARE(get(saved.asset.historyRoot + "/.xips.json"), metadata);
    QLockFile lock(library + "/.xips-library.lock"); QVERIFY(lock.tryLock(0));
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, newRoot).ok); lock.unlock();
    const auto relocated = SnapshotLibrary::relocateSource(missing.asset, newRoot);
    QVERIFY2(relocated.ok, qPrintable(relocated.error));
    QCOMPARE(relocated.asset.id, saved.asset.id); QCOMPARE(relocated.asset.root, newRoot);
    QCOMPARE(relocated.asset.historyRoot, saved.asset.historyRoot); QVERIFY(relocated.asset.sourceProblem.isEmpty());
    QCOMPARE(relocated.asset.document["source"].toObject()["path"].toString(), QString("relocated uart"));
    QCOMPARE(get(manifest), manifestBytes);
    QCOMPARE(CatalogGroups::scan(library).groups.first().members, QStringList{saved.asset.id});
    QVERIFY(SnapshotLibrary::verifySnapshot(relocated.asset, saved.snapshot.id).ok);
    const auto reference = SnapshotLibrary::scan(receiver); QCOMPARE(reference.assets.size(), 1);
    QVERIFY(SnapshotLibrary::verifySnapshot(reference.assets.first(), saved.snapshot.id).ok);
    QVERIFY(!SnapshotLibrary::relocateSource(missing.asset, newRoot).ok); // stale metadata
    const auto catalog = SnapshotLibrary::scan(library); QVERIFY(catalog.problems.isEmpty());
    QCOMPARE(catalog.assets.size(), 2);
    const auto preview = SnapshotLibrary::previewSelected(relocated.asset, relocated.asset.workingFiles);
    QVERIFY(preview.ok && preview.preview.added.isEmpty() && preview.preview.modified.isEmpty());
}
QTEST_GUILESS_MAIN(SnapshotTest)
#include "tst_snapshots.moc"
