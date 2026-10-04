#include "CatalogFixture.h"
#include "RevisionDriver.h"
#include "ElaComboBox.h"
#include "DialogDriver.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaTreeView.h"
#include "ElaTabWidget.h"
#include "ElaCheckBox.h"
#include "app/WorkingFilesModel.h"
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
#include <QDesktopServices>
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
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStandardPaths>
#include <QScrollBar>
#include "library/CatalogIndex.h"
#include "app/CatalogWatcher.h"
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>
#include <memory>
using namespace xips;
class FileOpenCapture : public QObject
{
    Q_OBJECT
  public:
    QList<QUrl> urls;
    FileOpenCapture() { QDesktopServices::setUrlHandler("file", this, "open"); }
    ~FileOpenCapture() override { QDesktopServices::unsetUrlHandler("file"); }
  public slots:
    void open(const QUrl &url) { urls.append(url); }
};
class GuiSmokeTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
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
    void doubleClickAndEnterOpenTheSelectedVersion();
    void emptyIpImportCheckAndCreateVersions();
    void workingCheckboxStatesStayVisible();
    void archivedVersionsCanBeDeleted();
    void externalDropsReachEveryWorkingArea();
    void localUpdatesPreserveViewsAndReviewChanges();
    void externalChangesRefreshAfterSettling();
    void refreshPreservesArchivedFileBrowsing();
};
#include "EmptyWorkspaceGui.inc"
#include "VersionAndDropGui.inc"
#include "LocalUpdatesGui.inc"
#include "AutomaticRefreshGui.inc"
#include "ArchivedViewGui.inc"
void GuiSmokeTest::workingCheckboxStatesStayVisible()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "checkbox_states";
    const auto created = SnapshotLibrary::create(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    for (const auto &relative : QStringList{"notes.txt", "rtl/sub/helper.sv", "rtl/top.sv", "rtl/unused.sv"})
    {
        const auto path = created.asset.root + '/' + relative;
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
    }
    BrowserPanel panel;
    panel.resize(720, 480); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *view = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(view->model());
    class Probe : public QStyledItemDelegate
    {
      public:
        QRect indicator(QTreeView *tree, const QModelIndex &index) const
        {
            QStyleOptionViewItem option;
            option.initFrom(tree); initStyleOption(&option, index);
            option.widget = tree; option.rect = tree->visualRect(index);
            return tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, tree);
        }
    } probe;
    const auto checked = model->fileIndex("notes.txt");
    const auto helper = model->fileIndex("rtl/sub/helper.sv");
    const auto partial = helper.parent().parent();
    const auto unchecked = model->fileIndex("rtl/top.sv");
    const auto previous = eTheme->getThemeMode();
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    bool visibleStates = true;
    qInfo() << "Checkbox rendering:" << QGuiApplication::platformName() << "DPR" << panel.devicePixelRatioF();
    for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
    {
        eTheme->setThemeMode(mode);
        model->checkAll(false);
        view->scrollToTop(); QTest::qWait(250);
        const auto clickPoint = probe.indicator(view, checked).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);
        QCOMPARE(checked.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        view->setCurrentIndex(helper); QTest::keyClick(view, Qt::Key_Space);
        QCOMPARE(helper.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QCOMPARE(partial.data(Qt::CheckStateRole).toInt(), int(Qt::PartiallyChecked));
        QCOMPARE(unchecked.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
        view->clearSelection(); view->setCurrentIndex({});
        QTest::mouseMove(&panel, QPoint(5, 5));
        view->scrollToTop(); QTest::qWait(1200);
        const auto suffix = mode == ElaThemeType::Light ? QString("light") : QString("dark");
        if (!screenshots.isEmpty())
        {
            QDir().mkpath(screenshots);
            panel.grab().save(screenshots + "/checkbox-stable-" + suffix + ".png");
        }
        for (const auto &index : {checked, helper, helper.parent(), partial, unchecked})
        {
            auto rect = probe.indicator(view, index);
            QVERIFY(rect.isValid() && view->viewport()->rect().contains(rect));
            if (index.data(Qt::CheckStateRole).toInt() != Qt::Unchecked) rect.adjust(3, 3, -3, -3);
            const auto pixels = view->viewport()->grab(rect).toImage().convertToFormat(QImage::Format_RGB32);
            int minR = 255, minG = 255, minB = 255, maxR = 0, maxG = 0, maxB = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x)
                {
                    const auto color = pixels.pixelColor(x, y);
                    minR = qMin(minR, color.red()); maxR = qMax(maxR, color.red());
                    minG = qMin(minG, color.green()); maxG = qMax(maxG, color.green());
                    minB = qMin(minB, color.blue()); maxB = qMax(maxB, color.blue());
                }
            const int contrast = qMax(maxR - minR, qMax(maxG - minG, maxB - minB));
            qInfo() << suffix << index.data(Qt::UserRole).toString() << "state"
                    << index.data(Qt::CheckStateRole).toInt() << "indicator" << rect << "contrast" << contrast;
            visibleStates &= contrast > 60;
        }
        // Repeated real clicks also clear the check, independently of row selection.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);
        QCOMPARE(checked.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    }
    eTheme->setThemeMode(previous);
    QVERIFY2(visibleStates, "Checkbox border/check/dash must remain visible after native painting has settled");
}
void GuiSmokeTest::doubleClickAndEnterOpenTheSelectedVersion()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library with spaces");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "counter";
    const auto created = savedCatalogFixture(library, definition);
    QVERIFY(created.ok);
    const auto original = library + "/counter/rtl/counter.sv";
    QFile source(original);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("module changed; endmodule\n");
    source.close();
    FileOpenCapture opened;
    BrowserPanel panel;
    panel.resize(720, 420);
    panel.show();
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *files = panel.findChild<ElaListView *>("fileList");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *archive = panel.findChild<QToolButton *>("updateButton");
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *workingModel = qobject_cast<WorkingFilesModel *>(working->model());
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    QAbstractItemModelTester tableCheck(versions->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    QCOMPARE(versions->model()->headerData(1, Qt::Horizontal).toString(), QString("Status"));
    QCOMPARE(versions->model()->rowCount(), 1);
    QCOMPARE(versions->model()->index(0, 1).data().toString(), QString("Archived"));
    QVERIFY(versions->model()->index(0, 1).data(Qt::AccessibleTextRole).toString().contains("read-only"));
    QVERIFY(!archive->isEnabled());
    panel.findChild<ElaCheckBox *>("checkAllFiles")->click();
    QVERIFY(archive->isEnabled());
    for (int row = 0; row < 1; ++row)
    {
        QVERIFY(!(versions->model()->flags(versions->model()->index(row, 1)) & Qt::ItemIsEditable));
    }
    QCOMPARE(pages->currentIndex(), 0);
    const auto workingIndex = workingModel->fileIndex("rtl/counter.sv");
    const auto position = working->visualRect(workingIndex).center();
    QTest::mouseClick(working->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    QTest::mouseDClick(working->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    QTRY_COMPARE(opened.urls.size(), 1);
    QCOMPARE(QFileInfo(opened.urls.last().toLocalFile()).absoluteFilePath(), QFileInfo(original).absoluteFilePath());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArray("module changed; endmodule\n"));
    source.close();
    versions->setCurrentIndex(revisionIndex(versions, created.snapshot.id));
    pages->setCurrentIndex(1);
    QVERIFY(archive->isEnabled());
    QCOMPARE(panel.findChild<QToolButton *>("openFileButton")->text(), QString("Open read-only"));
    panel.activateWindow();
    files->setFocus();
    QTRY_VERIFY(panel.isActiveWindow() && files->hasFocus());
    QTest::keyClick(files, Qt::Key_Return);
    QTRY_VERIFY2(opened.urls.size() == 2, qPrintable(panel.findChild<ElaText *>("browserNotice")->toolTip()));
    const auto copy = opened.urls.last().toLocalFile();
    QVERIFY(copy != original);
    QVERIFY(!QFileInfo(copy).permissions().testFlag(QFile::WriteOwner));
    QFile saved(copy);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QVERIFY(saved.readAll().contains("module counter"));
    saved.close();
    panel.findChild<QToolButton *>("openFileButton")->click();
    QTRY_COMPARE(opened.urls.size(), 3);
    QCOMPARE(opened.urls.last().toLocalFile(), copy);
    panel.findChild<QToolButton *>("openSourceButton")->click();
    QCOMPARE(opened.urls.size(), 4);
    QCOMPARE(QFileInfo(opened.urls.last().toLocalFile()).absoluteFilePath(), QFileInfo(library + "/counter").absoluteFilePath());
    pages->setCurrentIndex(0);
    QVERIFY(archive->isEnabled());
    QCOMPARE(panel.findChild<QToolButton *>("openWorkingButton")->text(), QString("Edit file"));
    QVERIFY(QFile::remove(original));
    panel.findChild<QToolButton *>("openWorkingButton")->click();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(opened.urls.size(), 4);
    QVERIFY(panel.findChild<ElaText *>("browserNotice")->property("error").toBool());
}
void GuiSmokeTest::parallelReviewMakesAdoptionExplicit()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition;
    definition.name = "counter";
    const auto created = savedCatalogFixture(library, definition);
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
    panel.findChild<ElaCheckBox *>("checkAllFiles")->click();
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
        QTimer::singleShot(0, accept, &QPushButton::click);
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
    panel.findChild<QToolButton *>("updateButton")->click();
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
    const auto created = savedCatalogFixture(library, definition);
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
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    versions->setCurrentIndex(revisionIndex(versions, created.snapshot.id));
    panel.findChild<ElaTabWidget *>("assetPages")->setCurrentIndex(1);
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
        auto *receiptButton = panel.findChild<QToolButton *>("saveReceiptButton");
        QVERIFY(receiptButton && receiptButton->isVisible() && receiptButton->isEnabled());
        receiptButton->click();
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
    const auto created = savedCatalogFixture(library, definition);
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
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    QVERIFY(revisionIndex(versions, created.snapshot.id).row() >= 0);
    panel.findChild<ElaTabWidget *>("assetPages")->setCurrentIndex(1);
    QVERIFY(!panel.findChild<QToolButton *>("updateButton")->isEnabled());
    versions->setCurrentIndex(revisionIndex(versions, created.snapshot.id));
    QVERIFY(panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    QVERIFY(QFile::remove(library + "/counter/rtl/counter.sv"));
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 1);
    QVERIFY(panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    QVERIFY(QDir().rename(library, tmp.filePath("relocated")));
    panel.setContext(receiver, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(panel.findChild<ElaTreeView *>("assetList")->model()->rowCount(), 1);
    QCOMPARE(versions->model()->rowCount(), 0);
    QVERIFY(!panel.findChild<ElaPushButton *>("takeButton")->isEnabled());
    for (const auto *name : {"removeSourceButton", "changeReferenceButton"})
    {
        const auto *button = panel.findChild<QToolButton *>(name);
        QVERIFY(button && button->isEnabled() && button->isVisible());
    }
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
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
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
    QTRY_COMPARE(versions->model()->rowCount(), 2);
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), updated.snapshot.id);
    QVERIFY(take->isEnabled());
    versions->selectRow(1);
    QCOMPARE(panel.saveState().value("revision").toString(), asset.snapshot.id);
    search->setText("missing");
    QTRY_COMPARE(list->model()->rowCount(), 0);
    QVERIFY(!take->isEnabled());
    search->setText("uart.sv");
    QTRY_COMPARE(list->model()->rowCount(), 1);
    QTRY_COMPARE(versions->model()->rowCount(), 2);
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
    auto *folder = panel.findChild<QToolButton *>("folderButton");
    auto *list = panel.findChild<ElaTreeView *>("assetList");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *update = panel.findChild<QToolButton *>("updateButton");
    QVERIFY(folder);
    QVERIFY(folder->isVisible());
    panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(folder->isVisible());
    QCOMPARE(list->model()->rowCount(), 0);
    CatalogDefinition definition;
    definition.name = "UART";
    definition.source = source.fileName();
    const auto created = savedCatalogFixture(library, definition);
    QVERIFY2(created.ok, qPrintable(created.error));
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(list->model()->rowCount(), 1);
    QCOMPARE(versions->model()->rowCount(), 1);
    QCOMPARE(panel.findChild<ElaTabWidget *>("assetPages")->currentIndex(), 0);
    QCOMPARE(update->text(), QString("Create version"));
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
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), created.snapshot.id);
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
    auto *update = panel.findChild<QToolButton *>("updateButton");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    QTRY_VERIFY(!panel.isCatalogBusy());
    panel.findChild<ElaCheckBox *>("checkAllFiles")->click();
    QTRY_VERIFY(update->isEnabled());
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
        QVERIFY(accept);
        accept->click();
    });
    update->click();
    QTRY_COMPARE(versions->model()->rowCount(), 1);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(update->isEnabled());
    QVERIFY(versions->currentIndex().data(Qt::UserRole).toString() != "current");
    QVERIFY(versions->currentIndex().siblingAtColumn(0).data().toString().startsWith("rev1"));
    const auto first = versions->currentIndex().data(Qt::UserRole).toString();
    const auto asset = SnapshotLibrary::scan(library).assets.first();
    QVERIFY(SnapshotLibrary::verifySnapshot(asset, first).ok);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("second source");
    file.close();
    QVERIFY(update->isEnabled());
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        auto *accept = form->findChild<ElaPushButton *>("formAccept");
        QVERIFY(accept);
        accept->click();
    });
    update->click();
    QTRY_COMPARE(versions->model()->rowCount(), 2);
    QVERIFY(versions->currentIndex().siblingAtColumn(0).data().toString().startsWith("rev2"));
    versions->setCurrentIndex(revisionIndex(versions, first));
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
    QVERIFY(QFileInfo(library + "/uart_top").isDir());
    QVERIFY(!QFileInfo::exists(library + "/uart_top/rtl/uart_top.sv"));
    QFile fixture(library + "/uart_top/source.sv");
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("module source; endmodule\n"); fixture.close();
    QVERIFY(SnapshotLibrary::saveCurrent(SnapshotLibrary::scan(library).assets.first()).ok);
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
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
    QCOMPARE(filters->accessibleName(), QString("Filter · 2 active"));
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
    whenVisible(&panel, "referenceDestination", [&](QWidget *field)
    {
        QCOMPARE(qobject_cast<ElaLineEdit *>(field)->text(), project);
        panel.findChild<ElaPushButton *>("formAccept")->click();
    });
    panel.findChild<QToolButton *>("referenceButton")->click();
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
    definition.category = "ip";
    const auto created = savedCatalogFixture(library, definition);
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
    QVERIFY(QFileInfo(library + "/fifo").isDir());
    QVERIFY(!QFileInfo::exists(library + "/fifo/rtl/fifo.sv"));
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
    auto *members = panel.findChild<ElaListView *>("groupMembers");
    QVERIFY(members);
    const auto selectGroup = [&]
    {
        const auto index = model->indexForId({}, bus);
        tree->scrollTo(index);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(index).center());
    };
    tree->collapse(model->indexForId({}, bus));
    selectGroup();
    QTRY_VERIFY(members->isVisible());
    // Short groups keep their first member near the heading instead of spreading
    // the heading/actions through the unused vertical space.
    QTRY_VERIFY(members->y() < 100);
    QVERIFY(!tree->isExpanded(model->indexForId({}, bus)));
    QCOMPARE(members->rootIndex(), model->indexForId({}, bus));
    QCOMPARE(members->model()->rowCount(members->rootIndex()), 2);
    QStringList memberNames;
    for (int row = 0; row < 2; ++row)
        memberNames.append(members->model()->index(row, 0, members->rootIndex()).data().toString());
    memberNames.sort();
    QCOMPARE(memberNames, QStringList({"axi_lite_adapter", "fifo"}));
    QVERIFY(!panel.findChild<ElaTableView *>("revisionTable")->isVisible());
    search->setText("axi");
    QTRY_COMPARE(members->model()->rowCount(members->rootIndex()), 1);
    QCOMPARE(members->model()->index(0, 0, members->rootIndex()).data().toString(), QString("axi_lite_adapter"));
    search->setText("no_group_members_match");
    QTRY_COMPARE(model->rowCount(model->indexForId({}, bus)), 0);
    QVERIFY(!members->isVisible());
    search->clear();
    QTRY_VERIFY(members->isVisible());
    QCOMPARE(members->model()->rowCount(members->rootIndex()), 2);
    const auto groupScreenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!groupScreenshots.isEmpty())
    {
        QDir().mkpath(groupScreenshots);
        const auto previous = eTheme->getThemeMode();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            panel.grab().save(groupScreenshots + (mode == ElaThemeType::Light
                ? "/group-members-light.png" : "/group-members-dark.png"));
        }
        eTheme->setThemeMode(previous);
    }
    QTest::mouseClick(members->viewport(), Qt::LeftButton, Qt::NoModifier,
        members->visualRect(model->indexForId(created.asset.id, bus)).center());
    QTRY_COMPARE(panel.saveState().value("assetId").toString(), created.asset.id);
    QVERIFY(tree->isExpanded(model->indexForId({}, bus)));
    QVERIFY(!members->isVisible());
    QVERIFY(panel.findChild<ElaTreeView *>("workingFiles")->isVisible());
    selectGroup();
    QTRY_VERIFY(members->isVisible());
    const auto secondMember = model->index(1, 0, members->rootIndex());
    const auto secondName = secondMember.data().toString();
    members->setCurrentIndex(secondMember);
    members->setFocus();
    QTest::keyClick(members, Qt::Key_Return);
    QTRY_COMPARE(tree->currentIndex(), secondMember);
    QCOMPARE(panel.findChild<ElaText *>("assetName")->text(), secondName);
    QVERIFY(!members->isVisible());
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
    const auto asset = savedCatalogFixture(library, definition);
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
    auto *versions = panel->findChild<ElaTableView *>("revisionTable");
    QCOMPARE(versions->model()->rowCount(), 0);
    QFile source(library + "/fifo/fifo.sv");
    QVERIFY(source.open(QIODevice::Append));
    source.write("\n// test revision\n");
    source.close();
    panel->refresh(); QTRY_VERIFY(!panel->isCatalogBusy());
    panel->findChild<ElaCheckBox *>("checkAllFiles")->click();
    focusTree();
    fillForm("revisionNote", "Keyboard save");
    QTest::keyClick(tree, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY(!panel->isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 1);
    QVERIFY(versions->currentIndex().data(Qt::ToolTipRole).toString().contains("Keyboard save"));
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
    const auto created = savedCatalogFixture(library, definition);
    QVERIFY(created.ok);
    QFile source(library + "/axi_lite_adapter/rtl/axi_lite_adapter.sv");
    QVERIFY(source.open(QIODevice::Append));
    source.write("\n// second archived revision\n");
    source.close();
    QVERIFY(SnapshotLibrary::saveCurrent(created.asset, "Second revision").ok);
    const auto group = CatalogGroups::create(library, "AXI");
    QVERIFY(group.ok);
    QVERIFY(CatalogGroups::setMember(library, group.group.id, created.asset.id, true).ok);
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
    QVERIFY(toolbar->height() <= 48);
    for (const auto &name : {"assetSearch", "newAssetButton", "filterButton", "collectButton"})
    {
        auto *control = toolbar->findChild<QWidget *>(name);
        QVERIFY(control && control->isVisible());
        QVERIFY(toolbar->rect().contains(control->geometry()));
    }
    auto *versions = panel->findChild<ElaTableView *>("revisionTable");
    panel->findChild<ElaTabWidget *>("assetPages")->setCurrentIndex(1);
    QCOMPARE(versions->model()->columnCount(), 2);
    QCOMPARE(versions->model()->rowCount(), 2);
    const auto contentTop = versions->mapTo(panel, QPoint()).y();
    for (const auto &name : {"takeButton", "updateButton"})
    {
        auto *button = panel->findChild<QAbstractButton *>(name);
        QVERIFY(button->isVisible());
        QVERIFY(button->mapTo(panel, QPoint(0, button->height())).y() <= contentTop);
    }
    QCOMPARE(panel->findChild<ElaPushButton *>("takeButton")->text(), QString("Copy to project"));
    QVERIFY(panel->findChild<ElaListView *>("fileList")->height() <= 32);
    QVERIFY(!panel->findChild<QToolButton *>("moreButton"));
    QList<QRect> actionRects;
    for (const auto *name : {"folderButton", "refreshButton", "themeButton", "openFileButton", "takeButton", "updateButton",
                             "openSourceButton", "referenceButton", "editAssetButton", "removeSourceButton"})
    {
        auto *button = panel->findChild<QWidget *>(name);
        QVERIFY2(button && button->isVisible(), name);
        const QRect rect(button->mapTo(panel, QPoint()), button->size());
        QVERIFY2(panel->rect().contains(rect), name);
        for (const auto &other : actionRects) QVERIFY2(!other.intersects(rect), name);
        actionRects.append(rect);
    }
    QVERIFY(!panel->findChild<QWidget *>("operationFeedback")->isVisible());
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
            window.resize(1000, 600);
            QCoreApplication::processEvents();
            window.grab().save(screenshots + (mode == ElaThemeType::Light
                ? "/window-light.png" : "/window-dark.png"));
            window.resize(720, 420);
            QCoreApplication::processEvents();
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
