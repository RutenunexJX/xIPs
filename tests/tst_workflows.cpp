#include "library/CatalogIndex.h"
#include "library/OperationControl.h"
#include "library/SnapshotLibrary.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <thread>
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
    return SnapshotLibrary::create(library, d);
}
} // namespace
class WorkflowTest : public QObject
{
    Q_OBJECT
    QTemporaryDir cache;
  private slots:
    void initTestCase() { qputenv("XIPS_TEST_CACHE_ROOT", cache.path().toUtf8()); }
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
