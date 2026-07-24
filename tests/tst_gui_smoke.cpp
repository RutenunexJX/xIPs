#include "app/MainWindow.h"
#include "assetindex/AssetIndex.h"
#include "manifest/ManifestService.h"

#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using namespace xips;

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

bool createAsset(const QString &library, const QString &id)
{
    const QString root = QDir(library).absoluteFilePath(id);
    Manifest manifest;
    manifest.id = id;
    manifest.type = AssetType::Module;
    manifest.name = id;
    manifest.top = id;
    manifest.sources = {QStringLiteral("rtl/%1.sv").arg(id)};
    manifest.includeDirs = {QStringLiteral("rtl/include")};
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), id},
    };
    QString error;
    return writeFile(QDir(root).absoluteFilePath(manifest.sources.first()),
                     QStringLiteral("module %1; endmodule\n").arg(id).toUtf8())
           && writeFile(
               QDir(root).absoluteFilePath(
                   QStringLiteral("rtl/include/%1.svh").arg(id)),
               QByteArrayLiteral("`define VALUE 1\n"))
           && ManifestService().write(
               QDir(root).absoluteFilePath(QStringLiteral(".xips.json")),
               manifest,
               &error);
}

} // namespace

class GuiSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void firstScreenShowsIndexedAssetLibrary();
    void sourcePreviewCanSelectDeclaredFiles();
    void fileChangeRefreshesOnlyAffectedAsset();
};

void GuiSmokeTest::firstScreenShowsIndexedAssetLibrary()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    MainWindow window(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY),
                      {},
                      temporary.filePath(QStringLiteral("index.sqlite")));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QTableView *table = window.findChild<QTableView *>();
    QLineEdit *search = window.findChild<QLineEdit *>();
    QTabWidget *tabs = window.findChild<QTabWidget *>();
    const QList<QTreeWidget *> trees = window.findChildren<QTreeWidget *>();
    QVERIFY(table);
    QVERIFY(search);
    QVERIFY(tabs);
    QVERIFY(trees.size() >= 3);
    QCOMPARE(tabs->count(), 5);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 4, 15000);
    QVERIFY(table->currentIndex().isValid());

    AssetTableModel *sourceModel = window.findChild<AssetTableModel *>();
    QVERIFY(sourceModel);
    search->setText(QStringLiteral("reset_gen"));
    QTRY_VERIFY_WITH_TIMEOUT(sourceModel->hitAt(0), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        sourceModel->hitAt(0)->asset.manifest.id,
        QStringLiteral("reset_gen"),
        3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        table->model()
            ->index(0, AssetTableModel::NameColumn)
            .data()
            .toString(),
        QStringLiteral("Reset Generator"),
        3000);
    QCOMPARE(table->currentIndex().row(), 0);
    auto *sourcePreview =
        window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    QVERIFY(sourcePreview);
    QTRY_VERIFY_WITH_TIMEOUT(
        sourcePreview->toPlainText().contains(
            QStringLiteral("module reset_gen")),
        3000);
    bool foundResetGenerator = false;
    bool foundUnrelatedVivadoIp = false;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const QString name =
            table->model()
                ->index(row, AssetTableModel::NameColumn)
                .data()
                .toString();
        foundResetGenerator |= name == QStringLiteral("Reset Generator");
        foundUnrelatedVivadoIp |=
            name == QStringLiteral("Vivado Clocking Wizard Package");
    }
    QVERIFY(foundResetGenerator);
    QVERIFY(!foundUnrelatedVivadoIp);

    const QString snapshotPath =
        qEnvironmentVariable("XIPS_GUI_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
        QVERIFY2(window.grab().save(snapshotPath),
                 qPrintable(QStringLiteral("Cannot save GUI snapshot: %1")
                                .arg(snapshotPath)));
    }
}

void GuiSmokeTest::sourcePreviewCanSelectDeclaredFiles()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(createAsset(library, QStringLiteral("multi_source")));
    const QString assetRoot =
        QDir(library).absoluteFilePath(QStringLiteral("multi_source"));
    const QString manifestPath =
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    ManifestLoadResult loaded = ManifestService().load(manifestPath);
    QVERIFY(loaded.ok());
    loaded.manifest->sources.append(QStringLiteral("rtl/second.sv"));
    loaded.manifest->sources.append(QStringLiteral("rtl/state.dcp"));
    QVERIFY(writeFile(
        QDir(assetRoot).absoluteFilePath(QStringLiteral("rtl/second.sv")),
        QByteArrayLiteral("module second; // SECOND_SOURCE_MARKER\nendmodule\n")));
    QVERIFY(writeFile(
        QDir(assetRoot).absoluteFilePath(QStringLiteral("rtl/state.dcp")),
        QByteArray::fromHex("504b03040000000102030004000500")));
    QString error;
    QVERIFY2(ManifestService().write(manifestPath, *loaded.manifest, &error),
             qPrintable(error));

    MainWindow window(
        library,
        {},
        temporary.filePath(QStringLiteral("index.sqlite")));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *selector =
        window.findChild<QComboBox *>(QStringLiteral("sourceSelector"));
    auto *preview =
        window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    QVERIFY(selector);
    QVERIFY(preview);
    QTRY_COMPARE_WITH_TIMEOUT(selector->count(), 3, 10000);
    QVERIFY(!selector->isHidden());
    selector->setCurrentIndex(1);
    QTRY_VERIFY_WITH_TIMEOUT(
        preview->toPlainText().contains(
            QStringLiteral("SECOND_SOURCE_MARKER")),
        3000);
    selector->setCurrentIndex(2);
    QTRY_VERIFY_WITH_TIMEOUT(
        preview->toPlainText().contains(
            QStringLiteral("[Binary preview unavailable]")),
        3000);
    const QString snapshotPath =
        qEnvironmentVariable("XIPS_GUI_MULTI_SOURCE_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
        QVERIFY2(window.grab().save(snapshotPath),
                 qPrintable(QStringLiteral("Cannot save multi-source GUI snapshot: %1")
                                .arg(snapshotPath)));
    }
}

void GuiSmokeTest::fileChangeRefreshesOnlyAffectedAsset()
{
    QTemporaryDir temporary;
    const QString library = temporary.filePath(QStringLiteral("library"));
    QVERIFY(createAsset(library, QStringLiteral("first")));
    QVERIFY(createAsset(library, QStringLiteral("second")));
    const QString database = temporary.filePath(QStringLiteral("index.sqlite"));
    LibraryController controller(database);
    controller.setRoots({LibraryRoot{
        .path = library,
        .origin = AssetOrigin::Managed,
    }});

    int completions = 0;
    QStringList incrementallyRefreshed;
    connect(&controller,
            &LibraryController::indexingFinished,
            this,
            [&completions](const QList<AssetRecord> &,
                           const QList<ScanIssue> &,
                           const qint64) {
                completions += 1;
            });
    connect(&controller,
            &LibraryController::incrementalRefreshStarted,
            this,
            [&incrementallyRefreshed](const QStringList &ids) {
                incrementallyRefreshed = ids;
            });
    controller.rebuild();
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);

    QString error;
    const QList<AssetRecord> initial = AssetIndex(database).allAssets(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(initial.size(), 2);
    QHash<QString, qint64> initialGenerations;
    QHash<QString, QString> initialHashes;
    for (const AssetRecord &asset : initial) {
        initialGenerations.insert(asset.manifest.id, asset.generation);
        initialHashes.insert(asset.manifest.id, asset.contentHash);
    }

    const QString include = QDir(library).absoluteFilePath(
        QStringLiteral("first/rtl/include/first.svh"));
    QVERIFY(writeFile(include, QByteArrayLiteral("`define VALUE 2\n")));
    QTRY_VERIFY_WITH_TIMEOUT(
        incrementallyRefreshed.contains(QStringLiteral("first")),
        5000);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 2, 10000);

    const QList<AssetRecord> refreshed = AssetIndex(database).allAssets(&error);
    QCOMPARE(refreshed.size(), 2);
    for (const AssetRecord &asset : refreshed) {
        if (asset.manifest.id == QStringLiteral("first")) {
            QVERIFY(asset.generation
                    > initialGenerations.value(QStringLiteral("first")));
            QVERIFY(asset.contentHash
                    != initialHashes.value(QStringLiteral("first")));
        } else {
            QCOMPARE(asset.generation,
                     initialGenerations.value(QStringLiteral("second")));
            QCOMPARE(asset.contentHash,
                     initialHashes.value(QStringLiteral("second")));
        }
    }
}

QTEST_MAIN(GuiSmokeTest)

#include "tst_gui_smoke.moc"
