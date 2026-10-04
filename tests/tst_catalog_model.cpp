#include "app/BrowserPanel.h"
#include "app/CatalogModel.h"
#include "library/CatalogIndex.h"
#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>
using namespace xips;
class CatalogModelTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { initializeEla(); }
    void filtersPreserveSurvivingNodesAndMemberships()
    {
        CatalogAsset a, b, c;
        a.id = a.name = "axi_a"; a.root = "/a"; a.category = "ip";
        a.indexes = {{"interface", {"AXI4 Lite"}}, {"category", {"Bus/AXI"}}};
        b = a; b.id = b.name = "axi_b"; b.root = "/b";
        c.id = c.name = "uart"; c.root = "/c"; c.category = "module";
        CatalogModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setAssets({a, b, c}, {}, {{"bus", "Bus", {a.id, b.id}}, {"reuse", "Reuse", {a.id}}});
        const QPersistentModelIndex kept(model.indexForId(a.id, "bus"));
        const QPersistentModelIndex duplicate(model.indexForId(a.id, "reuse"));
        const QPersistentModelIndex group(model.indexForId({}, "bus"));
        QPersistentModelIndex removed(model.indexForId(b.id, "bus"));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        model.filter({}, {"axi_a"});
        QVERIFY(kept.isValid() && duplicate.isValid() && group.isValid());
        QVERIFY(!removed.isValid()); QCOMPARE(model.rowCount(group), 1); QCOMPARE(model.rowCount(), 2);
        model.filter("ip", CatalogIndex::queryTerms("interface:\"AXI4 Lite\" category:Bus"));
        QCOMPARE(model.rowCount(group), 2); QVERIFY(kept.isValid());
        QCOMPARE(model.index(1, 0, group).data().toString(), b.name);
        model.filter("module", {});
        QVERIFY(group.isValid()); QCOMPARE(model.rowCount(group), 0); QCOMPARE(model.rowCount(), 3);
        model.filter({}, {}); QCOMPARE(model.rowCount(group), 2); QCOMPARE(resets.count(), 0);
        // Updates to hidden assets use cached search text when shown again.
        model.filter({}, {"no results"}); b.description = "findme"; QVERIFY(model.updateAsset(b));
        model.filter({}, {"findme"}); QCOMPARE(model.rowCount(group), 1);
        QCOMPARE(model.index(0, 0, group).data().toString(), b.name); QCOMPARE(resets.count(), 0);
    }
    void repeatedSearchDoesNotResetTheCatalog()
    {
        QList<CatalogAsset> assets;
        for (int i = 0; i < 5000; ++i)
        {
            CatalogAsset asset; asset.id = QString::number(i); asset.root = "/" + asset.id;
            asset.name = (i % 2 ? "uart_" : "axi_") + asset.id; asset.category = "module";
            asset.description = "hardware"; assets.append(asset);
        }
        CatalogModel model; model.setAssets(assets);
        const QPersistentModelIndex kept(model.indexForId("0"));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QElapsedTimer elapsed; elapsed.start();
        for (int i = 0; i < 4; ++i) { model.filter({}, {"axi"}); model.filter({}, {"hardware"}); }
        QCOMPARE(resets.count(), 0); QVERIFY(kept.isValid()); QCOMPARE(model.rowCount(), 5000);
        qInfo() << "5000 assets / 8 in-memory filters:" << elapsed.elapsed() << "ms; model resets:" << resets.count()
                << "; excludes disk scanning and desktop rendering";
    }
};
QTEST_MAIN(CatalogModelTest)
#include "tst_catalog_model.moc"
