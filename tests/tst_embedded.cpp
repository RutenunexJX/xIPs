#include "DialogDriver.h"
#include "library/SnapshotLibrary.h"
#include "library/CatalogGroups.h"
#include "xips/BrowserApi.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QSemaphore>
#include <QSettings>
#include <QTabWidget>
#include <QTabBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QMenu>
#include <QScopedValueRollback>
#include <QSignalSpy>
#include <QPersistentModelIndex>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtTest>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace xips;
namespace
{
bool pointerClicks = false;
QJsonArray hitEvidence;
QString caseName;
QRect globalRect(QWidget *widget)
{ return QRect(widget->mapToGlobal(QPoint()), widget->size()); }
void expose(QWidget *widget)
{
    for (auto *parent = widget->parentWidget(); parent && !widget->isWindow(); parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(widget, 4, 4);
    QCoreApplication::processEvents();
}
void reachable(QWidget *widget)
{
    QVERIFY(widget && widget->isVisible());
    expose(widget);
    const auto rect = globalRect(widget);
    for (auto *parent = widget->parentWidget(); parent && !widget->isWindow(); parent = parent->parentWidget())
    {
        if (!globalRect(parent).contains(rect))
        {
            qWarning() << "Clipped control" << widget->objectName() << rect << "parent" << parent->objectName()
                       << globalRect(parent) << "minimum" << widget->minimumSizeHint();
            const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
            if (!shots.isEmpty())
            { QDir().mkpath(shots); widget->window()->grab().save(shots + "/clipped-" + caseName + '-' + widget->objectName() + ".png"); }
        }
        QVERIFY2(globalRect(parent).contains(rect), qPrintable(widget->objectName() + " clipped by " + parent->objectName()));
        if (parent->isWindow()) break;
    }
    if (QGuiApplication::platformName() == "windows")
    {
        auto *hit = QApplication::widgetAt(rect.center());
        if (!hit)
        {
            qWarning() << "Pointer geometry" << rect << "owner" << widget->window()->geometry()
                       << "screen" << widget->screen()->availableGeometry() << "DPR" << widget->devicePixelRatioF()
                       << "top-level" << QApplication::topLevelAt(rect.center());
#ifdef Q_OS_WIN
            RECT native{};
            const auto window = reinterpret_cast<HWND>(widget->window()->winId());
            GetWindowRect(window, &native);
            qWarning() << "Native frame" << native.left << native.top << native.right << native.bottom
                       << "visible" << bool(IsWindowVisible(window));
#endif
            const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
            if (!shots.isEmpty())
            { QDir().mkpath(shots); widget->window()->grab().save(shots + "/pointer-failure-" + widget->objectName() + ".png"); }
        }
        QVERIFY2(hit == widget || (hit && widget->isAncestorOf(hit)), qPrintable(widget->objectName() +
            " cannot receive a pointer click; hit=" + (hit ? QString::fromLatin1(hit->metaObject()->className()) +
            '/' + hit->objectName() : QStringLiteral("none"))));
    }
    if (auto *dialog = qobject_cast<QDialog *>(widget->window()))
    {
        if (!dialog->screen()->availableGeometry().contains(dialog->frameGeometry()))
            qWarning() << "Dialog outside screen" << dialog->objectName() << dialog->frameGeometry()
                       << "available" << dialog->screen()->availableGeometry();
        QVERIFY2(dialog->screen()->availableGeometry().contains(dialog->frameGeometry()), "Dialog outside available screen");
    }
    hitEvidence.append(QJsonObject{{"case", caseName}, {"name", widget->objectName()}, {"width", widget->width()},
        {"height", widget->height()}, {"fullyInsideAncestors", true}, {"pointerHit", true}});
}
void showPage(QWidget *panel, int index)
{
    auto *pages = panel->findChild<QTabWidget *>("assetPages");
    if (!pointerClicks) { pages->setCurrentIndex(index); return; }
    auto *bar = pages->tabBar(); reachable(bar);
    QVERIFY(bar->rect().contains(bar->tabRect(index)));
    QTest::mouseClick(bar, Qt::LeftButton, {}, bar->tabRect(index).center());
    QCOMPARE(pages->currentIndex(), index);
}
void checkFile(QAbstractItemView *view, const QModelIndex &index)
{
    QVERIFY(index.isValid());
    view->scrollTo(index); QCoreApplication::processEvents();
    QStyleOptionViewItem option;
    option.initFrom(view);
    option.rect = view->visualRect(index);
    option.features = QStyleOptionViewItem::HasCheckIndicator;
    option.checkState = Qt::Unchecked;
    const auto point = view->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, view).center();
    for (auto *parent = view->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent))
        {
            const auto contentPoint = view->viewport()->mapTo(scroll->widget(), point);
            scroll->ensureVisible(contentPoint.x(), contentPoint.y(), 12, 16);
        }
    QCoreApplication::processEvents();
    const auto global = view->viewport()->mapToGlobal(point);
    for (auto *parent = view->viewport(); parent; parent = parent->parentWidget())
    {
        QVERIFY2(globalRect(parent).contains(global), "File checkbox is clipped");
        if (parent->isWindow()) break;
    }
    if (QGuiApplication::platformName() == "windows")
        QCOMPARE(QApplication::widgetAt(global), view->viewport());
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(index.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
    hitEvidence.append(QJsonObject{{"case", caseName}, {"name", "workingFileCheckbox"}, {"pointerHit", true}});
}
bool context(QWidget *panel, const QString &library, const QString &workspace = {})
{ return QMetaObject::invokeMethod(panel, "setContext", Q_ARG(QString, library), Q_ARG(QString, workspace)); }
bool busy(QWidget *panel)
{
    bool result = true;
    QMetaObject::invokeMethod(panel, "isCatalogBusy", Q_RETURN_ARG(bool, result));
    return result;
}
QVariantMap state(QWidget *panel)
{
    QVariantMap result;
    QMetaObject::invokeMethod(panel, "saveState", Q_RETURN_ARG(QVariantMap, result));
    return result;
}
bool restore(QWidget *panel, const QVariantMap &value)
{ return QMetaObject::invokeMethod(panel, "restoreState", Q_ARG(QVariantMap, value)); }
void click(QWidget *owner, const char *name)
{
    auto *button = owner->findChild<QAbstractButton *>(name);
    QVERIFY2(button, name);
    QVERIFY2(button->isEnabled(), name);
    if (pointerClicks)
    {
        reachable(button);
        QTest::mouseClick(button, Qt::LeftButton);
    }
    else button->click();
}
void put(const QString &path, const QByteArray &bytes)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
}
QModelIndex fileIndex(QAbstractItemModel *model, const QString &path, const QModelIndex &parent = {})
{
    for (int row = 0; row < model->rowCount(parent); ++row)
    {
        const auto index = model->index(row, 0, parent);
        if (index.data(Qt::UserRole).toString() == path) return index;
        const auto child = fileIndex(model, path, index);
        if (child.isValid()) return child;
    }
    return {};
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
        release.release(); future.waitForFinished();
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(previous);
    }
};
}
class HostBridge final : public QObject
{
    Q_OBJECT
  public:
    QString validationError, receiptError;
    QList<QVariantMap> receipts;
    Q_INVOKABLE QString destinationError(const QString &) { return validationError; }
    Q_INVOKABLE QString exportCompleted(const QVariantMap &receipt)
    { receipts.append(receipt); return receiptError; }
    Q_INVOKABLE QStringList collectionSources() { return {}; }
};

class EmbeddedTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir settings;
    QLibrary component, hostEla;
    XipsCreateBrowserV1 create = nullptr;
    QSize hostSize{920, 620};
    QWidget *panelIn(QWidget *owner, QObject *host)
    {
        auto *panel = create(owner, host);
        if (!panel) return nullptr;
        if (!owner->layout()) new QVBoxLayout(owner);
        owner->layout()->setContentsMargins(0, 0, 0, 0);
        owner->layout()->setSpacing(0);
        owner->layout()->setSizeConstraint(QLayout::SetNoConstraint);
        owner->layout()->addWidget(panel);
        owner->setFixedSize(hostSize); owner->show();
#ifdef Q_OS_WIN
        // STARTUPINFO's SW_HIDE can suppress the process's first ShowWindow
        // despite QWidget::isVisible(). Show this test-owned window again.
        if (QGuiApplication::platformName() == "windows" &&
            !IsWindowVisible(reinterpret_cast<HWND>(owner->winId())))
        { owner->hide(); owner->show(); }
#endif
        QCoreApplication::processEvents();
        return panel;
    }
  private slots:
    void initTestCase()
    {
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const auto hostPath = qEnvironmentVariable("XIPS_TEST_HOST_ELA");
        if (!hostPath.isEmpty())
        {
            hostEla.setFileName(hostPath);
            hostEla.setLoadHints(QLibrary::PreventUnloadHint);
            QVERIFY2(hostEla.load(), qPrintable(hostEla.errorString()));
        }
        component.setFileName(qEnvironmentVariable("XIPS_TEST_BROWSER", QString::fromUtf8(XIPS_BROWSER_PATH)));
        component.setLoadHints(QLibrary::PreventUnloadHint);
        QVERIFY2(component.load(), qPrintable(component.errorString()));
        const auto abi = reinterpret_cast<XipsBrowserAbiV1>(component.resolve("xips_browser_abi_v1"));
        create = reinterpret_cast<XipsCreateBrowserV1>(component.resolve("xips_create_browser_v1"));
        QVERIFY(abi && create); QCOMPARE(QByteArray(abi()), xipsExpectedBrowserAbi());
    }
    void missingImplementationReturnsDiagnostic()
    {
        QTemporaryDir tmp;
        const auto isolated = tmp.filePath("isolated-entry.dll");
        QVERIFY(QFile::copy(component.fileName(), isolated));
        QLibrary entry(isolated); QVERIFY2(entry.load(), qPrintable(entry.errorString()));
        const auto factory = reinterpret_cast<XipsCreateBrowserV1>(entry.resolve("xips_create_browser_v1"));
        const auto error = reinterpret_cast<XipsBrowserLastErrorV1>(entry.resolve("xips_browser_last_error_v1"));
        QVERIFY(factory && error);
        QVERIFY(!factory(nullptr, nullptr));
        QVERIFY(QByteArray(error()).contains("xips-browser-impl"));
        QVERIFY(entry.unload());
    }
    void factoryPreservesHostAndRejectsWorkerThread()
    {
        QCoreApplication::setOrganizationName("EmbeddingHost");
        QCoreApplication::setApplicationName("HostApp");
        QCoreApplication::setApplicationVersion("9.1");
        QFont font("Consolas", 17); qApp->setFont(font);
        const auto palette = qApp->palette();
        const auto style = qApp->style();
        const bool siblings = qApp->testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
        const bool quit = qApp->quitOnLastWindowClosed();
        QWidget owner; owner.setWindowTitle("Host project");
        HostBridge host;
        auto *panel = panelIn(&owner, &host); QVERIFY(panel);
#ifdef Q_OS_WIN
        QVERIFY(GetModuleHandleW(L"XipsEla.dll"));
        if (hostEla.isLoaded()) QVERIFY(GetModuleHandleW(L"ElaWidgetTools.dll"));
#endif
        QCOMPARE(panel->parentWidget(), &owner); QVERIFY(!panel->isWindow());
        QCOMPARE(qApp->font(), font); QCOMPARE(qApp->palette(), palette); QCOMPARE(qApp->style(), style);
        QCOMPARE(qApp->testAttribute(Qt::AA_DontCreateNativeWidgetSiblings), siblings);
        QCOMPARE(qApp->quitOnLastWindowClosed(), quit);
        QCOMPARE(QCoreApplication::applicationName(), QString("HostApp"));
        QCOMPARE(QCoreApplication::organizationName(), QString("EmbeddingHost"));
        QCOMPARE(QCoreApplication::applicationVersion(), QString("9.1"));
        QCOMPARE(owner.windowTitle(), QString("Host project"));
        QCOMPARE(panel->findChild<QAbstractItemView *>("assetList")->font().pixelSize(), 13);
        QCOMPARE(panel->findChild<QTabWidget *>("assetPages")->tabBar()->font().pixelSize(), 13);
        QCOMPARE(panel->findChild<QAbstractButton *>("addFilesButton")->font().pixelSize(), 13);
        QCOMPARE(panel->findChild<QAbstractButton *>("addFolderButton")->font().pixelSize(), 13);
        QCOMPARE(panel->findChild<QAbstractButton *>("deleteRevisionButton")->font().pixelSize(), 13);
        qInfo() << "Native host probe" << QGuiApplication::platformName() << "DPR" << panel->devicePixelRatioF();
        QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, true)));
        QVERIFY(panel->palette().color(QPalette::Window).lightness() < 128);
        QCOMPARE(qApp->palette(), palette);
        QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, false)));
        auto worker = QtConcurrent::run([this] { return create(nullptr, nullptr); });
        worker.waitForFinished(); QVERIFY(!worker.result());
        QPointer<QWidget> guard(panel); delete panel; QVERIFY(!guard);
        QVERIFY(owner.isVisible());
    }
    void fullEmbeddedWorkingAndVersionFlow_data()
    {
        QTest::addColumn<int>("hostWidth"); QTest::addColumn<int>("hostHeight");
        QTest::newRow("280") << 280 << 760;
        QTest::newRow("520") << 520 << 760;
        QTest::newRow("960") << 960 << 760;
        QTest::newRow("280-short") << 280 << 380;
    }
    void narrowLayoutStateAndModelIdentity();
    void fullEmbeddedWorkingAndVersionFlow()
    {
        QFETCH(int, hostWidth); QFETCH(int, hostHeight);
        const QScopedValueRollback<bool> pointerGuard(pointerClicks, true);
        const QScopedValueRollback<QSize> sizeGuard(hostSize, QSize(hostWidth, hostHeight));
        caseName = QString::fromLatin1(QTest::currentDataTag());
        QTemporaryDir tmp; const auto root = tmp.filePath("library"), workspace = tmp.filePath("project");
        QVERIFY(QDir().mkpath(root)); QVERIFY(QDir().mkpath(workspace));
        QWidget owner; HostBridge host;
        auto *panel = panelIn(&owner, &host); QVERIFY(panel);
        QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, false)));
        const auto screen = owner.screen()->availableGeometry();
        const QPoint framePosition(screen.right() - owner.frameGeometry().width() - 15,
                                   screen.bottom() - owner.frameGeometry().height() - 15);
        owner.move(owner.pos() + framePosition - owner.frameGeometry().topLeft());
        owner.raise(); owner.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&owner));
        QTest::qWait(50);
        QCOMPARE(panel->width(), hostWidth);
        reachable(panel->findChild<QWidget *>("assetSearch"));
        // Exercise the component's own library picker, without writing host settings.
        whenVisible(&owner, "xipsFilePicker", [root](QWidget *form)
        { form->findChild<QLineEdit *>("pickerPath")->setText(root); click(form, "pickerAccept"); });
        click(panel, "folderButton"); QTRY_VERIFY(!busy(panel));
        QCOMPARE(state(panel)["library"].toString(), root);
        QVERIFY(context(panel, {}, workspace));
        click(panel, "filterButton");
        reachable(panel->findChild<QWidget *>("typeCombo"));
        reachable(panel->findChild<QWidget *>("indexCombo"));
        click(panel, "clearFiltersButton"); click(panel, "filterButton");
        bool menuOpened = false;
        QTimer menuTimer;
        connect(&menuTimer, &QTimer::timeout, &owner, [&]
        {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->isVisible())
                { menuOpened = true; QTest::keyClick(menu, Qt::Key_Escape); }
        });
        menuTimer.start(10);
        click(panel, "collectButton"); menuTimer.stop(); QVERIFY(menuOpened);
        click(panel, "refreshButton"); QTRY_VERIFY(!busy(panel));
        whenVisible(&owner, "groupName", [](QWidget *field)
        { qobject_cast<QLineEdit *>(field)->setText("AXI"); click(field->window(), "formAccept"); });
        click(panel, "newGroupButton"); QTRY_VERIFY(!busy(panel));
        const auto groupState = state(panel); QVERIFY(!groupState["groupId"].toString().isEmpty());
        for (const auto &kind : {QString("module"), QString("ip")})
        {
            QVERIFY(restore(panel, groupState));
            whenVisible(&owner, "newAssetName", [kind](QWidget *field)
            {
                qobject_cast<QLineEdit *>(field)->setText(kind + "_empty");
                auto *type = field->window()->findChild<QComboBox *>("newAssetType");
                type->setCurrentIndex(type->findData(kind)); click(field->window(), "formAccept");
            });
            click(panel, "newAssetButton"); QTRY_VERIFY(!busy(panel));
            auto *versions = panel->findChild<QAbstractItemView *>("revisionTable");
            QCOMPARE(versions->model()->rowCount(), 0);
            QCOMPARE(panel->findChild<QAbstractItemView *>("workingFiles")->model()->rowCount(), 0);
        }
        const auto ipState = state(panel); const auto assetId = ipState["assetId"].toString();
        QVERIFY(restore(panel, groupState));
        auto *members = panel->findChild<QAbstractItemView *>("groupMembers");
        QCOMPARE(members->model()->rowCount(members->rootIndex()), 2);
        members->setCurrentIndex(members->model()->index(0, 0, members->rootIndex()));
        QTest::keyClick(members, Qt::Key_Return);
        QVERIFY(!state(panel)["assetId"].toString().isEmpty());
        QVERIFY(restore(panel, ipState));
        put(tmp.filePath("external/top.sv"), "module top; endmodule");
        put(tmp.filePath("external/rtl/sub/helper.sv"), "module helper; endmodule");
        put(tmp.filePath("external/settings/config.txt"), "settings");
        whenVisible(&owner, "xipsFilePicker", [&tmp](QWidget *form)
        {
            auto *path = form->findChild<QLineEdit *>("pickerPath");
            path->selectAll(); QTest::keyClicks(path, tmp.filePath("external/top.sv"));
            click(form, "pickerAccept");
        });
        click(panel, "addFilesButton"); QTRY_VERIFY(!busy(panel));
        whenVisible(&owner, "xipsFilePicker", [&tmp](QWidget *form)
        { form->findChild<QLineEdit *>("pickerPath")->setText(tmp.filePath("external/settings")); click(form, "pickerAccept"); });
        click(panel, "addFolderButton"); QTRY_VERIFY(!busy(panel));
        auto *working = panel->findChild<QAbstractItemView *>("workingFiles");
        expose(working);
        const auto dropPoint = working->viewport()->rect().center();
        const auto dropGlobal = working->viewport()->mapToGlobal(dropPoint);
        for (auto *parent = working->viewport(); parent; parent = parent->parentWidget())
        {
            QVERIFY2(globalRect(parent).contains(dropGlobal), "Working drop target is clipped");
            if (parent->isWindow()) break;
        }
        if (QGuiApplication::platformName() == "windows")
            QCOMPARE(QApplication::widgetAt(dropGlobal), working->viewport());
        hitEvidence.append(QJsonObject{{"case", caseName}, {"name", "workingDropTarget"}, {"pointerHit", true}});
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(tmp.filePath("external/rtl"))});
        QDragEnterEvent enter(dropPoint, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(working->viewport(), &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(dropPoint, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(working->viewport(), &drop); QVERIFY(drop.isAccepted()); QTRY_VERIFY(!busy(panel));
        auto *model = working->model();
        QVERIFY(fileIndex(model, "top.sv").isValid()); QVERIFY(fileIndex(model, "rtl/sub/helper.sv").isValid());
        QVERIFY(fileIndex(model, "settings/config.txt").isValid());
        checkFile(working, fileIndex(model, "rtl/sub/helper.sv"));
        auto checkedState = state(panel);
        showPage(panel, 1);
        const auto emptyHistoryState = state(panel);
        delete panel; panel = panelIn(&owner, &host); QVERIFY(panel);
        QVERIFY(context(panel, {}, workspace)); QVERIFY(restore(panel, emptyHistoryState)); QTRY_VERIFY(!busy(panel));
        QCOMPARE(panel->findChild<QTabWidget *>("assetPages")->currentIndex(), 1);
        QCOMPARE(state(panel)["workingChecks"], checkedState["workingChecks"]);
        QVERIFY(restore(panel, checkedState));
        whenVisible(&owner, "payloadReviewForm", [](QWidget *form) { click(form, "formAccept"); });
        click(panel, "updateButton"); QTRY_VERIFY(!busy(panel));
        showPage(panel, 0); showPage(panel, 1);
        auto *versions = panel->findChild<QAbstractItemView *>("revisionTable");
        QCOMPARE(versions->model()->rowCount(), 1);
        QCOMPARE(versions->model()->index(0, 1).data().toString(), QString("Archived"));
        const auto saved = SnapshotLibrary::scan(root); QCOMPARE(saved.assets.size(), 2);
        const auto asset = *std::find_if(saved.assets.begin(), saved.assets.end(), [&](const auto &a) { return a.id == assetId; });
        QCOMPARE(SnapshotLibrary::verifySnapshot(asset, state(panel)["revision"].toString()).snapshot.files,
                 QStringList{"rtl/sub/helper.sv"});
        const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!shots.isEmpty())
        {
            QDir().mkpath(shots); QTest::qWait(50);
            QVERIFY(owner.grab().save(shots + "/embedded-versions-" + caseName + ".png"));
            QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, true)));
            QTest::qWait(50);
            QVERIFY(owner.grab().save(shots + "/embedded-versions-dark-" + caseName + ".png"));
            QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, false)));
        }
        host.validationError = "Host rejected this destination";
        auto exportTo = [&](const QString &target)
        {
            whenVisible(&owner, "exportDestination", [target](QWidget *field)
            { qobject_cast<QLineEdit *>(field)->setText(target); click(field->window(), "formAccept"); });
            click(panel, "takeButton");
        };
        const auto rejected = workspace + "/rejected.sv";
        exportTo(rejected); QTRY_VERIFY(!busy(panel)); QVERIFY(!QFileInfo::exists(rejected));
        QVERIFY(panel->findChild<QWidget *>("browserNotice")->property("error").toBool());
        host.validationError.clear(); host.receiptError = "Receipt store unavailable";
        const auto copied = workspace + "/copied.sv";
        exportTo(copied); QTRY_VERIFY(!busy(panel)); QVERIFY(QFileInfo::exists(copied));
        QCOMPARE(host.receipts.size(), 1); QCOMPARE(host.receipts.first()["workspace"].toString(), workspace);
        QVERIFY(panel->findChild<QWidget *>("browserNotice")->property("error").toBool());
        whenVisible(&owner, "deleteRevisionForm", [](QWidget *form) { qobject_cast<QDialog *>(form)->reject(); });
        click(panel, "deleteRevisionButton"); QCOMPARE(versions->model()->rowCount(), 1);
        whenVisible(&owner, "deleteRevisionForm", [](QWidget *form) { click(form, "formAccept"); });
        click(panel, "deleteRevisionButton"); QTRY_VERIFY(!busy(panel)); QCOMPARE(versions->model()->rowCount(), 0);
        QVERIFY(QFileInfo::exists(asset.root + "/top.sv"));
        QVERIFY(QMetaObject::invokeMethod(panel, "refresh")); QTRY_VERIFY(!busy(panel));
        QCOMPARE(state(panel)["workingChecks"], checkedState["workingChecks"]);
        if (!shots.isEmpty())
        {
            QDir().mkpath(shots);
            showPage(panel, 0);
            QTest::qWait(100);
            QVERIFY(owner.grab().save(shots + "/embedded-working-" + caseName + ".png"));
            QVERIFY(QMetaObject::invokeMethod(panel, "setDarkTheme", Q_ARG(bool, true)));
            QTest::qWait(100); QVERIFY(owner.grab().save(shots + "/embedded-working-dark-" + caseName + ".png"));
        }
    }
    void cleanupTestCase()
    {
        const auto path = qEnvironmentVariable("XIPS_LAYOUT_EVIDENCE");
        if (path.isEmpty()) return;
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(hitEvidence).toJson());
    }
    void queuedContextsRestoreLatestStateAndCancel()
    {
        QTemporaryDir tmp;
        const auto a = tmp.filePath("a"), b = tmp.filePath("b");
        QVERIFY(QDir().mkpath(a)); QVERIFY(QDir().mkpath(b));
        CatalogDefinition def; def.name = "first"; QVERIFY(SnapshotLibrary::create(a, def).ok);
        def.name = "second"; auto second = SnapshotLibrary::create(b, def); QVERIFY(second.ok);
        put(second.asset.root + "/source.sv", "second");
        QWidget owner; HostBridge host; auto *panel = panelIn(&owner, &host); QVERIFY(panel);
        const QVariantMap wanted{{"library", b}, {"assetId", second.asset.id}, {"query", "second"}, {"page", 0},
            {"workingChecks", QVariantMap{{second.asset.id, QStringList{"source.sv"}}}}};
        {
            PoolGate gate;
            QVERIFY(context(panel, a, tmp.filePath("workspace-one"))); QVERIFY(busy(panel));
            QVERIFY(context(panel, b, tmp.filePath("workspace-two"))); QVERIFY(restore(panel, wanted));
            QCOMPARE(state(panel), wanted);
            click(panel, "cancelOperation");
        }
        QTRY_VERIFY(!busy(panel));
        QCOMPARE(state(panel)["library"].toString(), b);
        QCOMPARE(state(panel)["assetId"].toString(), second.asset.id);
        QCOMPARE(state(panel)["query"].toString(), QString("second"));
        QCOMPARE(state(panel)["workingChecks"], wanted["workingChecks"]);
        const auto before = state(panel);
        QVERIFY(context(panel, {}, {})); // Workspace closed, library remains selected.
        QVERIFY(restore(panel, {})); QCOMPARE(state(panel), before);
        panel->hide(); panel->show(); QCOMPARE(state(panel), before);
        // A workspace switch rejects an open editing dialog before it can publish.
        whenVisible(&owner, "newAssetName", [panel](QWidget *) { context(panel, {}, "new-workspace"); });
        click(panel, "newAssetButton"); QCOMPARE(SnapshotLibrary::scan(b).assets.size(), 1);
    }
    void destructionDuringScanAndReview()
    {
        QTemporaryDir tmp; const auto root = tmp.filePath("library"); QVERIFY(QDir().mkpath(root));
        CatalogDefinition def; def.name = "closing"; auto asset = SnapshotLibrary::create(root, def); QVERIFY(asset.ok);
        put(asset.asset.root + "/top.sv", "closing");
        HostBridge host;
        {
            PoolGate gate;
            auto *owner = new QWidget;
            QPointer<QWidget> panel = panelIn(owner, &host); QVERIFY(panel);
            QVERIFY(context(panel, root)); QVERIFY(busy(panel)); delete owner; QVERIFY(!panel);
        }
        auto *owner = new QWidget;
        QPointer<QWidget> panel = panelIn(owner, &host); QVERIFY(panel);
        QVERIFY(context(panel, root)); QTRY_VERIFY(!busy(panel));
        click(panel, "checkAllFiles");
        whenVisible(owner, "payloadReviewForm", [owner](QWidget *) { delete owner; });
        click(panel, "updateButton"); QTRY_VERIFY(!panel);
        QCOMPARE(SnapshotLibrary::scan(root).assets.first().snapshots.size(), 1); // current only
        QWidget lastOwner; auto *connection = new HostBridge;
        panel = panelIn(&lastOwner, connection); QVERIFY(context(panel, root)); QTRY_VERIFY(!busy(panel));
        delete connection;
        QVERIFY(!panel->findChild<QWidget *>("saveReceiptButton")->isVisible());
        QVERIFY(lastOwner.isVisible());
    }
};
#include "NarrowLayoutGui.inc"
QTEST_MAIN(EmbeddedTest)
#include "tst_embedded.moc"
