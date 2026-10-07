#include "CatalogFixture.h"
#include "library/CatalogIndex.h"
#include "library/OperationControl.h"
#include "library/SnapshotLibrary.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <QScopeGuard>
#endif
using namespace xips;
namespace
{
void put(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        qFatal("fixture write failed");
}
QByteArray get(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
SnapshotResult create(const QString &library)
{
    QDir().mkpath(library);
    CatalogDefinition d;
    d.name = "counter";
    return savedCatalogFixture(library, d);
}
} // namespace
class WorkflowTest : public QObject
{
    Q_OBJECT
    QTemporaryDir cache;
  private slots:
    void initTestCase() { qputenv("XIPS_TEST_CACHE_ROOT", cache.path().toUtf8()); }
    void emptyWorkspaceImportsAndExactVersions_data()
    {
        QTest::addColumn<QString>("category");
        for (const auto &category : QStringList{"module", "ip", "project"})
            QTest::newRow(qPrintable(category)) << category;
    }
    void emptyWorkspaceImportsAndExactVersions()
    {
        QFETCH(QString, category);
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library");
        QVERIFY(QDir().mkpath(library));
        CatalogDefinition definition; definition.name = "empty_ip"; definition.category = category;
        const auto created = SnapshotLibrary::create(library, definition);
        QVERIFY2(created.ok, qPrintable(created.error));
        QVERIFY(created.snapshot.id.isEmpty());
        QVERIFY(created.asset.snapshots.isEmpty());
        QVERIFY(created.asset.workingFiles.isEmpty());
        QCOMPARE(QDir(created.asset.root).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        auto scan = SnapshotLibrary::scan(library);
        QCOMPARE(scan.assets.size(), 1);
        QVERIFY(scan.problems.isEmpty());
        QVERIFY(scan.assets.first().snapshots.isEmpty());
        QCOMPARE(scan.assets.first().id, created.asset.id);
        QProcess reopened;
        reopened.start(QString::fromUtf8(XIPS_CLI_PATH), {"--action", "list", "--library", library});
        QVERIFY(reopened.waitForFinished());
        QCOMPARE(reopened.exitCode(), 0);
        const auto reloaded = QJsonDocument::fromJson(reopened.readAllStandardOutput()).object()
            .value("data").toObject().value("assets").toArray();
        QCOMPARE(reloaded.size(), 1);
        QCOMPARE(reloaded.first().toObject().value("id").toString(), created.asset.id);
        QCOMPARE(reloaded.first().toObject().value("category").toString(), category);
        QVERIFY(reloaded.first().toObject().value("version").toString().isEmpty());
        QVERIFY(!SnapshotLibrary::previewSelected(created.asset, {}).ok);
        QVERIFY(!SnapshotLibrary::saveSelected(created.asset, {}).ok);
        const auto source = tmp.filePath("external");
        put(source + "/rtl/top.sv", "module original; endmodule");
        put(source + "/rtl/nested/helper.sv", "helper");
        put(source + "/rtl/unused.sv", "unused");
        put(source + "/notes.txt", "notes");
        const auto imported = SnapshotLibrary::importFiles(created.asset, {source + "/rtl", source + "/notes.txt"});
        QVERIFY2(imported.ok, qPrintable(imported.error));
        QCOMPARE(imported.asset.workingFiles.size(), 4);
        QCOMPARE(SnapshotLibrary::heads(imported.asset).size(), 0);
        QCOMPARE(get(created.asset.root + "/rtl/nested/helper.sv"), QByteArray("helper"));
        const auto duplicate = SnapshotLibrary::importFiles(imported.asset, {source + "/rtl", source + "/notes.txt"});
        QVERIFY(duplicate.ok && duplicate.unchanged);
        put(source + "/notes.txt", "conflict");
        put(source + "/aaa_new.txt", "must not be published");
        auto rejected = SnapshotLibrary::importFiles(imported.asset, {source + "/aaa_new.txt", source + "/notes.txt"});
        QVERIFY(!rejected.ok);
        QVERIFY(!QFileInfo::exists(created.asset.root + "/aaa_new.txt"));
        QCOMPARE(get(created.asset.root + "/notes.txt"), QByteArray("notes"));
        OperationControl cancellation; QVERIFY(cancellation.cancel());
        {
            OperationScope scope(&cancellation);
            QVERIFY(SnapshotLibrary::importFiles(imported.asset, {source + "/aaa_new.txt"}).cancelled);
        }
        QVERIFY(!QFileInfo::exists(created.asset.root + "/aaa_new.txt"));
        const QStringList checked{"notes.txt", "rtl/nested/helper.sv"};
        const auto preview = SnapshotLibrary::previewSelected(imported.asset, checked);
        QVERIFY2(preview.ok, qPrintable(preview.error));
        QVERIFY(!SnapshotLibrary::saveSelected(imported.asset, {"rtl/top.sv"}, {}, &preview.preview).ok);
        QVERIFY(!SnapshotLibrary::saveSelected(imported.asset, {"../escape"}).ok);
        put(created.asset.root + "/notes.txt", "changed after review");
        QVERIFY(!SnapshotLibrary::saveSelected(imported.asset, checked, {}, &preview.preview).ok);
        put(created.asset.root + "/notes.txt", "notes");
        const auto first = SnapshotLibrary::saveSelected(imported.asset, checked, "selected subset", &preview.preview);
        QVERIFY2(first.ok, qPrintable(first.error));
        QCOMPARE(first.snapshot.sequence, 1);
        QCOMPARE(first.snapshot.files, checked);
        QVERIFY(SnapshotLibrary::exportSnapshot(first.asset, first.snapshot.id, tmp.filePath("copy1")).ok);
        QCOMPARE(get(tmp.filePath("copy1/rtl/nested/helper.sv")), QByteArray("helper"));
        QVERIFY(!QFileInfo::exists(tmp.filePath("copy1/rtl/top.sv")));
        const auto firstManifest = first.asset.historyRoot + "/.xips/revisions/" + first.snapshot.id + ".json";
        const auto originalManifest = get(firstManifest);
        const auto beforeChange = SnapshotLibrary::previewSelected(first.asset, checked);
        put(created.asset.root + "/notes.txt", "second notes");
        const auto second = SnapshotLibrary::saveSelected(first.asset, checked);
        QVERIFY2(second.ok, qPrintable(second.error));
        QCOMPARE(second.snapshot.sequence, 2);
        QCOMPARE(second.snapshot.files, checked);
        QCOMPARE(get(firstManifest), originalManifest);
        QVERIFY(!SnapshotLibrary::saveSelected(first.asset, checked, {}, &beforeChange.preview).ok);
        QVERIFY(SnapshotLibrary::exportSnapshot(first.asset, first.snapshot.id, tmp.filePath("old")).ok);
        QCOMPARE(get(tmp.filePath("old/notes.txt")), QByteArray("notes"));
        QCOMPARE(get(source + "/rtl/top.sv"), QByteArray("module original; endmodule"));
        QCOMPARE(get(source + "/notes.txt"), QByteArray("conflict"));
        QVERIFY(QFile::remove(created.asset.root + "/notes.txt"));
        QVERIFY(!SnapshotLibrary::saveSelected(second.asset, checked).ok);
        const auto metadataRoot = created.asset.historyRoot;
        CatalogDefinition invalid; invalid.name = "invalid"; invalid.source = metadataRoot;
        QVERIFY(!SnapshotLibrary::create(library, invalid).ok);
        QCOMPARE(SnapshotLibrary::scan(library).assets.size(), 1);
    }
    void folderImportCreatesAndExtendsAssets_data()
    {
        QTest::addColumn<QString>("category");
        QTest::addColumn<bool>("move");
        for (const auto &category : QStringList{"module", "ip", "project"})
            for (bool move : {false, true})
                QTest::newRow(qPrintable(category + (move ? "-move" : "-copy"))) << category << move;
    }
    void folderImportCreatesAndExtendsAssets()
    {
        QFETCH(QString, category);
        QFETCH(bool, move);
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library"), source = tmp.filePath("incoming folder");
        QVERIFY(QDir().mkpath(library));
        put(source + "/rtl/top.sv", "top");
        put(source + "/rtl/nested/helper.sv", "helper");
        put(source + "/unused.txt", "retain");
        ImportRequest request;
        request.sources = {source};
        const auto preview = SnapshotLibrary::previewImport(request.sources);
        QVERIFY2(preview.ok, qPrintable(preview.error));
        request.selection = preview.preview;
        request.selection.objects.remove("incoming folder/unused.txt");
        request.selection.files = request.selection.objects.keys();
        request.move = move;
        request.note = "selected import";
        CatalogDefinition definition; definition.name = "imported"; definition.category = category;
        const auto created = SnapshotLibrary::create(library, definition, &request);
        QVERIFY2(created.ok, qPrintable(created.error));
        QVERIFY(created.retainedSources.isEmpty());
        QCOMPARE(created.asset.category, category);
        QCOMPARE(created.snapshot.sequence, 1);
        QCOMPARE(created.snapshot.files, request.selection.files);
        QCOMPARE(created.snapshot.note, request.note);
        QCOMPARE(created.asset.workingFiles, request.selection.files);
        QVERIFY(SnapshotLibrary::verifySnapshot(created.asset, created.snapshot.id).ok);
        QCOMPARE(get(created.asset.root + "/incoming folder/rtl/top.sv"), QByteArray("top"));
        QCOMPARE(QFileInfo::exists(source + "/rtl/top.sv"), !move);
        QCOMPARE(QFileInfo::exists(source + "/rtl/nested/helper.sv"), !move);
        QCOMPARE(get(source + "/unused.txt"), QByteArray("retain"));
        QVERIFY(!QFileInfo::exists(created.asset.root + "/incoming folder/unused.txt"));
        const auto reloaded = SnapshotLibrary::scan(library);
        QCOMPARE(reloaded.assets.size(), 1);
        QCOMPARE(reloaded.assets.first().category, category);
        QVERIFY(reloaded.problems.isEmpty());
        const auto firstManifest = get(created.asset.historyRoot + "/.xips/revisions/" + created.snapshot.id + ".json");

        const auto next = tmp.filePath("next");
        put(next + "/readme.txt", "second batch");
        request.sources = {next};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        const auto extended = SnapshotLibrary::importAndSave(created.asset, request);
        QVERIFY2(extended.ok, qPrintable(extended.error));
        QCOMPARE(extended.snapshot.sequence, 2);
        QCOMPARE(extended.snapshot.parents, QStringList{created.snapshot.id});
        QCOMPARE(extended.snapshot.files, QStringList{"next/readme.txt"});
        QCOMPARE(extended.asset.workingFiles.size(), 3);
        QCOMPARE(QFileInfo::exists(next), !move);
        QCOMPARE(get(created.asset.historyRoot + "/.xips/revisions/" + created.snapshot.id + ".json"), firstManifest);
        QVERIFY(SnapshotLibrary::verifySnapshot(extended.asset, created.snapshot.id).ok);
        const auto exported = SnapshotLibrary::exportSnapshot(extended.asset, extended.snapshot.id, tmp.filePath("exported"));
        QVERIFY(exported.ok);
        QCOMPARE(get(exported.exportedPath), QByteArray("second batch"));
        if (!move)
        {
            const auto duplicate = SnapshotLibrary::importAndSave(extended.asset, request);
            QVERIFY2(duplicate.ok, qPrintable(duplicate.error));
            QVERIFY(duplicate.unchanged);
            QCOMPARE(duplicate.snapshot.id, extended.snapshot.id);
            request.sources = {source};
            request.selection = preview.preview;
            request.selection.objects.remove("incoming folder/unused.txt");
            request.selection.files = request.selection.objects.keys();
            const auto previousFiles = SnapshotLibrary::importAndSave(extended.asset, request);
            QVERIFY2(previousFiles.ok, qPrintable(previousFiles.error));
            QVERIFY(!previousFiles.unchanged);
            QCOMPARE(previousFiles.snapshot.sequence, 3);
        }
    }
    void folderImportFailuresPreserveSourcesAndDestination()
    {
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library"), source = tmp.filePath("incoming");
        QVERIFY(QDir().mkpath(library));
        put(source + "/top.sv", "reviewed");
        ImportRequest request;
        request.sources = {source};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = true;
        CatalogDefinition definition; definition.name = "project"; definition.category = "project";
        OperationControl cancellation; QVERIFY(cancellation.cancel());
        {
            OperationScope scope(&cancellation);
            QVERIFY(SnapshotLibrary::create(library, definition, &request).cancelled);
        }
        QVERIFY(SnapshotLibrary::scan(library).assets.isEmpty());
        put(source + "/top.sv", "changed after review");
        auto failed = SnapshotLibrary::create(library, definition, &request);
        QVERIFY(!failed.ok);
        QVERIFY(SnapshotLibrary::scan(library).assets.isEmpty());
        QVERIFY(!QFileInfo::exists(library + "/project"));
        QCOMPARE(get(source + "/top.sv"), QByteArray("changed after review"));
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = false;
        const auto created = SnapshotLibrary::create(library, definition, &request);
        QVERIFY2(created.ok, qPrintable(created.error));

        put(source + "/aaa-new.sv", "new");
        put(source + "/top.sv", "conflicting");
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = true;
        failed = SnapshotLibrary::importAndSave(created.asset, request);
        QVERIFY(!failed.ok);
        QVERIFY(!QFileInfo::exists(created.asset.root + "/incoming/aaa-new.sv"));
        QCOMPARE(get(created.asset.root + "/incoming/top.sv"), QByteArray("changed after review"));
        QCOMPARE(get(source + "/top.sv"), QByteArray("conflicting"));
        QCOMPARE(get(source + "/aaa-new.sv"), QByteArray("new"));
        QCOMPARE(SnapshotLibrary::heads(SnapshotLibrary::scan(library).assets.first()), QStringList{created.snapshot.id});
        request.sources = {created.asset.root};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        QVERIFY(!SnapshotLibrary::importAndSave(created.asset, request).ok);
        QVERIFY(QFileInfo::exists(created.asset.root + "/incoming/top.sv"));
        definition.category = "unknown";
        QVERIFY(!SnapshotLibrary::create(library, definition).ok);
    }
    void folderImportArchiveFailureRollsBackWorkingFiles()
    {
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library"), source = tmp.filePath("incoming");
        QVERIFY(QDir().mkpath(library));
        CatalogDefinition definition; definition.name = "empty";
        const auto created = SnapshotLibrary::create(library, definition);
        QVERIFY(created.ok);
        put(source + "/top.sv", "keep source");
        ImportRequest request;
        request.sources = {source};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = true;
        put(created.asset.historyRoot + "/.xips/revisions", "blocks revision directory");
        const auto failed = SnapshotLibrary::importAndSave(created.asset, request);
        QVERIFY(!failed.ok);
        QVERIFY(!QFileInfo::exists(created.asset.root + "/incoming/top.sv"));
        QCOMPARE(get(source + "/top.sv"), QByteArray("keep source"));
        QVERIFY(SnapshotLibrary::heads(SnapshotLibrary::scan(library).assets.first()).isEmpty());
    }
    void movingOverlappingSelectionsRemovesEachSourceOnce()
    {
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library"), folder = tmp.filePath("incoming");
        QVERIFY(QDir().mkpath(library));
        put(folder + "/top.sv", "selected twice");
        ImportRequest request;
        request.sources = {folder, folder + "/top.sv"};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = true;
        CatalogDefinition definition; definition.name = "overlap";
        const auto result = SnapshotLibrary::create(library, definition, &request);
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.retainedSources.isEmpty());
        QCOMPARE(result.snapshot.files, (QStringList{"incoming/top.sv", "top.sv"}));
        QCOMPARE(get(result.asset.root + "/incoming/top.sv"), QByteArray("selected twice"));
        QCOMPARE(get(result.asset.root + "/top.sv"), QByteArray("selected twice"));
        QVERIFY(!QFileInfo::exists(folder));
        QVERIFY(SnapshotLibrary::verifySnapshot(result.asset, result.snapshot.id).ok);
    }
    void movingLockedSourceKeepsArchivedCopy()
    {
#ifdef Q_OS_WIN
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library"), source = tmp.filePath("incoming/top.sv");
        QVERIFY(QDir().mkpath(library));
        put(source, "locked source");
        const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const auto closeHandle = qScopeGuard([&] { CloseHandle(handle); });
        ImportRequest request;
        request.sources = {QFileInfo(source).absolutePath()};
        request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.move = true;
        CatalogDefinition definition; definition.name = "locked_move"; definition.category = "project";
        const auto result = SnapshotLibrary::create(library, definition, &request);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.retainedSources, QStringList{source});
        QCOMPARE(get(source), QByteArray("locked source"));
        QCOMPARE(get(result.asset.root + "/incoming/top.sv"), QByteArray("locked source"));
        QVERIFY(SnapshotLibrary::verifySnapshot(result.asset, result.snapshot.id).ok);
#else
        QSKIP("Windows source sharing semantics");
#endif
    }
    void damagedRevisionAndEmptySourceKeepHealthyHistory()
    {
        QTemporaryDir tmp;
        const auto lib = tmp.filePath("lib");
        auto first = create(lib);
        QVERIFY(first.ok);
        const auto source = first.asset.root + "/rtl/counter.sv";
        put(source, "second");
        const auto second = SnapshotLibrary::saveCurrent(first.asset);
        QVERIFY(second.ok);
        const auto manifest =
            first.asset.historyRoot + "/.xips/revisions/" + second.snapshot.id + ".json";
        const auto good = get(manifest);
        put(manifest, "broken json");
        auto catalog = SnapshotLibrary::scan(lib);
        QCOMPARE(catalog.assets.size(), 1);
        QVERIFY(catalog.assets.first().historyIncomplete);
        QVERIFY(!catalog.problems.isEmpty());
        QVERIFY(SnapshotLibrary::exportSnapshot(catalog.assets.first(), first.snapshot.id,
                                                tmp.filePath("good.sv"))
                    .ok);
        QVERIFY(!SnapshotLibrary::saveCurrent(catalog.assets.first()).ok);
        QVERIFY(!SnapshotLibrary::unregisterSource(catalog.assets.first()).ok);
        put(manifest, good);
        QVERIFY(QFile::remove(source));
        catalog = SnapshotLibrary::scan(lib);
        QCOMPARE(catalog.assets.size(), 1);
        QVERIFY(!catalog.assets.first().sourceProblem.isEmpty());
        QCOMPARE(catalog.assets.first().snapshots.size(), 2);
        QVERIFY(SnapshotLibrary::exportSnapshot(catalog.assets.first(), second.snapshot.id,
                                                tmp.filePath("empty-source.sv"))
                    .ok);
        QCOMPARE(get(tmp.filePath("empty-source.sv")), QByteArray("second"));
        put(manifest, "bad");
        put(first.asset.historyRoot + "/.xips/revisions/" + first.snapshot.id + ".json", "bad");
        catalog = SnapshotLibrary::scan(lib);
        QCOMPARE(catalog.assets.size(), 1);
        QVERIFY(catalog.assets.first().snapshots.isEmpty());
    }
    void previewsBindSaveAndExportToReviewedBytesAndShape()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("lib"));
        QVERIFY(first.ok);
        auto preview = SnapshotLibrary::previewExport(first.asset, "current");
        QVERIFY(preview.ok);
        QCOMPARE(preview.preview.files.size(), 1);
        put(first.asset.root + "/rtl/helper.sv", "module helper; endmodule");
        auto exported = SnapshotLibrary::exportSnapshot(first.asset, "current",
                                                        tmp.filePath("wrong.sv"), &preview.preview);
        QVERIFY(!exported.ok);
        QVERIFY(!QFileInfo::exists(tmp.filePath("wrong.sv")));
        auto save = SnapshotLibrary::previewSave(first.asset);
        QVERIFY(save.ok);
        QCOMPARE(save.preview.added, QStringList{"rtl/helper.sv"});
        put(first.asset.root + "/build/generated.bit", "skip");
        save = SnapshotLibrary::previewSave(first.asset);
        QVERIFY(!save.preview.excluded.isEmpty());
        put(first.asset.root + "/rtl/helper.sv", "changed after preview");
        QVERIFY(!SnapshotLibrary::saveCurrent(first.asset, {}, &save.preview).ok);
        QCOMPARE(SnapshotLibrary::scan(first.asset.library).assets.first().snapshots.size(),
                 2); // saved + current
        preview = SnapshotLibrary::previewExport(first.asset, "current");
        QVERIFY(preview.ok);
        put(first.asset.root + "/rtl/helper.sv", "changed again");
        exported = SnapshotLibrary::exportSnapshot(first.asset, "current",
                                                   tmp.filePath("unreviewed"), &preview.preview);
        QVERIFY(!exported.ok);
        QVERIFY(!QFileInfo::exists(tmp.filePath("unreviewed")));
        preview = SnapshotLibrary::previewExport(first.asset, "current");
        QVERIFY(preview.ok);
        exported = SnapshotLibrary::exportSnapshot(first.asset, "current", tmp.filePath("reviewed"),
                                                   &preview.preview);
        QVERIFY2(exported.ok, qPrintable(exported.error));
        QVERIFY(QFileInfo(tmp.filePath("reviewed")).isDir());
        QVERIFY(!SnapshotLibrary::exportSnapshot(first.asset, "current", tmp.filePath("reviewed"),
                                                 &preview.preview)
                     .ok);
        save = SnapshotLibrary::previewSave(first.asset);
        QVERIFY(save.ok);
        QVERIFY(SnapshotLibrary::saveCurrent(first.asset, {}, &save.preview).ok);
        const auto count =
            SnapshotLibrary::scan(first.asset.library).assets.first().snapshots.size();
        save = SnapshotLibrary::previewSave(first.asset);
        auto unchanged = SnapshotLibrary::saveCurrent(first.asset, {}, &save.preview);
        QVERIFY(unchanged.ok && unchanged.unchanged);
        QCOMPARE(SnapshotLibrary::scan(first.asset.library).assets.first().snapshots.size(), count);
    }
    void unregisterReregisterAndReferenceLifecycle()
    {
        QTemporaryDir tmp;
        const auto owner = tmp.filePath("owner"), receiver = tmp.filePath("receiver");
        auto first = create(owner);
        QVERIFY(first.ok);
        QDir().mkpath(receiver);
        auto ref = SnapshotLibrary::addReference(first.asset, first.snapshot.id, receiver);
        QVERIFY(ref.ok);
        const auto original = get(first.asset.root + "/rtl/counter.sv");
        auto removed = SnapshotLibrary::unregisterSource(first.asset);
        QVERIFY(removed.ok);
        QVERIFY(SnapshotLibrary::unregisterSource(first.asset).unchanged);
        QVERIFY(SnapshotLibrary::scan(owner).assets.isEmpty());
        QCOMPARE(get(first.asset.root + "/rtl/counter.sv"), original);
        QVERIFY(
            SnapshotLibrary::exportSnapshot(ref.asset, first.snapshot.id, tmp.filePath("pinned.sv"))
                .ok);
        CatalogDefinition d;
        d.name = "counter";
        d.source = first.asset.root;
        auto restored = SnapshotLibrary::create(owner, d);
        QVERIFY2(restored.ok, qPrintable(restored.error));
        QCOMPARE(restored.asset.id, first.asset.id);
        put(first.asset.root + "/rtl/counter.sv", "next");
        const auto next = SnapshotLibrary::saveCurrent(restored.asset);
        QVERIFY(next.ok);
        auto changed = SnapshotLibrary::changeReference(ref.asset, owner, next.snapshot.id);
        QVERIFY2(changed.ok, qPrintable(changed.error));
        QVERIFY(!SnapshotLibrary::changeReference(ref.asset, owner, first.snapshot.id)
                     .ok); // stale record
        QVERIFY(SnapshotLibrary::changeReference(changed.asset, owner, next.snapshot.id).unchanged);
        const auto moved = tmp.filePath("moved-owner");
        QVERIFY(QDir().rename(owner, moved));
        auto catalog = SnapshotLibrary::scan(receiver);
        QCOMPARE(catalog.assets.size(), 1);
        QVERIFY(catalog.assets.first().snapshots.isEmpty());
        const auto foreign = create(tmp.filePath("foreign"));
        QVERIFY(foreign.ok);
        QVERIFY(!SnapshotLibrary::changeReference(catalog.assets.first(), foreign.asset.library,
                                                  foreign.snapshot.id)
                     .ok);
        changed = SnapshotLibrary::changeReference(catalog.assets.first(), moved, next.snapshot.id);
        QVERIFY2(changed.ok, qPrintable(changed.error));
        auto detached = SnapshotLibrary::removeReference(changed.asset);
        QVERIFY(detached.ok);
        QVERIFY(QFileInfo::exists(detached.retainedPath));
        QVERIFY(SnapshotLibrary::removeReference(changed.asset).unchanged);
        QVERIFY(SnapshotLibrary::scan(receiver).assets.isEmpty());
        QVERIFY(QFileInfo::exists(moved + "/counter/rtl/counter.sv"));
    }
    void receiptsAndQuotedQueries()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("lib"));
        QVERIFY(first.ok);
        auto exported = SnapshotLibrary::exportSnapshot(first.asset, first.snapshot.id,
                                                        tmp.filePath("copy.sv"));
        QVERIFY(exported.ok);
        const auto receipt = tmp.filePath("copy.origin.json");
        QVERIFY(SnapshotLibrary::saveReceipt(exported, receipt).ok);
        QVERIFY(SnapshotLibrary::saveReceipt(exported, receipt).unchanged);
        auto data = QJsonDocument::fromJson(get(receipt)).object();
        QCOMPARE(data["revision"].toString(), first.snapshot.id);
        QVERIFY(data["sourceImmutable"].toBool());
        put(receipt, "different");
        QVERIFY(!SnapshotLibrary::saveReceipt(exported, receipt).ok);
        QCOMPARE(get(receipt), QByteArray("different"));
        QVERIFY(!SnapshotLibrary::saveReceipt(exported, exported.exportedPath).ok);
        CatalogDefinition d;
        d.name = first.asset.name;
        d.indexes = {{"interface", {"AXI4 Lite"}}, {"purpose", {"clock domain crossing"}}};
        auto edited = SnapshotLibrary::setDefinition(first.asset, d);
        QVERIFY(edited.ok);
        QString error;
        const auto terms = CatalogIndex::queryTerms(
            "interface:\"AXI4 Lite\" purpose:\"clock domain crossing\"", &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(terms.size(), 2);
        QVERIFY(CatalogIndex::matches(edited.asset, terms));
        QVERIFY(CatalogIndex::queryTerms("interface:\"AXI4 Lite", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        auto catalog = SnapshotLibrary::scan(first.asset.library);
        auto indexed = CatalogIndex::matchingRoots(
            first.asset.library, CatalogIndex::generation(catalog.assets), {}, terms);
        QVERIFY(indexed.has_value() && indexed->contains(first.asset.root));
        QProcess cli;
        cli.start(QString::fromUtf8(XIPS_CLI_PATH),
                  {"--action", "list", "--library", first.asset.library, "--query",
                   "interface:\"AXI4 Lite\""});
        QVERIFY(cli.waitForFinished());
        QCOMPARE(cli.exitCode(), 0);
        QVERIFY(cli.readAllStandardOutput().contains("counter"));
    }
    void versionNamesPreserveArchivesAndReferences_data()
    {
        QTest::addColumn<bool>("collected");
        QTest::newRow("working-source") << false;
        QTest::newRow("collected") << true;
    }
    void versionNamesPreserveArchivesAndReferences()
    {
        QFETCH(bool, collected);
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library");
        QVERIFY(QDir().mkpath(library));
        QString source;
        SnapshotResult first;
        if (collected)
        {
            source = tmp.filePath("external/source.sv"); put(source, "first");
            first = SnapshotLibrary::collect(library, {source}, "versions", "module");
        }
        else
        {
            CatalogDefinition definition; definition.name = "versions";
            auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
            source = created.asset.root + "/source.sv"; put(source, "first");
            first = SnapshotLibrary::saveCurrent(created.asset);
        }
        QVERIFY2(first.ok, qPrintable(first.error));
        const auto root = collected ? first.asset.root : first.asset.historyRoot;
        const auto manifest = root + "/.xips/revisions/" + first.snapshot.id + ".json";
        const auto manifestBytes = get(manifest);
        const auto receiver = tmp.filePath("receiver"); QVERIFY(QDir().mkpath(receiver));
        const auto reference = SnapshotLibrary::addReference(first.asset, first.snapshot.id, receiver);
        QVERIFY2(reference.ok, qPrintable(reference.error));
        const auto referenceBytes = get(reference.asset.referencePath);
        auto renamed = SnapshotLibrary::renameSnapshot(first.asset, first.snapshot.id, "  v1.0.0  ");
        QVERIFY2(renamed.ok, qPrintable(renamed.error));
        QCOMPARE(SnapshotLibrary::revisionLabel(renamed.snapshot), QString("v1.0.0"));
        QCOMPARE(renamed.snapshot.id, first.snapshot.id);
        QCOMPARE(renamed.snapshot.hash, first.snapshot.hash);
        QCOMPARE(renamed.snapshot.sequence, first.snapshot.sequence);
        QCOMPARE(renamed.snapshot.parents, first.snapshot.parents);
        QCOMPARE(renamed.snapshot.files, first.snapshot.files);
        QCOMPARE(renamed.asset.nextSequence, first.asset.nextSequence);
        QCOMPARE(SnapshotLibrary::heads(renamed.asset), SnapshotLibrary::heads(first.asset));
        QCOMPARE(get(manifest), manifestBytes);
        QCOMPARE(get(source), QByteArray("first"));
        QVERIFY(CatalogIndex::generation({renamed.asset}) != CatalogIndex::generation({first.asset}));
        const auto reopened = SnapshotLibrary::scan(library);
        QVERIFY(reopened.problems.isEmpty()); QCOMPARE(reopened.assets.size(), 1);
        QCOMPARE(SnapshotLibrary::revisionLabel(SnapshotLibrary::matchingRevisions(reopened.assets.first(), first.snapshot.id).first()), QString("v1.0.0"));
        QCOMPARE(SnapshotLibrary::matchingRevisions(reopened.assets.first(), "1").first().id, first.snapshot.id);
        const auto received = SnapshotLibrary::scan(receiver);
        QVERIFY(received.problems.isEmpty()); QCOMPARE(received.assets.size(), 1);
        QCOMPARE(SnapshotLibrary::revisionLabel(received.assets.first().snapshots.first()), QString("v1.0.0"));
        QCOMPARE(received.assets.first().pinnedRevision, first.snapshot.id);
        QCOMPARE(get(reference.asset.referencePath), referenceBytes);
        QVERIFY(!SnapshotLibrary::renameSnapshot(received.assets.first(), first.snapshot.id, "forbidden").ok);
        const auto exported = SnapshotLibrary::exportSnapshot(renamed.asset, first.snapshot.id, tmp.filePath("copy.sv"));
        QVERIFY2(exported.ok, qPrintable(exported.error)); QCOMPARE(get(exported.exportedPath), QByteArray("first"));
        put(source, "second");
        auto next = collected ? SnapshotLibrary::update(renamed.asset, {source}) : SnapshotLibrary::saveCurrent(renamed.asset);
        QVERIFY2(next.ok, qPrintable(next.error));
        QCOMPARE(next.snapshot.sequence, 2);
        QCOMPARE(next.snapshot.parents, QStringList{first.snapshot.id});
        QCOMPARE(SnapshotLibrary::revisionLabel(next.snapshot), QString("rev2"));
        QCOMPARE(SnapshotLibrary::revisionLabel(SnapshotLibrary::matchingRevisions(next.asset, first.snapshot.id).first()), QString("v1.0.0"));
        const auto reset = SnapshotLibrary::renameSnapshot(next.asset, first.snapshot.id, "rev1");
        QVERIFY2(reset.ok, qPrintable(reset.error)); QVERIFY(reset.snapshot.label.isEmpty());
        QVERIFY(!reset.asset.document.value("revisionLabels").toObject().contains(first.snapshot.id));
        QCOMPARE(get(manifest), manifestBytes);
    }
    void versionNamesRejectInvalidAndStaleEdits()
    {
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library");
        auto first = create(library); QVERIFY(first.ok);
        const auto source = first.asset.root + '/' + first.snapshot.files.first();
        put(source, "second"); auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
        const auto metadata = first.asset.historyRoot + "/.xips.json";
        const auto original = get(metadata);
        for (const auto &name : QStringList{"", " \t ", "line\nbreak", QString(129, 'x'), "REV2"})
            QVERIFY(!SnapshotLibrary::renameSnapshot(second.asset, first.snapshot.id, name).ok);
        QVERIFY(!SnapshotLibrary::renameSnapshot(second.asset, "current", "changed").ok);
        QVERIFY(!SnapshotLibrary::renameSnapshot(second.asset, "working", "changed").ok);
        QCOMPARE(get(metadata), original);
        OperationControl cancelled; QVERIFY(cancelled.cancel());
        { OperationScope scope(&cancelled); QVERIFY(SnapshotLibrary::renameSnapshot(second.asset, first.snapshot.id, "cancelled").cancelled); }
        QLockFile lock(library + "/.xips-library.lock"); QVERIFY(lock.tryLock());
        QVERIFY(!SnapshotLibrary::renameSnapshot(second.asset, first.snapshot.id, "locked").ok); lock.unlock();
        QCOMPARE(get(metadata), original);
        auto renamed = SnapshotLibrary::renameSnapshot(second.asset, first.snapshot.id, "v1");
        QVERIFY2(renamed.ok, qPrintable(renamed.error));
        QVERIFY(!SnapshotLibrary::renameSnapshot(second.asset, first.snapshot.id, "stale").ok);
        const auto same = SnapshotLibrary::renameSnapshot(renamed.asset, first.snapshot.id, "v1");
        QVERIFY(same.ok && same.unchanged);
        QVERIFY(!SnapshotLibrary::renameSnapshot(renamed.asset, second.snapshot.id, "V1").ok);
        const auto savedBytes = get(metadata);
        auto document = renamed.asset.document;
        document.insert("revisionLabels", QJsonObject{{first.snapshot.id, 123}});
        put(metadata, QJsonDocument(document).toJson());
        auto damaged = SnapshotLibrary::describe(renamed.asset);
        QVERIFY(damaged.ok && damaged.asset.historyIncomplete);
        QVERIFY(!damaged.asset.problems.isEmpty());
        QVERIFY(SnapshotLibrary::verifySnapshot(damaged.asset, first.snapshot.id).ok);
        QVERIFY(!SnapshotLibrary::renameSnapshot(damaged.asset, first.snapshot.id, "blocked").ok);
        put(metadata, savedBytes);
        const auto removed = SnapshotLibrary::eraseSnapshot(renamed.asset, second.snapshot.id); QVERIFY(removed.ok);
        QVERIFY(!SnapshotLibrary::renameSnapshot(renamed.asset, second.snapshot.id, "deleted").ok);
        // Editing an archived name does not depend on the working source being present.
        const auto movedSource = tmp.filePath("retained-working-folder");
        QVERIFY(QDir().rename(first.asset.root, movedSource));
        auto unavailable = SnapshotLibrary::describe(removed.asset); QVERIFY(unavailable.ok);
        QVERIFY(!unavailable.asset.sourceProblem.isEmpty());
        renamed = SnapshotLibrary::renameSnapshot(unavailable.asset, first.snapshot.id, "archived-v1");
        QVERIFY2(renamed.ok, qPrintable(renamed.error));
        QCOMPARE(SnapshotLibrary::revisionLabel(renamed.snapshot), QString("archived-v1"));
        QCOMPARE(get(movedSource + '/' + first.snapshot.files.first()), QByteArray("second"));
    }
    void archivedVersionDeletion_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"managed", "linked-file", "linked-folder", "registered-history", "collected"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void archivedVersionDeletion()
    {
        QFETCH(QString, kind);
        QTemporaryDir tmp;
        const auto library = tmp.filePath("library");
        QVERIFY(QDir().mkpath(library));
        CatalogDefinition definition; definition.name = "delete_versions";
        definition.description = "preserved metadata";
        definition.indexes.insert("interface", {"AXI"});
        QString source;
        SnapshotResult first;
        if (kind == "collected")
        {
            source = tmp.filePath("external/top.sv"); put(source, "first");
            first = SnapshotLibrary::collect(library, {source}, definition.name, definition.category);
        }
        else if (kind == "registered-history")
        {
            first = savedCatalogFixture(library, definition);
            QVERIFY2(first.ok, qPrintable(first.error));
            source = first.asset.root + '/' + first.snapshot.files.first();
        }
        else
        {
            if (kind.startsWith("linked"))
            {
                source = library + "/original/top.sv"; put(source, "first");
                definition.source = kind == "linked-file" ? source : QFileInfo(source).absolutePath();
            }
            auto created = SnapshotLibrary::create(library, definition);
            QVERIFY2(created.ok, qPrintable(created.error));
            if (source.isEmpty()) { source = created.asset.root + "/rtl/top.sv"; put(source, "first"); }
            first = SnapshotLibrary::saveCurrent(created.asset);
        }
        QVERIFY2(first.ok, qPrintable(first.error));
        const auto save = [&](const CatalogAsset &asset, const PayloadPreview *preview = nullptr)
        { return asset.discovered ? SnapshotLibrary::saveCurrent(asset, {}, preview)
                                  : SnapshotLibrary::update(asset, {source}, {}, preview); };
        const auto root = first.asset.discovered ? first.asset.historyRoot : first.asset.root;
        const auto definitionBytes = get(root + "/.xips.json");
        const auto group = CatalogGroups::create(library, "AXI"); QVERIFY(group.ok);
        QVERIFY(CatalogGroups::setMember(library, group.group.id, first.asset.id, true).ok);
        const auto groups = CatalogGroups::scan(library).groups;
        auto shared = SnapshotLibrary::collect(library, {source}, "shared_content", "module");
        QVERIFY(shared.ok);
        put(source, "second"); const auto second = save(first.asset); QVERIFY(second.ok);
        put(source, "third"); const auto third = save(second.asset); QVERIFY(third.ok);
        const auto tombstone = root + "/.xips/deleted-revisions/" + second.snapshot.id + ".json";
        OperationControl cancel; QVERIFY(cancel.cancel());
        { OperationScope scope(&cancel); QVERIFY(SnapshotLibrary::eraseSnapshot(third.asset, second.snapshot.id).cancelled); }
        QVERIFY(!QFileInfo::exists(tombstone));
        QLockFile lock(library + "/.xips-library.lock"); QVERIFY(lock.tryLock());
        QVERIFY(!SnapshotLibrary::eraseSnapshot(third.asset, second.snapshot.id).ok); lock.unlock();
        QVERIFY(!QFileInfo::exists(tombstone));
        // A stale asset handle must resolve the exact requested version from fresh history.
        auto removed = SnapshotLibrary::eraseSnapshot(first.asset, second.snapshot.id);
        QVERIFY2(removed.ok, qPrintable(removed.error));
        QCOMPARE(SnapshotLibrary::heads(removed.asset), QStringList{third.snapshot.id});
        QVERIFY(QFileInfo::exists(tombstone));
        QVERIFY(!SnapshotLibrary::eraseSnapshot(third.asset, second.snapshot.id).ok);
        QVERIFY(!SnapshotLibrary::exportSnapshot(third.asset, second.snapshot.id, tmp.filePath("deleted.sv")).ok);
        QVERIFY(!QFileInfo::exists(tmp.filePath("deleted.sv")));
        QVERIFY(SnapshotLibrary::verifySnapshot(third.asset, first.snapshot.id).ok);
        QVERIFY(SnapshotLibrary::verifySnapshot(third.asset, third.snapshot.id).ok);
        for (const auto &object : second.snapshot.objects) ContentStore(library).verify(object);
        removed = SnapshotLibrary::eraseSnapshot(third.asset, third.snapshot.id); QVERIFY(removed.ok);
        QCOMPARE(SnapshotLibrary::heads(removed.asset), QStringList{first.snapshot.id});
        put(source, "fourth"); const auto fourth = save(first.asset); QVERIFY2(fourth.ok, qPrintable(fourth.error));
        QCOMPARE(fourth.snapshot.sequence, 4);
        QVERIFY(fourth.snapshot.id != second.snapshot.id && fourth.snapshot.id != third.snapshot.id);
        QVERIFY(SnapshotLibrary::eraseSnapshot(first.asset, first.snapshot.id).ok);
        removed = SnapshotLibrary::eraseSnapshot(fourth.asset, fourth.snapshot.id);
        QVERIFY2(removed.ok, qPrintable(removed.error));
        QVERIFY(!removed.asset.historyIncomplete);
        QVERIFY(SnapshotLibrary::heads(removed.asset).isEmpty());
        QCOMPARE(removed.asset.nextSequence, 5);
        const auto reopened = SnapshotLibrary::scan(library);
        QVERIFY2(reopened.problems.isEmpty(), qPrintable(reopened.problems.join('\n')));
        QCOMPARE(reopened.assets.size(), 2);
        for (const auto &asset : reopened.assets)
            if (asset.id == first.asset.id)
            {
                QVERIFY(!asset.historyIncomplete);
                QVERIFY(SnapshotLibrary::heads(asset).isEmpty());
                QVERIFY(std::none_of(asset.snapshots.cbegin(), asset.snapshots.cend(),
                    [](const auto &version) { return version.id != "current"; }));
            }
        QCOMPARE(get(root + "/.xips.json"), definitionBytes);
        QCOMPARE(CatalogGroups::scan(library).groups, groups);
        QCOMPARE(get(source), QByteArray("fourth"));
        QVERIFY(SnapshotLibrary::verifySnapshot(shared.asset, shared.snapshot.id).ok);
        QProcess cli; cli.start(QString::fromUtf8(XIPS_CLI_PATH), {"--action", "list", "--library", library});
        QVERIFY(cli.waitForFinished()); QCOMPARE(cli.exitCode(), 0);
        QVERIFY(cli.readAllStandardOutput().contains("delete_versions"));
        const auto preview = SnapshotLibrary::previewSave(first.asset, first.asset.discovered ? QStringList{} : QStringList{source});
        QVERIFY2(preview.ok, qPrintable(preview.error)); QVERIFY(preview.preview.heads.isEmpty());
        const auto fifth = save(first.asset, &preview.preview);
        QVERIFY2(fifth.ok, qPrintable(fifth.error));
        QCOMPARE(fifth.snapshot.sequence, 5); QVERIFY(fifth.snapshot.parents.isEmpty());
        QCOMPARE(SnapshotLibrary::heads(fifth.asset), QStringList{fifth.snapshot.id});
        QCOMPARE(get(root + "/.xips.json"), definitionBytes);
    }
    void deletionProtectsReferencesAndIncompleteSync()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("owner")); QVERIFY(first.ok);
        const auto receiver = tmp.filePath("receiver"); QVERIFY(QDir().mkpath(receiver));
        const auto reference = SnapshotLibrary::addReference(first.asset, first.snapshot.id, receiver);
        QVERIFY2(reference.ok, qPrintable(reference.error));
        const auto ref = SnapshotLibrary::scan(receiver).assets.first();
        const auto record = get(ref.referencePath);
        QVERIFY(!SnapshotLibrary::eraseSnapshot(ref, first.snapshot.id).ok);
        QCOMPARE(get(ref.referencePath), record);
        const auto root = first.asset.historyRoot;
        const auto manifest = root + "/.xips/revisions/" + first.snapshot.id + ".json";
        const auto manifestBytes = get(manifest);
        QVERIFY(SnapshotLibrary::eraseSnapshot(first.asset, first.snapshot.id).ok);
        // A deleted owner version leaves the receiving record intact and unavailable.
        const auto unavailable = SnapshotLibrary::scan(receiver);
        QCOMPARE(unavailable.assets.size(), 1); QVERIFY(!unavailable.problems.isEmpty());
        QCOMPARE(get(ref.referencePath), record);
        QVERIFY(!SnapshotLibrary::verifySnapshot(ref, first.snapshot.id).ok);
        const auto tombstone = root + "/.xips/deleted-revisions/" + first.snapshot.id + ".json";
        const auto deletedBytes = get(tombstone);
        auto corrupted = QJsonDocument::fromJson(deletedBytes).object(); corrupted.insert("assetId", "wrong owner");
        put(tombstone, QJsonDocument(corrupted).toJson());
        auto described = SnapshotLibrary::describe(first.asset); QVERIFY(described.ok);
        QVERIFY(described.asset.historyIncomplete);
        QVERIFY(!SnapshotLibrary::saveCurrent(first.asset).ok);
        QVERIFY(!SnapshotLibrary::eraseSnapshot(first.asset, first.snapshot.id).ok);
        put(tombstone, deletedBytes);
        QVERIFY(QFile::remove(manifest)); // Simulate the tombstone arriving before its manifest.
        described = SnapshotLibrary::describe(first.asset); QVERIFY(described.ok);
        QVERIFY(described.asset.historyIncomplete);
        QVERIFY(!SnapshotLibrary::saveCurrent(first.asset).ok);
        put(manifest, manifestBytes);
        described = SnapshotLibrary::describe(first.asset); QVERIFY(described.ok);
        QVERIFY(!described.asset.historyIncomplete); QVERIFY(SnapshotLibrary::heads(described.asset).isEmpty());
        const auto next = SnapshotLibrary::saveCurrent(first.asset);
        QVERIFY2(next.ok, qPrintable(next.error)); QCOMPARE(next.snapshot.sequence, 2);
        QVERIFY(!SnapshotLibrary::verifySnapshot(ref, first.snapshot.id).ok);
    }
    void deletionKeepsParallelAncestry()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("library")); QVERIFY(first.ok);
        const auto source = first.asset.root + '/' + first.snapshot.files.first();
        put(source, "second"); const auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
        put(source, "third"); const auto third = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(third.ok);
        const auto root = first.asset.historyRoot;
        auto branch = QJsonDocument::fromJson(get(root + "/.xips/revisions/" + second.snapshot.id + ".json")).object();
        const auto branchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        branch.insert("id", branchId);
        ContentStore::publishJson(root + "/.xips/revisions/" + branchId + ".json", branch);
        auto removed = SnapshotLibrary::eraseSnapshot(first.asset, second.snapshot.id); QVERIFY(removed.ok);
        auto heads = SnapshotLibrary::heads(removed.asset); heads.sort();
        auto expected = QStringList{third.snapshot.id, branchId}; expected.sort(); QCOMPARE(heads, expected);
        const auto preview = SnapshotLibrary::previewSave(first.asset); QVERIFY(preview.ok);
        removed = SnapshotLibrary::eraseSnapshot(first.asset, branchId); QVERIFY(removed.ok);
        QCOMPARE(SnapshotLibrary::heads(removed.asset), QStringList{third.snapshot.id});
        QVERIFY(!SnapshotLibrary::saveCurrent(first.asset, {}, &preview.preview).ok);
        put(source, "fourth"); const auto fourth = SnapshotLibrary::saveCurrent(first.asset);
        QVERIFY(fourth.ok); QCOMPARE(fourth.snapshot.sequence, 4);
        QCOMPARE(fourth.snapshot.parents, QStringList{third.snapshot.id});
    }
    void parallelHeadsRequireReviewAgain()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("lib"));
        QVERIFY(first.ok);
        auto preview = SnapshotLibrary::previewSave(first.asset);
        QVERIFY(preview.ok);
        const auto path =
            first.asset.historyRoot + "/.xips/revisions/" + first.snapshot.id + ".json";
        auto branch = QJsonDocument::fromJson(get(path)).object();
        const auto branchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        branch.insert("id", branchId);
        ContentStore::publishJson(
            first.asset.historyRoot + "/.xips/revisions/" + branchId + ".json", branch);
        QVERIFY(!SnapshotLibrary::saveCurrent(first.asset, {}, &preview.preview).ok);
        preview = SnapshotLibrary::previewSave(first.asset);
        QVERIFY(preview.ok);
        QCOMPARE(preview.preview.heads.size(), 2);
        const auto joined =
            SnapshotLibrary::saveCurrent(first.asset, "Adopt reviewed files", &preview.preview);
        QVERIFY(joined.ok);
        QCOMPARE(joined.snapshot.parents.size(), 2);
        QCOMPARE(SnapshotLibrary::heads(joined.asset).size(), 1);
    }
    void cancellationLeavesNoPublishedDestination()
    {
        QTemporaryDir tmp;
        auto first = create(tmp.filePath("lib"));
        QVERIFY(first.ok);
        OperationControl cancelled;
        QVERIFY(cancelled.cancel());
        {
            OperationScope scope(&cancelled);
            auto scan = SnapshotLibrary::scan(first.asset.library);
            QVERIFY(scan.cancelled);
            auto saved = SnapshotLibrary::saveCurrent(first.asset);
            QVERIFY(saved.cancelled && !saved.ok);
            CatalogDefinition definition;
            definition.name = "cancelled_create";
            QVERIFY(SnapshotLibrary::create(first.asset.library, definition).cancelled);
            QVERIFY(!QFileInfo::exists(first.asset.library + "/cancelled_create"));
            auto exported = SnapshotLibrary::exportSnapshot(first.asset, first.snapshot.id,
                                                            tmp.filePath("cancelled.sv"));
            QVERIFY(exported.cancelled);
        }
        QVERIFY(!QFileInfo::exists(tmp.filePath("cancelled.sv")));
        OperationControl committed;
        committed.publish();
        QVERIFY(!committed.cancel());
        {
            OperationScope scope(&committed);
            QVERIFY(SnapshotLibrary::exportSnapshot(first.asset, first.snapshot.id,
                                                    tmp.filePath("published.sv"))
                        .ok);
        }
        const auto large = tmp.filePath("large.dcp");
        put(large, QByteArray(64 * 1024 * 1024, 'x'));
        OperationControl during;
        SnapshotResult result;
        std::thread worker(
            [&]
            {
                OperationScope scope(&during);
                result =
                    SnapshotLibrary::collect(first.asset.library, {large}, "large", "artifact");
            });
        QElapsedTimer timer;
        timer.start();
        while (during.status().isEmpty() && timer.elapsed() < 10000)
            std::this_thread::yield();
        const bool accepted = during.cancel();
        worker.join();
        QVERIFY(accepted);
        QVERIFY(result.cancelled);
        QCOMPARE(SnapshotLibrary::scan(first.asset.library).assets.size(), 1);
        const auto largeSaved =
            SnapshotLibrary::collect(first.asset.library, {large}, "large", "artifact");
        QVERIFY(largeSaved.ok);
        OperationControl exporting;
        std::thread exportWorker(
            [&]
            {
                OperationScope scope(&exporting);
                result = SnapshotLibrary::exportSnapshot(largeSaved.asset, largeSaved.snapshot.id,
                                                         tmp.filePath("interrupted.dcp"));
            });
        timer.restart();
        while (exporting.status().isEmpty() && timer.elapsed() < 10000)
            std::this_thread::yield();
        const bool exportAccepted = exporting.cancel();
        exportWorker.join();
        QVERIFY(exportAccepted);
        QVERIFY(result.cancelled);
        QVERIFY(!QFileInfo::exists(tmp.filePath("interrupted.dcp")));
        QVERIFY(SnapshotLibrary::exportSnapshot(largeSaved.asset, largeSaved.snapshot.id,
                                                tmp.filePath("after-cancel.dcp"))
                    .ok);
        QCOMPARE(QFileInfo(tmp.filePath("after-cancel.dcp")).size(), qint64(64 * 1024 * 1024));
    }
};
QTEST_GUILESS_MAIN(WorkflowTest)
#include "tst_workflows.moc"
