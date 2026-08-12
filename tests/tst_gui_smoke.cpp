#include "app/MainWindow.h"
#include "app/LibraryController.h"
#include "library/AssetLibraryService.h"
#include "library/FileSystemUtil.h"
#include "manifest/ManifestService.h"

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
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

class UrlCapture final : public QObject {
    Q_OBJECT

public:
    QUrl openedUrl;
    int openCount = 0;

public slots:
    void capture(const QUrl &url)
    {
        openedUrl = url;
        ++openCount;
    }
};

class GuiSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void firstRunRequiresAnExplicitLibrary();
    void startupReportsUnfinishedOperationPaths();
    void startupRecoversInterruptedManifestTransaction();
    void firstScreenIsACompactAssetLibrary();
    void binaryFilesRemainInventoryEntries();
    void activationSelectsAnAsset();
    void singleFileCanBeVersionedWhenTheLibraryRefreshesDuringInput();
    void editDetailsSurvivesRefreshWhileDialogIsOpen();
    void deleteVersionSurvivesRefreshWhileConfirmationIsOpen();
    void directoryVersionPreviewIsIsolatedAndRepeatable();
    void ungroupedFilterAndSelectedGroupRemovalStayConsistent();
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

void GuiSmokeTest::startupReportsUnfinishedOperationPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString orphan = QDir(library).absoluteFilePath(
        QStringLiteral(
            ".xips-create-recovery-11111111-1111-1111-1111-111111111111"));
    const QString retainedFile = QDir(orphan).absoluteFilePath(
        QStringLiteral("rtl/retained.sv"));
    QVERIFY(writeFile(retainedFile,
                      QByteArrayLiteral("module retained; endmodule\n")));
    const QString assetRoot = QDir(library).absoluteFilePath(
        QStringLiteral("staged_asset"));
    const QString manifestPath = QDir(assetRoot).absoluteFilePath(
        QStringLiteral(".xips.json"));
    QVERIFY(writeFile(
        manifestPath,
        QByteArrayLiteral(
            R"({"schemaVersion":1,"id":"staged_asset","name":"Staged asset"})")));
    QVERIFY(writeFile(
        QDir(assetRoot).absoluteFilePath(QStringLiteral("rtl/live.sv")),
        QByteArrayLiteral("module live; endmodule\n")));
    const QString versionStaging = QDir(assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions/.staging-22222222"));
    const QString stagedFile = QDir(versionStaging).absoluteFilePath(
        QStringLiteral("rtl/staged.sv"));
    QVERIFY(writeFile(stagedFile,
                      QByteArrayLiteral("module staged; endmodule\n")));

    {
        MainWindow window(library, nullptr, RemovalMode::Permanent);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *problems = window.findChild<QAction *>(
            QStringLiteral("problemAction"));
        auto *notice = window.findChild<QLabel *>(
            QStringLiteral("noticeLabel"));
        QVERIFY(problems);
        QVERIFY(notice);
        QTRY_VERIFY_WITH_TIMEOUT(problems->isEnabled(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(
            notice->text().contains(QStringLiteral("unfinished xIPs operation")),
            3000);

        bool exactPathsReported = false;
        QTimer::singleShot(0, &window, [&] {
            QMessageBox *message = window.findChild<QMessageBox *>();
            if (!message) {
                return;
            }
            exactPathsReported =
                message->text().contains(QDir::toNativeSeparators(orphan))
                && message->text().contains(
                    QDir::toNativeSeparators(versionStaging));
            message->accept();
        });
        problems->trigger();
        QVERIFY(exactPathsReported);
        QVERIFY(!problems->isEnabled());
    }

    QVERIFY(QFileInfo(retainedFile).isFile());
    QVERIFY(QFileInfo(stagedFile).isFile());
}

void GuiSmokeTest::startupRecoversInterruptedManifestTransaction()
{
    struct SimulatedCrash final {
    };

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString source = temporary.filePath(
        QStringLiteral("incoming/recovery_target.sv"));
    QVERIFY(writeFile(
        source,
        QByteArrayLiteral("module recovery_target; endmodule\n")));

    AssetLibraryService service;
    AssetRecord imported;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("recovery_target"),
                               .name = QStringLiteral("Recovery Target"),
                               .description = QStringLiteral(
                                   "Original metadata"),
                               .tags = {QStringLiteral("CDC")}}},
                 &imported,
                 &error),
             qPrintable(error));
    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    bool interrupted = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            imported,
            {.id = imported.manifest.id,
             .name = QStringLiteral("Unpublished Replacement"),
             .description = QStringLiteral("Replacement metadata"),
             .tags = imported.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        interrupted = true;
    }
    QVERIFY(interrupted);
    QVERIFY(!QFileInfo::exists(imported.manifestPath));
    const auto transactionDirectories = [&] {
        return QDir(library).entryList(
            {QStringLiteral(".xips-create-metadata-*")},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };
    QCOMPARE(transactionDirectories().size(), 1);

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *notice = window.findChild<QLabel *>(QStringLiteral("noticeLabel"));
    QVERIFY(table);
    QVERIFY(notice);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data(AssetTableModel::AssetNameRole)
                 .toString(),
             QStringLiteral("Recovery Target"));

    const ManifestLoadResult recovered = ManifestService().load(
        imported.manifestPath);
    QVERIFY(recovered.ok());
    QCOMPARE(recovered.manifest->id, QStringLiteral("recovery_target"));
    QCOMPARE(recovered.manifest->name, QStringLiteral("Recovery Target"));
    QCOMPARE(recovered.manifest->description,
             QStringLiteral("Original metadata"));
    QCOMPARE(recovered.manifest->tags,
             QStringList{QStringLiteral("CDC")});
    QTRY_VERIFY_WITH_TIMEOUT(transactionDirectories().isEmpty(), 3000);
    QVERIFY(notice->text().contains(
        QStringLiteral("Resolved 1 interrupted manifest update")));

    service.setWorkingCopyTestHook(
        WorkingCopyTestPoint::ManifestOriginalIsolatedBeforePublish,
        [](const WorkingCopyTestPoint, const QString &) {
            throw SimulatedCrash{};
        });
    interrupted = false;
    try {
        MetadataUpdateResult ignored;
        service.updateMetadata(
            imported,
            {.id = imported.manifest.id,
             .name = QStringLiteral("Second Unpublished Replacement"),
             .description = imported.manifest.description,
             .tags = imported.manifest.tags},
            &ignored,
            &error);
    } catch (const SimulatedCrash &) {
        interrupted = true;
    }
    QVERIFY(interrupted);
    QVERIFY(!QFileInfo::exists(imported.manifestPath));
    QCOMPARE(transactionDirectories().size(), 1);

    auto *refresh = window.findChild<QAction *>(
        QStringLiteral("refreshAction"));
    QVERIFY(refresh);
    refresh->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(imported.manifestPath), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(transactionDirectories().isEmpty(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    const ManifestLoadResult refreshed = ManifestService().load(
        imported.manifestPath);
    QVERIFY(refreshed.ok());
    QCOMPARE(refreshed.manifest->name, QStringLiteral("Recovery Target"));
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
    auto *restoreVersion = window.findChild<QAction *>(
        QStringLiteral("restoreVersionAction"));
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
    QVERIFY(restoreVersion);
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
    QVERIFY(!restoreVersion->isVisible());
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

void GuiSmokeTest::singleFileCanBeVersionedWhenTheLibraryRefreshesDuringInput()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString source = temporary.filePath(QStringLiteral("incoming/blink.v"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module blink; endmodule\n")));

    AssetLibraryService service;
    AssetRecord imported;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = service.suggestedMetadata(source)},
                 &imported,
                 &error),
             qPrintable(error));
    QCOMPARE(imported.files, QStringList{QStringLiteral("blink.v")});

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *saveVersion = window.findChild<QAction *>(
        QStringLiteral("saveVersionAction"));
    auto *versions = window.findChild<QTreeWidget *>(
        QStringLiteral("versionTree"));
    auto *tabs = window.findChild<QTabWidget *>();
    auto *controller = window.findChild<LibraryController *>();
    QVERIFY(table);
    QVERIFY(saveVersion);
    QVERIFY(versions);
    QVERIFY(tabs);
    QVERIFY(controller);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(saveVersion->isEnabled(), 3000);

    bool inputHandled = false;
    bool refreshCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QInputDialog *>();
        if (!dialog) {
            return;
        }
        inputHandled = true;
        dialog->setTextValue(QStringLiteral("1.0.0"));
        connect(controller,
                &LibraryController::refreshFinished,
                dialog,
                [&, dialog] {
                    refreshCompleted = true;
                    dialog->accept();
                },
                Qt::SingleShotConnection);
        controller->rebuild();
    });
    QTimer::singleShot(3000, &window, [&] {
        if (auto *message = window.findChild<QMessageBox *>()) {
            message->accept();
        }
    });
    saveVersion->trigger();

    QVERIFY(inputHandled);
    QVERIFY(refreshCompleted);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 1, 5000);
    QTreeWidgetItem *savedItem = versions->topLevelItem(0);
    QCOMPARE(savedItem->text(0), QStringLiteral("1.0.0"));
    QTRY_VERIFY_WITH_TIMEOUT(versions->currentItem() == savedItem, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(tabs->currentWidget()
                                 == versions->parentWidget(),
                             3000);
    const QString savedFile = QDir(imported.assetRoot).absoluteFilePath(
        QStringLiteral(".xips/versions/1.0.0/blink.v"));
    QVERIFY(QFileInfo(savedFile).isFile());

    versions->scrollToItem(savedItem);
    QTRY_VERIFY_WITH_TIMEOUT(versions->isVisible(), 1000);
    const QRect savedItemRect = versions->visualItemRect(savedItem);
    QVERIFY(!savedItemRect.isEmpty());
    UrlCapture capture;
    QSignalSpy doubleClick(versions, &QTreeWidget::itemDoubleClicked);
    QDesktopServices::setUrlHandler(QStringLiteral("file"),
                                    &capture,
                                    "capture");
    versions->viewport()->setFocus();
    QTest::mouseClick(versions->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      savedItemRect.center());
    QTest::mouseDClick(versions->viewport(),
                       Qt::LeftButton,
                       Qt::NoModifier,
                       savedItemRect.center());
    QCoreApplication::processEvents();
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    QCOMPARE(doubleClick.count(), 1);
    QCOMPARE(capture.openCount, 1);
    QVERIFY(capture.openedUrl.isLocalFile());
    const QString previewFile = capture.openedUrl.toLocalFile();
    QVERIFY(QFileInfo(previewFile).isFile());
    QVERIFY(QFileInfo(previewFile).absoluteFilePath()
            != QFileInfo(savedFile).absoluteFilePath());
    QFile preview(previewFile);
    QVERIFY(preview.open(QIODevice::ReadOnly));
    QCOMPARE(preview.readAll(), QByteArrayLiteral("module blink; endmodule\n"));
    preview.close();
    QVERIFY(QFile::setPermissions(
        previewFile,
        QFileInfo(previewFile).permissions()
            | QFileDevice::WriteOwner | QFileDevice::WriteUser));
    QVERIFY(writeFile(previewFile,
                      QByteArrayLiteral("module edited_preview; endmodule\n")));
    QFile saved(savedFile);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), QByteArrayLiteral("module blink; endmodule\n"));
}

void GuiSmokeTest::editDetailsSurvivesRefreshWhileDialogIsOpen()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString source = temporary.filePath(
        QStringLiteral("incoming/axi_timer.sv"));
    QVERIFY(writeFile(source,
                      QByteArrayLiteral("module axi_timer; endmodule\n")));

    AssetLibraryService service;
    AssetRecord imported;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = {.id = QStringLiteral("axi_timer"),
                               .name = QStringLiteral("AXI Timer"),
                               .description = QStringLiteral(
                                   "Original description"),
                               .tags = {QStringLiteral("AXI")}}},
                 &imported,
                 &error),
             qPrintable(error));

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *edit = window.findChild<QAction *>(QStringLiteral("editAction"));
    auto *controller = window.findChild<LibraryController *>();
    QVERIFY(table);
    QVERIFY(edit);
    QVERIFY(controller);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(edit->isEnabled(), 3000);

    bool dialogHandled = false;
    bool refreshCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("metadataDialog"));
        if (!dialog) {
            return;
        }
        auto *name = dialog->findChild<QLineEdit *>(
            QStringLiteral("ipNameEdit"));
        auto *description = dialog->findChild<QPlainTextEdit *>();
        if (!name || !description) {
            dialog->reject();
            return;
        }
        dialogHandled = true;
        name->setText(QStringLiteral("AXI Timer Updated"));
        description->setPlainText(
            QStringLiteral("Updated after an in-dialog library refresh"));
        connect(controller,
                &LibraryController::refreshFinished,
                dialog,
                [&, dialog] {
                    refreshCompleted = true;
                    dialog->accept();
                },
                Qt::SingleShotConnection);
        controller->rebuild();
    });
    QTimer::singleShot(5000, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>(
                QStringLiteral("metadataDialog"))) {
            dialog->reject();
        }
    });
    edit->trigger();

    QVERIFY(dialogHandled);
    QVERIFY(refreshCompleted);
    const ManifestLoadResult updated = ManifestService().load(
        imported.manifestPath);
    QVERIFY(updated.ok());
    QCOMPARE(updated.manifest->id, QStringLiteral("axi_timer"));
    QCOMPARE(updated.manifest->name, QStringLiteral("AXI Timer Updated"));
    QCOMPARE(updated.manifest->description,
             QStringLiteral("Updated after an in-dialog library refresh"));
    QCOMPARE(updated.manifest->tags,
             QStringList{QStringLiteral("AXI")});
    QTRY_COMPARE_WITH_TIMEOUT(
        table->model()
            ->index(0, AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("AXI Timer Updated"),
        5000);
}

void GuiSmokeTest::deleteVersionSurvivesRefreshWhileConfirmationIsOpen()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString source = temporary.filePath(
        QStringLiteral("incoming/pulse_sync.v"));
    const QByteArray workingContents = QByteArrayLiteral(
        "module pulse_sync; endmodule\n");
    QVERIFY(writeFile(source, workingContents));

    AssetLibraryService service;
    AssetRecord imported;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = service.suggestedMetadata(source)},
                 &imported,
                 &error),
             qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(imported,
                                   QStringLiteral("1.0.0"),
                                   &saved,
                                   &error),
             qPrintable(error));
    const QString snapshotRoot = saved.path;
    const QString workingFile = QDir(imported.assetRoot).absoluteFilePath(
        QStringLiteral("pulse_sync.v"));
    QVERIFY(QFileInfo(snapshotRoot).isDir());
    QVERIFY(QFileInfo(workingFile).isFile());

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *versions = window.findChild<QTreeWidget *>(
        QStringLiteral("versionTree"));
    auto *deleteVersion = window.findChild<QAction *>(
        QStringLiteral("deleteVersionAction"));
    auto *controller = window.findChild<LibraryController *>();
    QVERIFY(table);
    QVERIFY(versions);
    QVERIFY(deleteVersion);
    QVERIFY(controller);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 1, 5000);
    QTreeWidgetItem *savedItem = findItem(versions,
                                          0,
                                          QStringLiteral("1.0.0"));
    QVERIFY(savedItem);
    versions->setCurrentItem(savedItem);
    QTRY_VERIFY_WITH_TIMEOUT(deleteVersion->isEnabled(), 3000);

    bool confirmationHandled = false;
    bool refreshCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QMessageBox *confirmation = window.findChild<QMessageBox *>();
        if (!confirmation) {
            return;
        }
        connect(controller,
                &LibraryController::refreshFinished,
                confirmation,
                [&, confirmation] {
                    refreshCompleted = true;
                    QAbstractButton *yes = confirmation->button(
                        QMessageBox::Yes);
                    confirmationHandled = yes
                                          && confirmation->windowTitle()
                                                 == QStringLiteral(
                                                     "Delete saved version")
                                          && confirmation->text().contains(
                                              QStringLiteral("1.0.0"));
                    if (yes) {
                        yes->click();
                    } else {
                        confirmation->reject();
                    }
                },
                Qt::SingleShotConnection);
        controller->rebuild();
    });
    QTimer::singleShot(5000, &window, [&] {
        if (auto *confirmation = window.findChild<QMessageBox *>()) {
            confirmation->reject();
        }
    });
    deleteVersion->trigger();

    QVERIFY(confirmationHandled);
    QVERIFY(refreshCompleted);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(snapshotRoot), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 0, 5000);
    QFile working(workingFile);
    QVERIFY(working.open(QIODevice::ReadOnly));
    QCOMPARE(working.readAll(), workingContents);
    QVERIFY(QFileInfo(imported.manifestPath).isFile());
    QCOMPARE(service.versions(imported.assetRoot, &error).size(), 0);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void GuiSmokeTest::directoryVersionPreviewIsIsolatedAndRepeatable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString source = temporary.filePath(
        QStringLiteral("incoming/packet_router"));
    const QByteArray firstRevision = QByteArrayLiteral(
        "module packet_router; localparam REV = 1; endmodule\n");
    const QByteArray secondRevision = QByteArrayLiteral(
        "module packet_router; localparam REV = 2; endmodule\n");
    QVERIFY(writeFile(QDir(source).absoluteFilePath(
                          QStringLiteral("rtl/packet_router.sv")),
                      firstRevision));
    QVERIFY(writeFile(QDir(source).absoluteFilePath(
                          QStringLiteral("constraints/packet_router.xdc")),
                      QByteArrayLiteral("set_property PACKAGE_PIN A1 [get_ports clk]\n")));

    AssetLibraryService service;
    AssetRecord imported;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = source,
                  .metadata = service.suggestedMetadata(source)},
                 &imported,
                 &error),
             qPrintable(error));
    VersionInfo firstVersion;
    QVERIFY2(service.createVersion(imported,
                                   QStringLiteral("1.0.0"),
                                   &firstVersion,
                                   &error),
             qPrintable(error));
    const QString workingSource = QDir(imported.assetRoot).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"));
    QVERIFY(writeFile(workingSource, secondRevision));
    VersionInfo secondVersion;
    QVERIFY2(service.createVersion(imported,
                                   QStringLiteral("1.0.1"),
                                   &secondVersion,
                                   &error),
             qPrintable(error));
    QFile savedSecondFile(QDir(secondVersion.path).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv")));
    QVERIFY(savedSecondFile.open(QIODevice::ReadOnly));
    QCOMPARE(savedSecondFile.readAll(), secondRevision);

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *versions = window.findChild<QTreeWidget *>(
        QStringLiteral("versionTree"));
    QVERIFY(table);
    QVERIFY(versions);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 2, 5000);
    QTreeWidgetItem *firstItem = findItem(versions, 0, QStringLiteral("1.0.0"));
    QTreeWidgetItem *secondItem = findItem(versions, 0, QStringLiteral("1.0.1"));
    QVERIFY(firstItem);
    QVERIFY(secondItem);
    versions->setCurrentItem(secondItem);
    QCOMPARE(versions->currentItem(), secondItem);

    UrlCapture capture;
    QDesktopServices::setUrlHandler(QStringLiteral("file"),
                                    &capture,
                                    "capture");
    versions->itemActivated(firstItem, 0);
    QCoreApplication::processEvents();
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));

    QCOMPARE(capture.openCount, 1);
    QVERIFY(capture.openedUrl.isLocalFile());
    const QString firstPreview = capture.openedUrl.toLocalFile();
    QVERIFY(QFileInfo(firstPreview).isDir());
    const QString repositoryRoot =
        QDir(QDir(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY))
                 .absoluteFilePath(QStringLiteral("../..")))
            .absolutePath();
    QVERIFY(files::isWithin(firstPreview, QDir::tempPath()));
    QVERIFY(!files::isWithin(firstPreview, repositoryRoot));
    QVERIFY(!files::isWithin(firstPreview, library));
    QVERIFY(!files::isWithin(firstPreview, imported.assetRoot));
    QVERIFY(!QFileInfo(QDir(firstPreview).absoluteFilePath(
                           QStringLiteral(".xips"))).exists());
    QVERIFY(!QFileInfo(QDir(firstPreview).absoluteFilePath(
                           QStringLiteral(".xips.json"))).exists());
    QVERIFY(!QFileInfo(QDir(firstPreview).absoluteFilePath(
                           QStringLiteral(".snapshot.json"))).exists());
    const QString firstPreviewSource = QDir(firstPreview).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"));
    QFile firstPreviewFile(firstPreviewSource);
    QVERIFY(firstPreviewFile.open(QIODevice::ReadOnly));
    QCOMPARE(firstPreviewFile.readAll(), firstRevision);
    firstPreviewFile.close();
    QFile firstPreviewConstraint(QDir(firstPreview).absoluteFilePath(
        QStringLiteral("constraints/packet_router.xdc")));
    QVERIFY(firstPreviewConstraint.open(QIODevice::ReadOnly));
    QCOMPARE(firstPreviewConstraint.readAll(),
             QByteArrayLiteral("set_property PACKAGE_PIN A1 [get_ports clk]\n"));
    QCOMPARE(versions->currentItem(), secondItem);

    QVERIFY(QFile::setPermissions(
        firstPreviewSource,
        QFileInfo(firstPreviewSource).permissions()
            | QFileDevice::WriteOwner | QFileDevice::WriteUser));
    QVERIFY(writeFile(firstPreviewSource,
                      QByteArrayLiteral(
                          "module packet_router; localparam PREVIEW_EDIT = 1; endmodule\n")));
    QVERIFY(writeFile(QDir(firstPreview).absoluteFilePath(
                          QStringLiteral("editor-generated.log")),
                      QByteArrayLiteral("preview-only\n")));
    const QString savedFirstSource = QDir(firstVersion.path).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"));
    QFile savedFirstFile(savedFirstSource);
    QVERIFY(savedFirstFile.open(QIODevice::ReadOnly));
    QCOMPARE(savedFirstFile.readAll(), firstRevision);
    savedFirstFile.close();
    QFile unchangedWorking(workingSource);
    QVERIFY(unchangedWorking.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedWorking.readAll(), secondRevision);
    QCOMPARE(service.versions(imported.assetRoot, &error).size(), 2);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    capture.openedUrl = QUrl();
    QDesktopServices::setUrlHandler(QStringLiteral("file"),
                                    &capture,
                                    "capture");
    versions->itemActivated(firstItem, 0);
    QCoreApplication::processEvents();
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));

    QCOMPARE(capture.openCount, 2);
    QVERIFY(capture.openedUrl.isLocalFile());
    const QString secondPreview = capture.openedUrl.toLocalFile();
    QVERIFY(QFileInfo(secondPreview).isDir());
    QVERIFY(QFileInfo(secondPreview).absoluteFilePath()
            != QFileInfo(firstPreview).absoluteFilePath());
    const QString secondPreviewSource = QDir(secondPreview).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"));
    QFile secondPreviewFile(secondPreviewSource);
    QVERIFY(secondPreviewFile.open(QIODevice::ReadOnly));
    QCOMPARE(secondPreviewFile.readAll(), firstRevision);
    QVERIFY(QFileInfo(QDir(secondPreview).absoluteFilePath(
                          QStringLiteral("constraints/packet_router.xdc")))
                .isFile());
    QVERIFY(!QFileInfo(QDir(secondPreview).absoluteFilePath(
                           QStringLiteral(".xips"))).exists());
    QVERIFY(!QFileInfo(QDir(secondPreview).absoluteFilePath(
                           QStringLiteral(".xips.json"))).exists());
    QVERIFY(!QFileInfo(QDir(secondPreview).absoluteFilePath(
                           QStringLiteral(".snapshot.json"))).exists());
    QVERIFY(!QFileInfo(QDir(secondPreview).absoluteFilePath(
                           QStringLiteral("editor-generated.log"))).exists());
    QCOMPARE(versions->currentItem(), secondItem);
}

void GuiSmokeTest::ungroupedFilterAndSelectedGroupRemovalStayConsistent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("library"));
    const QString axiSource = temporary.filePath(
        QStringLiteral("incoming/axi_bridge.sv"));
    const QString plainSource = temporary.filePath(
        QStringLiteral("incoming/crc_checker.v"));
    QVERIFY(writeFile(axiSource,
                      QByteArrayLiteral("module axi_bridge; endmodule\n")));
    QVERIFY(writeFile(plainSource,
                      QByteArrayLiteral("module crc_checker; endmodule\n")));

    AssetLibraryService service;
    AssetRecord axiAsset;
    AssetRecord plainAsset;
    QString error;
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = axiSource,
                  .metadata = {.id = QStringLiteral("axi_bridge"),
                               .name = QStringLiteral("AXI Bridge"),
                               .description = QStringLiteral(
                                   "AXI4 register bridge"),
                               .tags = {QStringLiteral("AXI")}}},
                 &axiAsset,
                 &error),
             qPrintable(error));
    QVERIFY2(service.importAsset(
                 {.libraryRoot = library,
                  .sourcePath = plainSource,
                  .metadata = {.id = QStringLiteral("crc_checker"),
                               .name = QStringLiteral("CRC Checker"),
                               .description = QStringLiteral(
                                   "Packet checksum checker"),
                               .tags = {}}},
                 &plainAsset,
                 &error),
             qPrintable(error));

    ManifestService manifests;
    ManifestLoadResult axiLoaded = manifests.load(axiAsset.manifestPath);
    ManifestLoadResult plainLoaded = manifests.load(plainAsset.manifestPath);
    QVERIFY(axiLoaded.ok());
    QVERIFY(plainLoaded.ok());
    axiLoaded.manifest->rawObject.insert(
        QStringLiteral("userNote"),
        QStringLiteral("preserve AXI metadata"));
    plainLoaded.manifest->rawObject.insert(
        QStringLiteral("userNote"),
        QStringLiteral("preserve plain metadata"));
    QVERIFY2(manifests.write(axiAsset.manifestPath,
                             *axiLoaded.manifest,
                             &error),
             qPrintable(error));
    QVERIFY2(manifests.write(plainAsset.manifestPath,
                             *plainLoaded.manifest,
                             &error),
             qPrintable(error));
    axiLoaded = manifests.load(axiAsset.manifestPath);
    plainLoaded = manifests.load(plainAsset.manifestPath);
    QVERIFY(axiLoaded.ok());
    QVERIFY(plainLoaded.ok());
    QJsonObject expectedAxiMetadata = manifests.toJson(*axiLoaded.manifest);
    expectedAxiMetadata.remove(QStringLiteral("tags"));
    const QJsonObject expectedPlainMetadata = manifests.toJson(
        *plainLoaded.manifest);

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("groupTree"));
    auto *groupMenuButton = window.findChild<QToolButton *>(
        QStringLiteral("groupMenuButton"));
    QVERIFY(table);
    QVERIFY(groups);
    QVERIFY(groupMenuButton);
    QVERIFY(groupMenuButton->menu());
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(findItem(groups,
                                      0,
                                      QStringLiteral("All assets")),
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(findItem(groups, 0, QStringLiteral("AXI")),
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(findItem(groups,
                                      0,
                                      QStringLiteral("Ungrouped")),
                             5000);
    QTreeWidgetItem *all = findItem(groups, 0, QStringLiteral("All assets"));
    QTreeWidgetItem *axi = findItem(groups, 0, QStringLiteral("AXI"));
    QTreeWidgetItem *ungrouped = findItem(groups,
                                          0,
                                          QStringLiteral("Ungrouped"));
    QCOMPARE(all->text(1), QStringLiteral("2"));
    QCOMPARE(axi->text(1), QStringLiteral("1"));
    QCOMPARE(ungrouped->text(1), QStringLiteral("1"));

    groups->setCurrentItem(ungrouped);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data(AssetTableModel::AssetNameRole)
                 .toString(),
             QStringLiteral("CRC Checker"));

    groups->setCurrentItem(axi);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, AssetTableModel::NameColumn)
                 .data(AssetTableModel::AssetNameRole)
                 .toString(),
             QStringLiteral("AXI Bridge"));
    table->selectRow(0);
    table->setCurrentIndex(
        table->model()->index(0, AssetTableModel::NameColumn));

    QAction *removeSelected = nullptr;
    for (QAction *action : groupMenuButton->menu()->actions()) {
        if (action->text()
            == QStringLiteral("Remove selected assets from current group")) {
            removeSelected = action;
            break;
        }
    }
    QVERIFY(removeSelected);
    QVERIFY(removeSelected->isEnabled());
    removeSelected->trigger();

    QTRY_VERIFY_WITH_TIMEOUT(!findItem(groups, 0, QStringLiteral("AXI")),
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(findItem(groups,
                                      0,
                                      QStringLiteral("Ungrouped")),
                             5000);
    all = findItem(groups, 0, QStringLiteral("All assets"));
    ungrouped = findItem(groups, 0, QStringLiteral("Ungrouped"));
    QVERIFY(all);
    QVERIFY(ungrouped);
    QCOMPARE(all->text(1), QStringLiteral("2"));
    QCOMPARE(ungrouped->text(1), QStringLiteral("2"));

    const ManifestLoadResult axiAfter = manifests.load(axiAsset.manifestPath);
    const ManifestLoadResult plainAfter = manifests.load(
        plainAsset.manifestPath);
    QVERIFY(axiAfter.ok());
    QVERIFY(plainAfter.ok());
    QVERIFY(axiAfter.manifest->tags.isEmpty());
    QVERIFY(plainAfter.manifest->tags.isEmpty());
    QCOMPARE(manifests.toJson(*axiAfter.manifest), expectedAxiMetadata);
    QCOMPARE(manifests.toJson(*plainAfter.manifest),
             expectedPlainMetadata);
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
