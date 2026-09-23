#include "app/BrowserPanel.h"
#include "app/UiSupport.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaMenu.h"
#include "ElaProgressRing.h"
#include "ElaPushButton.h"
#include "ElaScrollBar.h"
#include "ElaTheme.h"
#include "ElaToolTip.h"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QHelpEvent>
#include <QPainter>
#include <QSemaphore>
#include <QSplitter>
#include <QStringListModel>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtTest>

using namespace xips;
namespace
{
SnapshotResult collect(const QString &library, const QString &name)
{
    QDir().mkpath(library);
    const QString source = QDir(library).absoluteFilePath("../" + name + ".sv");
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write("module example; endmodule\n");
    file.close();
    return SnapshotLibrary::collect(library, {source}, name, "module");
}
struct PoolGate
{
    int previous = QThreadPool::globalInstance()->maxThreadCount();
    QSemaphore entered, release;
    QFuture<void> future;
    PoolGate()
    {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        future = QtConcurrent::run([this] { entered.release(); release.acquire(); });
        entered.acquire();
    }
    ~PoolGate()
    {
        release.release();
        future.waitForFinished();
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(previous);
    }
};
ElaMenu *editMenu(ElaLineEdit *editor)
{
    QContextMenuEvent event(QContextMenuEvent::Keyboard, QPoint(4, 4), editor->mapToGlobal(QPoint(4, 4)));
    QApplication::sendEvent(editor, &event);
    return editor->findChild<ElaMenu *>("xipsEditMenu");
}
QAction *action(ElaMenu *menu, const QString &text)
{
    for (auto *candidate : menu->actions())
        if (candidate->text() == text)
            return candidate;
    return nullptr;
}
void dragSplitter(QSplitter *split, int distance)
{
    auto *handle = split->handle(1);
    const auto start = handle->rect().center();
    const auto target = start + (split->orientation() == Qt::Horizontal ? QPoint(distance, 0) : QPoint(0, distance));
    QTest::mousePress(handle, Qt::LeftButton, {}, start);
    QMouseEvent move(QEvent::MouseMove, target, handle->mapToGlobal(target),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, {}, handle->rect().center());
}
}
class Interactions final : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { initializeEla(); }
    void comboInterruptsAndDestroys()
    {
        QWidget window;
        window.resize(450, 300);
        auto *combo = new ElaComboBox(&window);
        combo->addItems({"rev1", "rev2", "rev3", "rev4"});
        combo->setGeometry(20, 20, 200, 35);
        window.show();
        for (int i = 0; i < 12; ++i)
        {
            static_cast<QComboBox *>(combo)->showPopup();
            QVERIFY(combo->isPopupAnimating());
            QTest::keyClick(combo->view(), Qt::Key_Escape);
            QVERIFY(!combo->isPopupAnimating());
            QVERIFY(!combo->view()->window()->isVisible());
        }
        static_cast<QComboBox *>(combo)->showPopup();
        QTest::keyClick(combo->view(), Qt::Key_Down);
        QTest::keyClick(combo->view(), Qt::Key_Return);
        QCOMPARE(combo->currentIndex(), 1);
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            static_cast<QComboBox *>(combo)->showPopup();
            combo->resize(230, 35);
            combo->finishPopupAnimation();
            QVERIFY(!combo->isPopupAnimating());
            combo->hide();
            QVERIFY(!combo->view()->window()->isVisible());
            combo->show();
        }
        static_cast<QComboBox *>(combo)->showPopup();
        delete combo;
        QTest::qWait(220);
    }
    void precisionWheelAndInputCancel()
    {
        ElaListView list;
        QStringList rows;
        for (int i = 0; i < 300; ++i)
            rows.append(QString::number(i));
        QStringListModel model(rows);
        list.setModel(&model);
        enableSmoothScrolling(&list);
        list.resize(400, 400);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        list.setCurrentIndex(model.index(10));
        list.setFocus();
        auto *bar = qobject_cast<ElaScrollBar *>(list.verticalScrollBar());
        QVERIFY(bar && bar->smoothWheelEnabled());
        QTRY_VERIFY(bar->maximum() > 1000);
        bar->setValue(200);
        auto wheel = [&](QPoint pixels, QPoint angles) {
            QWheelEvent event(QPointF(20, 20), list.viewport()->mapToGlobal(QPoint(20, 20)),
                pixels, angles, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(list.viewport(), &event);
        };
        wheel(QPoint(0, -37), {});
        QCOMPARE(bar->value(), 237);
        wheel({}, QPoint(0, -120));
        QTRY_VERIFY(bar->value() > 237);
        wheel({}, QPoint(0, 120));
        QTest::keyClick(&list, Qt::Key_Home);
        QTest::qWait(200);
        QCOMPARE(bar->value(), 0);
        wheel({}, QPoint(0, -120));
        bar->setValue(120);
        list.hide();
        QTest::qWait(200);
        QCOMPARE(bar->value(), 120);
    }
    void englishEditingAndOwnerDestruction()
    {
        auto *panel = new BrowserPanel;
        panel->show();
        auto *search = panel->findChild<ElaLineEdit *>("assetSearch");
        search->setFocus();
        QTest::keyClicks(search, "alpha");
        search->selectAll();
        auto *menu = editMenu(search);
        QVERIFY(menu);
        QVERIFY(action(menu, "Undo")->isEnabled());
        QVERIFY(action(menu, "Copy")->isEnabled());
        action(menu, "Delete")->trigger();
        menu->close();
        QCOMPARE(search->text(), QString());
        QTest::keyClick(search, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(search->text(), QString("alpha"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        search->setReadOnly(true);
        search->selectAll();
        menu = editMenu(search);
        QVERIFY(!action(menu, "Cut")->isEnabled());
        QVERIFY(action(menu, "Copy")->isEnabled());
        action(menu, "Copy")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), QString("alpha"));
        QPointer<ElaMenu> guard(menu);
        delete panel;
        QVERIFY(guard.isNull());
        QTest::qWait(220);
    }
    void splitterRestoresBothOrientations()
    {
        BrowserPanel panel;
        panel.resize(1000, 700);
        panel.show();
        auto *split = panel.findChild<QSplitter *>("assetSplitter");
        QVERIFY(split);
        dragSplitter(split, 120);
        const double horizontal = panel.saveState()["horizontalRatio"].toDouble();
        panel.resize(450, 800);
        QCOMPARE(split->orientation(), Qt::Vertical);
        dragSplitter(split, -60);
        const double vertical = panel.saveState()["verticalRatio"].toDouble();
        panel.resize(1000, 700);
        QVERIFY(qAbs(panel.saveState()["horizontalRatio"].toDouble() - horizontal) < 0.025);
        const auto saved = panel.saveState();
        BrowserPanel restored;
        restored.resize(1000, 700);
        restored.restoreState(saved);
        restored.show();
        QVERIFY(qAbs(restored.saveState()["horizontalRatio"].toDouble() - horizontal) < 0.025);
        restored.resize(450, 800);
        QVERIFY(qAbs(restored.saveState()["verticalRatio"].toDouble() - vertical) < 0.025);
    }
    void catalogSelectionDoesNotWaitForIo()
    {
        QTemporaryDir fixture;
        const auto root = fixture.filePath("library");
        QVERIFY(collect(root, "first").ok);
        const auto last = collect(root, "last");
        QVERIFY(last.ok);
        BrowserPanel panel;
        panel.setContext(root, {});
        QTRY_VERIFY(!panel.isCatalogBusy());
        auto *list = panel.findChild<ElaListView *>("assetList");
        auto *take = panel.findChild<ElaPushButton *>("takeButton");
        QCOMPARE(list->model()->rowCount(), 2);
        PoolGate gate;
        panel.revealAsset(last.asset.id);
        QCOMPARE(panel.saveState()["assetId"].toString(), last.asset.id);
        QCOMPARE(panel.saveState()["revision"].toString(), QString("1"));
        QVERIFY(take->isEnabled());
        QSignalSpy resets(list->model(), &QAbstractItemModel::modelReset);
        panel.restoreState({{"query", ".sv"}, {"assetId", last.asset.id}, {"revision", "1"}});
        QCOMPARE(resets.size(), 0);
        QCOMPARE(panel.saveState()["assetId"].toString(), last.asset.id);
    }
    void queuedContextAndRealActivity()
    {
        QTemporaryDir fixture;
        const auto first = fixture.filePath("one"), second = fixture.filePath("two");
        QVERIFY(collect(first, "first").ok);
        const auto last = collect(second, "last");
        QVERIFY(last.ok);
        BrowserPanel panel;
        panel.resize(900, 700);
        panel.show();
        {
            PoolGate gate;
            panel.setContext(first, {});
            auto *activity = panel.findChild<ElaProgressRing *>("browserActivity");
            QVERIFY(activity && activity->isVisible() && activity->getIsBusying());
            panel.setContext(second, fixture.path());
            panel.hide();
            QVERIFY(!activity->getIsBusying());
            panel.show();
            QVERIFY(activity->getIsBusying());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!panel.isCatalogBusy(), 10000);
        QTRY_COMPARE(panel.saveState()["assetId"].toString(), last.asset.id);
        QVERIFY(!panel.findChild<ElaProgressRing *>("browserActivity")->isVisible());
        {
            PoolGate gate;
            auto *closing = new BrowserPanel;
            closing->setContext(first, {});
            delete closing;
        }
        QCoreApplication::processEvents();
    }
    void legacyReadsKeepOnlyTheLatestSelection()
    {
        BrowserPanel panel;
        panel.setContext(QString::fromUtf8(XIPS_EXAMPLE_LIBRARY), {});
        QTRY_VERIFY(!panel.isCatalogBusy());
        auto *list = panel.findChild<ElaListView *>("assetList");
        auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
        QVERIFY(list->model()->rowCount() >= 2);
        QTRY_VERIFY(versions->count() > 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QString selectedId;
        {
            PoolGate gate;
            for (int i = 0; i < 30; ++i)
                list->setCurrentIndex(list->model()->index(i % list->model()->rowCount(), 0));
            selectedId = panel.saveState()["assetId"].toString();
            QVERIFY(panel.findChildren<QFutureWatcherBase *>().size() <= 1);
        }
        QTRY_VERIFY(!panel.saveState()["revision"].toString().isEmpty());
        QCOMPARE(panel.saveState()["assetId"].toString(), selectedId);
        QVERIFY(versions->count() > 0);
    }
    void formClosesWhenEmbeddedOwnerDies()
    {
        QTemporaryDir fixture;
        const auto root = fixture.filePath("library");
        const auto asset = collect(root, "source");
        QVERIFY(asset.ok);
        auto *panel = new BrowserPanel;
        panel->setContext(root, {});
        QTRY_VERIFY(!panel->isCatalogBusy());
        QPointer<BrowserPanel> guard(panel);
        QTimer::singleShot(0, [panel] { delete panel; });
        panel->collectPaths({fixture.filePath("source.sv")});
        QVERIFY(guard.isNull());
        QCOMPARE(SnapshotLibrary::scan(root).assets.size(), 1);
    }
    void toolTipDoesNotTakeFocusAndClosesOnInput()
    {
        QWidget owner;
        owner.setToolTip(QStringLiteral("A library path"));
        enableToolTip(&owner);
        owner.show();
        auto *focused = QApplication::focusWidget();
        QHelpEvent help(QEvent::ToolTip, QPoint(4, 4), owner.mapToGlobal(QPoint(4, 4)));
        QApplication::sendEvent(&owner, &help);
        ElaToolTip *tip = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<ElaToolTip *>(widget); candidate && candidate->isVisible())
                tip = candidate;
        QVERIFY(tip);
        QCOMPARE(QApplication::focusWidget(), focused);
        QTest::keyClick(&owner, Qt::Key_Escape);
        QVERIFY(!tip->isVisible());
    }
    void listBackgroundTracksHostTheme()
    {
        BrowserPanel panel;
        panel.resize(1000, 700);
        panel.show();
        for (const auto mode : {ElaThemeType::Light, ElaThemeType::Dark})
        {
            eTheme->setThemeMode(mode);
            QCoreApplication::processEvents();
            const auto overlay = eTheme->getThemeColor(mode, ElaThemeType::WindowCentralStackBase);
            QImage reference(1, 1, QImage::Format_ARGB32_Premultiplied);
            reference.fill(eTheme->getThemeColor(mode, ElaThemeType::WindowBase));
            { QPainter painter(&reference); painter.fillRect(reference.rect(), overlay); }
            const auto expected = reference.pixelColor(0, 0);
            for (const auto &name : {"assetList", "fileList"})
            {
                auto *view = panel.findChild<ElaListView *>(name);
                const auto pixels = view->viewport()->grab().toImage();
                const auto actual = pixels.pixelColor(pixels.width() / 2, pixels.height() / 2);
                QVERIFY(qAbs(actual.red() - expected.red()) <= 1);
                QVERIFY(qAbs(actual.green() - expected.green()) <= 1);
                QVERIFY(qAbs(actual.blue() - expected.blue()) <= 1);
            }
        }
    }
};
QTEST_MAIN(Interactions)
#include "tst_interactions.moc"
