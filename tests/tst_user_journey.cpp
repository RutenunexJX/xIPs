#include "app/MainWindow.h"
#include "app/LibraryController.h"
#include "library/AssetLibraryService.h"
#include "library/AssetScanner.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>

#include <algorithm>

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

const AssetRecord *findAsset(const ScanResult &scan, const QString &id)
{
    const auto found = std::find_if(
        scan.assets.cbegin(), scan.assets.cend(), [&id](const AssetRecord &asset) {
            return asset.manifest.id == id;
        });
    return found == scan.assets.cend() ? nullptr : &*found;
}

bool hasGroup(const QTreeWidget *tree, const QString &name, const QString &count)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        const QTreeWidgetItem *item = tree->topLevelItem(index);
        if (item->text(0) == name && item->text(1) == count) {
            return true;
        }
    }
    return false;
}

} // namespace

class UrlCapture final : public QObject {
    Q_OBJECT

public:
    QUrl openedUrl;

public slots:
    void capture(const QUrl &url)
    {
        openedUrl = url;
    }
};

class UserJourneyTest final : public QObject {
    Q_OBJECT

private slots:
    void userCanCollectFindVersionCopyAndResyncAssets();
};

void UserJourneyTest::userCanCollectFindVersionCopyAndResyncAssets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString library = temporary.filePath(QStringLiteral("Jianguoyun/xIPs"));
    QVERIFY(QDir().mkpath(library));
    const QString singleSource = temporary.filePath(
        QStringLiteral("incoming/uart_rx.sv"));
    const QString folderSource = temporary.filePath(
        QStringLiteral("incoming/packet_router"));
    const QByteArray firstRevision = QByteArrayLiteral(
        "module uart_rx; localparam REV = 1; endmodule\n");
    QVERIFY(writeFile(singleSource, firstRevision));
    QVERIFY(writeFile(QDir(folderSource).absoluteFilePath(
                          QStringLiteral("rtl/packet_router.sv")),
                      QByteArrayLiteral("module packet_router; endmodule\n")));
    QVERIFY(writeFile(QDir(folderSource).absoluteFilePath(
                          QStringLiteral("doc/usage.md")),
                      QByteArrayLiteral("Packet router usage\n")));

    MainWindow window(library, nullptr, RemovalMode::Permanent);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("groupTree"));
    auto *files = window.findChild<QTreeWidget *>(QStringLiteral("fileTree"));
    auto *versions = window.findChild<QTreeWidget *>(QStringLiteral("versionTree"));
    auto *openMatchedAction = window.findChild<QAction *>(
        QStringLiteral("openMatchedFileAction"));
    auto *updateAction = window.findChild<QAction *>(
        QStringLiteral("updateAssetAction"));
    auto *copyAction = window.findChild<QAction *>(QStringLiteral("copyAction"));
    auto *restoreVersionAction = window.findChild<QAction *>(
        QStringLiteral("restoreVersionAction"));
    auto *deleteAssetAction = window.findChild<QAction *>(
        QStringLiteral("deleteAssetAction"));
    auto *problemAction = window.findChild<QAction *>(
        QStringLiteral("problemAction"));
    auto *controller = window.findChild<LibraryController *>();
    auto *noticeAction = window.findChild<QToolButton *>(
        QStringLiteral("noticeActionButton"));
    auto *dismissNotice = window.findChild<QToolButton *>(
        QStringLiteral("dismissNoticeButton"));
    auto *noticeLabel = window.findChild<QLabel *>(QStringLiteral("noticeLabel"));
    auto *noticeBanner = window.findChild<QWidget *>(
        QStringLiteral("noticeBanner"));
    QVERIFY(table);
    QVERIFY(search);
    QVERIFY(groups);
    QVERIFY(files);
    QVERIFY(versions);
    QVERIFY(openMatchedAction);
    QVERIFY(updateAction);
    QVERIFY(copyAction);
    QVERIFY(restoreVersionAction);
    QVERIFY(deleteAssetAction);
    QVERIFY(problemAction);
    QVERIFY(controller);
    QVERIFY(noticeAction);
    QVERIFY(dismissNotice);
    QVERIFY(noticeLabel);
    QVERIFY(noticeBanner);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 5000);

    QMimeData mimeData;
    mimeData.setUrls({QUrl::fromLocalFile(singleSource),
                      QUrl::fromLocalFile(folderSource)});
    QDragEnterEvent dragEnter(QPoint(10, 10),
                              Qt::CopyAction,
                              &mimeData,
                              Qt::LeftButton,
                              Qt::NoModifier);
    QApplication::sendEvent(&window, &dragEnter);
    QVERIFY(dragEnter.isAccepted());
    QDropEvent drop(QPointF(10.0, 10.0),
                    Qt::CopyAction,
                    &mimeData,
                    Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(dismissNotice->text(), QStringLiteral("Close"));
    QVERIFY(QFileInfo::exists(singleSource));
    QVERIFY(QFileInfo::exists(QDir(folderSource).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"))));

    search->setText(QStringLiteral("packet_router.sv"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()
                 ->index(0, 0)
                 .data(AssetTableModel::AssetNameRole)
                 .toString(),
             QStringLiteral("packet_router"));
    QVERIFY(table->model()
                ->index(0, 0)
                .data()
                .toString()
                .contains(QStringLiteral("Matched:")));
    QVERIFY(table->model()
                ->index(0, 0)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("rtl/packet_router.sv")));
    QTRY_VERIFY_WITH_TIMEOUT(openMatchedAction->isVisible(), 3000);
    QVERIFY(openMatchedAction->isEnabled());
    const QString matchedTarget = openMatchedAction->data().toString();
    QVERIFY(QFileInfo(matchedTarget).isFile());
    QVERIFY(files->currentItem());
    QVERIFY(files->currentItem()->text(0).endsWith(
        QStringLiteral("packet_router.sv")));
    UrlCapture capture;
    QDesktopServices::setUrlHandler(QStringLiteral("file"),
                                    &capture,
                                    "capture");
    openMatchedAction->trigger();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    QCOMPARE(capture.openedUrl, QUrl::fromLocalFile(matchedTarget));

    AssetLibraryService service;
    ScanResult scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 2);
    const AssetRecord *single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    const AssetRecord *folder = findAsset(scan, QStringLiteral("packet_router"));
    QVERIFY(single);
    QVERIFY(folder);
    QCOMPARE(QDir::cleanPath(openMatchedAction->data().toString()),
             QDir::cleanPath(QDir(folder->assetRoot).absoluteFilePath(
                 QStringLiteral("rtl/packet_router.sv"))));
    QCOMPARE(QDir::cleanPath(MainWindow::openTarget(*single)),
             QDir::cleanPath(QDir(single->assetRoot).absoluteFilePath(
                 QStringLiteral("uart_rx.sv"))));

    QString error;
    VersionInfo firstSavedResult;
    QVERIFY2(service.createVersion(*single,
                                   QStringLiteral("1.0.0"),
                                   &firstSavedResult,
                                   &error),
             qPrintable(error));
    QCOMPARE(firstSavedResult.version, QStringLiteral("1.0.0"));
    WorkingCopyState state = service.workingCopyState(*single);
    QVERIFY(state.hasSavedVersion);
    QVERIFY(!state.changed);
    const QString workingFile = QDir(single->assetRoot).absoluteFilePath(
        QStringLiteral("uart_rx.sv"));
    const QByteArray secondRevision = QByteArrayLiteral(
        "module uart_rx; localparam REV = 2; endmodule\n");
    const QString updateSource = temporary.filePath(
        QStringLiteral("revision-2/uart_rx.sv"));
    QVERIFY(writeFile(updateSource, secondRevision));
    const QString malformedAsset = QDir(library).absoluteFilePath(
        QStringLiteral("malformed_sync_asset"));
    QVERIFY(writeFile(QDir(malformedAsset).absoluteFilePath(
                          QStringLiteral(".xips.json")),
                      QByteArrayLiteral("{ not valid json")));
    bool refreshReportedProblem = false;
    connect(controller,
            &LibraryController::refreshFinished,
            &window,
            [&](const QList<AssetRecord> &, const QStringList &errors) {
                if (!errors.isEmpty()) {
                    refreshReportedProblem = true;
                }
            });

    window.applyActivation({
        .action = ActivationAction::OpenAsset,
        .value = QStringLiteral("uart_rx_sv"),
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        table->currentIndex()
            .siblingAtColumn(AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("uart_rx.sv"),
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(!openMatchedAction->isVisible(), 3000);
    bool updateCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (!dialog) {
            return;
        }
        auto *sourceEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("updateSourceEdit"));
        auto *preview = dialog->findChild<QLabel *>(
            QStringLiteral("updatePreviewLabel"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("updateDialogButtons"));
        if (!sourceEdit || !preview || !buttons) {
            dialog->reject();
            return;
        }
        sourceEdit->setText(updateSource);
        updateCompleted = preview->text().contains(QStringLiteral("Replace 1"))
                          && buttons->button(QDialogButtonBox::Ok)->isEnabled();
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    updateAction->trigger();
    QVERIFY(updateCompleted);
    QFile updatedWorking(workingFile);
    QVERIFY(updatedWorking.open(QIODevice::ReadOnly));
    QCOMPARE(updatedWorking.readAll(), secondRevision);
    updatedWorking.close();
    QTRY_VERIFY_WITH_TIMEOUT(refreshReportedProblem, 5000);
    QVERIFY(problemAction->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(dismissNotice->text(), QStringLiteral("Discard Undo"));
    QCOMPARE(QDir(library).entryList(
                 {QStringLiteral(".xips-create-recovery-*")},
                 QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                 .size(),
             1);
    QVERIFY(QDir(malformedAsset).removeRecursively());
    noticeAction->click();
    QTRY_VERIFY_WITH_TIMEOUT(noticeLabel->text().contains(
                                 QStringLiteral("previous working copy")),
                             3000);
    QFile undoneUpdate(workingFile);
    QVERIFY(undoneUpdate.open(QIODevice::ReadOnly));
    QCOMPARE(undoneUpdate.readAll(), firstRevision);
    undoneUpdate.close();

    QTRY_VERIFY_WITH_TIMEOUT(updateAction->isEnabled(), 3000);
    bool repeatedUpdateCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (!dialog) {
            return;
        }
        auto *sourceEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("updateSourceEdit"));
        auto *preview = dialog->findChild<QLabel *>(
            QStringLiteral("updatePreviewLabel"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("updateDialogButtons"));
        if (!sourceEdit || !preview || !buttons) {
            dialog->reject();
            return;
        }
        sourceEdit->setText(updateSource);
        repeatedUpdateCompleted = preview->text().contains(
                                      QStringLiteral("Replace 1"))
                                  && buttons->button(
                                      QDialogButtonBox::Ok)->isEnabled();
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    updateAction->trigger();
    QVERIFY(repeatedUpdateCompleted);
    QFile reupdatedWorking(workingFile);
    QVERIFY(reupdatedWorking.open(QIODevice::ReadOnly));
    QCOMPARE(reupdatedWorking.readAll(), secondRevision);
    reupdatedWorking.close();
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(dismissNotice->text(), QStringLiteral("Discard Undo"));
    QFile unchangedUpdateSource(updateSource);
    QVERIFY(unchangedUpdateSource.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedUpdateSource.readAll(), secondRevision);
    state = service.workingCopyState(*single);
    QVERIFY(state.changed);
    QCOMPARE(service.suggestedNextVersion(state.latestVersion),
             QStringLiteral("1.0.1"));

    const QString copyDirectory = temporary.filePath(QStringLiteral("project/rtl"));
    QVERIFY(QDir().mkpath(copyDirectory));
    const QString requestedTarget = QDir(copyDirectory).absoluteFilePath(
        QStringLiteral("uart_rx_from_1_0_0.sv"));
    QTRY_VERIFY_WITH_TIMEOUT(copyAction->isEnabled(), 3000);
    bool copyCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(QStringLiteral("copyDialog"));
        if (!dialog) {
            return;
        }
        auto *version = dialog->findChild<QComboBox *>(
            QStringLiteral("copyVersionCombo"));
        auto *destination = dialog->findChild<QLineEdit *>(
            QStringLiteral("copyDestinationEdit"));
        auto *name = dialog->findChild<QLineEdit *>(
            QStringLiteral("copyNameEdit"));
        auto *path = dialog->findChild<QLabel *>(
            QStringLiteral("copyFinalPathLabel"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("copyDialogButtons"));
        if (!version || !destination || !name || !path || !buttons) {
            dialog->reject();
            return;
        }
        version->setCurrentIndex(version->findData(QStringLiteral("1.0.0")));
        destination->setText(copyDirectory);
        name->setText(QStringLiteral("uart_rx_from_1_0_0.sv"));
        copyCompleted = path->text().contains(
                            QDir::toNativeSeparators(requestedTarget))
                        && buttons->button(QDialogButtonBox::Ok)->isEnabled();
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    copyAction->trigger();
    QVERIFY(copyCompleted);
    QFile copied(requestedTarget);
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), firstRevision);
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(dismissNotice->text(), QStringLiteral("Discard Undo"));
    const QStringList preservedUpdateRecovery = QDir(library).entryList(
        {QStringLiteral(".xips-create-recovery-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(preservedUpdateRecovery.size(), 1);
    QVERIFY(!QFileInfo(QDir(copyDirectory).absoluteFilePath(
        QStringLiteral(".xips.json"))).exists());
    dismissNotice->click();
    QTRY_VERIFY_WITH_TIMEOUT(!noticeAction->isVisible(), 3000);
    QVERIFY(QDir(library).entryList(
                {QStringLiteral(".xips-create-recovery-*"),
                 QStringLiteral(".xips-create-discard-*")},
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());

    scan = AssetScanner().scan(library);
    single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    QVERIFY(single);
    VersionInfo secondSavedResult;
    QVERIFY2(service.createVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   &secondSavedResult,
                                   &error),
             qPrintable(error));
    QCOMPARE(secondSavedResult.version, QStringLiteral("1.0.1"));

    QEvent versionRefresh(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &versionRefresh);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    window.applyActivation({
        .action = ActivationAction::OpenAsset,
        .value = QStringLiteral("uart_rx_sv"),
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        table->currentIndex()
            .siblingAtColumn(AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("uart_rx.sv"),
        3000);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 2, 10000);
    QTreeWidgetItem *firstSavedVersion = nullptr;
    for (int index = 0; index < versions->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = versions->topLevelItem(index);
        if (item->text(0) == QStringLiteral("1.0.0")) {
            firstSavedVersion = item;
            break;
        }
    }
    QVERIFY(firstSavedVersion);
    versions->setCurrentItem(firstSavedVersion);
    QTRY_VERIFY_WITH_TIMEOUT(restoreVersionAction->isVisible(), 3000);
    QVERIFY(restoreVersionAction->isEnabled());
    bool restoreConfirmed = false;
    QTimer::singleShot(0, &window, [&] {
        QMessageBox *confirmation = window.findChild<QMessageBox *>();
        if (!confirmation) {
            return;
        }
        restoreConfirmed = confirmation->text().contains(
                               QStringLiteral("Restore saved version 1.0.0"))
                           && confirmation->text().contains(
                               QStringLiteral("Files replaced: 1"))
                           && confirmation->text().contains(
                               QStringLiteral("kept temporarily"))
                           && confirmation->text().contains(
                               QStringLiteral("Saved versions will not be changed"));
        confirmation->button(QMessageBox::Yes)->click();
    });
    restoreVersionAction->trigger();
    QVERIFY(restoreConfirmed);
    QFile restoredWorking(workingFile);
    QVERIFY(restoredWorking.open(QIODevice::ReadOnly));
    QCOMPARE(restoredWorking.readAll(), firstRevision);
    restoredWorking.close();
    QCOMPARE(service.versions(single->assetRoot, &error).size(), 2);
    QFile preservedUpdateSource(updateSource);
    QVERIFY(preservedUpdateSource.open(QIODevice::ReadOnly));
    QCOMPARE(preservedUpdateSource.readAll(), secondRevision);
    QTRY_VERIFY_WITH_TIMEOUT(noticeLabel->text().contains(
                                 QStringLiteral("saved versions were kept")),
                             3000);
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    noticeAction->click();
    QTRY_VERIFY_WITH_TIMEOUT(noticeLabel->text().contains(
                                 QStringLiteral("previous working copy")),
                             3000);
    QFile undoneRestore(workingFile);
    QVERIFY(undoneRestore.open(QIODevice::ReadOnly));
    QCOMPARE(undoneRestore.readAll(), secondRevision);
    undoneRestore.close();
    QCOMPARE(service.versions(single->assetRoot, &error).size(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(
        table->currentIndex()
            .siblingAtColumn(AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("uart_rx.sv"),
        3000);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 2, 3000);

    int changed = 0;
    QVERIFY2(service.changeGroupMembership(scan.assets,
                                           QString(),
                                           QStringLiteral("UART"),
                                           &changed,
                                           &error),
             qPrintable(error));
    QCOMPARE(changed, 2);
    scan = AssetScanner().scan(library);
    QVERIFY2(service.changeGroupMembership(scan.assets,
                                           QStringLiteral("UART"),
                                           QStringLiteral("Serial"),
                                           &changed,
                                           &error),
             qPrintable(error));
    QCOMPARE(changed, 2);

    search->clear();
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &activate);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(hasGroup(groups,
                                      QStringLiteral("Serial"),
                                      QStringLiteral("2")),
                             5000);

    QTreeWidgetItem *serialGroup = nullptr;
    for (int index = 1; index < groups->topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = groups->topLevelItem(index);
        if (item->text(0) == QStringLiteral("Serial")) {
            serialGroup = item;
            break;
        }
    }
    QVERIFY(serialGroup);
    groups->setCurrentItem(serialGroup);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 3000);
    const QString undoSource = temporary.filePath(
        QStringLiteral("incoming/temp_defs.svh"));
    const QString undoFolderSource = temporary.filePath(
        QStringLiteral("incoming/temp_spi"));
    QVERIFY(writeFile(undoSource, QByteArrayLiteral("`define TEMP_WIDTH 16\n")));
    QVERIFY(writeFile(QDir(undoFolderSource).absoluteFilePath(
                          QStringLiteral("rtl/spi_top.sv")),
                      QByteArrayLiteral("module spi_top; endmodule\n")));
    QMimeData undoMime;
    undoMime.setUrls({QUrl::fromLocalFile(undoSource),
                      QUrl::fromLocalFile(undoFolderSource)});
    QDragEnterEvent undoDragEnter(QPoint(10, 10),
                                  Qt::CopyAction,
                                  &undoMime,
                                  Qt::LeftButton,
                                  Qt::NoModifier);
    QApplication::sendEvent(&window, &undoDragEnter);
    QVERIFY(undoDragEnter.isAccepted());
    QDropEvent undoDrop(QPointF(10.0, 10.0),
                        Qt::CopyAction,
                        &undoMime,
                        Qt::LeftButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&window, &undoDrop);
    QVERIFY(undoDrop.isAccepted());
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 4, 10000);
    QCOMPARE(groups->currentItem(), groups->topLevelItem(0));
    const ScanResult importedWhileFiltered = AssetScanner().scan(library);
    const AssetRecord *ungroupedFile = findAsset(
        importedWhileFiltered, QStringLiteral("temp_defs_svh"));
    const AssetRecord *ungroupedFolder = findAsset(
        importedWhileFiltered, QStringLiteral("temp_spi"));
    QVERIFY(ungroupedFile);
    QVERIFY(ungroupedFolder);
    QVERIFY(ungroupedFile->manifest.tags.isEmpty());
    QVERIFY(ungroupedFolder->manifest.tags.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QVERIFY(noticeLabel->text().contains(QStringLiteral("2 ungrouped asset(s)")));
    noticeAction->click();
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QVERIFY(QFileInfo::exists(undoSource));
    QVERIFY(QFileInfo::exists(QDir(undoFolderSource).absoluteFilePath(
        QStringLiteral("rtl/spi_top.sv"))));
    QVERIFY(!findAsset(AssetScanner().scan(library),
                       QStringLiteral("temp_defs_svh")));
    QVERIFY(!findAsset(AssetScanner().scan(library),
                       QStringLiteral("temp_spi")));
    QVERIFY(noticeLabel->text().contains(
        QStringLiteral("source files were unchanged")));

    const QString syncedSource = temporary.filePath(
        QStringLiteral("other-device/common_defs.svh"));
    QVERIFY(writeFile(syncedSource, QByteArrayLiteral("`define BUS_WIDTH 32\n")));
    QCOMPARE(service.importAssets(library, {syncedSource}).created.size(), 1);
    QEvent syncedActivate(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &syncedActivate);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 3, 10000);

    scan = AssetScanner().scan(library);
    single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    QVERIFY(single);
    DeleteVersionResult deletedVersion;
    QVERIFY2(service.deleteVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   RemovalMode::Permanent,
                                   &deletedVersion,
                                   &error),
             qPrintable(error));
    QVERIFY(deletedVersion.snapshotRemoved);
    QFile working(workingFile);
    QVERIFY(working.open(QIODevice::ReadOnly));
    QCOMPARE(working.readAll(), secondRevision);
    QCOMPARE(service.versions(single->assetRoot, &error).size(), 1);

    window.applyActivation({
        .action = ActivationAction::OpenAsset,
        .value = QStringLiteral("packet_router"),
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        table->currentIndex()
            .siblingAtColumn(AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("packet_router"),
        3000);
    const QString deleteUpdateSource = temporary.filePath(
        QStringLiteral("incoming/packet_router_update"));
    QVERIFY(writeFile(QDir(deleteUpdateSource).absoluteFilePath(
                          QStringLiteral("rtl/packet_router.sv")),
                      QByteArrayLiteral(
                          "module packet_router; localparam REV = 2; endmodule\n")));
    QVERIFY(writeFile(QDir(deleteUpdateSource).absoluteFilePath(
                          QStringLiteral("doc/usage.md")),
                      QByteArrayLiteral("Packet router usage revision 2\n")));
    bool deleteUpdateCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (!dialog) {
            return;
        }
        auto *sourceEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("updateSourceEdit"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("updateDialogButtons"));
        if (!sourceEdit || !buttons) {
            dialog->reject();
            return;
        }
        sourceEdit->setText(deleteUpdateSource);
        QPushButton *updateButton = buttons->button(QDialogButtonBox::Ok);
        deleteUpdateCompleted = updateButton && updateButton->isEnabled();
        if (updateButton) {
            updateButton->click();
        }
    });
    updateAction->trigger();
    QVERIFY(deleteUpdateCompleted);
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(QDir(library).entryList(
                 {QStringLiteral(".xips-create-recovery-*")},
                 QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                 .size(),
             1);
    const QString concurrentlyEditedPacket = QDir(library).absoluteFilePath(
        QStringLiteral("packet_router/rtl/packet_router.sv"));
    QVERIFY(writeFile(
        concurrentlyEditedPacket,
        QByteArrayLiteral(
            "module packet_router; localparam EXTERNAL_REV = 3; endmodule\n")));
    noticeAction->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        noticeLabel->text().contains(QStringLiteral("Undo was not applied")),
        3000);
    QVERIFY(!noticeAction->isVisible());
    QCOMPARE(dismissNotice->text(), QStringLiteral("Discard Undo"));
    QVERIFY(QFileInfo(concurrentlyEditedPacket).isFile());
    QCOMPARE(QDir(library).entryList(
                 {QStringLiteral(".xips-create-recovery-*")},
                 QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                 .size(),
             1);
    bool deletionConfirmed = false;
    QTimer::singleShot(0, &window, [&] {
        QMessageBox *confirmation = window.findChild<QMessageBox *>();
        if (!confirmation) {
            return;
        }
        deletionConfirmed = confirmation->text().contains(
                                QStringLiteral("working file(s)"))
                            && confirmation->text().contains(
                                QStringLiteral("Original import sources are not changed"))
                            && confirmation->text().contains(
                                QStringLiteral("pending Undo"));
        confirmation->button(QMessageBox::Yes)->click();
    });
    deleteAssetAction->trigger();
    QVERIFY(deletionConfirmed);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QVERIFY(QFileInfo::exists(QDir(folderSource).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"))));
    const ScanResult finalScan = AssetScanner().scan(library);
    QVERIFY(!findAsset(finalScan, QStringLiteral("packet_router")));
    QVERIFY(QDir(library).entryList(
                {QStringLiteral(".xips-create-recovery-*"),
                 QStringLiteral(".xips-create-discard-*")},
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty());
    QVERIFY(noticeLabel->text().contains(QStringLiteral("recycle bin")));

    working.close();
    window.applyActivation({
        .action = ActivationAction::OpenAsset,
        .value = QStringLiteral("uart_rx_sv"),
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        table->currentIndex()
            .siblingAtColumn(AssetTableModel::NameColumn)
            .data(AssetTableModel::AssetNameRole)
            .toString(),
        QStringLiteral("uart_rx.sv"),
        3000);

    const QByteArray thirdRevision = QByteArrayLiteral(
        "module uart_rx; localparam REV = 3; endmodule\n");
    const QString tamperedUpdateSource = temporary.filePath(
        QStringLiteral("incoming-v3/uart_rx.sv"));
    QVERIFY(writeFile(tamperedUpdateSource, thirdRevision));
    bool tamperedUpdateCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (!dialog) {
            return;
        }
        auto *sourceEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("updateSourceEdit"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("updateDialogButtons"));
        if (!sourceEdit || !buttons) {
            dialog->reject();
            return;
        }
        sourceEdit->setText(tamperedUpdateSource);
        QPushButton *updateButton = buttons->button(QDialogButtonBox::Ok);
        tamperedUpdateCompleted = updateButton && updateButton->isEnabled();
        if (updateButton) {
            updateButton->click();
        }
    });
    updateAction->trigger();
    QVERIFY(tamperedUpdateCompleted);
    QTRY_VERIFY_WITH_TIMEOUT(noticeBanner->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    const QStringList recoveryEntries = QDir(library).entryList(
        {QStringLiteral(".xips-create-recovery-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(recoveryEntries.size(), 1);
    const QString invalidRecoveryPath = QDir(library).absoluteFilePath(
        recoveryEntries.first());
    const QString invalidRecoveryFile = QDir(invalidRecoveryPath)
                                            .absoluteFilePath(
                                                QStringLiteral("uart_rx.sv"));
    const QByteArray tamperedRecoveryBytes = QByteArrayLiteral(
        "module uart_rx; localparam RECOVERY_TAMPERED = 1; endmodule\n");
    QVERIFY(writeFile(invalidRecoveryFile, tamperedRecoveryBytes));

    noticeAction->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        noticeLabel->text().contains(QStringLiteral("Undo was not applied")),
        3000);
    QVERIFY(noticeBanner->isVisible());
    QCOMPARE(dismissNotice->text(), QStringLiteral("Discard Undo"));
    dismissNotice->click();
    QTRY_VERIFY_WITH_TIMEOUT(!noticeBanner->isVisible(), 3000);
    QVERIFY(QFileInfo(invalidRecoveryFile).isFile());
    QFile retainedRecovery(invalidRecoveryFile);
    QVERIFY(retainedRecovery.open(QIODevice::ReadOnly));
    QCOMPARE(retainedRecovery.readAll(), tamperedRecoveryBytes);
    retainedRecovery.close();
    QVERIFY(problemAction->isEnabled());

    bool retainedPathReported = false;
    QTimer::singleShot(0, &window, [&] {
        QMessageBox *message = window.findChild<QMessageBox *>();
        if (!message) {
            return;
        }
        retainedPathReported = message->text().contains(
            QDir::toNativeSeparators(invalidRecoveryPath));
        message->accept();
    });
    problemAction->trigger();
    QVERIFY(retainedPathReported);

    const QByteArray fourthRevision = QByteArrayLiteral(
        "module uart_rx; localparam REV = 4; endmodule\n");
    const QString followupUpdateSource = temporary.filePath(
        QStringLiteral("incoming-v4/uart_rx.sv"));
    QVERIFY(writeFile(followupUpdateSource, fourthRevision));
    bool followupUpdateCompleted = false;
    QTimer::singleShot(0, &window, [&] {
        QDialog *dialog = window.findChild<QDialog *>(
            QStringLiteral("updateAssetDialog"));
        if (!dialog) {
            return;
        }
        auto *sourceEdit = dialog->findChild<QLineEdit *>(
            QStringLiteral("updateSourceEdit"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("updateDialogButtons"));
        if (!sourceEdit || !buttons) {
            dialog->reject();
            return;
        }
        sourceEdit->setText(followupUpdateSource);
        QPushButton *updateButton = buttons->button(QDialogButtonBox::Ok);
        followupUpdateCompleted = updateButton && updateButton->isEnabled();
        if (updateButton) {
            updateButton->click();
        }
    });
    updateAction->trigger();
    QVERIFY(followupUpdateCompleted);
    QFile followedUpWorking(workingFile);
    QVERIFY(followedUpWorking.open(QIODevice::ReadOnly));
    QCOMPARE(followedUpWorking.readAll(), fourthRevision);
    followedUpWorking.close();
    QTRY_VERIFY_WITH_TIMEOUT(noticeBanner->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    QCOMPARE(QDir(library)
                 .entryList({QStringLiteral(".xips-create-recovery-*")},
                            QDir::Dirs | QDir::Hidden
                                | QDir::NoDotAndDotDot)
                 .size(),
             2);
    dismissNotice->click();
    QTRY_VERIFY_WITH_TIMEOUT(!noticeBanner->isVisible(), 3000);
    const QStringList retainedRecoveries = QDir(library).entryList(
        {QStringLiteral(".xips-create-recovery-*")},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(retainedRecoveries.size(), 1);
    QCOMPARE(QDir(library).absoluteFilePath(retainedRecoveries.first()),
             invalidRecoveryPath);
    QVERIFY(QFileInfo(invalidRecoveryFile).isFile());
}

QTEST_MAIN(UserJourneyTest)

#include "tst_user_journey.moc"
