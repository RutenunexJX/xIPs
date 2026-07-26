#include "app/MainWindow.h"
#include "library/AssetLibraryService.h"
#include "manifest/ManifestService.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
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

} // namespace

class GuiSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void firstScreenIsACompactIpLibrary();
    void binaryFilesRemainInventoryEntries();
    void activationSelectsAnIp();
};

void GuiSmokeTest::firstScreenIsACompactIpLibrary()
{
    MainWindow window(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *table = window.findChild<QTableView *>(QStringLiteral("assetTable"));
    auto *search = window.findChild<QLineEdit *>(QStringLiteral("searchEdit"));
    auto *tags = window.findChild<QComboBox *>(QStringLiteral("tagFilter"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("detailTabs"));
    auto *files = window.findChild<QTreeWidget *>(QStringLiteral("fileTree"));
    auto *versions = window.findChild<QTreeWidget *>(QStringLiteral("versionTree"));
    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("sourcePreview"));
    QVERIFY(table);
    QVERIFY(search);
    QVERIFY(!tags);
    QVERIFY(tabs);
    QVERIFY(files);
    QVERIFY(versions);
    QVERIFY(!preview);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(table->model()->columnCount(), 5);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 3, 10000);
    QVERIFY(table->currentIndex().isValid());

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

void GuiSmokeTest::activationSelectsAnIp()
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
    QTRY_COMPARE_WITH_TIMEOUT(search->text(), QStringLiteral("reset_gen"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 3000);
    QCOMPARE(table->currentIndex()
                 .siblingAtColumn(AssetTableModel::NameColumn)
                 .data()
                 .toString(),
             QStringLiteral("Reset Generator"));
}

QTEST_MAIN(GuiSmokeTest)

#include "tst_gui_smoke.moc"
