#include "ArchiveEngine.h"
#include "ArchivePage.h"
#include "ArchiveBatchPanel.h"
#include "ArchiveScan.h"
#include "VivadoWorkspace.h"
#include <QtCore/private/qzipwriter_p.h>
#include "ArchiveWindow.h"
#include "ArchiveState.h"
#include <QSettings>
#include <QStandardPaths>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <stdexcept>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDomDocument>
#include <QFile>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QProcess>
#include <QScopeGuard>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTest>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace xips::archive;
namespace {
void put(const QString& file, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file); if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) qFatal("Cannot create fixture");
}
QByteArray get(const QString& file) { QFile f(file); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
QString fixture(const QString& base, const QString& filePath = "$PSRCDIR/top.v")
{
    const auto root = base + "/source";
    put(root + "/demo.srcs/top.v", "module top(); endmodule\n");
    put(root + "/demo.runs/impl_1/top.bit", "bit-a\0payload");
    put(root + "/demo.runs/impl_2/top.bit", "bit-b");
    put(root + "/demo.runs/impl_1/top.BIN", "bin");
    put(root + "/demo.srcs/sources_1/bd/design/design.bd", "<bd/>");
    put(root + "/demo.srcs/sources_1/bd/design/hw_handoff/design.hwh", "hwh");
    put(root + "/demo.runs/impl_1/top.ltx", "ltx");
    put(root + "/demo.runs/impl_1/top.mcs", "mcs");
    put(root + "/misc/delete-me.txt", "temporary");
    const auto xpr = root + "/demo.xpr";
    const auto data = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- Product Version: Vivado v2023.1 (64-bit) -->\n"
        "<Project Product=\"Vivado\" Version=\"7\" Path=\"%1\">"
        "<DefaultLaunch Dir=\"$PRUNDIR\"/><Configuration><Option Name=\"Part\" Val=\"xc7a35tcpg236-1\"/></Configuration>"
        "<FileSets><FileSet Name=\"sources_1\" Type=\"DesignSrcs\" RelSrcDir=\"$PSRCDIR/sources_1\"><File Path=\"%2\"/>"
        "<File Path=\"$PSRCDIR/sources_1/bd/design/design.bd\"/></FileSet></FileSets>"
        "<Runs><Run Id=\"synth_1\" Type=\"Ft3:Synth\" SrcSet=\"sources_1\" State=\"current\"/>"
        "<Run Id=\"impl_1\" Type=\"Ft2:EntireDesign\" SynthRun=\"synth_1\" Dir=\"$PRUNDIR/impl_1\" State=\"current\"/>"
        "<Run Id=\"impl_2\" Type=\"Ft2:EntireDesign\" SynthRun=\"synth_1\" Dir=\"$PRUNDIR/impl_2\"/></Runs></Project>").arg(xpr, filePath).toUtf8();
    put(xpr, data);
    return xpr;
}
QMap<QString, QByteArray> snapshot(const QString& root)
{
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        result.insert(QDir(root).relativeFilePath(path), QCryptographicHash::hash(get(path), QCryptographicHash::Sha256).toHex()
            + QByteArray::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()));
    }
    return result;
}
QString fake()
{
    const auto dir = QCoreApplication::applicationDirPath();
    const auto launcher = dir + "/xips_fake_vivado.bat";
    const auto content = "@echo off\r\nset \"PATH=" + QDir::toNativeSeparators(QLibraryInfo::path(QLibraryInfo::BinariesPath))
        + ';' + QDir::toNativeSeparators(QString::fromUtf8(FTB_TEST_COMPILER_BIN)) + ";%PATH%\"\r\n\"" + QDir::toNativeSeparators(dir + "/xips_fake_vivado.exe") + "\"\r\nexit /b %errorlevel%\r\n";
    put(launcher, content.toLocal8Bit());
    return launcher;
}
void container(const QString& file, const QString& kind = {})
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QZipWriter zip(file);
    zip.setCompressionPolicy(QZipWriter::NeverCompress);
    const auto core = kind == "missing-core" ? "missing.xci" : kind == "core-case" ? "core/CORE.xci" : "core/core.xci";
    zip.addFile("cc.xml", QString("<CoreContainer MajorVersion=\"%1\" MinorVersion=\"0\"><CoreFile>%2</CoreFile></CoreContainer>")
        .arg(kind == "version" ? "1" : "0", core).toUtf8());
    QByteArray metadata("<core>fixture-crc-payload</core>");
    if (kind == "absolute-metadata") metadata = "<core>C:/original/external.coe</core>";
    if (kind == "escaping-metadata") metadata = "<core>../../../../../external.coe</core>";
    zip.addFile("core/core.xci", metadata);
    if (kind == "traversal") zip.addFile("../escaped.txt", "unsafe");
    if (kind == "absolute") zip.addFile("C:/escaped.txt", "unsafe");
    if (kind == "duplicate") zip.addFile("core/CORE.xci", "duplicate");
    if (kind == "symlink") zip.addSymLink("core/link", "../../outside");
    if (kind == "collision") zip.addFile("core", "not a directory");
    if (kind == "nested") zip.addFile("core/nested.xcix", "nested");
    zip.close();
    if (zip.status() != QZipWriter::NoError) qFatal("Cannot write container fixture");
    if (kind == "crc") {
        auto bytes = get(file); const auto index = bytes.indexOf("fixture-crc-payload");
        if (index < 0) qFatal("Cannot find fixture payload");
        bytes[index] = 'X'; put(file, bytes);
    }
}
QVariantList execute(const Request& request, int timeout = 15000, QStringList* messages = nullptr)
{
    ArchiveJob job(request);
    QSignalSpy done(&job, &ArchiveJob::completed);
    QSignalSpy logs(&job, &ArchiveJob::logMessage);
    job.start();
    if (!done.wait(timeout)) { job.requestInterruption(); job.wait(); return {}; }
    job.wait();
    if (messages) for (const auto& line : logs) messages->append(line.first().toString());
    if (!done.first()[0].toBool()) for (const auto& line : logs) qInfo().noquote() << line.first().toString();
    return done.first();
}
}
class ArchiveTests : public QObject {
    Q_OBJECT
private slots:
    void init() { qputenv("FTB_FAKE_MODE", ""); qunsetenv("FTB_FAKE_7ZIP"); qunsetenv("FTB_TEST_SOURCE"); }
    void stateMigrationIsOwnedAndIdempotent()
    {
        QTemporaryDir temp;
        const auto legacyPath = temp.filePath("legacy.ini"), ownPath = temp.filePath("xips.ini");
        const QVariantMap old{{"schema", 1}, {"archiveName", "Old release"}, {"project", "source/demo.xpr"},
            {"batch", QVariantMap{{"folder", "batch"}}}};
        { QSettings legacy(legacyPath, QSettings::IniFormat); legacy.setValue("tools/project.archive/state", old); }
        const auto original = get(legacyPath);
        const auto modified = QFileInfo(legacyPath).lastModified();
        ArchiveState state(ownPath, legacyPath);
        QCOMPARE(state.load(), old);
        QVariantMap updated = old; updated["archiveName"] = "New release";
        QVERIFY(state.save(updated)); QCOMPARE(state.load(), updated);
        QCOMPARE(get(legacyPath), original); QCOMPARE(QFileInfo(legacyPath).lastModified(), modified);
        { QSettings own(ownPath, QSettings::IniFormat); own.setValue("tools/project.archive/state", QVariantMap{}); }
        QVERIFY(state.load().isEmpty()); // An existing empty state also prevents migration.
        ArchiveState absent(temp.filePath("new.ini"), temp.filePath("missing.ini"));
        QVERIFY(absent.load().isEmpty());
    }
    void createRealFixture()
    {
        const auto root = qEnvironmentVariable("XIPS_REAL_FIXTURE");
        const auto launcher = qEnvironmentVariable("FTB_TEST_VIVADO");
        if (root.isEmpty() || launcher.isEmpty()) QSKIP("Opt-in fresh Vivado fixture creation.");
        QVERIFY(!QFileInfo::exists(root + "/fixture"));
        auto script = get(QString(FTB_TEST_SOURCE_DIR) + "/fixtures/create-bd-project.tcl");
        script.replace("exit 0", "set marker [open $::env(FTB_SUCCESS_FILE) w]; puts $marker ready; close $marker; exit 0");
        try {
            runBatch({installationVersion(launcher), launcher}, root, script, {}, [] {}, [](const QString& line) { qInfo().noquote() << line; });
        } catch (const std::exception& ex) { QFAIL(ex.what()); }
        QVERIFY(QFileInfo::exists(root + "/fixture/fixture.xpr"));
    }
    void parseAndDiscover()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto info = inspectProject(path);
        QCOMPARE(info.version, "2023.1"); QCOMPARE(info.name, "demo");
        put(temp.filePath("inst/Vivado/2023.1/bin/vivado.bat"), "@exit /b 0\n");
        const auto installs = discoverInstallations({temp.filePath("inst")});
        QCOMPARE(installs.size(), 1); QCOMPARE(installs.first().version, "2023.1");
        put(path, "<Project Product=\"Vivado\"/>");
        QVERIFY_EXCEPTION_THROWN(inspectProject(path), std::runtime_error);
    }
    void batchEnvironment_data()
    {
        QTest::addColumn<QByteArray>("foreignPath");
        QTest::newRow("other-tool") << QByteArray("C:\\OtherTool\\bin");
        QTest::newRow("line-break") << QByteArray("C:\\OtherTool\nbin");
        QTest::newRow("long-path") << QByteArray("C:\\OtherTool;").repeated(1000);
    }
    void batchEnvironment()
    {
#ifdef Q_OS_WIN
        QFETCH(QByteArray, foreignPath);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto originalPath = qgetenv("PATH");
        const auto originalLicense = qgetenv("XILINXD_LICENSE_FILE");
        const auto restore = qScopeGuard([&] {
            if (originalPath.isNull()) qunsetenv("PATH"); else qputenv("PATH", originalPath);
            if (originalLicense.isNull()) qunsetenv("XILINXD_LICENSE_FILE"); else qputenv("XILINXD_LICENSE_FILE", originalLicense);
        });
        qputenv("PATH", foreignPath + ';' + originalPath);
        qputenv("XILINXD_LICENSE_FILE", "2100@fixture-license-host");
        const auto inheritedPath = qgetenv("PATH");
        const auto launcher = temp.filePath("Vivado/bin/vivado.bat");
        put(launcher, "@echo off\r\necho %PATH%>child-path.txt\r\nset XILINXD_LICENSE_FILE>child-license.txt\r\n"
                      "echo ready>\"%FTB_SUCCESS_FILE%\"\r\nexit /b 0\r\n");
        const auto control = temp.filePath("control with spaces");
        try { runBatch({"2023.1", launcher}, control, "exit\n", {}, [] {}, [](const QString&) {}); }
        catch (const std::exception& ex) { QFAIL(ex.what()); }
        const auto childPath = get(control + "/child-path.txt").trimmed();
        QVERIFY2(!childPath.contains("OtherTool"), childPath.constData());
        QVERIFY(!childPath.contains('\n'));
        QVERIFY(childPath.toLower().contains("system32"));
        QCOMPARE(get(control + "/child-license.txt").trimmed(), QByteArray("XILINXD_LICENSE_FILE=2100@fixture-license-host"));
        QCOMPARE(qgetenv("PATH"), inheritedPath);
        QCOMPARE(qgetenv("XILINXD_LICENSE_FILE"), QByteArray("2100@fixture-license-host"));
#else
        QSKIP("Windows batch-launcher environment regression.");
#endif
    }
    void copyArtifactsResetAndPrune()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto root = QFileInfo(path).absolutePath();
        const auto before = snapshot(root);
        qputenv("FTB_TEST_SOURCE", root.toUtf8());
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QVERIFY(QFileInfo(dest).fileName().startsWith("demo-"));
        QCOMPARE(QDir(dest).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name), QStringList({"artifacts", "demo.srcs", "demo.xpr"}));
        QCOMPARE(get(dest + "/artifacts/top.bit"), QByteArray("bit-a"));
        QCOMPARE(get(dest + "/artifacts/top__2.bit"), QByteArray("bit-b"));
        QCOMPARE(get(dest + "/artifacts/design.hwh"), QByteArray("hwh"));
        QCOMPARE(get(dest + "/artifacts/top.BIN"), QByteArray("bin"));
        QCOMPARE(get(dest + "/artifacts/top.mcs"), QByteArray("mcs"));
        QCOMPARE(get(dest + "/artifacts/top.ltx"), QByteArray("ltx"));
        QVERIFY(QDir(dest + "/artifacts").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(QDir(dest + "/artifacts").entryList(QDir::Files).size(), 6);
        QProcess extract;
        extract.start(QCoreApplication::applicationDirPath() + "/tools/7zip/7za.exe",
            {"x", "-y", "-o" + temp.filePath("extracted"), "--", dest + ".7z"});
        QVERIFY(extract.waitForFinished(15000)); QCOMPARE(extract.exitCode(), 0);
        const auto extracted = temp.filePath("extracted/") + QFileInfo(dest).fileName();
        QDirIterator entries(dest, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        int count = 0;
        while (entries.hasNext()) {
            const auto file = entries.next(); ++count;
            QCOMPARE(get(extracted + '/' + QDir(dest).relativeFilePath(file)), get(file));
        }
        QCOMPARE(snapshot(extracted).size(), count);
        QCOMPARE(snapshot(root), before);
        QVERIFY(!get(dest + "/demo.xpr").contains(root.toUtf8()));
    }
    void customArchiveName_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("spaces-unicode") << QStringLiteral("\u5f52\u6863 UART & v1");
        QTest::newRow("project") << QString("project");
        QTest::newRow("control") << QString("control");
        QTest::newRow("publish") << QString("publish");
        QTest::newRow("list-prefix") << QString("@release");
        QTest::newRow("switch-prefix") << QString("-release");
    }
    void customArchiveName()
    {
        QFETCH(QString, name);
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}, {}, "  " + name + "  "});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = temp.filePath("output/" + name);
        QCOMPARE(result[2].toString(), dest);
        QDomDocument doc;
        QVERIFY(doc.setContent(get(dest + "/demo.xpr")));
        QCOMPARE(doc.documentElement().attribute("Path"), dest + "/demo.xpr");
        QVERIFY(QFileInfo(dest + "/demo.srcs/top.v").isFile());
        QProcess extract;
        extract.start(QCoreApplication::applicationDirPath() + "/tools/7zip/7za.exe",
            {"x", "-y", "-o" + temp.filePath("extracted"), "--", dest + ".7z"});
        QVERIFY(extract.waitForFinished(15000)); QCOMPARE(extract.exitCode(), 0);
        QCOMPARE(QDir(temp.filePath("extracted")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{name});
        QDirIterator entries(dest, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            const auto file = entries.next();
            QCOMPARE(get(temp.filePath("extracted/" + name + '/' + QDir(dest).relativeFilePath(file))), get(file));
        }
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void rejectsArchiveNameBeforeCopy_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : QStringList{"../outside", "folder/child", "folder\\child", "C:/outside", "C:relative", ".", "..",
                                            "trailing.", "CON", "con.txt", "LPT1.log", "CONOUT$", "bad*name", "bad\nname"})
            QTest::newRow(qPrintable(name)) << name;
        QTest::newRow("superscript-device") << QStringLiteral("COM\u00b9");
        QTest::newRow("too-long") << QString(253, 'a');
    }
    void rejectsArchiveNameBeforeCopy()
    {
        QFETCH(QString, name);
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto before = snapshot(temp.path());
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}, {}, name});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        QVERIFY(!result[4].toString().isEmpty()); QVERIFY(result[3].toString().isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        QCOMPARE(snapshot(temp.path()), before);
    }
    void rejectsArchiveNameCollision_data()
    {
        QTest::addColumn<QString>("existing");
        QTest::newRow("folder") << QString("release/keep.txt");
        QTest::newRow("file") << QString("release");
        QTest::newRow("7z") << QString("release.7z");
    }
    void rejectsArchiveNameCollision()
    {
        QFETCH(QString, existing);
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        put(temp.filePath("output/" + existing), "previous result");
        const auto before = snapshot(temp.path());
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}, {}, "release"});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        QVERIFY(result[4].toString().contains("already exists")); QVERIFY(result[3].toString().isEmpty());
        QCOMPARE(snapshot(temp.path()), before);
        QCOMPARE(QDir(temp.filePath("output")).entryList({".xips-archive-*"}, QDir::Dirs | QDir::Hidden).size(), 0);
    }
    void rejectsCollisionBeforePublication()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        ArchiveJob job({path, temp.filePath("output"), {"2023.1", fake()}, {}, "release"});
        connect(&job, &ArchiveJob::progress, &job, [&](int stage, int percent, const QString&) {
            if (stage == 5 && percent == 90) put(temp.filePath("output/release/keep.txt"), "concurrent result");
        }, Qt::DirectConnection);
        QSignalSpy done(&job, &ArchiveJob::completed);
        job.start(); QVERIFY(done.wait(15000)); QVERIFY(job.wait(5000));
        QVERIFY(!done.first()[0].toBool()); QVERIFY(done.first()[4].toString().contains("already exists"));
        QCOMPARE(get(temp.filePath("output/release/keep.txt")), QByteArray("concurrent result"));
        QVERIFY(!QFileInfo::exists(temp.filePath("output/release.7z")));
    }
    void legacyProjectWithoutProductAttribute()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        auto data = get(path);
        data.replace(" Product=\"Vivado\"", "");
        data.replace("Vivado v2023.1", "Vivado v2022.2");
        put(path, data);
        const auto before = snapshot(QFileInfo(path).absolutePath());
        QCOMPARE(inspectProject(path).version, "2022.2");
        const auto launcher = fake();
        ArchivePage page;
        page.restoreState({{"schema", 1}, {"project", path}, {"installations",
            QVariantList{QVariantMap{{"version", "2022.2"}, {"launcher", launcher}}}}});
        QCOMPARE(page.findChild<QLabel*>("archiveProjectInfo")->text(), "Requires Vivado 2022.2");
        QCOMPARE(page.findChild<QComboBox*>("archiveVersion")->currentData().toString(), launcher);
        const auto result = execute({path, temp.filePath("output"), {"2022.2", launcher}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        QCOMPARE(inspectProject(result[2].toString() + "/demo.xpr").version, "2022.2");
        QCOMPARE(get(result[2].toString() + "/artifacts/top__2.bit"), QByteArray("bit-b"));
        QCOMPARE(snapshot(QFileInfo(path).absolutePath()), before);
    }
    void refusesInvalidProjectIdentity_data()
    {
        QTest::addColumn<QByteArray>("document");
        const QByteArray header("<!-- Product Version: Vivado v2022.2 (64-bit) -->\n");
        QTest::newRow("different-product") << header + "<Project Product=\"ISE\"/>";
        QTest::newRow("empty-explicit-product") << header + "<Project Product=\"\"/>";
        QTest::newRow("different-root") << header + "<NotProject/>";
        QTest::newRow("missing-release") << QByteArray("<Project Version=\"7\"/>");
        QTest::newRow("conflicting-releases") << header + "<!-- Product Version: Vivado v2023.1 -->\n<Project/>";
        QTest::newRow("malformed-xml") << header + "<Project>";
        QTest::newRow("document-type") << header + "<!DOCTYPE Project [<!ELEMENT Project ANY>]>\n<Project/>";
    }
    void refusesInvalidProjectIdentity()
    {
        QFETCH(QByteArray, document);
        QTemporaryDir temp;
        const auto path = temp.filePath("invalid.xpr");
        put(path, document);
        QVERIFY_EXCEPTION_THROWN(inspectProject(path), std::runtime_error);
        QCOMPARE(get(path), document);
    }
    void noArtifactsStillCreatesFolder()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("source/empty.XPR");
        put(path, "<!-- Product Version: Vivado v2023.1 -->\n<Project Product=\"Vivado\"/>\n");
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        QVERIFY(QFileInfo(result[2].toString() + "/artifacts").isDir());
        QVERIFY(QFileInfo(result[2].toString() + "/empty.srcs").isDir());
        QVERIFY(QDir(result[2].toString() + "/artifacts").isEmpty());
    }
    void localSourcesAreRetained()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path(), "$PPRDIR/rtl/top.v");
        put(temp.filePath("source/rtl/top.v"), "`include \"defs.vh\"\nmodule top(); endmodule\n");
        put(temp.filePath("source/rtl/defs.vh"), "`define TEST 1\n");
        put(temp.filePath("source/include/constants.vh"), "`define VALUE 42\n");
        auto sourceXml = get(path);
        sourceXml.replace("</Configuration>", "<Option Name=\"IncludeDirs\" Val=\"$PPRDIR/include\"/></Configuration>");
        put(path, sourceXml);
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QCOMPARE(get(dest + "/demo.srcs/_archive_imports/rtl/defs.vh"), QByteArray("`define TEST 1\n"));
        QVERIFY(get(dest + "/demo.xpr").contains("$PSRCDIR/_archive_imports/rtl/top.v"));
        QCOMPARE(get(dest + "/demo.srcs/_archive_imports/include/constants.vh"), QByteArray("`define VALUE 42\n"));
        QVERIFY(get(dest + "/demo.xpr").contains("$PSRCDIR/_archive_imports/include"));
    }
    void coreContainerRemainsIndependent()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path(), "$PSRCDIR/ip/core.xcix");
        const auto source = temp.filePath("source/demo.srcs/ip/core.xcix");
        container(source);
        auto data = get(path);
        data.replace("</FileSet>", "<File Path=\"$PSRCDIR/ip/core/core.xci\"/></FileSet>");
        put(path, data);
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QCOMPARE(get(dest + "/demo.srcs/ip/core.xcix"), get(source));
        QVERIFY(get(dest + "/demo.xpr").contains("$PPRDIR/demo.srcs/ip/core/core.xci"));
        QVERIFY(!QFileInfo::exists(dest + "/demo.srcs/ip/core/core.xci"));
        const auto copied = isolateProject(path, temp.filePath("isolated"));
        QCOMPARE(get(copied.root + "/demo.srcs/ip/core.xcix"), get(source));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void refusesInvalidContainers_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"traversal", "absolute", "duplicate", "symlink", "collision", "nested",
                                "crc", "version", "missing-core", "core-case", "absolute-metadata", "escaping-metadata", "unregistered"})
            QTest::newRow(kind) << QString(kind);
    }
    void refusesInvalidContainers()
    {
        QFETCH(QString, kind);
        QTemporaryDir temp;
        const auto path = fixture(temp.path(), kind == "unregistered" ? "$PSRCDIR/ip/core/core.xci" : "$PSRCDIR/ip/core.xcix");
        container(temp.filePath("source/demo.srcs/ip/core.xcix"), kind);
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        QVERIFY(result[3].toString().isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        QVERIFY_EXCEPTION_THROWN(isolateProject(path, temp.filePath("isolated")), std::runtime_error);
        QVERIFY(!QFileInfo::exists(temp.filePath("isolated")));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void disabledSimulationReferences_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("success");
        QTest::newRow("missing-disabled-simulation") << QString("disabled") << true;
        QTest::newRow("existing-disabled-simulation") << QString("existing") << true;
        QTest::newRow("missing-active-simulation") << QString("active") << false;
        QTest::newRow("missing-disabled-design") << QString("design") << false;
        QTest::newRow("also-used-by-active-set") << QString("shared") << false;
        QTest::newRow("outside-disabled-simulation") << QString("external") << false;
    }
    void disabledSimulationReferences()
    {
        QFETCH(QString, kind); QFETCH(bool, success);
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto reference = kind == "external" ? "$PSRCDIR/../../outside.sv" : "$PSRCDIR/obsolete.sv";
        const auto attrs = kind == "active" ? "" : "<FileInfo><Attr Name=\"AutoDisabled\" Val=\"1\"/></FileInfo>";
        const auto file = QString("<File Path=\"%1\">%2</File>").arg(reference, attrs);
        auto data = get(path);
        data.replace("</FileSets>", QString("<FileSet Name=\"sim_1\" Type=\"%1\">%2</FileSet>%3</FileSets>")
            .arg(kind == "design" ? "DesignSrcs" : "SimulationSrcs", file,
                 kind == "shared" ? "<FileSet Name=\"other\" Type=\"DesignSrcs\"><File Path=\"$PSRCDIR/obsolete.sv\"/></FileSet>" : "").toUtf8());
        put(path, data);
        if (kind == "existing") put(temp.filePath("source/demo.srcs/obsolete.sv"), "module existing; endmodule");
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QCOMPARE(result[0].toBool(), success);
        if (success) QCOMPARE(get(result[2].toString() + "/demo.xpr").contains("obsolete.sv"), kind == "existing");
        else QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        // Composer's selected source IDs depend on file-set order; its isolation must not drop entries.
        if (kind == "disabled") QVERIFY_EXCEPTION_THROWN(isolateProject(path, temp.filePath("isolated")), std::runtime_error);
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void flatArtifactNamesNeverOverwrite()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        put(temp.filePath("source/demo.runs/impl_3/top.BIT"), "case duplicate");
        put(temp.filePath("source/demo.runs/impl_3/top__2.bit"), "existing suffix");
        auto data = get(path);
        data.replace("</Runs>", "<Run Id=\"impl_3\" Type=\"Ft2:EntireDesign\" SynthRun=\"synth_1\" Dir=\"$PRUNDIR/impl_3\"/></Runs>");
        put(path, data);
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto artifacts = result[2].toString() + "/artifacts";
        QCOMPARE(get(artifacts + "/top.bit"), QByteArray("bit-a"));
        QCOMPARE(get(artifacts + "/top__2.bit"), QByteArray("existing suffix"));
        QCOMPARE(get(artifacts + "/top__3.bit"), QByteArray("bit-b"));
        QCOMPARE(get(artifacts + "/top__4.BIT"), QByteArray("case duplicate"));
        QCOMPARE(QDir(artifacts).entryList(QDir::Files).size(), 8);
        QVERIFY(QDir(artifacts).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void onlyProjectArtifactsAreCollected()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        for (const auto& relative : QStringList{"backup/top.bit", "other.runs/impl_1/alien.bin", "demo.runs/impl_1/backup/old.bit",
                "demo.runs/impl_old/unregistered.bit", "demo.runs/synth_1/not-an-output.ltx", "demo.runs/ip_impl/ooc.bit",
                "demo.srcs/old.hwh", "demo.gen/sources_1/bd/design/hw_handoff/design.hwh", "images/export.mcs"})
            put(temp.filePath("source/" + relative), "unrelated");
        auto data = get(path);
        data.replace("</FileSets>", "<FileSet Name=\"ip_sources\" Type=\"BlockSrcs\"/></FileSets>");
        data.replace("</Runs>", "<Run Id=\"ip_synth\" Type=\"Ft3:Synth\" SrcSet=\"ip_sources\"/>"
                     "<Run Id=\"ip_impl\" Type=\"Ft2:EntireDesign\" SynthRun=\"ip_synth\" Dir=\"$PRUNDIR/ip_impl\"/></Runs>");
        put(path, data);
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto artifacts = result[2].toString() + "/artifacts";
        QCOMPARE(QDir(artifacts).entryList(QDir::Files, QDir::Name), QStringList({"design.hwh", "top.BIN", "top.bit", "top.ltx", "top.mcs", "top__2.bit"}));
        QCOMPARE(get(artifacts + "/design.hwh"), QByteArray("hwh"));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void handoffFollowsActiveBdConfiguration_data()
    {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QByteArray>("expected");
        QTest::newRow("generated-directory") << QString("generated") << QByteArray("current generated handoff");
        QTest::newRow("missing-current-handoff") << QString("missing") << QByteArray{};
        QTest::newRow("disabled-bd") << QString("disabled") << QByteArray{};
        QTest::newRow("simulation-only-bd") << QString("simulation") << QByteArray{};
        QTest::newRow("unregistered-bd") << QString("unregistered") << QByteArray{};
    }
    void handoffFollowsActiveBdConfiguration()
    {
        QFETCH(QString, kind); QFETCH(QByteArray, expected);
        QTemporaryDir temp; const auto path = fixture(temp.path());
        auto data = get(path);
        if (kind == "generated" || kind == "missing") {
            data.replace("RelSrcDir=", "RelGenDir=\"$PGENDIR/sources_1\" RelSrcDir=");
            if (kind == "generated") put(temp.filePath("source/demo.gen/sources_1/bd/design/hw_handoff/design.hwh"), expected);
        } else {
            const QByteArray bd("<File Path=\"$PSRCDIR/sources_1/bd/design/design.bd\"/>");
            const QByteArray attr = kind == "disabled" ? "<Attr Name=\"UserDisabled\" Val=\"1\"/>" : "<Attr Name=\"UsedIn\" Val=\"simulation\"/>";
            data.replace(bd, kind == "unregistered" ? QByteArray{} : "<File Path=\"$PSRCDIR/sources_1/bd/design/design.bd\"><FileInfo>" + attr + "</FileInfo></File>");
        }
        put(path, data);
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto handoff = result[2].toString() + "/artifacts/design.hwh";
        QCOMPARE(QFileInfo::exists(handoff), !expected.isEmpty());
        if (!expected.isEmpty()) QCOMPARE(get(handoff), expected);
    }
    void implementationSourceSetSelectsHandoff()
    {
        QTemporaryDir temp; const auto path = fixture(temp.path());
        put(temp.filePath("source/demo.srcs/alternate/bd/new/new.bd"), "<bd/>");
        put(temp.filePath("source/demo.srcs/alternate/bd/new/hw_handoff/new.hwh"), "active handoff");
        auto data = get(path);
        data.replace("</FileSets>", "<FileSet Name=\"alternate\" Type=\"DesignSrcs\" RelSrcDir=\"$PSRCDIR/alternate\">"
                     "<File Path=\"$PSRCDIR/alternate/bd/new/new.bd\"/></FileSet></FileSets>");
        data.replace("SynthRun=\"synth_1\"", "SynthRun=\"alternate_synth\"");
        data.replace("</Runs>", "<Run Id=\"alternate_synth\" Type=\"Ft3:Synth\" SrcSet=\"alternate\"/></Runs>");
        put(path, data);
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        QCOMPARE(QDir(result[2].toString() + "/artifacts").entryList({"*.hwh"}, QDir::Files), QStringList{"new.hwh"});
    }
    void scanFindsEveryXprAndReportsInvalidProjects()
    {
        QTemporaryDir temp;
        const auto first = fixture(temp.filePath("projects/first"));
        const auto second = fixture(temp.filePath("projects/deep/second"));
        const auto upper = QFileInfo(second).path() + "/DEMO.XPR";
        QVERIFY(QFile::rename(second, upper));
        put(temp.filePath("projects/invalid.xpr"), "invalid");
        const auto before = snapshot(temp.filePath("projects"));
        ArchiveScan scan(temp.filePath("projects"));
        QSignalSpy finished(&scan, &QThread::finished); scan.start(); QVERIFY(finished.wait(5000)); scan.wait();
        QVERIFY(scan.error.isEmpty()); QVERIFY(!scan.cancelled); QCOMPARE(scan.projects.size(), 3);
        int valid = 0, invalid = 0;
        for (const auto& project : scan.projects) { if (project.error.isEmpty()) { ++valid; QCOMPARE(project.version, "2023.1"); } else ++invalid; }
        QCOMPARE(valid, 2); QCOMPARE(invalid, 1); QCOMPARE(snapshot(temp.filePath("projects")), before);
        ArchiveScan cancelled(temp.filePath("projects"));
        connect(&cancelled, &ArchiveScan::progress, &cancelled, [&] { cancelled.requestInterruption(); }, Qt::DirectConnection);
        cancelled.start(); QVERIFY(cancelled.wait(5000)); QVERIFY(cancelled.cancelled);
        Q_UNUSED(first)
    }
    void batchNamesPersistAndJobsRunInSequence()
    {
        QTemporaryDir temp;
        fixture(temp.filePath("projects/first")); fixture(temp.filePath("projects/second"));
        const auto before = snapshot(temp.filePath("projects"));
        ArchivePage page;
        page.restoreState({{"schema", 1}, {"inputMode", 1}, {"output", temp.filePath("output")},
            {"installations", QVariantList{QVariantMap{{"version", "2023.1"}, {"launcher", fake()}}}}});
        auto* batch = page.findChild<ArchiveBatchPanel*>();
        batch->scanFolder(temp.filePath("projects")); QVERIFY(!page.closeBlockReason().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(page.closeBlockReason().isEmpty(), 5000);
        auto* table = page.findChild<QTableWidget*>("archiveProjects"); QCOMPARE(table->rowCount(), 2);
        table->item(0, 3)->setText("First release"); table->item(1, 3)->setText("Second release");
        ArchivePage restored; restored.restoreState(page.saveState());
        auto* restoredTable = restored.findChild<QTableWidget*>("archiveProjects");
        QCOMPARE(restoredTable->rowCount(), 2); QCOMPARE(restoredTable->item(1, 3)->text(), QString("Second release"));
        batch->scanFolder(temp.filePath("projects")); QTRY_VERIFY_WITH_TIMEOUT(!batch->isScanning(), 5000);
        QCOMPARE(table->item(0, 3)->text(), QString("First release"));
        page.resize(1080, 850); page.show();
        const auto capture = qEnvironmentVariable("FTB_CAPTURE_DIR");
        if (!capture.isEmpty()) { QDir().mkpath(capture); QTest::qWait(100); QVERIFY(page.grab().save(capture + "/archive-batch.png")); }
        page.findChild<QPushButton*>("archiveStart")->click();
        QVERIFY(!page.closeBlockReason().isEmpty());
        QVERIFY(!(table->item(0, 3)->flags() & Qt::ItemIsEditable));
        QTRY_VERIFY_WITH_TIMEOUT(page.closeBlockReason().isEmpty(), 15000);
        for (const auto& name : {"First release", "Second release"}) {
            QVERIFY(QFileInfo(temp.filePath(QString("output/") + name + "/demo.xpr")).isFile());
            QVERIFY(QFileInfo(temp.filePath(QString("output/") + name + ".7z")).isFile());
        }
        QCOMPARE(table->item(0, 4)->text(), QString("Complete")); QCOMPARE(table->item(1, 4)->text(), QString("Complete"));
        QCOMPARE(page.findChild<QProgressBar*>("archiveOverall")->value(), 100);
        QCOMPARE(snapshot(temp.filePath("projects")), before);
    }
    void batchPreflightRejectsConflictsAndMissingReleases()
    {
        QTemporaryDir temp; fixture(temp.filePath("projects/first")); fixture(temp.filePath("projects/second"));
        ArchiveBatchPanel panel; panel.scanFolder(temp.filePath("projects")); QTRY_VERIFY_WITH_TIMEOUT(!panel.isScanning(), 5000);
        auto* table = panel.findChild<QTableWidget*>("archiveProjects");
        table->item(0, 3)->setText("release"); table->item(1, 3)->setText("RELEASE.7z");
        QString error;
        QVERIFY(panel.requests(temp.filePath("output"), {{"2023.1", fake()}}, error).isEmpty()); QVERIFY(error.contains("conflicts"));
        table->item(1, 3)->setText("second");
        QVERIFY(panel.requests(temp.filePath("output"), {{"2022.2", fake()}}, error).isEmpty()); QVERIFY(error.contains("Configure Vivado 2023.1"));
        table->item(0, 0)->setCheckState(Qt::Unchecked);
        QCOMPARE(panel.requests(temp.filePath("output"), {{"2023.1", fake()}}, error).size(), 1); QVERIFY(error.isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("output")));
    }
    void batchContinuesAfterFailureAndStopsOnCancel_data()
    {
        QTest::addColumn<bool>("cancel"); QTest::newRow("failure") << false; QTest::newRow("cancel") << true;
    }
    void batchContinuesAfterFailureAndStopsOnCancel()
    {
        QFETCH(bool, cancel);
        QTemporaryDir temp; fixture(temp.filePath("projects/first")); fixture(temp.filePath("projects/second"));
        if (!cancel) QVERIFY(QFile::remove(temp.filePath("projects/first/source/demo.srcs/top.v")));
        const auto before = snapshot(temp.filePath("projects"));
        ArchivePage page;
        page.restoreState({{"schema", 1}, {"inputMode", 1}, {"output", temp.filePath("output")},
            {"installations", QVariantList{QVariantMap{{"version", "2023.1"}, {"launcher", fake()}}}}});
        auto* panel = page.findChild<ArchiveBatchPanel*>(); panel->scanFolder(temp.filePath("projects"));
        QTRY_VERIFY_WITH_TIMEOUT(!panel->isScanning(), 5000);
        auto* table = page.findChild<QTableWidget*>("archiveProjects");
        table->item(0, 3)->setText("first"); table->item(1, 3)->setText("second");
        if (cancel) qputenv("FTB_FAKE_MODE", "hang");
        page.findChild<QPushButton*>("archiveStart")->click();
        if (cancel) {
            QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 4)->text(), QString("Running"), 5000);
            page.findChild<QPushButton*>("archiveCancel")->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(page.closeBlockReason().isEmpty(), 15000);
        QCOMPARE(table->item(0, 4)->text(), cancel ? QString("Cancelled") : QString("Failed"));
        QCOMPARE(table->item(1, 4)->text(), cancel ? QString("Not started") : QString("Complete"));
        QCOMPARE(QFileInfo::exists(temp.filePath("output/second.7z")), !cancel);
        QCOMPARE(snapshot(temp.filePath("projects")), before);
    }
    void compressionFailuresDoNotPublish_data()
    {
        QTest::addColumn<QByteArray>("mode");
        for (const auto* mode : {"fail", "invalid", "test-failure", "missing"}) QTest::newRow(mode) << QByteArray(mode);
    }
    void compressionFailuresDoNotPublish()
    {
        QFETCH(QByteArray, mode); qputenv("FTB_FAKE_7ZIP", mode);
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto before = snapshot(temp.filePath("source"));
        const auto compressor = mode == "missing" ? temp.filePath("missing.exe") : QCoreApplication::applicationDirPath() + "/xips_fake_7zip.exe";
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}, compressor});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool()); QVERIFY(result[2].toString().isEmpty());
        for (const auto& name : QDir(temp.filePath("output")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
            QVERIFY2(name.startsWith(".xips-archive-"), qPrintable(name));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void cancelCompressionStopsHelper()
    {
        qputenv("FTB_FAKE_7ZIP", "hang");
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        const auto before = snapshot(temp.filePath("source"));
        ArchiveJob job({path, temp.filePath("output"), {"2023.1", fake()}, QCoreApplication::applicationDirPath() + "/xips_fake_7zip.exe"});
        QSignalSpy done(&job, &ArchiveJob::completed);
        job.start();
        QString pidFile;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const auto& name : QDir(temp.filePath("output")).entryList({".xips-archive-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
                const auto candidate = temp.filePath("output/" + name + "/control/fake-7zip.pid");
                if (QFileInfo::exists(candidate) && !get(candidate).isEmpty()) { pidFile = candidate; return true; }
            }
            return false;
        })(), 15000);
#ifdef Q_OS_WIN
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, get(pidFile).toULong());
        QVERIFY(child != nullptr);
#endif
        job.requestInterruption();
        QVERIFY(done.wait(10000)); QVERIFY(job.wait(5000));
        QVERIFY(!done.first()[0].toBool()); QVERIFY(done.first()[1].toBool());
#ifdef Q_OS_WIN
        QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0)); CloseHandle(child);
#endif
        for (const auto& name : QDir(temp.filePath("output")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) QVERIFY(name.startsWith(".xips-archive-"));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void compactPageShowsDetailsOnDemand()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        ArchivePage page;
        page.restoreState({{"schema", 1}, {"project", path}, {"output", temp.filePath("output")},
            {"installations", QVariantList{QVariantMap{{"version", "2023.1"}, {"launcher", fake()}}}}});
        page.resize(820, 640); page.show();
        QVERIFY(page.findChild<QWidget*>("archiveProgress")->isHidden());
        QVERIFY(page.findChild<QWidget*>("archiveDetailsPanel")->isHidden());
        QCOMPARE(page.findChild<QComboBox*>("archiveVersion")->currentText(), "Vivado 2023.1");
        QVERIFY(!page.findChild<QPushButton*>("archiveDiscover"));
        auto* start = page.findChild<QPushButton*>("archiveStart"); QVERIFY(start->isEnabled());
        auto* name = page.findChild<QLineEdit*>("archiveName"); QVERIFY(name->text().isEmpty());
        name->setText("../outside"); QVERIFY(!start->isEnabled());
        QVERIFY(page.findChild<QLabel*>("archiveNameHint")->text().contains("without paths"));
        name->setText("Release candidate"); QVERIFY(start->isEnabled()); start->click();
        QVERIFY(!name->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(page.closeBlockReason().isEmpty(), 15000);
        QVERIFY(name->isEnabled());
        QVERIFY(QFileInfo(temp.filePath("output/Release candidate/demo.xpr")).isFile());
        QVERIFY(QFileInfo(temp.filePath("output/Release candidate.7z")).isFile());
        QCOMPARE(page.findChild<QProgressBar*>("archiveOverall")->value(), 100);
        QCOMPARE(page.findChild<QProgressBar*>("archiveStage5")->value(), 100);
        QVERIFY(page.findChild<QWidget*>("archiveDetailsPanel")->isHidden());
        QVERIFY(!page.findChild<QPushButton*>("archiveOpen")->isHidden());
        page.findChild<QPushButton*>("archiveDetails")->click();
        QVERIFY(!page.findChild<QWidget*>("archiveDetailsPanel")->isHidden());
        QVERIFY(page.findChild<QLabel*>("archiveResult")->text().contains(".7z"));
        const auto capture = qEnvironmentVariable("FTB_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            page.findChild<QPushButton*>("archiveDetails")->click(); QTest::qWait(100);
            QVERIFY(page.grab().save(capture + "/archive-complete.png"));
        }
    }
    void generatedHdlSourcesAreRetained()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path(), "$PGENDIR/sources_1/bd/design_1/hdl/design_1_wrapper.v");
        const QByteArray wrapper("`include \"defs.vh\"\nmodule design_1_wrapper(); endmodule\n");
        const auto hdl = QString("demo.gen/sources_1/bd/design_1/hdl");
        const auto stub = QString("demo.ip_user_files/ip/core/core_stub.v");
        put(temp.filePath("source/" + hdl + "/design_1_wrapper.v"), wrapper);
        put(temp.filePath("source/" + hdl + "/defs.vh"), "`define WIDTH 1\n");
        put(temp.filePath("source/" + stub), "module core(); endmodule\n");
        put(temp.filePath("source/demo.gen/include/shared.vh"), "`define SHARED 1\n");
        put(temp.filePath("source/demo.gen/unreferenced/output.v"), "discard\n");
        auto data = get(path);
        data.replace("RelSrcDir=", "RelGenDir=\"$PGENDIR/sources_1\" RelSrcDir=");
        data.replace("</FileSet>", "<File Path=\"$PIPUSERFILESDIR/ip/core/core_stub.v\"/></FileSet>");
        data.replace("</Configuration>", "<Option Name=\"IncludeDirs\" Val=\"$PGENDIR/include\"/></Configuration>");
        put(path, data);
        const auto before = snapshot(temp.filePath("source"));
        qputenv("FTB_TEST_SOURCE", temp.filePath("source").toUtf8());
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QCOMPARE(get(dest + "/demo.srcs/_archive_generated/" + hdl + "/design_1_wrapper.v"), wrapper);
        QCOMPARE(get(dest + "/demo.srcs/_archive_generated/" + hdl + "/defs.vh"), QByteArray("`define WIDTH 1\n"));
        QCOMPARE(get(dest + "/demo.srcs/_archive_generated/" + stub), QByteArray("module core(); endmodule\n"));
        QCOMPARE(get(dest + "/demo.srcs/_archive_generated/demo.gen/include/shared.vh"), QByteArray("`define SHARED 1\n"));
        QVERIFY(!QFileInfo::exists(dest + "/demo.srcs/_archive_generated/demo.gen/unreferenced"));
        const auto relocated = get(dest + "/demo.xpr");
        QVERIFY(relocated.contains(("$PSRCDIR/_archive_generated/" + hdl + "/design_1_wrapper.v").toUtf8()));
        QVERIFY(relocated.contains(("$PSRCDIR/_archive_generated/" + stub).toUtf8()));
        QVERIFY(relocated.contains("RelGenDir=\"$PPRDIR/demo.gen/sources_1\""));
        QVERIFY(!QFileInfo::exists(dest + "/demo.gen"));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void missingIpCatalogPathsOnlyChangeTheArchiveCopy()
    {
        QTemporaryDir temp; const auto path = fixture(temp.path());
        put(temp.filePath("source/catalog/component.xml"), "<component/>");
        auto data = get(path);
        data.replace("</Configuration>", (QString("<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../obsolete/myip\"/>"
            "<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/missing-local\"/>"
            "<Option Name=\"IPRepoPath\" Val=\"%1\"/>"
            "<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/catalog\"/></Configuration>")
            .arg(temp.filePath("old catalog/myip").toHtmlEscaped())).toUtf8());
        put(path, data);
        const auto before = snapshot(temp.filePath("source"));
        QVERIFY_EXCEPTION_THROWN(isolateProject(path, temp.filePath("strict-copy")), std::runtime_error);
        QVERIFY(!QFileInfo::exists(temp.filePath("strict-copy")));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QDomDocument copy; QVERIFY(copy.setContent(get(dest + "/demo.xpr")));
        QStringList repositories;
        const auto options = copy.elementsByTagName("Option");
        for (int i = 0; i < options.size(); ++i) {
            const auto option = options.at(i).toElement();
            if (option.attribute("Name") == "IPRepoPath") repositories.append(option.attribute("Val"));
        }
        QCOMPARE(repositories, QStringList{"$PSRCDIR/_archive_imports/catalog"});
        QCOMPARE(get(dest + "/demo.srcs/_archive_imports/catalog/component.xml"), QByteArray("<component/>"));
        const auto reopened = isolateProject(dest + "/demo.xpr", temp.filePath("reopened"));
        QVERIFY(QFileInfo(reopened.xpr).isFile());
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void missingIpCatalogCleanupKeepsDependencyGuards_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"existing-external", "required-source", "include-directory", "board-repository", "path-list", "unknown-token", "wrong-context", "not-directory"})
            QTest::newRow(kind) << QString(kind);
    }
    void missingIpCatalogCleanupKeepsDependencyGuards()
    {
        QFETCH(QString, kind);
        QTemporaryDir temp; const auto path = fixture(temp.path());
        QByteArray option("<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../external\"/>");
        if (kind == "existing-external") put(temp.filePath("external/component.xml"), "<component/>");
        if (kind == "include-directory") option.replace("IPRepoPath", "IncludeDirs");
        if (kind == "board-repository") option.replace("IPRepoPath", "BoardRepoPath");
        if (kind == "path-list") option.replace("../external", "../external;../another");
        if (kind == "unknown-token") option.replace("$PPRDIR", "$UNKNOWN");
        if (kind == "not-directory") { option.replace("../external", "catalog-file"); put(temp.filePath("source/catalog-file"), "not a directory"); }
        auto data = get(path);
        if (kind == "wrong-context") data.replace("</Project>", "<Other>" + option + "</Other></Project>");
        else data.replace("</Configuration>", option + "</Configuration>");
        if (kind == "required-source") data.replace("$PSRCDIR/top.v", "$PSRCDIR/missing.v");
        put(path, data);
        const auto before = snapshot(temp.path());
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        QVERIFY(!QFileInfo::exists(temp.filePath("output")));
        QCOMPARE(snapshot(temp.path()), before);
    }
    void catalogVerificationPrecedesReset_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("success");
        QTest::newRow("available") << QString("available") << true;
        QTest::newRow("missing-standalone-ip") << QString("missing-ip") << false;
        QTest::newRow("missing-bd-ip") << QString("missing-bd-ip") << false;
        QTest::newRow("catalog-error") << QString("catalog-error") << false;
        QTest::newRow("unreadable-bd") << QString("unreadable-bd") << false;
    }
    void catalogVerificationPrecedesReset()
    {
        QFETCH(QString, mode); QFETCH(bool, success);
        QTemporaryDir temp; const auto path = fixture(temp.path());
        auto data = get(path); data.replace("</Configuration>", "<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../missing\"/></Configuration>"); put(path, data);
        qputenv("FTB_FAKE_MODE", "fail");
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        const auto workspace = result[3].toString();
        QVERIFY(QFileInfo(workspace + "/control/reset.tcl").isFile());
        const auto tclsh = qEnvironmentVariable("FTB_TEST_TCLSH");
        QVERIFY2(QFileInfo::exists(tclsh), "Set FTB_TEST_TCLSH to run the catalog script checks.");
        QProcess process; auto env = QProcessEnvironment::systemEnvironment();
        env.insert("FTB_VERSION", "2023.1"); env.insert("FTB_XPR", workspace + "/project/demo.xpr");
        env.insert("FTB_PROJECT_ROOT", workspace + "/project"); env.insert("FTB_VERIFY_IP_CATALOG", "1");
        env.insert("FTB_RESET_MARKER", temp.filePath("reset.called")); env.insert("FTB_SUCCESS_FILE", temp.filePath("reset.ok"));
        process.setProcessEnvironment(env); process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(tclsh, {QString(FTB_TEST_SOURCE_DIR) + "/fixtures/archive_catalog_harness.tcl", workspace + "/control/reset.tcl", mode});
        QVERIFY(process.waitForFinished(8000)); const auto output = process.readAll();
        QCOMPARE(process.exitCode(), success ? 0 : 1);
        QCOMPARE(QFileInfo::exists(temp.filePath("reset.called")), success);
        QCOMPARE(QFileInfo::exists(temp.filePath("reset.ok")), success);
        if (success) QVERIFY(output.contains("FTB_IP_CATALOG_VERIFIED"));
        else QVERIFY2(output.contains("FTB_ERROR"), output.constData());
    }
    void metadataNamespacesAreNotDiskPaths()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        put(temp.filePath("source/demo.srcs/core.xci"),
            "<spirit:design xmlns:spirit=\"http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009\" "
            "xmlns:xilinx=\"http://www.xilinx.com\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"/>");
        put(temp.filePath("source/demo.srcs/design.bd"), "{\"url\":\"https://www.xilinx.com\"}");
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void staleAutomaticCheckpointIsRemoved()
    {
        QTemporaryDir temp;
        const auto path = fixture(temp.path());
        auto data = get(path);
        data.replace("</FileSets>", "<FileSet Name=\"utils_1\" Type=\"Utils\">"
            "<File Path=\"$PSRCDIR/utils_1/imports/obsolete.dcp\"><FileInfo><Attr Name=\"AutoDcp\" Val=\"1\"/></FileInfo></File>"
            "<File Path=\"$PSRCDIR/utils_1/imports/current.dcp\"><FileInfo><Attr Name=\"AutoDcp\" Val=\"1\"/></FileInfo></File>"
            "</FileSet></FileSets>");
        put(path, data);
        put(temp.filePath("source/demo.srcs/utils_1/imports/current.dcp"), "existing checkpoint");
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString()));
        const auto dest = result[2].toString();
        QVERIFY(!get(dest + "/demo.xpr").contains("obsolete.dcp"));
        QVERIFY(get(dest + "/demo.xpr").contains("current.dcp"));
        QCOMPARE(get(dest + "/demo.srcs/utils_1/imports/current.dcp"), QByteArray("existing checkpoint"));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void refusesUnsafeInputs_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto* k : {"version", "missing-vivado", "outside", "traversal", "overlap", "metadata", "metadata-unc",
             "metadata-relative", "missing-source", "missing-checkpoint", "design-checkpoint", "generated-missing",
             "generated-metadata", "generated-collision", "tcl-hook"}) QTest::newRow(k) << QString(k);
    }
    void refusesUnsafeInputs()
    {
        QFETCH(QString, kind);
        QTemporaryDir temp;
        QString path = fixture(temp.path());
        Request request{path, temp.filePath("output"), {"2023.1", fake()}};
        if (kind == "version") request.vivado.version = "2022.2";
        if (kind == "missing-vivado") request.vivado.launcher = temp.filePath("missing.exe");
        if (kind == "outside") fixture(temp.path(), temp.filePath("outside.v"));
        if (kind == "traversal") fixture(temp.path(), "$PSRCDIR/../../outside.v");
        if (kind == "overlap") request.outputRoot = temp.filePath("source/output");
        if (kind == "metadata") put(temp.filePath("source/demo.srcs/core.xci"), "<file>D:/external/original.coe</file>");
        if (kind == "metadata-unc") put(temp.filePath("source/demo.srcs/core.xci"), "<file>\\\\server\\share\\original.coe</file>");
        if (kind == "metadata-relative") put(temp.filePath("source/demo.srcs/core.xci"), "<file>../../../external.coe</file>");
        if (kind == "missing-source") fixture(temp.path(), "$PSRCDIR/missing.v");
        if (kind == "missing-checkpoint") fixture(temp.path(), "$PSRCDIR/required.dcp");
        if (kind == "design-checkpoint") {
            fixture(temp.path(), "$PSRCDIR/required.dcp");
            auto data = get(path);
            data.replace("<File Path=\"$PSRCDIR/required.dcp\"/>", "<File Path=\"$PSRCDIR/required.dcp\"><FileInfo><Attr Name=\"AutoDcp\" Val=\"1\"/></FileInfo></File>");
            put(path, data);
        }
        if (kind == "generated-missing") fixture(temp.path(), "$PGENDIR/hdl/missing.v");
        if (kind == "generated-metadata") {
            fixture(temp.path(), "$PGENDIR/core/core.xci");
            put(temp.filePath("source/demo.gen/core/core.xci"), "<core/>");
        }
        if (kind == "generated-collision") {
            fixture(temp.path(), "$PGENDIR/hdl/wrapper.v");
            put(temp.filePath("source/demo.gen/hdl/wrapper.v"), "module wrapper(); endmodule\n");
            put(temp.filePath("source/demo.srcs/_archive_generated/demo.gen/hdl/keep.v"), "existing user source");
        }
        if (kind == "tcl-hook") {
            auto data = get(path);
            data.replace("</Project>", "<Runs><Run><Strategy><Step Id=\"synth_design\"><Option Id=\"STEPS.SYNTH_DESIGN.TCL.PRE\"><OptionVal><Str Val=\"$PPRDIR/hook.tcl\"/></OptionVal></Option></Step></Strategy></Run></Runs></Project>");
            put(path, data);
            put(temp.filePath("source/hook.tcl"), "error {must never execute}\n");
        }
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute(request);
        QVERIFY(!result.isEmpty()); QVERIFY2(!result[0].toBool(), qPrintable(kind));
        QVERIFY(!QFileInfo::exists(request.outputRoot));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void failureAndMissingSuccessMarker_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("nonzero-exit") << QByteArray("fail");
        QTest::newRow("no-success-marker") << QByteArray("missing-marker");
    }
    void failureAndMissingSuccessMarker()
    {
        QFETCH(QByteArray, mode); qputenv("FTB_FAKE_MODE", mode);
        QTemporaryDir temp; const auto path = fixture(temp.path());
        const auto before = snapshot(temp.filePath("source"));
        const auto result = execute({path, temp.filePath("output"), {"2023.1", fake()}});
        QVERIFY(!result.isEmpty()); QVERIFY(!result[0].toBool());
        QVERIFY(result[2].toString().isEmpty()); QVERIFY(QFileInfo::exists(result[3].toString() + "/control/vivado.log"));
        QCOMPARE(snapshot(temp.filePath("source")), before);
    }
    void cancelAndCloseGuard()
    {
        QTemporaryDir temp; const auto path = fixture(temp.path());
        qputenv("FTB_FAKE_MODE", "hang");
        ArchiveState store(temp.filePath("settings.ini"), temp.filePath("absent.ini"));
        const QVariantList installs{QVariantMap{{"version", "2023.1"}, {"launcher", fake()}}};
        store.save({{"schema", 1}, {"installations", installs}, {"project", path}, {"output", temp.filePath("output")}, {"selected", fake()}});
        ArchiveWindow workspace(nullptr, store);
        auto* page = workspace.page(); QVERIFY(page);
        auto* start = page->findChild<QPushButton*>("archiveStart");
        start->click();
        QTRY_VERIFY(!page->closeBlockReason().isEmpty());
        QVERIFY(!workspace.close());
        auto* stage = page->findChild<QProgressBar*>("archiveStage3");
        QTRY_COMPARE_WITH_TIMEOUT(stage->maximum(), 0, 10000);
        const auto workspaces = QDir(temp.filePath("output")).entryList({".xips-archive-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(workspaces.size(), 1);
        const auto pidFile = temp.filePath("output/" + workspaces.first() + "/control/fake.pid");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(pidFile), 15000);
#ifdef Q_OS_WIN
        const auto pid = get(pidFile).toULong();
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
        QVERIFY(child != nullptr);
#endif
        page->findChild<QPushButton*>("archiveCancel")->click();
        QTRY_VERIFY_WITH_TIMEOUT(page->closeBlockReason().isEmpty(), 10000);
        QVERIFY(page->findChild<QLabel*>("archiveStatus")->text().contains("Cancelled"));
#ifdef Q_OS_WIN
        QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0));
        CloseHandle(child);
#endif
        QVERIFY(workspace.close());
    }
    void dropAndPersistPaths()
    {
        QTemporaryDir temp; const auto path = fixture(temp.path());
        ArchiveState store(temp.filePath("settings.ini"), temp.filePath("absent.ini"));
        store.save({{"schema", 1}, {"installations", QVariantList{QVariantMap{{"version", "2023.1"}, {"launcher", fake()}}}}});
        ArchiveWindow workspace(nullptr, store);
        auto* page = workspace.page(); workspace.resize(1000, 900); workspace.show();
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(page, &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(page, &drop); QVERIFY(drop.isAccepted());
        QCOMPARE(page->findChild<QLineEdit*>("archiveProject")->text(), path);
        QCOMPARE(store.load().value("project").toString(), path);
        auto* name = page->findChild<QLineEdit*>("archiveName"); QVERIFY(name->text().isEmpty());
        name->setText("UART release 1");
        QVERIFY(QMetaObject::invokeMethod(name, "editingFinished"));
        QCOMPARE(store.load().value("archiveName").toString(), QString("UART release 1"));
        const auto capture = qEnvironmentVariable("FTB_CAPTURE_DIR");
        if (!capture.isEmpty()) { QDir().mkpath(capture); QTest::qWait(100); QVERIFY(workspace.grab().save(capture + "/archive-page.png")); }
        QVERIFY(workspace.close());
        auto* reopened = workspace.page();
        QCOMPARE(reopened->findChild<QLineEdit*>("archiveProject")->text(), path);
        QCOMPARE(reopened->findChild<QComboBox*>("archiveVersion")->count(), 1);
        QCOMPARE(reopened->findChild<QLineEdit*>("archiveName")->text(), QString("UART release 1"));
    }
    void inspectExistingProjectReadOnly()
    {
        const auto xpr = qEnvironmentVariable("FTB_TEST_INSPECT_XPR");
        if (xpr.isEmpty()) QSKIP("Set FTB_TEST_INSPECT_XPR for read-only inspection of an existing XPR.");
        const auto before = get(xpr);
        const auto modified = QFileInfo(xpr).lastModified();
        const auto info = inspectProject(xpr);
        qInfo().noquote() << "REQUIRED_RELEASE=" + info.version;
        QCOMPARE(get(xpr), before);
        QCOMPARE(QFileInfo(xpr).lastModified(), modified);
    }
    void realMissingCatalogDefinition()
    {
        const auto launcher = qEnvironmentVariable("FTB_TEST_VIVADO");
        if (launcher.isEmpty()) QSKIP("Set FTB_TEST_VIVADO for a real missing-catalog regression.");
        const auto version = installationVersion(launcher);
        QVERIFY(!version.isEmpty());
        QTemporaryDir temp; const auto root = temp.filePath("source");
        const QByteArray create = R"TCL(
if {[catch {
    if {[version -short] ne $::env(FTB_VERSION)} {error "Unexpected Vivado version"}
    create_project missing_catalog $::env(FTB_PROJECT_ROOT) -part xc7a35tcpg236-1
    set defs [get_ipdefs -all -quiet xilinx.com:ip:xlconstant:*]
    if {[llength $defs] == 0} {error "Fixture IP is not available"}
    create_ip -vlnv [lindex $defs 0] -module_name fixture_constant
    close_project
    set f [open $::env(FTB_SUCCESS_FILE) w]; puts $f "Fixture created"; close $f
} reason]} {puts stderr $reason; exit 1}
exit 0
)TCL";
        try { runBatch({version, launcher}, temp.filePath("create"), create, {{"FTB_PROJECT_ROOT", root}}, [] {}, [](const QString&) {}); }
        catch (const std::exception& ex) { QFAIL(ex.what()); }
        QDirIterator ips(root, {"*.xci"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(ips.hasNext()); const auto xci = ips.next(); QVERIFY(!ips.hasNext());
        auto data = get(xci); QVERIFY(data.contains("spirit:name=\"xlconstant\""));
        data.replace("spirit:name=\"xlconstant\"", "spirit:name=\"ftb_missing_catalog_core\""); put(xci, data);
        const auto xpr = root + "/missing_catalog.xpr";
        data = get(xpr); data.replace("</Configuration>", "<Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../missing_repo\"/></Configuration>"); put(xpr, data);
        const auto before = snapshot(root);
        const auto result = execute({xpr, temp.filePath("output"), {version, launcher}, {}, "must_not_publish"}, 240000);
        QVERIFY(!result.isEmpty()); QVERIFY2(!result[0].toBool(), "An unavailable IP definition must prevent archive publication.");
        const auto log = get(result[3].toString() + "/control/vivado.log");
        QVERIFY2(log.contains("Missing IP definition"), log.constData());
        QVERIFY(log.contains("ftb_missing_catalog_core")); QVERIFY(!log.contains("FTB_STAGE_RESET"));
        QVERIFY(!QFileInfo::exists(temp.filePath("output/must_not_publish")));
        QVERIFY(!QFileInfo::exists(temp.filePath("output/must_not_publish.7z")));
        QCOMPARE(snapshot(root), before);
        qInfo() << "Real unavailable IP definition blocked before reset/publication; input unchanged.";
    }
    void realVivado()
    {
        const auto launcher = qEnvironmentVariable("FTB_TEST_VIVADO");
        const auto xpr = qEnvironmentVariable("FTB_TEST_XPR");
        const auto output = qEnvironmentVariable("FTB_TEST_OUTPUT");
        const auto archiveName = qEnvironmentVariable("FTB_TEST_ARCHIVE_NAME");
        if (launcher.isEmpty() || xpr.isEmpty() || output.isEmpty()) QSKIP("Set FTB_TEST_VIVADO, FTB_TEST_XPR and FTB_TEST_OUTPUT for a real integration run.");
        const auto info = inspectProject(xpr);
        const auto before = snapshot(info.root);
        QStringList messages;
        const auto result = execute({xpr, output, {info.version, launcher}, {}, archiveName}, 240000, &messages);
        QVERIFY(!result.isEmpty()); QVERIFY2(result[0].toBool(), qPrintable(result[4].toString() + " Diagnostics: " + result[3].toString()));
        QCOMPARE(snapshot(info.root), before);
        if (!archiveName.trimmed().isEmpty()) QCOMPARE(QFileInfo(result[2].toString()).fileName(), archiveName.trimmed());
        QStringList expectedEntries{"artifacts", info.name + ".srcs", info.name + ".xpr"}; expectedEntries.sort();
        QCOMPARE(QDir(result[2].toString()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name), expectedEntries);
        if (qEnvironmentVariableIsSet("FTB_TEST_EXPECT_CATALOG_CHECK")) {
            QVERIFY(messages.join('\n').contains("FTB_IP_CATALOG_VERIFIED"));
            QVERIFY(!get(result[2].toString() + '/' + info.name + ".xpr").contains("IPRepoPath"));
            qInfo() << "Missing catalog paths removed; real IP definitions verified before reset.";
        }
        QVERIFY(QFileInfo(result[2].toString() + ".7z").isFile());
        QVERIFY(QDir(result[2].toString() + "/artifacts").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        qInfo().noquote() << "REAL_ARCHIVE=" + result[2].toString();
        QTemporaryDir reopened(QDir(output).filePath("reopen-XXXXXX"));
        QVERIFY(reopened.isValid());
        const auto isolated = isolateProject(result[2].toString() + '/' + info.name + ".xpr",
            reopened.filePath(archiveName.trimmed().isEmpty() ? "project" : archiveName.trimmed()));
        const QByteArray script = R"TCL(
if {[catch {
    if {[version -short] ne $::env(FTB_VERSION)} {error "Unexpected Vivado version"}
    open_project $::env(FTB_XPR)
    set root [string tolower [file normalize $::env(FTB_PROJECT_ROOT)]]
    foreach f [get_files -all -quiet] {
        set path [string tolower [file normalize $f]]
        if {$path ne $root && [string first "$root/" $path] != 0} {error "External file after relocation: $f"}
    }
    close_project
    set marker [open $::env(FTB_SUCCESS_FILE) w]
    puts $marker "Relocated archive reopened"
    close $marker
} reason]} { puts stderr $reason; exit 1 }
exit 0
)TCL";
        try {
            runBatch({info.version, launcher}, reopened.filePath("control"), script,
                     {{"FTB_XPR", isolated.xpr}, {"FTB_PROJECT_ROOT", isolated.root}}, [] {},
                     [](const QString& line) { qInfo().noquote() << line; });
        } catch (const std::exception& ex) { QFAIL(ex.what()); }
        QCOMPARE(snapshot(info.root), before);
    }
};
QTEST_MAIN(ArchiveTests)
#include "tst_archive.moc"
