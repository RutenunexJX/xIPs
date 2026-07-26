#include "app/MainWindow.h"
#include "library/AssetLibraryService.h"
#include "manifest/ManifestService.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
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
    auto *update = window.findChild<QAction *>(
        QStringLiteral("updateAssetAction"));
    auto *deleteAsset = window.findChild<QAction *>(
        QStringLiteral("deleteAssetAction"));
    auto *edit = window.findChild<QAction *>(QStringLiteral("editAction"));
    auto *open = window.findChild<QAction *>(QStringLiteral("openAction"));
    auto *openMatched = window.findChild<QAction *>(
        QStringLiteral("openMatchedFileAction"));
    auto *more = window.findChild<QToolButton *>(QStringLiteral("moreButton"));
    auto *groupMenu = window.findChild<QToolButton *>(QStringLiteral("groupMenuButton"));
    auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("groupTree"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    auto *scope = window.findChild<QLabel *>(QStringLiteral("searchScopeLabel"));
    auto *emptyResults = window.findChild<QLabel *>(
        QStringLiteral("emptyResultsLabel"));
    auto *tags = window.findChild<QComboBox *>(QStringLiteral("tagFilter"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("detailTabs"));
    auto *info = window.findChild<QTreeWidget *>(QStringLiteral("infoTree"));
    auto *files = window.findChild<QTreeWidget *>(QStringLiteral("fileTree"));
    auto *versions = window.findChild<QTreeWidget *>(QStringLiteral("versionTree"));
    auto *deleteVersion = window.findChild<QAction *>(
        QStringLiteral("deleteVersionAction"));
    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    auto *notice = window.findChild<QWidget *>(QStringLiteral("noticeBanner"));
    QVERIFY(table);
    QVERIFY(add);
    QVERIFY(addFolder);
    QVERIFY(addFiles);
    QVERIFY(copy);
    QVERIFY(update);
    QVERIFY(deleteAsset);
    QVERIFY(edit);
    QVERIFY(open);
    QVERIFY(openMatched);
    QVERIFY(more);
    QVERIFY(groupMenu);
    QVERIFY(groups);
    QVERIFY(groups->isVisible());
    QVERIFY(groups->width() >= 150);
    QVERIFY(table->width() > groups->width());
    QCOMPARE(add->text(), QStringLiteral("Add"));
    QCOMPARE(addFolder->text(), QStringLiteral("Folder..."));
    QCOMPARE(addFiles->text(), QStringLiteral("Files..."));
    QCOMPARE(copy->text(), QStringLiteral("Copy working copy..."));
    QVERIFY(search);
    QVERIFY(scope);
    QCOMPARE(scope->text(), QStringLiteral("Scope: All assets"));
    QVERIFY(emptyResults);
    QVERIFY(!tags);
    QVERIFY(tabs);
    QVERIFY(info);
    QVERIFY(files);
    QVERIFY(versions);
    QVERIFY(deleteVersion);
    QVERIFY(!preview);
    QVERIFY(notice);
    QVERIFY(!notice->isVisible());
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(table->model()->columnCount(), 3);
    QCOMPARE(table->model()->headerData(AssetTableModel::GroupsColumn,
                                        Qt::Horizontal).toString(),
             QStringLiteral("Groups"));
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
    QVERIFY(!findItem(info, 0, QStringLiteral("Files")));
    QVERIFY(!findItem(info, 0, QStringLiteral("Last saved version")));
    QCOMPARE(files->columnCount(), 1);
    QCOMPARE(open->text(), QStringLiteral("Open file"));
    QVERIFY(!openMatched->isVisible());
    QCOMPARE(versions->topLevelItemCount(), 0);
    QVERIFY(!deleteVersion->isVisible());

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
                 .data(AssetTableModel::AssetNameRole)
                 .toString(),
             QStringLiteral("Reset Generator"));
    QCOMPARE(open->text(), QStringLiteral("Open folder"));
    search->setText(QStringLiteral("clocking"));
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 1, 3000);
    QCOMPARE(groups->currentItem(), groups->topLevelItem(0));
    QCOMPARE(scope->text(), QStringLiteral("Scope: All assets"));
    groups->setCurrentItem(resetGroup);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 3000);
    QCOMPARE(scope->text(), QStringLiteral("Scope: reset"));
    QVERIFY(emptyResults->isVisible());
    QVERIFY(emptyResults->text().contains(QStringLiteral("group reset")));
    search->clear();
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QVERIFY(!openMatched->isVisible());
    groups->setCurrentItem(groups->topLevelItem(0));
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 3000);

    search->setText(QStringLiteral("reset_config.svh"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    const QString displayedMatch = table->model()
                                       ->index(0, AssetTableModel::NameColumn)
                                       .data()
                                       .toString();
    QVERIFY(displayedMatch.startsWith(
        QStringLiteral("Reset Generator\nMatched: ")));
    QVERIFY(displayedMatch.endsWith(QStringLiteral("reset_config.svh")));
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data(AssetTableModel::MatchedFileRole)
                 .toString(),
             QStringLiteral("rtl/include/reset_config.svh"));
    QVERIFY(table->model()
                ->index(0, AssetTableModel::NameColumn)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("reset_config.svh")));
    QTRY_VERIFY_WITH_TIMEOUT(openMatched->isVisible(), 3000);
    QVERIFY(openMatched->isEnabled());
    QCOMPARE(QDir::cleanPath(openMatched->data().toString()),
             QDir::cleanPath(QDir(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY))
                                 .absoluteFilePath(
                                     QStringLiteral("reset_gen/rtl/include/reset_config.svh"))));
    QVERIFY(files->currentItem());
    QVERIFY(files->currentItem()->text(0).endsWith(
        QStringLiteral("reset_config.svh")));

    search->setText(QStringLiteral("reset_gen"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data(AssetTableModel::AssetNameRole)
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
    QCOMPARE(versions->topLevelItemCount(), 0);

    bool copyDialogObserved = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(QStringLiteral("copyDialog"));
        if (dialog) {
            auto *version = dialog->findChild<QComboBox *>(
                QStringLiteral("copyVersionCombo"));
            auto *name = dialog->findChild<QLineEdit *>(
                QStringLiteral("copyNameEdit"));
            auto *path = dialog->findChild<QLabel *>(
                QStringLiteral("copyFinalPathLabel"));
            copyDialogObserved = version && name && path
                                 && version->currentData().toString().isEmpty()
                                 && name->text() == QStringLiteral("Reset Generator")
                                 && path->text().contains(QStringLiteral("Final path:"));
            dialog->reject();
        }
    });
    copy->trigger();
    QVERIFY(copyDialogObserved);

    bool metadataDialogObserved = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("metadataDialog"));
        if (dialog) {
            metadataDialogObserved = dialog->findChild<QListWidget *>(
                                         QStringLiteral("groupChecklist"))
                                     && dialog->findChild<QLineEdit *>(
                                         QStringLiteral("newGroupEdit"));
            dialog->reject();
        }
    });
    edit->trigger();
    QVERIFY(metadataDialogObserved);

    bool updateDialogObserved = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (dialog) {
            updateDialogObserved = dialog->findChild<QLineEdit *>(
                                       QStringLiteral("updateSourceEdit"))
                                   && dialog->findChild<QLabel *>(
                                       QStringLiteral("updatePreviewLabel"));
            dialog->reject();
        }
    });
    update->trigger();
    QVERIFY(updateDialogObserved);

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
