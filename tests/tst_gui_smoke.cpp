#include "app/MainWindow.h"
#include "library/AssetLibraryService.h"
#include "manifest/ManifestService.h"

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
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

QTreeWidgetItem *findItem(QTreeWidget *tree,
                          const int column,
                          const QString &text)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = tree->topLevelItem(index);
        if (item->text(column) == text) {
            return item;
        }
    }
    return nullptr;
}

} // namespace

class GuiSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void firstRunRequiresAnExplicitLibrary();
    void firstScreenIsACompactAssetLibrary();
    void binaryFilesRemainInventoryEntries();
    void activationSelectsAnAsset();
};

void GuiSmokeTest::firstRunRequiresAnExplicitLibrary()
{
    MainWindow window{QString()};
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *welcome = window.findChild<QWidget *>(QStringLiteral("welcomePage"));
    auto *choose = window.findChild<QToolButton *>(
        QStringLiteral("chooseLibraryButton"));
    auto *addFiles = window.findChild<QAction *>(QStringLiteral("addFilesAction"));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    QVERIFY(welcome);
    QVERIFY(welcome->isVisible());
    QVERIFY(choose);
    QVERIFY(choose->isVisible());
    QVERIFY(addFiles);
    QVERIFY(!addFiles->isEnabled());
    QVERIFY(table);
    QVERIFY(!table->isVisible());
}

void GuiSmokeTest::firstScreenIsACompactAssetLibrary()
{
    MainWindow window(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *add = window.findChild<QToolButton *>(QStringLiteral("addButton"));
    auto *addFolder = window.findChild<QAction *>(QStringLiteral("addFolderAction"));
    auto *addFiles = window.findChild<QAction *>(QStringLiteral("addFilesAction"));
    auto *copy = window.findChild<QAction *>(QStringLiteral("copyAction"));
    auto *more = window.findChild<QToolButton *>(QStringLiteral("moreButton"));
    auto *groupMenu = window.findChild<QToolButton *>(QStringLiteral("groupMenuButton"));
    auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("groupTree"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    auto *tags = window.findChild<QComboBox *>(QStringLiteral("tagFilter"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("detailTabs"));
    auto *info = window.findChild<QTreeWidget *>(QStringLiteral("infoTree"));
    auto *files = window.findChild<QTreeWidget *>(QStringLiteral("fileTree"));
    auto *versions = window.findChild<QTreeWidget *>(QStringLiteral("versionTree"));
    auto *deleteVersion = window.findChild<QAction *>(
        QStringLiteral("deleteVersionAction"));
    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    QVERIFY(table);
    QVERIFY(add);
    QVERIFY(addFolder);
    QVERIFY(addFiles);
    QVERIFY(copy);
    QVERIFY(more);
    QVERIFY(groupMenu);
    QVERIFY(groups);
    QVERIFY(groups->isVisible());
    QVERIFY(groups->width() >= 150);
    QVERIFY(table->width() > groups->width());
    QCOMPARE(add->text(), QStringLiteral("Add"));
    QCOMPARE(addFolder->text(), QStringLiteral("Folder..."));
    QCOMPARE(addFiles->text(), QStringLiteral("Files..."));
    QCOMPARE(copy->text(), QStringLiteral("Copy to..."));
    QVERIFY(search);
    QVERIFY(!tags);
    QVERIFY(tabs);
    QVERIFY(info);
    QVERIFY(files);
    QVERIFY(versions);
    QVERIFY(deleteVersion);
    QVERIFY(!preview);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(table->model()->columnCount(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 10000);
    QVERIFY(table->currentIndex().isValid());
    QTRY_VERIFY_WITH_TIMEOUT(findItem(info,
                                      0,
                                      QStringLiteral("Working copy")),
                             3000);
    QTRY_COMPARE_WITH_TIMEOUT(findItem(info,
                                       0,
                                       QStringLiteral("Working copy"))->text(1),
                              QStringLiteral("Not saved yet"),
                              3000);
    QVERIFY(!findItem(info, 0, QStringLiteral("ID")));
    QVERIFY(!findItem(info, 0, QStringLiteral("Path")));

    QTRY_VERIFY_WITH_TIMEOUT(groups->topLevelItemCount() > 1, 10000);
    QCOMPARE(groups->topLevelItem(0)->text(0), QStringLiteral("All assets"));
    QCOMPARE(groups->topLevelItem(0)->text(1), QStringLiteral("3"));
    QTreeWidgetItem *resetGroup = nullptr;
    for (int index = 1; index < groups->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = groups->topLevelItem(index);
        if (item->text(0).compare(QStringLiteral("reset"),
                                  Qt::CaseInsensitive) == 0) {
            resetGroup = item;
            break;
        }
    }
    QVERIFY(resetGroup);
    QCOMPARE(resetGroup->text(1), QStringLiteral("1"));
    groups->setCurrentItem(resetGroup);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data()
                 .toString(),
             QStringLiteral("Reset Generator"));
    search->setText(QStringLiteral("clocking"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 3000);
    search->clear();
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    groups->setCurrentItem(groups->topLevelItem(0));
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 3000);

    search->setText(QStringLiteral("reset_config.svh"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data()
                 .toString(),
             QStringLiteral("Reset Generator"));
    QVERIFY(table->model()
                ->index(0, AssetTableModel::NameColumn)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("reset_config.svh")));

    search->setText(QStringLiteral("reset_gen"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data()
                 .toString(),
             QStringLiteral("Reset Generator"));
    QTreeWidgetItem *rtlSource = nullptr;
    for (int index = 0; index < files->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = files->topLevelItem(index);
        if (item->text(0).endsWith(QStringLiteral("reset_gen.sv"))) {
            rtlSource = item;
            break;
        }
    }
    QVERIFY(rtlSource);
    QVERIFY(versions->topLevelItemCount() >= 1);
    QCOMPARE(versions->topLevelItem(0)->text(0), QStringLiteral("Working copy"));

    const QString snapshot = qEnvironmentVariable("XIPS_GUI_SNAPSHOT");
    if (!snapshot.isEmpty()) {
        QVERIFY(window.grab().save(snapshot));
    }
}

void GuiSmokeTest::binaryFilesRemainInventoryEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library/binary_ip"));
    QVERIFY(writeFile(QDir(root).absoluteFilePath(QStringLiteral("rtl/top.sv")),
                      QByteArrayLiteral("module top; endmodule\n")));
    QVERIFY(writeFile(QDir(root).absoluteFilePath(QStringLiteral("rtl/state.dcp")),
                      QByteArray::fromHex("504b03040000000102030004000500")));
    Manifest manifest;
    manifest.id = QStringLiteral("binary_ip");
    manifest.name = QStringLiteral("Binary IP");
    QString error;
    QVERIFY2(ManifestService().write(
                 QDir(root).absoluteFilePath(QStringLiteral(".xips.json")),
                 manifest,
                 &error),
             qPrintable(error));

    MainWindow window(temporary.filePath(QStringLiteral("library")));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *files = window.findChild<QTreeWidget *>(QStringLiteral("fileTree"));
    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    QVERIFY(files);
    QVERIFY(!preview);
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 2, 10000);
    QTreeWidgetItem *binary = nullptr;
    for (int index = 0; index < files->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = files->topLevelItem(index);
        if (item->text(0).endsWith(QStringLiteral("state.dcp"))) {
            binary = item;
            break;
        }
    }
    QVERIFY(binary);
}

void GuiSmokeTest::activationSelectsAnAsset()
{
    MainWindow window(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    QVERIFY(table);
    QVERIFY(search);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 10000);

    window.applyActivation({
        .action = ActivationAction::OpenAsset,
        .value = QStringLiteral("reset_gen"),
    });
    QTRY_VERIFY_WITH_TIMEOUT(search->text().isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 3000);
    QCOMPARE(table->currentIndex()
                 .siblingAtColumn(AssetTableModel::NameColumn)
                 .data()
                 .toString(),
             QStringLiteral("Reset Generator"));
}

QTEST_MAIN(GuiSmokeTest)

#include "tst_gui_smoke.moc"
