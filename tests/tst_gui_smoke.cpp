#include "ElaComboBox.h"
#include "DialogDriver.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaTreeView.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "ElaTheme.h"
#include "ElaText.h"
#include "ElaToolBar.h"
#include "app/BrowserPanel.h"
#include "app/CatalogModel.h"
#include "app/ElaFilePicker.h"
#include "app/MainWindow.h"
#include <QContextMenuEvent>
#include <QAbstractItemModelTester>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QMenu>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QMimeData>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>
#include <memory>
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
    void groupsCanBeCreatedAndOrganized();
    void draggingAddsMembershipWithoutMovingSources();
    void keyboardActionsStayWithinTheCatalog();
    void reviewCanBeRejectedAndStandaloneReceiptSaved();
    void damagedHistoryAndUnavailableReferencesRemainVisible();
    void parallelReviewMakesAdoptionExplicit();
};
void GuiSmokeTest::parallelReviewMakesAdoptionExplicit()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "counter";
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY(created.ok);
    QFile manifest(created.asset.historyRoot + "/.xips/revisions/" + created.snapshot.id + ".json");
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    auto branch = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    const auto branchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    branch.insert("id", branchId);
    ContentStore::publishJson(created.asset.historyRoot + "/.xips/revisions/" + branchId + ".json", branch);
    BrowserPanel panel;
    panel.show();
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    bool reviewed = false;
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
        QCOMPARE(accept->text(), QString("Adopt reviewed files"));
        QVERIFY(accept->width() >= accept->fontMetrics().horizontalAdvance(accept->text()) + 20);
        auto *preview = form->findChild<QPlainTextEdit *>("payloadPreview");
        QVERIFY(preview->toPlainText().contains("rtl/counter.sv"));
        QVERIFY(preview->toPlainText().contains("2 parallel revision heads"));
        const QRect previewRect(preview->mapTo(form, QPoint()), preview->size());
        for (const auto *label : form->findChildren<ElaText *>())
            if (label->isVisible()) QVERIFY(!previewRect.intersects(QRect(label->mapTo(form, QPoint()), label->size())));
        const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) form->grab().save(screenshots + "/parallel-save-review.png");
        reviewed = true;
        accept->click();
    });
    panel.findChild<ElaPushButton *>("updateButton")->click();
    QTRY_VERIFY(reviewed && !panel.isCatalogBusy());
    auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(SnapshotLibrary::heads(catalog.assets.first()).size(), 1);
    const auto selected = panel.saveState().value("revision").toString();
    const auto saved = SnapshotLibrary::verifySnapshot(catalog.assets.first(), selected);
    QVERIFY(saved.ok);
    QCOMPARE(saved.snapshot.parents.size(), 2);
}
void GuiSmokeTest::reviewCanBeRejectedAndStandaloneReceiptSaved()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "axi_lite";
    definition.indexes.insert("interface", {"AXI4 Lite"});
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY(created.ok);
    BrowserPanel panel;
    panel.resize(850, 650);
    panel.show();
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    auto *list = panel.findChild<ElaTreeView *>("assetList");
    search->setText("interface:\"AXI4 Lite\"");
    QTest::qWait(160);
    QCOMPARE(list->model()->rowCount(), 1);
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    versions->setCurrentIndex(versions->findData(created.snapshot.id));
    auto *take = panel.findChild<ElaPushButton *>("takeButton");
    bool rejected = false;
    whenVisible(&panel, "xipsForm", [&](QWidget *form)
    {
        QVERIFY(form->findChild<QPlainTextEdit *>("payloadPreview"));
        qobject_cast<QDialog *>(form)->reject();
        rejected = true;
    });
    take->click();
    QTRY_VERIFY(rejected);
    QVERIFY(!panel.isCatalogBusy());
    const auto destination = tmp.filePath("copy.sv");
    whenVisible(&panel, "exportDestination", [&](QWidget *field)
    {
        auto *edit = qobject_cast<ElaLineEdit *>(field);
        auto *form = panel.findChild<QDialog *>("xipsForm");
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
        edit->setText(library);
        QVERIFY(!accept->isEnabled());
        edit->setText(destination);
        QVERIFY(accept->isEnabled());
        const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
        {
            QDir().mkpath(screenshots);
            form->grab().save(screenshots + "/export-review.png");
        }
        accept->click();
    });
    take->click();
    QTRY_VERIFY(QFileInfo::exists(destination));
    QTRY_VERIFY(!panel.isCatalogBusy());
    const auto receipt = destination + ".xips-use.json";
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        whenVisible(&panel, "receiptDestination", [&](QWidget *field)
        {
            QCOMPARE(qobject_cast<ElaLineEdit *>(field)->text(), receipt);
            panel.findChild<ElaPushButton *>("formAccept")->click();
        });
        QTimer::singleShot(0, &panel, [&]
        {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->objectName() == "xipsMoreMenu")
                {
                    auto *action = menu->findChild<QAction *>("saveReceiptAction");
                    QVERIFY(action && action->isVisible() && action->isEnabled());
                    menu->setActiveAction(action);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            QFAIL("Receipt menu missing");
        });
        panel.findChild<QToolButton *>("moreButton")->click();
        QTRY_VERIFY(!panel.isCatalogBusy());
    }
    QFile file(receipt);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto origin = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(origin.value("revision").toString(), created.snapshot.id);
    QCOMPARE(origin.value("contentHash").toString(), created.snapshot.hash);
    QVERIFY(origin.value("sourceImmutable").toBool());
}
void GuiSmokeTest::damagedHistoryAndUnavailableReferencesRemainVisible()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("owner"), receiver = tmp.filePath("receiver");
    QVERIFY(QDir().mkpath(library) && QDir().mkpath(receiver));
    CatalogDefinition definition;
    definition.name = "counter";
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY(created.ok);
    QVERIFY(SnapshotLibrary::addReference(created.asset, created.snapshot.id, receiver).ok);
    QFile damaged(created.asset.historyRoot + "/.xips/revisions/damaged.json");
    QVERIFY(damaged.open(QIODevice::WriteOnly));
    damaged.write("{");
    damaged.close();
    BrowserPanel panel;
    panel.show();
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    QVERIFY(versions->findData(created.snapshot.id) >= 0);
    QVERIFY(!panel.findChild<ElaPushButton *>("updateButton")->isEnabled());
    versions->setCurrentIndex(versions->findData(created.snapshot.id));
    QVERIFY(panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    QVERIFY(QFile::remove(library + "/counter/rtl/counter.sv"));
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->count(), 1);
    QVERIFY(panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    QVERIFY(QDir().rename(library, tmp.filePath("relocated")));
    panel.setContext(receiver, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(panel.findChild<ElaTreeView *>("assetList")->model()->rowCount(), 1);
    QCOMPARE(versions->count(), 0);
    QVERIFY(!panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    bool actionsChecked = false;
    QTimer::singleShot(0, &panel, [&]
    {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->objectName() == "xipsMoreMenu")
            {
                for (const auto *name : {"removeReferenceAction", "changeReferenceAction"})
                {
                    const auto *action = menu->findChild<QAction *>(name);
                    QVERIFY(action && action->isEnabled() && action->isVisible());
                }
                menu->close();
                actionsChecked = true;
                return;
            }
    });
    panel.findChild<QToolButton *>("moreButton")->click();
    QVERIFY(actionsChecked);
}
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
    auto *list = panel.findChild<ElaTreeView *>("assetList");
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    auto *take = panel.findChild<ElaPushButton *>("takeButton");
    QVERIFY(list);
    QVERIFY(versions);
    QVERIFY(search);
    QVERIFY(take);
    QTRY_VERIFY(!panel.isCatalogBusy());
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
    QTRY_VERIFY(!panel.isCatalogBusy());
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
    auto *list = panel.findChild<ElaTreeView *>("assetList");
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
    QCOMPARE(update->text(), QString("Save revision…"));
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
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
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
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
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
    auto *list = panel.findChild<ElaTreeView *>("assetList");
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
void GuiSmokeTest::groupsCanBeCreatedAndOrganized()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "axi_lite_adapter";
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    BrowserPanel panel;
    panel.resize(720, 388);
    panel.show();
    panel.setContext(library, {});
    auto *tree = panel.findChild<ElaTreeView *>("assetList");
    auto *model = static_cast<CatalogModel *>(tree->model());
    QAbstractItemModelTester modelTester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    auto *addGroup = panel.findChild<QToolButton *>("newGroupButton");
    QTRY_VERIFY(!panel.isCatalogBusy());
    const auto newGroup = [&](const QString &name)
    {
        QTimer::singleShot(0, &panel, [&, name]
        {
            auto *field = panel.findChild<ElaLineEdit *>("groupName");
            QVERIFY(field);
            field->setText(name);
            panel.findChild<ElaPushButton *>("formAccept")->click();
        });
        addGroup->click();
    };
    const auto chooseAction = [&](const QModelIndex &index, const QString &name)
    {
        QTimer::singleShot(0, &panel, [&, name]
        {
            ElaMenu *menu = nullptr;
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "groupMenu") menu = qobject_cast<ElaMenu *>(widget);
            QVERIFY(menu);
            auto *action = menu->findChild<QAction *>(name);
            if (!action) { menu->close(); QFAIL("Missing group action"); }
            auto *owner = qobject_cast<QMenu *>(action->parent());
            if (owner != menu)
            {
                menu->setActiveAction(owner->menuAction());
                QTest::keyClick(menu, Qt::Key_Right);
            }
            owner->setActiveAction(action);
            QTest::keyClick(owner, Qt::Key_Return);
        });
        tree->scrollTo(index);
        const auto point = tree->visualRect(index).center();
        QMetaObject::invokeMethod(tree, "customContextMenuRequested", Q_ARG(QPoint, point));
    };
    newGroup("Bus");
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->groups().size(), 1);
    const auto bus = model->groups().first().id;
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->rowCount(model->indexForId({}, bus)), 0);
    QCOMPARE(panel.saveState().value("groupId").toString(), bus);
    QVERIFY(!panel.findChild<ElaPushButton *>("takeButton")->isVisible());
    QTimer::singleShot(0, &panel, [&]
    {
        panel.findChild<ElaLineEdit *>("newAssetName")->setText("fifo");
        panel.findChild<ElaPushButton *>("formAccept")->click();
    });
    panel.findChild<ElaPushButton *>("newAssetButton")->click();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->rowCount(model->indexForId({}, bus)), 1);
    QVERIFY(QFileInfo::exists(library + "/fifo/rtl/fifo.sv"));
    chooseAction(model->indexForId(created.asset.id), "addToGroup_" + bus);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->rowCount(model->indexForId({}, bus)), 2);
    QVERIFY(tree->currentIndex().parent().isValid());
    newGroup("Empty");
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->groups().size(), 2);
    const auto empty = model->groups().last().id;
    chooseAction(model->indexForId(created.asset.id, bus), "addToGroup_" + empty);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->rowCount(model->indexForId({}, bus)), 2);
    QCOMPARE(model->rowCount(model->indexForId({}, empty)), 1);
    chooseAction(model->indexForId(created.asset.id, empty), "removeFromGroupAction");
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->rowCount(model->indexForId({}, empty)), 0);
    QCOMPARE(model->rowCount(model->indexForId({}, bus)), 2);
    auto *search = panel.findChild<ElaLineEdit *>("assetSearch");
    search->setText("axi");
    QTRY_COMPARE(model->rowCount(model->indexForId({}, bus)), 1);
    QCOMPARE(model->rowCount(), 2);
    search->clear();
    QTRY_COMPARE(model->rowCount(model->indexForId({}, bus)), 2);
    tree->setCurrentIndex(model->indexForId(created.asset.id, bus));
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
    {
        QDir().mkpath(screenshots);
        const auto previous = eTheme->getThemeMode();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            panel.grab().save(screenshots + (mode == ElaThemeType::Light
                ? "/groups-light.png" : "/groups-dark.png"));
        }
        eTheme->setThemeMode(previous);
    }
    tree->collapse(model->indexForId({}, bus));
    const auto state = panel.saveState();
    QVERIFY(state.value("collapsedGroups").toStringList().contains(bus));
    BrowserPanel reopened;
    reopened.setContext(library, {});
    QTRY_VERIFY(!reopened.isCatalogBusy());
    reopened.restoreState(state);
    auto *restoredTree = reopened.findChild<ElaTreeView *>("assetList");
    auto *restoredModel = static_cast<CatalogModel *>(restoredTree->model());
    QCOMPARE(restoredModel->groups().size(), 2);
    QCOMPARE(restoredModel->rowCount(restoredModel->indexForId({}, bus)), 2);
    QCOMPARE(restoredModel->rowCount(restoredModel->indexForId({}, empty)), 0);
    QVERIFY(!restoredTree->isExpanded(restoredModel->indexForId({}, bus)));
    chooseAction(model->indexForId({}, bus), "deleteGroupAction");
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->groups().size(), 1);
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(model->groups().first().id, empty);
    const auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.size(), 2);
    QCOMPARE(catalog.groups.size(), 1);
    QVERIFY(catalog.groups.first().members.isEmpty());
    QVERIFY(QFileInfo::exists(library + "/axi_lite_adapter/rtl/axi_lite_adapter.sv"));
    QVERIFY(SnapshotLibrary::verifySnapshot(created.asset, created.snapshot.id).ok);
}
void GuiSmokeTest::draggingAddsMembershipWithoutMovingSources()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "uart";
    const auto asset = SnapshotLibrary::create(library, definition);
    QVERIFY(asset.ok);
    const auto bus = CatalogGroups::create(library, "Bus");
    const auto serial = CatalogGroups::create(library, "Serial");
    QVERIFY(bus.ok && serial.ok);
    QVERIFY(CatalogGroups::setMember(library, bus.group.id, asset.asset.id, true).ok);
    QFile source(library + "/uart/rtl/uart.sv");
    QVERIFY(source.open(QIODevice::ReadOnly));
    const auto original = source.readAll();
    source.close();
    BrowserPanel panel;
    panel.resize(720, 388);
    panel.show();
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *tree = panel.findChild<ElaTreeView *>("assetList");
    auto *model = static_cast<CatalogModel *>(tree->model());
    QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const auto item = model->indexForId(asset.asset.id, bus.group.id);
    const auto target = model->indexForId({}, serial.group.id);
    std::unique_ptr<QMimeData> payload(model->mimeData({item}));
    QVERIFY(model->flags(item).testFlag(Qt::ItemIsDragEnabled));
    QVERIFY(model->flags(target).testFlag(Qt::ItemIsDropEnabled));
    QVERIFY(model->canDropMimeData(payload.get(), Qt::CopyAction, -1, -1, target));
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::MoveAction, -1, -1, target));
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::CopyAction, -1, -1, item));
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::CopyAction, -1, -1, {}));
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::CopyAction, -1, -1, model->indexForId({}, bus.group.id)));
    QMimeData invalid;
    invalid.setData(model->mimeTypes().first(), QJsonDocument(QJsonObject{
        {"catalog", tmp.filePath("another-library")}, {"assetId", asset.asset.id}}).toJson());
    QVERIFY(!model->dropMimeData(&invalid, Qt::CopyAction, -1, -1, target));
    tree->scrollTo(target);
    ElaTreeView::finishExpansion(tree);
    QCoreApplication::processEvents();
    const auto position = tree->visualRect(target).center();
    QDragEnterEvent enter(position, Qt::CopyAction, payload.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(position, Qt::CopyAction, payload.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop(position, Qt::CopyAction, payload.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->rowCount(model->indexForId({}, bus.group.id)), 1);
    QCOMPARE(model->rowCount(model->indexForId({}, serial.group.id)), 1);
    QCOMPARE(panel.saveState().value("groupId").toString(), serial.group.id);
    QCOMPARE(panel.saveState().value("assetId").toString(), asset.asset.id);
    QVERIFY(!model->canDropMimeData(payload.get(), Qt::CopyAction, -1, -1, model->indexForId({}, serial.group.id)));
    const auto saved = CatalogGroups::scan(library);
    QCOMPARE(saved.groups.size(), 2);
    for (const auto &group : saved.groups) QCOMPARE(group.members, QStringList{asset.asset.id});
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), original);
    QVERIFY(SnapshotLibrary::verifySnapshot(asset.asset, asset.snapshot.id).ok);
}
void GuiSmokeTest::keyboardActionsStayWithinTheCatalog()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    QWidget host;
    auto *layout = new QVBoxLayout(&host);
    auto *outside = new ElaLineEdit(&host);
    layout->addWidget(outside);
    auto *panel = new BrowserPanel(&host);
    layout->addWidget(panel);
    host.resize(720, 460);
    host.show();
    host.activateWindow();
    panel->setContext(library, {});
    QTRY_VERIFY(!panel->isCatalogBusy());
    auto *tree = panel->findChild<ElaTreeView *>("assetList");
    auto *model = static_cast<CatalogModel *>(tree->model());
    auto *search = panel->findChild<ElaLineEdit *>("assetSearch");
    const auto focusTree = [&]
    {
        host.activateWindow();
        tree->setFocus();
        QCoreApplication::processEvents();
        QTRY_VERIFY(host.isActiveWindow() && tree->hasFocus());
    };
    outside->setFocus();
    QTRY_VERIFY(outside->hasFocus());
    QTest::keyClick(outside, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(outside->hasFocus());
    focusTree();
    QTest::keyClick(tree, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(search->hasFocus());
    search->setText("uart");
    QTest::keyClick(search, Qt::Key_Escape);
    QCOMPARE(search->text(), QString());
    const auto fillForm = [&](const QString &field, const QString &text)
    {
        whenVisible(&host, field, [&, text](QWidget *widget)
        {
            auto *edit = qobject_cast<ElaLineEdit *>(widget);
            QVERIFY(edit);
            edit->setText(text);
            host.findChild<ElaPushButton *>("formAccept")->click();
        });
    };
    fillForm("groupName", "Work");
    QTest::keyClick(search, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(model->groups().size(), 1);
    const auto group = model->groups().first().id;
    focusTree();
    fillForm("groupName", "Data");
    QTest::keyClick(tree, Qt::Key_F2);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(model->groups().first().name, QString("Data"));
    focusTree();
    fillForm("newAssetName", "fifo");
    QTest::keyClick(tree, Qt::Key_N, Qt::ControlModifier);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(model->rowCount(model->indexForId({}, group)), 1);
    auto *versions = panel->findChild<ElaComboBox *>("versionCombo");
    QCOMPARE(versions->count(), 2);
    QFile source(library + "/fifo/rtl/fifo.sv");
    QVERIFY(source.open(QIODevice::Append));
    source.write("\n// test revision\n");
    source.close();
    focusTree();
    fillForm("revisionNote", "Keyboard save");
    QTest::keyClick(tree, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(versions->count(), 3);
    QVERIFY(versions->currentText().contains("Keyboard save"));
    QVERIFY(CatalogGroups::create(library, "Added elsewhere").ok);
    focusTree();
    QTest::keyClick(tree, Qt::Key_F5);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(model->groups().size(), 2);
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
