#include "app/MainWindow.h"
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
    auto *noticeAction = window.findChild<QToolButton *>(
        QStringLiteral("noticeActionButton"));
    auto *noticeLabel = window.findChild<QLabel *>(QStringLiteral("noticeLabel"));
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
    QVERIFY(noticeAction);
    QVERIFY(noticeLabel);
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
    QVERIFY2(service.createVersion(*single,
                                   QStringLiteral("1.0.0"),
                                   nullptr,
                                   &error),
             qPrintable(error));
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
    QCOMPARE(noticeAction->text(), QStringLiteral("Open destination"));
    QVERIFY(noticeLabel->text().contains(
        QStringLiteral("uart_rx_from_1_0_0.sv")));
    QVERIFY(!QFileInfo(QDir(copyDirectory).absoluteFilePath(
        QStringLiteral(".xips.json"))).exists());

    scan = AssetScanner().scan(library);
    single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    QVERIFY(single);
    QVERIFY2(service.createVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   nullptr,
                                   &error),
             qPrintable(error));

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
                               QStringLiteral("current working copy will be replaced"))
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

    const QString undoSource = temporary.filePath(
        QStringLiteral("incoming/temp_defs.svh"));
    QVERIFY(writeFile(undoSource, QByteArrayLiteral("`define TEMP_WIDTH 16\n")));
    QMimeData undoMime;
    undoMime.setUrls({QUrl::fromLocalFile(undoSource)});
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
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 3, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(noticeAction->isVisible(), 3000);
    QCOMPARE(noticeAction->text(), QStringLiteral("Undo"));
    noticeAction->click();
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QVERIFY(QFileInfo::exists(undoSource));
    QVERIFY(!findAsset(AssetScanner().scan(library),
                       QStringLiteral("temp_defs_svh")));
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
    QVERIFY2(service.deleteVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   RemovalMode::Permanent,
                                   &error),
             qPrintable(error));
    QFile working(workingFile);
    QVERIFY(working.open(QIODevice::ReadOnly));
    QCOMPARE(working.readAll(), firstRevision);
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
    bool deletionConfirmed = false;
    QTimer::singleShot(0, &window, [&] {
        QMessageBox *confirmation = window.findChild<QMessageBox *>();
        if (!confirmation) {
            return;
        }
        deletionConfirmed = confirmation->text().contains(
                                QStringLiteral("working file(s)"))
                            && confirmation->text().contains(
                                QStringLiteral("Original import sources are not changed"));
        confirmation->button(QMessageBox::Yes)->click();
    });
    deleteAssetAction->trigger();
    QVERIFY(deletionConfirmed);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 2, 10000);
    QVERIFY(QFileInfo::exists(QDir(folderSource).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"))));
    const ScanResult finalScan = AssetScanner().scan(library);
    QVERIFY(!findAsset(finalScan, QStringLiteral("packet_router")));
    QVERIFY(noticeLabel->text().contains(QStringLiteral("recycle bin")));
}

QTEST_MAIN(UserJourneyTest)

#include "tst_user_journey.moc"
