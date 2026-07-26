#include "app/MainWindow.h"
#include "library/AssetLibraryService.h"
#include "library/AssetScanner.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLineEdit>
#include <QMimeData>
#include <QTableView>
#include <QTemporaryDir>
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

    MainWindow window(library);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("groupTree"));
    QVERIFY(table);
    QVERIFY(search);
    QVERIFY(groups);
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
    QVERIFY(QFileInfo::exists(singleSource));
    QVERIFY(QFileInfo::exists(QDir(folderSource).absoluteFilePath(
        QStringLiteral("rtl/packet_router.sv"))));

    search->setText(QStringLiteral("packet_router.sv"));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->model()->index(0, 0).data().toString(),
             QStringLiteral("packet_router"));
    QVERIFY(table->model()
                ->index(0, 0)
                .data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("rtl/packet_router.sv")));

    AssetLibraryService service;
    ScanResult scan = AssetScanner().scan(library);
    QCOMPARE(scan.assets.size(), 2);
    const AssetRecord *single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    QVERIFY(single);
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
    QVERIFY(writeFile(workingFile, secondRevision));
    state = service.workingCopyState(*single);
    QVERIFY(state.changed);
    QCOMPARE(service.suggestedNextVersion(state.latestVersion),
             QStringLiteral("1.0.1"));

    const QString copyDirectory = temporary.filePath(QStringLiteral("project/rtl"));
    QVERIFY(QDir().mkpath(copyDirectory));
    QString copiedPath;
    QVERIFY2(service.copyVersionPayload(*single,
                                        QStringLiteral("1.0.0"),
                                        copyDirectory,
                                        &copiedPath,
                                        &error),
             qPrintable(error));
    QFile copied(copiedPath);
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), firstRevision);
    QVERIFY(!QFileInfo(QDir(copyDirectory).absoluteFilePath(
        QStringLiteral(".xips.json"))).exists());
    QVERIFY2(service.createVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   nullptr,
                                   &error),
             qPrintable(error));

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

    const QString syncedSource = temporary.filePath(
        QStringLiteral("other-device/common_defs.svh"));
    QVERIFY(writeFile(syncedSource, QByteArrayLiteral("`define BUS_WIDTH 32\n")));
    QCOMPARE(service.importAssets(library, {syncedSource}).created.size(), 1);
    search->clear();
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(&window, &activate);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 3, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(hasGroup(groups,
                                      QStringLiteral("Serial"),
                                      QStringLiteral("2")),
                             5000);

    scan = AssetScanner().scan(library);
    single = findAsset(scan, QStringLiteral("uart_rx_sv"));
    QVERIFY(single);
    QVERIFY2(service.deleteVersion(*single,
                                   QStringLiteral("1.0.1"),
                                   VersionDeleteMode::Permanent,
                                   &error),
             qPrintable(error));
    QFile working(workingFile);
    QVERIFY(working.open(QIODevice::ReadOnly));
    QCOMPARE(working.readAll(), secondRevision);
    QCOMPARE(service.versions(single->assetRoot, &error).size(), 1);
}

QTEST_MAIN(UserJourneyTest)

#include "tst_user_journey.moc"
