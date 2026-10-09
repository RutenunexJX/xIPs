#include "xips/BrowserApi.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QLabel>
#include <QLibrary>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QPointer>
#include <memory>
#ifdef XIPS_ARCHIVE_STANDALONE
#include "app/BrowserPanel.h"
#include "app/MainWindow.h"
#endif

namespace {
bool busy(QWidget* panel) {
    bool value = false;
    QMetaObject::invokeMethod(panel, "isCatalogBusy", Q_RETURN_ARG(bool, value));
    return value;
}
QVariantMap viewState(QWidget* panel) {
    QVariantMap result;
    QMetaObject::invokeMethod(panel, "saveState", Q_RETURN_ARG(QVariantMap, result));
    return result;
}
QString settingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/xIPs/archive.ini";
}
void put(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) qFatal("fixture write failed");
}
QByteArray get(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
QMap<QString, QByteArray> snapshot(const QString& root) {
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        result.insert(QDir(root).relativeFilePath(path), QCryptographicHash::hash(get(path), QCryptographicHash::Sha256).toHex()
            + QByteArray::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()));
    }
    return result;
}
QString fixture(const QString& root) {
    put(root + "/demo.srcs/top.v", "module top(); endmodule\n");
    put(root + "/demo.runs/impl_1/top.bit", "registered-bit");
    const auto xpr = root + "/demo.xpr";
    put(xpr, QString("<!-- Product Version: Vivado v2023.1 (64-bit) -->"
        "<Project Product=\"Vivado\" Version=\"7\" Path=\"%1\"><Configuration/>"
        "<FileSets><FileSet Name=\"sources_1\" Type=\"DesignSrcs\" RelSrcDir=\"$PSRCDIR/sources_1\"><File Path=\"$PSRCDIR/top.v\"/></FileSet></FileSets>"
        "<Runs><Run Id=\"synth_1\" Type=\"Ft3:Synth\" SrcSet=\"sources_1\"/>"
        "<Run Id=\"impl_1\" Type=\"Ft2:EntireDesign\" SynthRun=\"synth_1\" Dir=\"$PRUNDIR/impl_1\"/></Runs></Project>").arg(xpr).toUtf8());
    return xpr;
}
QString fake(const QString& root) {
    const auto launcher = root + "/vivado.bat";
    put(launcher, ("@echo off\r\nset \"PATH=" + QDir::toNativeSeparators(QLibraryInfo::path(QLibraryInfo::BinariesPath))
        + ";" + QDir::toNativeSeparators(QString(FTB_TEST_COMPILER_BIN)) + ";%PATH%\"\r\n\""
        + QDir::toNativeSeparators(QString(XIPS_TEST_HELPER_BIN) + "/xips_fake_vivado.exe") + "\"\r\nexit /b %errorlevel%\r\n").toLocal8Bit());
    return launcher;
}
void seed(const QString& xpr, const QString& output, const QString& launcher, const QString& name = "Release") {
    QSettings own(settingsPath(), QSettings::IniFormat);
    own.setValue("tools/project.archive/state", QVariantMap{{"schema",1},{"project",xpr},{"output",output},
        {"archiveName",name},{"selected",launcher},{"installations", QVariantList{QVariantMap{{"version","2023.1"},{"launcher",launcher}}}}});
    own.sync();
}
}
class ArchiveHostTests final : public QObject {
    Q_OBJECT
    QLibrary library;
    XipsCreateBrowserV1 factory = nullptr;
    QByteArray originalState;
    bool hadState = false;
    QWidget* create(QWidget* parent, QObject* host) {
#ifdef XIPS_ARCHIVE_STANDALONE
        Q_UNUSED(host)
        return new xips::BrowserPanel(parent);
#else
        return factory(parent, host);
#endif
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        hadState = QFileInfo::exists(settingsPath()); originalState = get(settingsPath());
        QCoreApplication::setOrganizationName("ArchiveHostTest");
        QCoreApplication::setApplicationName("Host identity");
#ifdef XIPS_ARCHIVE_STANDALONE
        xips::initializeEla();
#else
        auto component = qEnvironmentVariable("XIPS_BROWSER_LIBRARY");
        if (component.isEmpty()) component = QString(XIPS_BROWSER_PATH);
        library.setFileName(component); library.setLoadHints(QLibrary::PreventUnloadHint);
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        factory = reinterpret_cast<XipsCreateBrowserV1>(library.resolve("xips_create_browser_v1")); QVERIFY(factory);
        QVERIFY(!QFileInfo::exists(QCoreApplication::applicationDirPath() + "/tools/7zip/7za.exe"));
#endif
    }
    void cleanupTestCase() {
        if (hadState) put(settingsPath(), originalState); else QFile::remove(settingsPath());
    }
    void init() { qputenv("FTB_FAKE_MODE", ""); }
    void windowHostAndModuleHelper() {
        QTemporaryDir temp;
        const auto xpr=fixture(temp.filePath("source")), launcher=fake(temp.path());
        seed(xpr,temp.filePath("output"),launcher);
        const auto before=snapshot(temp.filePath("source"));
        QWidget owner; QObject bridge;
        const auto font=qApp->font(); const auto hostKeys=QSettings().allKeys();
        auto* layout=new QVBoxLayout(&owner);
        auto* panel=create(&owner,&bridge); QVERIFY(panel); layout->addWidget(panel);
        owner.resize(280,540); owner.show();
        auto* entry=panel->findChild<QAbstractButton*>("archiveProjectButton"); QVERIFY(entry); QVERIFY(entry->isEnabled());
        entry->click();
        auto* window=panel->findChild<QWidget*>("xipsArchiveWindow"); QVERIFY(window); QVERIFY(window->isVisible());
        entry->click(); QCOMPARE(panel->findChildren<QWidget*>("xipsArchiveWindow").size(),1);
        for (bool dark : {false,true}) {
            QVERIFY(QMetaObject::invokeMethod(panel,"setDarkTheme",Q_ARG(bool,dark)));
            window->resize(520,620); QTest::qWait(150);
            auto* page=window->findChild<QWidget*>("projectArchivePage");
            QVERIFY(page);
            const auto expected=panel->palette().color(QPalette::Window).name();
            QVERIFY2(page->styleSheet().contains(expected),qPrintable(page->styleSheet()));
            const auto captures=qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
            if (!captures.isEmpty()) {
                QDir().mkpath(captures);
#ifdef XIPS_ARCHIVE_STANDALONE
                const QString kind="standalone";
#else
                const QString kind="embedded";
#endif
                QVERIFY(window->grab().save(captures+"/"+kind+(dark?"-dark.png":"-light.png")));
                QVERIFY(owner.grab().save(captures+"/"+kind+(dark?"-entry-dark.png":"-entry-light.png")));
            }
        }
        auto* start=window->findChild<QPushButton*>("archiveStart"); QVERIFY(start->isEnabled()); start->click();
        QVERIFY(busy(panel)); QVERIFY(!window->close());
        QVERIFY(QMetaObject::invokeMethod(panel,"setContext",Q_ARG(QString,temp.filePath("library-next")),Q_ARG(QString,temp.filePath("workspace-next"))));
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),20000);
        const auto result=window->findChild<QLabel*>("archiveResult")->text();
        QVERIFY2(QFileInfo::exists(temp.filePath("output/Release.7z")), qPrintable(result));
        QVERIFY(QFileInfo::exists(temp.filePath("output/Release/demo.xpr")));
        QCOMPARE(snapshot(temp.filePath("source")),before);
        QCOMPARE(viewState(panel).value("library").toString(),temp.filePath("library-next"));
        QCOMPARE(QCoreApplication::applicationName(),QString("Host identity"));
        QCOMPARE(QCoreApplication::organizationName(),QString("ArchiveHostTest"));
#ifndef XIPS_ARCHIVE_STANDALONE
        QCOMPARE(qApp->font(),font);
#endif
        QCOMPARE(QSettings().allKeys(),hostKeys);
        QVERIFY(window->close());
        entry->click(); QCOMPARE(window->findChild<QLineEdit*>("archiveName")->text(),QString("Release"));
    }
#ifdef XIPS_ARCHIVE_STANDALONE
    void standaloneCloseGuard() {
        QTemporaryDir temp; const auto xpr=fixture(temp.filePath("source")),launcher=fake(temp.path());
        QVERIFY(QDir().mkpath(temp.filePath("library")));
        seed(xpr,temp.filePath("output"),launcher);
        qputenv("FTB_FAKE_MODE","hang");
        xips::MainWindow main(temp.filePath("library")); main.show();
        auto* panel=main.findChild<xips::BrowserPanel*>(); QVERIFY(panel);
        QTRY_VERIFY(!busy(panel)); panel->openArchiveProject();
        auto* window=panel->findChild<QWidget*>("xipsArchiveWindow");
        window->findChild<QPushButton*>("archiveStart")->click();
        QVERIFY(!main.close()); QVERIFY(main.isVisible());
        window->findChild<QPushButton*>("archiveCancel")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),10000); QVERIFY(main.close());
    }
#endif
    void scanCloseAndDestruction() {
        QTemporaryDir temp; const auto launcher=fake(temp.path());
        for(int n=0;n<60;++n) fixture(temp.filePath("sources/p"+QString::number(n)));
        for(bool destroy : {false,true}) {
            seed({},temp.filePath("output"),launcher);
            auto owner=std::make_unique<QWidget>(); QObject bridge;
            QPointer<QWidget> panel=create(owner.get(),&bridge);
            QVERIFY(QMetaObject::invokeMethod(panel,"openArchiveProject"));
            QPointer<QWidget> window=panel->findChild<QWidget*>("xipsArchiveWindow");
            window->findChild<QComboBox*>("archiveInputMode")->setCurrentIndex(1);
            window->findChild<QLineEdit*>("archiveScanFolder")->setText(temp.filePath("sources"));
            window->findChild<QPushButton*>("archiveScan")->click();
            QVERIFY(busy(panel)); QVERIFY(!window->close());
            if(destroy) { owner.reset(); QVERIFY(!panel); QVERIFY(!window); }
            else {
                window->findChild<QPushButton*>("archiveCancel")->click();
                QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),10000); QVERIFY(window->close());
            }
        }
    }
    void cancellationAndOwnerDestruction() {
        QTemporaryDir temp; const auto xpr=fixture(temp.filePath("source")),launcher=fake(temp.path());
        const auto before=snapshot(temp.filePath("source"));
        for(bool destroy : {false,true}) {
            qputenv("FTB_FAKE_MODE","hang");
            const auto output=temp.filePath(destroy?"destroy":"cancel"); seed(xpr,output,launcher);
            auto owner=std::make_unique<QWidget>(); QObject bridge;
            QPointer<QWidget> panel=create(owner.get(),&bridge); QVERIFY(panel);
            QVERIFY(QMetaObject::invokeMethod(panel,"openArchiveProject"));
            QPointer<QWidget> window=panel->findChild<QWidget*>("xipsArchiveWindow");
            window->findChild<QPushButton*>("archiveStart")->click();
            QTRY_COMPARE_WITH_TIMEOUT(window->findChild<QProgressBar*>("archiveStage3")->maximum(),0,15000);
            if(destroy) { owner.reset(); QVERIFY(!panel); QVERIFY(!window); }
            else {
                QVERIFY(busy(panel)); QVERIFY(!window->close());
                window->findChild<QPushButton*>("archiveCancel")->click();
                QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),10000); QVERIFY(window->close());
            }
            QVERIFY(!QFileInfo::exists(output+"/Release"));
            QCOMPARE(snapshot(temp.filePath("source")),before);
        }
    }
};
QTEST_MAIN(ArchiveHostTests)
#include "tst_archive_host.moc"
