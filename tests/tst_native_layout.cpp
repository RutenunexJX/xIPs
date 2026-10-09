#include "xips/BrowserApi.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <qt_windows.h>

namespace
{
QString modulePath(const wchar_t *name)
{
    const auto module = GetModuleHandleW(name);
    if (!module) return {};
    wchar_t path[32768];
    const auto size = GetModuleFileNameW(module, path, 32768);
    return size && size < 32768 ? QFileInfo(QString::fromWCharArray(path, int(size))).canonicalFilePath() : QString();
}
QString dllDirectory()
{
    wchar_t path[32768];
    const auto size = GetDllDirectoryW(32768, path);
    return size < 32768 ? QString::fromWCharArray(path, int(size)) : QStringLiteral("invalid");
}
int childProbe(const QStringList &arguments)
{
    if (arguments.size() != 5) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                      QCoreApplication::applicationDirPath() + "/settings");
    QCoreApplication::setOrganizationName("NativeLayoutHost");
    QCoreApplication::setApplicationName("Host application");
    QCoreApplication::setApplicationVersion("9.1");
    for (const auto &name : {"Qt6Network.dll", "Qt6Sql.dll"})
    {
        const auto path = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + '/' + name);
        if (!LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return 5;
    }
    const auto environment = QProcessEnvironment::systemEnvironment();
    const auto cwd = QDir::currentPath();
    const auto dllPath = dllDirectory();
    const auto pluginPaths = QCoreApplication::libraryPaths();
    const auto font = qApp->font();
    QJsonObject result{{"pathEmpty", qEnvironmentVariableIsEmpty("PATH")},
                       {"hostModulesPreloaded", !modulePath(L"Qt6Network.dll").isEmpty() && !modulePath(L"Qt6Sql.dll").isEmpty()},
                       {"cwd", cwd}, {"xmlPreloaded", !modulePath(L"Qt6Xml.dll").isEmpty()}};
    QLibrary entry(arguments[2]);
    entry.setLoadHints(QLibrary::PreventUnloadHint);
    const bool loaded = entry.load();
    result["entryLoaded"] = loaded;
    result["entryError"] = entry.errorString();
    if (loaded)
    {
        const auto factory = reinterpret_cast<XipsCreateBrowserV1>(entry.resolve("xips_create_browser_v1"));
        const auto error = reinterpret_cast<XipsBrowserLastErrorV1>(entry.resolve("xips_browser_last_error_v1"));
        if (!factory || !error) return 3;
        QObject bridge;
        QWidget owner;
        auto *panel = factory(&owner, &bridge);
        result["created"] = panel != nullptr;
        result["error"] = QString::fromUtf8(error());
        result["firstXmlModule"] = modulePath(L"Qt6Xml.dll");
        if (!panel && arguments[4] == "recover")
        {
            result["dependencyRestored"] = QFile::copy(cwd + "/Qt6Xml.dll",
                QCoreApplication::applicationDirPath() + "/Qt6Xml.dll");
            panel = factory(&owner, &bridge);
            result["retryCreated"] = panel != nullptr;
            result["retryError"] = QString::fromUtf8(error());
        }
        result["xmlModule"] = modulePath(L"Qt6Xml.dll");
        result["elaModule"] = modulePath(L"XipsEla.dll");
        result["implementationModule"] = modulePath(L"xips-browser-impl.dll");
        delete panel;
    }
    result["environmentUnchanged"] = environment == QProcessEnvironment::systemEnvironment();
    result["cwdUnchanged"] = cwd == QDir::currentPath();
    result["dllDirectoryUnchanged"] = dllPath == dllDirectory();
    result["pluginPathsUnchanged"] = pluginPaths == QCoreApplication::libraryPaths();
    result["hostIdentityUnchanged"] = QCoreApplication::organizationName() == "NativeLayoutHost"
        && QCoreApplication::applicationName() == "Host application"
        && QCoreApplication::applicationVersion() == "9.1";
    result["hostFontUnchanged"] = font == qApp->font();
    QFile output(arguments[3]);
    return output.open(QIODevice::WriteOnly) && output.write(QJsonDocument(result).toJson()) > 0 ? 0 : 4;
}
bool copyFile(const QString &source, const QString &destination)
{
    return QDir().mkpath(QFileInfo(destination).absolutePath()) && QFile::copy(source, destination);
}
QString normalized(const QString &path)
{
    return QFileInfo(path).canonicalFilePath().toLower();
}
}

class NativeLayoutTest final : public QObject
{
    Q_OBJECT
  private slots:
    void freshProcessLayout_data()
    {
        QTest::addColumn<bool>("embedded");
        QTest::addColumn<bool>("xmlInCwd");
        QTest::addColumn<bool>("missingImplementation");
        QTest::newRow("standalone") << false << false << false;
        QTest::newRow("embedded-host-shared-xml") << true << false << false;
        QTest::newRow("embedded-reject-cwd-and-retry") << true << true << false;
        QTest::newRow("missing-implementation") << true << false << true;
    }
    void freshProcessLayout()
    {
        QFETCH(bool, embedded);
        QFETCH(bool, xmlInCwd);
        QFETCH(bool, missingImplementation);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto root = fixture.filePath(QStringLiteral("host \u5171\u4eab"));
        const auto cwd = fixture.filePath("unrelated working directory");
        const auto component = embedded ? root + "/components/xips" : root;
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(cwd));
        QVERIFY(QDir().mkpath(root + "/empty-library"));
        const auto package = qEnvironmentVariable("XIPS_TEST_PACKAGE");
        const auto browserRoot = package.isEmpty()
            ? QFileInfo(QString(XIPS_BROWSER_PATH)).absolutePath() : package;
        const auto qtRoot = package.isEmpty() ? QString(XIPS_TEST_QT_BIN) : package;
        const auto compilerRoot = package.isEmpty() ? QString(XIPS_TEST_COMPILER_BIN) : package;
        const auto executable = root + "/layout-host.exe";
        QVERIFY(copyFile(QCoreApplication::applicationFilePath(), executable));
        for (const auto &name : {"Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Sql.dll"})
            QVERIFY2(copyFile(qtRoot + '/' + name, root + '/' + name), name);
        QVERIFY(copyFile(QString(XIPS_TEST_QT_BIN) + "/Qt6Test.dll", root + "/Qt6Test.dll"));
        for (const auto &name : {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"})
            QVERIFY2(copyFile(compilerRoot + '/' + name, root + '/' + name), name);
        QVERIFY(copyFile(QString(XIPS_TEST_QT_PLUGINS) + "/platforms/qoffscreen.dll",
                         root + "/platforms/qoffscreen.dll"));
        const auto plugins = package.isEmpty() ? QString(XIPS_TEST_QT_PLUGINS) : package;
        QVERIFY(copyFile(plugins + "/sqldrivers/qsqlite.dll", root + "/sqldrivers/qsqlite.dll"));
        for (const auto &name : {"xips-browser.dll", "xips-browser-impl.dll", "XipsEla.dll"})
        {
            if (missingImplementation && QByteArray(name) == "xips-browser-impl.dll") continue;
            QVERIFY2(copyFile(browserRoot + '/' + name, component + '/' + name), name);
        }
        QVERIFY(copyFile(qtRoot + "/Qt6Xml.dll", (xmlInCwd ? cwd : root) + "/Qt6Xml.dll"));
        if (embedded)
        {
            QVERIFY(!QFileInfo::exists(component + "/Qt6Xml.dll"));
            QVERIFY(!QFileInfo::exists(root + "/XipsEla.dll"));
        }
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", "");
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_PLUGIN_PATH", root);
        environment.insert("QT_QPA_PLATFORM_PLUGIN_PATH", root + "/platforms");
        environment.insert("XIPS_LIBRARY", root + "/empty-library");
        QProcess child;
        child.setProcessEnvironment(environment);
        child.setWorkingDirectory(cwd);
        child.setProcessChannelMode(QProcess::MergedChannels);
        const auto output = fixture.filePath("result.json");
        child.start(executable, {"--layout-child", component + "/xips-browser.dll", output,
                                xmlInCwd ? "recover" : "once"});
        QVERIFY2(child.waitForStarted(10000), qPrintable(child.errorString()));
        QVERIFY2(child.waitForFinished(30000), qPrintable(child.errorString()));
        const auto transcript = child.readAll();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, transcript.constData());
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        const auto result = QJsonDocument::fromJson(bytes).object();
        const auto evidence = qEnvironmentVariable("XIPS_NATIVE_LAYOUT_EVIDENCE");
        if (!evidence.isEmpty())
        {
            QVERIFY(QDir().mkpath(evidence));
            QFile report(evidence + '/' + QString::fromLatin1(QTest::currentDataTag()) + ".json");
            QVERIFY(report.open(QIODevice::WriteOnly));
            QCOMPARE(report.write(bytes), bytes.size());
            QFile log(evidence + '/' + QString::fromLatin1(QTest::currentDataTag()) + ".log");
            QVERIFY(log.open(QIODevice::WriteOnly));
            QCOMPARE(log.write(transcript), transcript.size());
        }
        QVERIFY2(result["pathEmpty"].toBool(), bytes.constData());
        QVERIFY2(result["hostModulesPreloaded"].toBool(), bytes.constData());
        QVERIFY2(!result["xmlPreloaded"].toBool(), bytes.constData());
        QVERIFY2(result["entryLoaded"].toBool(), bytes.constData());
        for (const auto &field : {"environmentUnchanged", "cwdUnchanged", "dllDirectoryUnchanged",
                                 "pluginPathsUnchanged", "hostIdentityUnchanged", "hostFontUnchanged"})
            QVERIFY2(result[field].toBool(), qPrintable(QString::fromLatin1(field) + ": " + QString::fromUtf8(bytes)));
        QCOMPARE(result["created"].toBool(), !xmlInCwd && !missingImplementation);
        if (xmlInCwd || missingImplementation)
        {
            QVERIFY2(result["error"].toString().contains("Windows error 126"), bytes.constData());
            QVERIFY(result["firstXmlModule"].toString().isEmpty());
        }
        if (xmlInCwd)
        {
            QVERIFY2(result["dependencyRestored"].toBool(), bytes.constData());
            QVERIFY2(result["retryCreated"].toBool(), bytes.constData());
            QVERIFY(result["retryError"].toString().isEmpty());
        }
        if (!missingImplementation)
        {
            QCOMPARE(normalized(result["xmlModule"].toString()), normalized(root + "/Qt6Xml.dll"));
            QCOMPARE(normalized(result["elaModule"].toString()), normalized(component + "/XipsEla.dll"));
            QCOMPARE(normalized(result["implementationModule"].toString()), normalized(component + "/xips-browser-impl.dll"));
        }
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "--layout-child")
    {
        QApplication application(argc, argv);
        return childProbe(application.arguments());
    }
    QCoreApplication application(argc, argv);
    NativeLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_native_layout.moc"
