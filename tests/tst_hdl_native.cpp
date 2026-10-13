#include "xips/BrowserApi.h"
#include "library/SnapshotLibrary.h"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLibrary>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QtTest>
namespace {
bool busy(QWidget *panel) { bool value=true; QMetaObject::invokeMethod(panel,"isCatalogBusy",Q_RETURN_ARG(bool,value)); return value; }
QVariantMap catalog(QWidget *panel) { QVariantMap value; QMetaObject::invokeMethod(panel,"hdlCommandCatalog",Q_RETURN_ARG(QVariantMap,value)); return value; }
QVariantMap use(QWidget *panel, const QVariantMap &entry, const QString &id="request")
{
    QVariantMap value, request{{"requestId",id},{"generation",catalog(panel).value("generation")},
        {"commandId",entry.value("commandId")},{"revision",entry.value("revision")}};
    QMetaObject::invokeMethod(panel,"useHdlCommand",Q_RETURN_ARG(QVariantMap,value),Q_ARG(QVariantMap,request)); return value;
}
void context(QWidget *panel,const QString &library,const QString &workspace)
{ QMetaObject::invokeMethod(panel,"setContext",Q_ARG(QString,library),Q_ARG(QString,workspace)); }
bool click(QWidget *owner, const char *name)
{ auto *b=owner->findChild<QAbstractButton *>(name); if(!b || !b->isEnabled()) return false; b->click(); return true; }
QVariantMap entryFor(QWidget *panel,const QString &asset={})
{
    for(const auto &v:catalog(panel).value("entries").toList())
        if(asset.isEmpty() || v.toMap().value("assetId")==asset) return v.toMap();
    return {};
}
}
class HdlHost : public QObject {
    Q_OBJECT
public:
    QString rejected,failed;
    QList<QVariantMap> receipts;
    Q_INVOKABLE QString destinationError(const QString &) { return rejected; }
    Q_INVOKABLE QString exportCompleted(const QVariantMap &r) { receipts.append(r); return failed; }
};
class HdlNativeTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    QLibrary library;
    XipsCreateBrowserV1 factory=nullptr;
    QWidget *create(QWidget &owner,HdlHost &host)
    {
        auto *panel=factory(&owner,&host);
        if(!panel) {
            const auto error=reinterpret_cast<XipsBrowserLastErrorV1>(library.resolve("xips_browser_last_error_v1"));
            if(error) qWarning("%s",error()); return nullptr;
        }
        auto *layout=new QVBoxLayout(&owner); layout->addWidget(panel); owner.resize(1000,700); owner.show(); return panel;
    }
private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
        library.setFileName(qEnvironmentVariable("XIPS_HDL_NATIVE_COMPONENT",QStringLiteral(XIPS_BROWSER_PATH)));
        library.setLoadHints(QLibrary::PreventUnloadHint);
        QVERIFY2(library.load(),qPrintable(library.errorString()));
        const auto capabilities=reinterpret_cast<XipsBrowserCapabilitiesV1>(library.resolve("xips_browser_capabilities_v1"));
        QVERIFY(capabilities); QVERIFY(QByteArray(capabilities()).contains("hdl-commands-v1"));
        factory=reinterpret_cast<XipsCreateBrowserV1>(library.resolve("xips_create_browser_v1")); QVERIFY(factory);
    }
    void catalogCacheAndBuiltinUse()
    {
        QTemporaryDir tmp; const auto root=tmp.filePath("library"), workspace=tmp.filePath("project");
        QDir().mkpath(root); QDir().mkpath(workspace); QWidget owner; HdlHost host;
        auto *panel=create(owner,host); QVERIFY(panel); context(panel,root,workspace);
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000); QCOMPARE(catalog(panel).value("state").toString(),"ready");
        QCOMPARE(catalog(panel).value("entries").toList().size(),4);
        const auto first=catalog(panel); QElapsedTimer elapsed; elapsed.start();
        for(int i=0;i<1000;i++) QCOMPARE(catalog(panel),first);
        QVERIFY(elapsed.elapsed()<2000); QVERIFY(QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
        QSignalSpy completed(panel,SIGNAL(hdlCommandUseFinished(QVariantMap))); QVERIFY(completed.isValid());
        const auto entry=entryFor(panel); const auto target=workspace+"/packages";
        QTimer dialog; dialog.setInterval(10); int imports=0,exports=0;
        connect(&dialog,&QTimer::timeout,this,[&] {
            auto *modal=qobject_cast<QDialog *>(QApplication::activeModalWidget()); if(!modal) return;
            if(modal->objectName()=="hdlBuiltinForm") { imports++; click(modal,"formAccept"); }
            else if(auto *edit=modal->findChild<QLineEdit *>("exportDestination")) { edit->setText(target); exports++; click(modal,"formAccept"); }
        }); dialog.start();
        QCOMPARE(use(panel,entry).value("status").toString(),"accepted");
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,30000); dialog.stop();
        QCOMPARE(completed[0][0].toMap().value("status").toString(),"exported");
        QCOMPARE(imports,1); QCOMPARE(exports,1); QCOMPARE(host.receipts.size(),1);
        const auto receipt=host.receipts.first(); QCOMPARE(receipt.value("workspace").toString(),workspace);
        const auto hdl=receipt.value("hdlCommand").toMap(); QCOMPARE(hdl.value("commandId"),entry.value("commandId"));
        QCOMPARE(hdl.value("compileOrder").toStringList().size(),4);
        QVERIFY(QFileInfo::exists(target+"/tb_reset_package.sv"));
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
        QCOMPARE(catalog(panel).value("entries").toList().size(),4);
        QVERIFY(entryFor(panel).value("assetId")!="builtin:tb-behaviors");
        auto stale=QVariantMap{{"requestId","stale"},{"generation",first.value("generation")},{"commandId",entry.value("commandId")},{"revision",entry.value("revision")}};
        QVariantMap rejected; QMetaObject::invokeMethod(panel,"useHdlCommand",Q_RETURN_ARG(QVariantMap,rejected),Q_ARG(QVariantMap,stale));
        QCOMPARE(rejected.value("status").toString(),"stale");
        // Reuse an existing asset, preserving export conflicts and host failure.
        host.failed="Receipt store unavailable";
        const auto saved=entryFor(panel); const auto target2=workspace+"/retained";
        disconnect(&dialog,nullptr,this,nullptr);
        connect(&dialog,&QTimer::timeout,this,[&] {
            auto *modal=qobject_cast<QDialog *>(QApplication::activeModalWidget()); if(!modal) return;
            if(auto *edit=modal->findChild<QLineEdit *>("exportDestination")) { edit->setText(target2); click(modal,"formAccept"); }
        }); dialog.start();
        QCOMPARE(use(panel,saved,"failed-receipt").value("status").toString(),"accepted");
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),2,30000); dialog.stop();
        QCOMPARE(completed[1][0].toMap().value("code").toString(),"receipt_failed");
        QVERIFY(QFileInfo::exists(target2+"/tb_random_package.sv"));
        QVERIFY(!completed[1][0].toMap().contains("receipt"));
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
        QCOMPARE(use(panel,saved,"context-cancel").value("status").toString(),"accepted");
        context(panel,root,tmp.filePath("another-project"));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),3,30000);
        QCOMPARE(completed[2][0].toMap().value("status").toString(),"cancelled");
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000); QCOMPARE(host.receipts.size(),2);
    }
    void userCreatesTaskAndFunctionFromUi()
    {
        QTemporaryDir tmp; QDir().mkpath(tmp.filePath("library"));
        xips::CatalogDefinition def; def.name="UI commands";
        const auto asset=xips::SnapshotLibrary::create(tmp.filePath("library"),def); QVERIFY(asset.ok);
        QWidget owner; HdlHost host; auto *panel=create(owner,host); QVERIFY(panel);
        context(panel,tmp.filePath("library"),tmp.path()); QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
        QMetaObject::invokeMethod(panel,"revealAsset",Q_ARG(QString,asset.asset.id)); QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
        for(int kind=0;kind<2;kind++)
        {
            const auto package=kind ? QString("ui_function_package") : QString("ui_task_package");
            QTimer dialog; dialog.setInterval(10); int edited=0;
            connect(&dialog,&QTimer::timeout,this,[&] {
                auto *modal=qobject_cast<QDialog *>(QApplication::activeModalWidget()); if(!modal) return;
                if(modal->objectName()=="hdlCommandsForm") click(modal,"newHdlCommandButton");
                else if(modal->objectName()=="hdlCommandEditor") {
                    modal->findChild<QLineEdit *>("hdlCommandName")->setText(kind ? "UI function" : "UI task");
                    modal->findChild<QComboBox *>("hdlCommandMode")->setCurrentIndex(kind);
                    modal->findChild<QComboBox *>("hdlCommandFile")->setCurrentText(package+".sv");
                    modal->findChild<QLineEdit *>("hdlCommandPackage")->setText(package);
                    modal->findChild<QLineEdit *>("hdlCommandSymbol")->setText(package+"::act");
                    modal->findChild<QPlainTextEdit *>("hdlCommandFiles")->setPlainText(package+".sv");
                    edited++; click(modal,"formAccept");
                }
            }); dialog.start();
            QMetaObject::invokeMethod(panel,"openHdlCommands"); dialog.stop();
            QCOMPARE(edited,1); QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
            QFile source(asset.asset.root+'/'+package+".sv"); QVERIFY(source.open(QIODevice::ReadOnly));
            QVERIFY(source.readAll().contains(kind ? "function automatic int act" : "task automatic act"));
        }
        QTimer dialog; dialog.setInterval(10);
        connect(&dialog,&QTimer::timeout,this,[&] { auto *modal=QApplication::activeModalWidget(); if(modal && modal->objectName()=="hdlCommandsForm") click(modal,"saveHdlVersionButton"); });
        dialog.start(); QMetaObject::invokeMethod(panel,"openHdlCommands"); dialog.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!busy(panel),30000);
        int count=0; for(const auto &v:catalog(panel).value("entries").toList()) if(v.toMap().value("assetId")==asset.asset.id) {
            QCOMPARE(v.toMap().value("availability").toString(),"available"); count++;
        }
        QCOMPARE(count,2);
    }
    void destructionCancelsAnalysis()
    {
        QTemporaryDir tmp; QWidget owner; HdlHost host; auto *panel=create(owner,host); QVERIFY(panel);
        QDir().mkpath(tmp.filePath("library")); context(panel,tmp.filePath("library"),tmp.path());
        QCoreApplication::processEvents(); delete panel; QCoreApplication::processEvents();
    }
};
QTEST_MAIN(HdlNativeTest)
#include "tst_hdl_native.moc"
