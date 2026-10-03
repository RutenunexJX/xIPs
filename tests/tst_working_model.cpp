#include "app/WorkingFilesModel.h"
#include "app/BrowserPanel.h"
#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QtTest>

using namespace xips;
class WorkingModelTest final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { initializeEla(); }
    void repeatedRefreshMetrics()
    {
        WorkingFilesModel model;
        QStringList files;
        for (int i = 0; i < 3000; ++i)
            files.append(QStringLiteral("rtl/block_%1/file_%2.sv").arg(i / 30).arg(i));
        files.sort();
        const QSet<QString> checked{files.first(), files.last()};
        model.setFiles(files, checked, true);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        QPersistentModelIndex retained(model.fileIndex(files.first()));
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < 3; ++i) model.setFiles(files, checked, true);
        const double elapsed = double(timer.nsecsElapsed()) / 1e6;
        const QJsonObject report{{"files", files.size()}, {"refreshes", 3}, {"modelResets", resets.count()},
            {"rowsInsertedSignals", inserts.count()}, {"dataChangedSignals", changes.count()},
            {"retainedIndexValid", retained.isValid()}, {"elapsedMs", elapsed},
            {"scope", "in-memory model refresh; no disk scan or rendering throughput claim"}};
        const auto path = qEnvironmentVariable("XIPS_WORKING_MODEL_METRICS");
        if (!path.isEmpty()) { QFile output(path); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QJsonDocument(report).toJson()); }
        qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
        QCOMPARE(model.checkedFiles(), QStringList({files.first(), files.last()}));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserts.count(), 0);
        QCOMPARE(changes.count(), 0);
        QVERIFY(retained.isValid());
    }
    void preservesSelectionAndAppliesChecks()
    {
        WorkingFilesModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setFiles({}, {}, false);
        QCOMPARE(model.columnCount(), 1);
        const QStringList paths{"rtl/a.sv", "rtl/sub/b.sv", "readme.txt"};
        model.setFiles(paths, {"rtl/a.sv"}, true);
        QPersistentModelIndex selected(model.fileIndex("rtl/a.sv"));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy checked(&model, &WorkingFilesModel::checkedFilesChanged);
        QCOMPARE(selected.parent().data(Qt::CheckStateRole).toInt(), int(Qt::PartiallyChecked));
        // A different asset can have the same relative file names but different checks.
        model.setFiles({"readme.txt", "rtl/sub/b.sv", "rtl/a.sv"}, {"rtl/sub/b.sv"}, true);
        QVERIFY(selected.isValid());
        QCOMPARE(resets.count(), 0);
        QCOMPARE(checked.count(), 0);
        QCOMPARE(model.checkedFiles(), QStringList{"rtl/sub/b.sv"});
        QVERIFY(model.setData(selected.parent(), Qt::Checked, Qt::CheckStateRole));
        QCOMPARE(checked.count(), 1);
        QCOMPARE(model.checkedFiles(), QStringList({"rtl/a.sv", "rtl/sub/b.sv"}));
        model.checkAll(false);
        QVERIFY(model.checkedFiles().isEmpty());
        model.setFiles(paths, {"readme.txt"}, true);
        QCOMPARE(model.checkedFiles(), QStringList{"readme.txt"});
        model.setFiles(paths, {}, false);
        QVERIFY(!(model.fileIndex("rtl/a.sv").flags() & Qt::ItemIsUserCheckable));
        QVERIFY(model.checkedFiles().isEmpty());
        model.setFiles(paths, {"rtl/a.sv"}, true);
        QVERIFY(model.fileIndex("rtl/a.sv").flags() & Qt::ItemIsUserCheckable);
        model.setFiles({"new.sv"}, {"rtl/a.sv", "new.sv"}, true);
        QVERIFY(!model.fileIndex("rtl/a.sv").isValid());
        QCOMPARE(model.checkedFiles(), QStringList{"new.sv"});
        model.setFiles({}, {}, true);
        QCOMPARE(model.rowCount(), 0);
    }
};
QTEST_MAIN(WorkingModelTest)
#include "tst_working_model.moc"
