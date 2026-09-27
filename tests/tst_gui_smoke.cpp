#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaPushButton.h"
#include "ElaTheme.h"
#include "ElaText.h"
#include "ElaToolBar.h"
#include "app/BrowserPanel.h"
#include "app/ElaFilePicker.h"
#include "app/MainWindow.h"
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QMenu>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
using namespace xips;
class GuiSmokeTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QFontDatabase::addApplicationFont(
            QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));
        initializeEla();
    }
    void compactPanelFiltersAndSelectsVersions();
    void unavailableLibraryStaysUnavailable();
    void selectedFolderScansAndRescansWithoutCollect();
    void scannedFilesSaveAndSelectHistory();
    void newIpSupportsFacetBrowsingAndProjectReferences();
    void compactWindowKeepsActionsBesideContent();
    void elaFilePickerNavigatesAndSelects();
};
void GuiSmokeTest::compactPanelFiltersAndSelectsVersions()
{
    QTemporaryDir tmp;
    QDir().mkpath(tmp.filePath("library"));
    QFile file(tmp.filePath("uart.sv"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("module uart;endmodule");
    file.close();
    auto asset =
        SnapshotLibrary::collect(tmp.filePath("library"), {file.fileName()}, "UART RX", "module");
    QVERIFY2(asset.ok, qPrintable(asset.error));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("module uart;wire valid;endmodule");
    file.close();
    const auto updated = SnapshotLibrary::update(asset.asset, {file.fileName()}, "board verified");
    QVERIFY(updated.ok);
    BrowserPanel panel;
    panel.resize(900, 560);
    panel.show();
    panel.setContext(tmp.filePath("library"), {});
    auto *list = panel.findChild<ElaListView *>("assetList");
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    auto *take = panel.findChild<ElaPushButton *>("takeButton");
    QVERIFY(list);
    QVERIFY(versions);
    QVERIFY(search);
    QVERIFY(take);
    QTRY_VERIFY(search->isEnabled());
    QStringList actions;
    QTimer::singleShot(0, &panel,
                       [&]
                       {
                           auto *menu = panel.findChild<QMenu *>("xipsEditMenu");
                           if (!menu)
                               return;
                           for (const auto *action : menu->actions())
                               if (!action->isSeparator())
                                   actions.append(action->text());
                           menu->close();
                       });
    QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(5, 5),
                              search->mapToGlobal(QPoint(5, 5)));
    QApplication::sendEvent(search, &context);
    QTRY_COMPARE(actions,
             QStringList({"Undo", "Redo", "Cut", "Copy", "Paste", "Delete", "Select all"}));
    QTRY_COMPARE(versions->count(), 2);
    QCOMPARE(versions->currentData().toString(), updated.snapshot.id);
    QVERIFY(take->isEnabled());
    versions->setCurrentIndex(1);
    QCOMPARE(panel.saveState().value("revision").toString(), asset.snapshot.id);
    search->setText("missing");
    QTRY_COMPARE(list->model()->rowCount(), 0);
    QVERIFY(!take->isEnabled());
    search->setText("uart.sv");
    QTRY_COMPARE(list->model()->rowCount(), 1);
    QTRY_COMPARE(versions->count(), 2);
    const QString screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        QDir().mkpath(screenshots);
        panel.grab().save(screenshots + "/xips-wide.png");
    }
    panel.resize(400, 700);
    QTest::qWait(40);
    QVERIFY(panel.findChild<QWidget *>("assetDetails")->isVisible());
    if (!screenshots.isEmpty())
        panel.grab().save(screenshots + "/xips-narrow.png");
}
void GuiSmokeTest::unavailableLibraryStaysUnavailable()
{
    QTemporaryDir tmp;
    const QString missing = tmp.filePath("offline");
    BrowserPanel panel;
    panel.setContext(missing, {});
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    QTRY_VERIFY(search->isEnabled());
    QVERIFY(!QFileInfo::exists(missing));
}
void GuiSmokeTest::selectedFolderScansAndRescansWithoutCollect()
{
    QTemporaryDir tmp;
    const QString library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library + "/rtl"));
    QFile source(library + "/rtl/uart.sv");
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("module uart; endmodule");
    source.close();
    BrowserPanel panel;
    panel.resize(850, 600);
    panel.show();
    auto *folder = panel.findChild<ElaPushButton *>("folderButton");
    auto *list = panel.findChild<ElaListView *>("assetList");
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    auto *update = panel.findChild<ElaPushButton *>("updateButton");
    QVERIFY(folder);
    QVERIFY(folder->isVisible());
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(!folder->isVisible());
    QCOMPARE(list->model()->rowCount(), 0);
    CatalogDefinition definition;
    definition.name = "UART";
    definition.source = source.fileName();
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(list->model()->rowCount(), 1);
    QCOMPARE(versions->currentData().toString(), QString("current"));
    QCOMPARE(versions->currentText(), QString("Current files"));
    QCOMPARE(update->text(), QString("Save…"));
    QCOMPARE(list->model()->index(0, 0).data().toString(), QString("UART"));
    QVERIFY(list->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains("Working files"));
    QVERIFY(!QFileInfo::exists(library + "/rtl/.xips.json"));
    QFile extra(library + "/fifo.xci");
    QVERIFY(extra.open(QIODevice::WriteOnly));
    extra.write("configuration");
    extra.close();
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(list->model()->rowCount(), 1);
    const QString screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        QDir().mkpath(screenshots);
        panel.grab().save(screenshots + "/xips-folder-scan.png");
        panel.resize(400, 700);
        QTest::qWait(40);
        panel.grab().save(screenshots + "/xips-folder-scan-narrow.png");
    }
    QVERIFY(source.remove());
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(list->model()->rowCount(), 1);
    QCOMPARE(versions->currentData().toString(), created.snapshot.id);
}
void GuiSmokeTest::scannedFilesSaveAndSelectHistory()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    QFile file(library + "/uart.sv");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("first source");
    file.close();
    CatalogDefinition definition;
    definition.name = "UART";
    definition.source = file.fileName();
    QVERIFY(SnapshotLibrary::create(library, definition).ok);
    BrowserPanel panel;
    panel.setContext(library, {});
    panel.show();
    auto *update = panel.findChild<ElaPushButton *>("updateButton");
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    QTRY_VERIFY(update->isEnabled());
    QTimer::singleShot(0, &panel, [&]
    {
        auto *accept = panel.findChild<ElaPushButton *>("formAccept");
        QVERIFY(accept);
        accept->click();
    });
    update->click();
    QTRY_COMPARE(versions->count(), 2);
    QTRY_VERIFY(update->isEnabled());
    QVERIFY(versions->currentData().toString() != "current");
    QVERIFY(versions->currentText().startsWith("rev1"));
    const auto first = versions->currentData().toString();
    const auto asset = SnapshotLibrary::scan(library).assets.first();
    QVERIFY(SnapshotLibrary::verifySnapshot(asset, first).ok);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("second source");
    file.close();
    QTimer::singleShot(0, &panel, [&]
    {
        auto *accept = panel.findChild<ElaPushButton *>("formAccept");
        QVERIFY(accept);
        accept->click();
    });
    update->click();
    QTRY_COMPARE(versions->count(), 3);
    QVERIFY(versions->currentText().startsWith("rev2"));
    versions->setCurrentIndex(versions->findData(first));
    QCOMPARE(panel.saveState().value("revision").toString(), first);
    QVERIFY(!QFileInfo::exists(library + "/.xips.json"));
}
void GuiSmokeTest::newIpSupportsFacetBrowsingAndProjectReferences()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), project = tmp.filePath("project");
    QVERIFY(QDir().mkpath(library));
    QVERIFY(QDir().mkpath(project));
    BrowserPanel panel;
    panel.resize(850, 650);
    panel.setContext(library, project);
    panel.show();
    auto *create = panel.findChild<ElaPushButton *>("newAssetButton");
    auto *list = panel.findChild<ElaListView *>("assetList");
    auto *indexes = panel.findChild<ElaComboBox *>("indexCombo");
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    auto *filters = panel.findChild<QToolButton *>("filterButton");
    auto *types = panel.findChild<ElaComboBox *>("typeCombo");
    QTRY_VERIFY(create->isEnabled());
    QVERIFY(!indexes->isVisible());
    QTimer::singleShot(0, &panel, [&]
    {
        panel.findChild<ElaLineEdit *>("newAssetName")->setText("uart_top");
        auto *type = panel.findChild<ElaComboBox *>("newAssetType");
        type->setCurrentIndex(type->findData("ip"));
        auto *fields = panel.findChild<QWidget *>("indexFields");
        QVERIFY(!fields->isVisible());
        const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
        {
            QDir().mkpath(screenshots);
            panel.findChild<QWidget *>("xipsForm")->grab().save(screenshots + "/new-ip-form.png");
            const auto mode = eTheme->getThemeMode();
            eTheme->setThemeMode(ElaThemeType::Dark);
            QCoreApplication::processEvents();
            panel.findChild<QWidget *>("xipsForm")->grab().save(screenshots + "/new-ip-form-dark.png");
            eTheme->setThemeMode(mode);
        }
        panel.findChild<QToolButton *>("indexFieldsToggle")->click();
        QVERIFY(fields->isVisible());
        panel.findChild<ElaLineEdit *>("index_category")->setText("Communication/UART, Diagnostics");
        panel.findChild<ElaLineEdit *>("index_tag")->setText("serial");
        panel.findChild<ElaLineEdit *>("index_interface")->setText("AXI, UART");
        panel.findChild<ElaLineEdit *>("index_purpose")->setText("telemetry");
        if (!screenshots.isEmpty())
        {
            QCoreApplication::processEvents();
            panel.findChild<QWidget *>("xipsForm")->grab().save(screenshots + "/new-ip-indexes.png");
        }
        panel.findChild<ElaPushButton *>("formAccept")->click();
    });
    create->click();
    QTRY_VERIFY(create->isEnabled());
    QCOMPARE(list->model()->rowCount(), 1);
    QVERIFY(QFileInfo::exists(library + "/uart_top/rtl/uart_top.sv"));
    filters->click();
    QVERIFY(indexes->isVisible());
    types->setCurrentIndex(types->findData("module"));
    QCOMPARE(list->model()->rowCount(), 0);
    types->setCurrentIndex(types->findData("ip"));
    QCOMPARE(list->model()->rowCount(), 1);
    const auto category = indexes->findData("category:Communication");
    QVERIFY(category >= 0);
    indexes->setCurrentIndex(category);
    QCOMPARE(list->model()->rowCount(), 1);
    indexes->setCurrentIndex(indexes->findData("category:Diagnostics"));
    QCOMPARE(list->model()->rowCount(), 1);
    QCOMPARE(filters->text(), QString("Filter · 2"));
    panel.findChild<QToolButton *>("clearFiltersButton")->click();
    QCOMPARE(types->currentIndex(), 0);
    QCOMPARE(indexes->currentIndex(), 0);
    QCOMPARE(filters->text(), QString("Filter"));
    filters->click();
    QVERIFY(!indexes->isVisible());
    search->setText("tag:serial interface:axi");
    QTRY_COMPARE(list->model()->rowCount(), 1);
    search->setText("interface:pcie");
    QTRY_COMPARE(list->model()->rowCount(), 0);
    search->clear();
    QTRY_COMPARE(list->model()->rowCount(), 1);
    QTimer::singleShot(0, &panel, [&]
    {
        QMenu *menu = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->objectName() == "xipsMoreMenu") menu = qobject_cast<QMenu *>(widget);
        QVERIFY(menu);
        auto *action = menu->findChild<QAction *>("referenceAction");
        QVERIFY(action && action->isEnabled());
        menu->setActiveAction(action);
        QTimer::singleShot(25, &panel, [&]
        {
            auto *target = panel.findChild<ElaLineEdit *>("referenceDestination");
            QVERIFY(target);
            QCOMPARE(target->text(), project);
            panel.findChild<ElaPushButton *>("formAccept")->click();
        });
        QTest::keyClick(menu, Qt::Key_Return);
    });
    panel.findChild<QToolButton *>("moreButton")->click();
    QTRY_VERIFY(create->isEnabled());
    const auto referenced = SnapshotLibrary::scan(project);
    QCOMPARE(referenced.assets.size(), 1);
    QVERIFY(!referenced.assets.first().referencePath.isEmpty());
    QVERIFY(!QFileInfo::exists(project + "/rtl"));
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        const auto previousMode = eTheme->getThemeMode();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            const auto suffix = mode == ElaThemeType::Light ? QString("light") : QString("dark");
            panel.grab().save(screenshots + "/ip-catalog-" + suffix + ".png");
            panel.resize(400, 700);
            QCoreApplication::processEvents();
            panel.grab().save(screenshots + "/ip-catalog-narrow-" + suffix + ".png");
            panel.resize(850, 650);
        }
        eTheme->setThemeMode(previousMode);
    }
}
void GuiSmokeTest::compactWindowKeepsActionsBesideContent()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "axi_lite_adapter";
    QVERIFY(SnapshotLibrary::create(library, definition).ok);
    MainWindow window(library);
    QCOMPARE(window.size(), QSize(720, 420));
    window.show();
    auto *panel = window.findChild<BrowserPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(!panel->isCatalogBusy());
    const auto top = panel->mapTo(&window, QPoint()).y();
    QVERIFY(top >= window.getAppBarHeight());
    QVERIFY2(top <= window.getAppBarHeight() + 2, "Title bar space must only be reserved once");
    auto *toolbar = panel->findChild<ElaToolBar *>("catalogToolbar");
    QVERIFY(toolbar);
    QVERIFY(toolbar->height() <= 40);
    for (const auto &name : {"assetSearch", "newAssetButton", "filterButton", "moreButton"})
    {
        auto *control = toolbar->findChild<QWidget *>(name);
        QVERIFY(control && control->isVisible());
        QVERIFY(toolbar->rect().contains(control->geometry()));
    }
    auto *versions = panel->findChild<ElaComboBox *>("versionCombo");
    for (const auto &name : {"takeButton", "updateButton"})
    {
        auto *button = panel->findChild<ElaPushButton *>(name);
        QVERIFY(button->isVisible());
        QCOMPARE(button->geometry().center().y(), versions->geometry().center().y());
        QVERIFY(button->mapTo(panel, QPoint()).y() < 120);
    }
    QVERIFY(panel->findChild<ElaListView *>("fileList")->height() <= 32);
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        QDir().mkpath(screenshots);
        const auto previousMode = eTheme->getThemeMode();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            window.grab().save(screenshots + (mode == ElaThemeType::Light
                ? "/compact-window-light.png" : "/compact-window-dark.png"));
        }
        eTheme->setThemeMode(previousMode);
    }
}
void GuiSmokeTest::elaFilePickerNavigatesAndSelects()
{
    QTemporaryDir tmp;
    const auto child = tmp.filePath("rtl");
    QVERIFY(QDir().mkpath(child));
    const QStringList sources{tmp.filePath("uart.sv"), tmp.filePath("fifo.sv")};
    for (const auto &source : sources)
    {
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("module sample; endmodule\n");
    }
    ElaFilePicker folder(nullptr, "Choose folder", tmp.path(), ElaFilePicker::Mode::Directory);
    folder.show();
    auto *entries = folder.findChild<ElaListView *>("pickerEntries");
    auto *model = qobject_cast<QFileSystemModel *>(entries->model());
    QVERIFY(model && model->isReadOnly());
    QTRY_COMPARE(model->rowCount(entries->rootIndex()), 1);
    entries->setCurrentIndex(model->index(child));
    QTest::keyClick(entries, Qt::Key_Return);
    QTRY_COMPARE(model->rootPath(), child);
    folder.findChild<QToolButton *>("pickerUp")->click();
    QCOMPARE(model->rootPath(), tmp.path());
    entries->setCurrentIndex(model->index(child));
    folder.findChild<ElaPushButton *>("pickerAccept")->click();
    QCOMPARE(folder.result(), int(QDialog::Accepted));
    QCOMPARE(folder.selectedPaths(), QStringList{child});

    ElaFilePicker files(nullptr, "Choose source files", tmp.path(), ElaFilePicker::Mode::Files);
    files.show();
    auto *path = files.findChild<ElaLineEdit *>("pickerPath");
    path->setText(tmp.filePath("missing.sv"));
    QTest::keyClick(path, Qt::Key_Return);
    QVERIFY(files.isVisible());
    QVERIFY(files.findChild<ElaText *>("pickerMessage")->isVisible());
    path->setText(tmp.path());
    QTest::keyClick(path, Qt::Key_Return);
    entries = files.findChild<ElaListView *>("pickerEntries");
    model = qobject_cast<QFileSystemModel *>(entries->model());
    QTRY_COMPARE(model->rowCount(entries->rootIndex()), 3);
    for (const auto &source : sources)
        entries->selectionModel()->select(model->index(source), QItemSelectionModel::Select);
    auto *accept = files.findChild<ElaPushButton *>("pickerAccept");
    QVERIFY(accept->isEnabled());
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        const auto previousMode = eTheme->getThemeMode();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            files.grab().save(screenshots + (mode == ElaThemeType::Light
                ? "/ela-file-picker-light.png" : "/ela-file-picker-dark.png"));
        }
        eTheme->setThemeMode(previousMode);
    }
    accept->click();
    QCOMPARE(files.result(), int(QDialog::Accepted));
    auto selected = files.selectedPaths(), expected = sources;
    selected.sort(); expected.sort();
    QCOMPARE(selected, expected);
    ElaFilePicker file(nullptr, "Choose source", tmp.path(), ElaFilePicker::Mode::File);
    file.show();
    path = file.findChild<ElaLineEdit *>("pickerPath");
    path->setText(sources.first());
    QTest::keyClick(path, Qt::Key_Return);
    QCOMPARE(file.result(), int(QDialog::Accepted));
    QCOMPARE(file.selectedPaths(), QStringList{sources.first()});
    for (const auto &source : sources)
    {
        QFile original(source);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("module sample; endmodule\n"));
    }
}
QTEST_MAIN(GuiSmokeTest)
#include "tst_gui_smoke.moc"
