#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaPushButton.h"
#include "app/BrowserPanel.h"
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QMenu>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
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
    QVERIFY(SnapshotLibrary::update(asset.asset, {file.fileName()}, "board verified").ok);
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
    QCOMPARE(versions->currentData().toString(), QString("2"));
    QVERIFY(take->isEnabled());
    versions->setCurrentIndex(1);
    QCOMPARE(panel.saveState().value("revision").toString(), QString("1"));
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
QTEST_MAIN(GuiSmokeTest)
#include "tst_gui_smoke.moc"
