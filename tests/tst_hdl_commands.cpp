#include "library/HdlCommands.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace xips;
namespace {
void put(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Cannot write fixture");
}
QByteArray read(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
QJsonObject command(const QString &file, const QString &symbol, const QStringList &files = {})
{
    return {{"name","User behavior"},{"aliases",QJsonArray{"用户行为"}}, {"description","User source implementation"},
        {"file",file},{"package",symbol.section("::",0,0)},{"symbol",symbol},
        {"files",QJsonArray::fromStringList(files.isEmpty() ? QStringList{file} : files)}};
}
SnapshotResult edit(const CatalogAsset &asset, const QJsonObject &cmd, const QString &kind = {})
{ return HdlCommands::edit(asset,cmd,kind,HdlCommands::digest(HdlCommands::workingManifest(asset))); }
QVariantList own(const CatalogAsset &asset, const QVariantList &previous = {})
{
    QVariantList result;
    for (const auto &entry : HdlCommands::catalog({asset}, previous).entries)
        if (entry.toMap().value("assetId") == asset.id) result.append(entry);
    return result;
}
CatalogAsset create(const QString &library, const QString &name = "user")
{
    QDir().mkpath(library); CatalogDefinition def; def.name = name;
    const auto result = SnapshotLibrary::create(library,def);
    if (!result.ok) qFatal("Cannot create fixture: %s", qPrintable(result.error));
    return result.asset;
}
}
class HdlCommandsTest : public QObject {
    Q_OBJECT
private slots:
    void importExternalFixture()
    {
        const auto source=qEnvironmentVariable("XIPS_HDL_IMPORT_SOURCE");
        const auto root=qEnvironmentVariable("XIPS_HDL_IMPORT_OUT");
        if(source.isEmpty() || root.isEmpty()) QSKIP("No external local fixture requested");
        QVERIFY(!QFileInfo::exists(root)); QDir().mkpath(root+"/library");
        QStringList paths; for(const auto &name:QDir(source).entryList(QDir::Files)) paths.append(QDir(source).filePath(name));
        const auto preview=SnapshotLibrary::previewImport(paths); QVERIFY2(preview.ok,qPrintable(preview.error));
        ImportRequest request; request.sources=paths; request.selection=preview.preview; request.note="Explicit local HDL source fixture";
        CatalogDefinition def; def.name="Local HDL algorithms";
        const auto created=SnapshotLibrary::create(root+"/library",def,&request); QVERIFY2(created.ok,qPrintable(created.error));
        const auto catalog=HdlCommands::catalog({created.asset});
        put(root+"/catalog.json",QJsonDocument(QJsonObject{{"schema","xips.hdl-catalog/v1"},{"entries",QJsonArray::fromVariantList(catalog.entries)},
            {"errors",QJsonArray::fromVariantList(catalog.errors)}}).toJson());
        QVERIFY(catalog.errors.isEmpty()); int count=0;
        for(const auto &v:catalog.entries) if(v.toMap().value("assetId")==created.asset.id) {
            const auto e=v.toMap(); QCOMPARE(e.value("availability").toString(),"available");
            QVERIFY2(e.value("diagnostics").toStringList().isEmpty(),qPrintable(e.value("diagnostics").toStringList().join('\n')));
            const auto prepared=HdlCommands::prepareUse(created.asset,e); QVERIFY2(prepared.ok,qPrintable(prepared.error)); count++;
        }
        QVERIFY(count>0);
        const auto exported=SnapshotLibrary::exportSnapshot(created.asset,created.snapshot.id,root+"/exported"); QVERIFY2(exported.ok,qPrintable(exported.error));
        put(root+"/identity.json",QJsonDocument(QJsonObject{{"assetId",created.asset.id},{"revision",created.snapshot.id},
            {"contentHash",created.snapshot.hash},{"commands",count},{"files",QJsonArray::fromStringList(created.snapshot.files)},
            {"library",root+"/library"},{"exported",exported.exportedPath}}).toJson());
    }
    void builtinsAreRealAndExplicit()
    {
        const auto catalog = HdlCommands::catalog({});
        QVERIFY(catalog.errors.isEmpty()); QCOMPARE(catalog.entries.size(),4);
        QSet<QString> identities;
        for (const auto &value : catalog.entries)
        {
            const auto entry = value.toMap(); QCOMPARE(entry.value("availability").toString(),"available");
            QCOMPARE(entry.value("kind").toString(),"task");
            QVERIFY(entry.value("parameters").toList().size() >= 7);
            QCOMPARE(entry.value("typeParameters").toStringList(),QStringList{"T"});
            QCOMPARE(entry.value("compileOrder").toStringList().size(),4);
            identities.insert(entry.value("commandId").toString());
        }
        QCOMPARE(identities.size(),4);
        QTemporaryDir tmp; const auto library = tmp.filePath("library"); QDir().mkpath(library);
        QVERIFY(QDir(library).entryList(QDir::Files).isEmpty());
        const auto result = HdlCommands::installBuiltins(library); QVERIFY2(result.ok,qPrintable(result.error));
        QCOMPARE(result.snapshot.files.size(),5); QVERIFY(!result.snapshot.id.isEmpty());
        const auto installed = HdlCommands::catalog({result.asset}); QCOMPARE(installed.entries.size(),4);
        for (const auto &value : installed.entries)
        {
            const auto entry = value.toMap(); QCOMPARE(entry.value("assetId").toString(),result.asset.id);
            const auto prepared = HdlCommands::prepareUse(result.asset,entry); QVERIFY2(prepared.ok,qPrintable(prepared.error));
        }
        const auto second = HdlCommands::installBuiltins(library);
        // The ordinary asset creation policy must never replace the first copy.
        if (second.ok) QVERIFY(second.asset.id != result.asset.id);
        QVERIFY(SnapshotLibrary::verifySnapshot(result.asset,result.snapshot.id).ok);
    }
    void sourceAuthorityDraftsAndImmutableVersions()
    {
        QTemporaryDir tmp; auto asset = create(tmp.filePath("library"));
        auto result = edit(asset,command("pulse_package.sv","pulse_package::pulse"),"task");
        QVERIFY2(result.ok,qPrintable(result.error)); asset = result.asset;
        auto entries = own(asset); QCOMPARE(entries.size(),1); QCOMPARE(entries[0].toMap().value("availability").toString(),"draft");
        QVERIFY(!HdlCommands::prepareUse(asset,entries[0].toMap()).ok);
        result = edit(asset,command("value_package.sv","value_package::twice"),"function");
        QVERIFY2(result.ok,qPrintable(result.error)); asset = result.asset;
        put(asset.root+"/pulse_package.sv","package pulse_package; timeunit 1ns; timeprecision 1ps; task automatic pulse(output int n); #2ns; n=7; endtask endpackage\n");
        put(asset.root+"/value_package.sv","package value_package; function automatic logic signed [64:0] twice(input logic signed [64:0] value); return value*2; endfunction endpackage\n");
        result = HdlCommands::saveVersion(asset); QVERIFY2(result.ok,qPrintable(result.error)); asset = result.asset;
        entries = own(asset); QCOMPARE(entries.size(),2);
        const auto firstSnapshot = result.snapshot;
        QVariantMap functionEntry;
        for (const auto &value : entries)
        {
            const auto e = value.toMap(); QCOMPARE(e.value("availability").toString(),"available");
            if (e.value("kind") == "function") functionEntry = e;
        }
        QVERIFY(!functionEntry.isEmpty()); QVERIFY(functionEntry.value("returnType").toString().contains("64:0"));
        QCOMPARE(functionEntry.value("parameters").toList().first().toMap().value("name").toString(),"value");
        QVERIFY(HdlCommands::prepareUse(asset,functionEntry).ok);
        auto stale = functionEntry; stale.insert("sourceIdentity","changed"); QVERIFY(!HdlCommands::prepareUse(asset,stale).ok);
        put(asset.root+"/value_package.sv","package value_package; function automatic int twice(input int value); #1; return value; endfunction endpackage\n");
        result = SnapshotLibrary::describe(asset); QVERIFY(result.ok); asset=result.asset;
        entries = own(asset,entries); QCOMPARE(entries.size(),4);
        int invalid = 0, saved = 0;
        for (const auto &e : entries) { invalid += e.toMap().value("availability")=="invalid"; saved += e.toMap().value("availability")=="available"; }
        QVERIFY(invalid >= 1); QCOMPARE(saved,2); QVERIFY(!HdlCommands::saveVersion(asset).ok);
        QVERIFY(read(asset.root+"/value_package.sv").contains("#1"));
        QVERIFY(HdlCommands::prepareUse(asset,functionEntry).ok);
        const auto exported = SnapshotLibrary::exportSnapshot(asset,firstSnapshot.id,tmp.filePath("independent"));
        QVERIFY2(exported.ok,qPrintable(exported.error)); QVERIFY(read(exported.exportedPath+"/value_package.sv").contains("value*2"));
        const auto receipt = HdlCommands::receiptDetails(functionEntry);
        QCOMPARE(receipt.value("schema").toString(),"xips.hdl-use/v1"); QCOMPARE(receipt.value("compileOrder").toStringList(),QStringList{"value_package.sv"});
    }
    void mappingConflictsDependenciesAndRename()
    {
        QTemporaryDir tmp; auto asset = create(tmp.filePath("library"));
        const auto before = HdlCommands::digest({});
        auto result = edit(asset,command("a_package.sv","a_package::work"),"task"); QVERIFY(result.ok); asset = result.asset;
        QVERIFY(!HdlCommands::edit(asset,command("b_package.sv","b_package::work"),"task",before).ok);
        QVERIFY(!QFileInfo::exists(asset.root+"/b_package.sv"));
        QVERIFY(!edit(asset,command("a_package.sv","a_package::other"),"task").ok);
        QVERIFY(!edit(asset,command("../escape_package.sv","escape_package::work"),"task").ok);
        result = edit(asset,command("b_package.sv","b_package::work"),"function"); QVERIFY(result.ok); asset = result.asset;
        result=HdlCommands::saveVersion(asset); QVERIFY2(result.ok,qPrintable(result.error)); asset=result.asset;
        auto list=own(asset); QCOMPARE(list.size(),2);
        QVERIFY(list[0].toMap().value("commandId") != list[1].toMap().value("commandId"));
        QCOMPARE(list[0].toMap().value("name"),list[1].toMap().value("name"));
        CatalogDefinition renamedDefinition; renamedDefinition.name="renamed";
        result=SnapshotLibrary::setDefinition(asset,renamedDefinition); QVERIFY2(result.ok,qPrintable(result.error)); asset=result.asset;
        const auto renamed=own(asset,list); QCOMPARE(renamed.size(),2);
        for (const auto &v : renamed) QCOMPARE(v.toMap().value("assetName").toString(),"renamed");
        auto doc=QJsonDocument::fromJson(HdlCommands::workingManifest(asset)).object();
        auto commands=doc.value("commands").toArray(); auto cmd=commands[0].toObject();
        cmd.insert("files",QJsonArray{"missing_package.sv","a_package.sv"}); commands[0]=cmd; doc.insert("commands",commands);
        put(asset.root+"/xips-commands.json",QJsonDocument(doc).toJson());
        asset=SnapshotLibrary::describe(asset).asset;
        list=own(asset); bool missing=false;
        for (const auto &v : list) if (v.toMap().value("diagnostics").toStringList().join(' ').contains("Missing dependency")) missing=true;
        QVERIFY(missing); QVERIFY(!HdlCommands::saveVersion(asset).ok);
    }
    void dependencySemantics()
    {
        QTemporaryDir tmp; auto asset=create(tmp.filePath("library"));
        put(asset.root+"/base_package.sv","package base_package; typedef logic [15:0] word_t; endpackage\n");
        put(asset.root+"/dep_package.sv","package dep_package; import base_package::*; function automatic word_t f(input word_t x); return x+1; endfunction endpackage\n");
        auto result=edit(asset,command("dep_package.sv","dep_package::f",{"base_package.sv","dep_package.sv"}));
        QVERIFY2(result.ok,qPrintable(result.error)); asset=result.asset;
        result=HdlCommands::saveVersion(asset); QVERIFY2(result.ok,qPrintable(result.error)); asset=result.asset;
        put(asset.root+"/base_package.sv","package base_package; import dep_package::*; typedef logic [15:0] word_t; endpackage\n");
        QVERIFY(!HdlCommands::saveVersion(asset).ok);
        put(asset.root+"/base_package.sv","package dep_package; typedef logic [15:0] word_t; endpackage\n");
        QVERIFY(!HdlCommands::saveVersion(asset).ok);
    }
    void includesMustSurviveTheSnapshot()
    {
        QTemporaryDir tmp; auto asset=create(tmp.filePath("library"));
        put(asset.root+"/types.svh","typedef logic signed [64:0] value_t;\n");
        put(asset.root+"/api_package.sv","package api_package;\n`include \"types.svh\"\nfunction automatic value_t f(input value_t x); return x; endfunction endpackage\n");
        auto result=edit(asset,command("api_package.sv","api_package::f")); QVERIFY(result.ok); asset=result.asset;
        result=HdlCommands::saveVersion(asset); QVERIFY2(result.ok,qPrintable(result.error)); asset=result.asset;
        QVERIFY(result.snapshot.files.contains("types.svh"));
        put(asset.root+"/.hidden.svh","typedef int value_t;\n");
        put(asset.root+"/api_package.sv","package api_package;\n`include \".hidden.svh\"\nfunction automatic value_t f(input value_t x); return x; endfunction endpackage\n");
        result=HdlCommands::saveVersion(asset); QVERIFY(!result.ok);
        QVERIFY2(result.error.contains(".hidden.svh"),qPrintable(result.error));
    }
    void exportStandaloneFixture()
    {
        const auto root=qEnvironmentVariable("XIPS_HDL_FIXTURE_OUT"); if(root.isEmpty()) QSKIP("No requested fixture output");
        QVERIFY(!QFileInfo::exists(root)); QDir().mkpath(root+"/library");
        auto builtin=HdlCommands::installBuiltins(root+"/library"); QVERIFY2(builtin.ok,qPrintable(builtin.error));
        auto out=SnapshotLibrary::exportSnapshot(builtin.asset,builtin.snapshot.id,root+"/exported/builtins");
        // Export requires an existing parent directory.
        if(!out.ok) { QDir().mkpath(root+"/exported"); out=SnapshotLibrary::exportSnapshot(builtin.asset,builtin.snapshot.id,root+"/exported/builtins"); }
        QVERIFY2(out.ok,qPrintable(out.error));
        auto asset=create(root+"/library","user_behaviors");
        put(asset.root+"/user_package.sv","package user_package; timeunit 1ns; timeprecision 1ps; task automatic delay_value(output int value); #3ns; value=42; endtask function automatic logic signed [64:0] twice(input logic signed [64:0] value); return value*2; endfunction endpackage\n");
        auto r=edit(asset,command("user_package.sv","user_package::delay_value")); QVERIFY(r.ok); asset=r.asset;
        r=edit(asset,command("user_package.sv","user_package::twice")); QVERIFY(r.ok); asset=r.asset;
        r=HdlCommands::saveVersion(asset); QVERIFY2(r.ok,qPrintable(r.error)); asset=r.asset;
        out=SnapshotLibrary::exportSnapshot(asset,r.snapshot.id,root+"/exported/user"); QVERIFY2(out.ok,qPrintable(out.error));
        auto catalog=HdlCommands::catalog({builtin.asset,asset});
        put(root+"/catalog.json",QJsonDocument(QJsonObject{{"schema","xips.hdl-catalog/v1"},{"entries",QJsonArray::fromVariantList(catalog.entries)},
            {"errors",QJsonArray::fromVariantList(catalog.errors)}}).toJson());
        QCOMPARE(catalog.entries.size(),6); QVERIFY(catalog.errors.isEmpty());
    }
};
QTEST_GUILESS_MAIN(HdlCommandsTest)
#include "tst_hdl_commands.moc"
