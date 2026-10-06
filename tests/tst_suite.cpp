#include "app/Branding.h"
#include "app/BrowserPanel.h"
#include "integration/IntegrationService.h"
#include "integration/SuiteIntegration.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <QtTest>
#include <suiteapp/client.h>

using namespace xips;
namespace {
template <typename Work> QJsonObject call(Work work) {
  QFutureWatcher<SuiteApp::TransportResult> watcher;
  QEventLoop loop;
  QObject::connect(&watcher,
                   &QFutureWatcher<SuiteApp::TransportResult>::finished,
                   &loop, &QEventLoop::quit);
  watcher.setFuture(QtConcurrent::run(work));
  if (!watcher.isFinished())
    loop.exec();
  return watcher.result().response;
}
void stopOwnedRuntime(QProcess &runtime) {
  if (runtime.state() == QProcess::NotRunning)
    return;
  runtime.terminate();
  if (!runtime.waitForFinished(1000)) {
    runtime.kill();
    runtime.waitForFinished(1000);
  }
}
} // namespace

class SuiteTest final : public QObject {
  Q_OBJECT
  QTemporaryDir settings;
private slots:
  void initTestCase() {
    QVERIFY(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    qputenv("XIPS_TEST_CACHE_ROOT", settings.filePath("cache").toUtf8());
    QCoreApplication::setApplicationVersion(QString::fromUtf8(XIPS_TEST_VERSION));
    initializeEla();
  }
  void applicationReportsVersion() {
    QProcess application;
    application.start(QString::fromUtf8(XIPS_GUI_PATH), {"--version"});
    QVERIFY(application.waitForStarted());
    QVERIFY(application.waitForFinished(10000));
    QCOMPARE(application.exitStatus(), QProcess::NormalExit);
    QCOMPARE(application.exitCode(), 0);
    QCOMPARE(application.readAllStandardOutput().trimmed(),
             QByteArray("xIPs ") + XIPS_TEST_VERSION);
  }
  void registersAndRoutesWithoutExposingPayload() {
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath("library")));
    QFile source(tmp.filePath("module.sv"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("module first;endmodule");
    source.close();
    auto asset = SnapshotLibrary::collect(
        tmp.filePath("library"), {source.fileName()}, "Module", "module");
    QVERIFY2(asset.ok, qPrintable(asset.error));
    const QString firstRevision = asset.snapshot.id;
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("module second;endmodule");
    source.close();
    asset = SnapshotLibrary::update(asset.asset, {source.fileName()});
    QVERIFY(asset.ok);
    const auto baseline = asset.asset.document;
    BrowserPanel panel;
    panel.setContext(tmp.filePath("library"), {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    QTRY_VERIFY(panel.catalogAsset(asset.asset.id).has_value());
    const auto icon = applicationIcon();
    QVERIFY(!icon.isNull());
    QVERIFY(!icon.pixmap(16, 16).isNull());
    QVERIFY(icon.availableSizes().contains(QSize(256, 256)));
    const QString endpoint = "suiteapp.xips.test." +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    SuiteApp::RuntimeStartOptions options;
    options.endpoint = endpoint;
    options.startIfMissing = false;
    QProcess runtime;
    runtime.start(QString::fromUtf8(XIPS_RUNTIME_PATH),
                  {"--endpoint", endpoint});
    const auto cleanup = qScopeGuard([&] { stopOwnedRuntime(runtime); });
    QVERIFY(runtime.waitForStarted());
    QTRY_VERIFY(SuiteApp::ensureRuntime(options).available);
    auto integration = std::make_unique<SuiteIntegration>(&panel, &panel);
    QString error;
    QVERIFY2(integration->start(&error, options), qPrintable(error));
    QVERIFY2(integration->start(&error, options), qPrintable(error));
    SuiteApp::Client client(endpoint);
    const auto listed = client.listProviders().response;
    QVERIFY(listed.value("ok").toBool());
    const auto providers =
        listed.value("result").toObject().value("providers").toArray();
    QCOMPARE(providers.size(), 1);
    const auto descriptor = providers.first().toObject();
    QCOMPARE(descriptor.value("appId").toString(), QString("xips"));
    QCOMPARE(descriptor.value("displayName").toString(), QString("xIPs"));
    QCOMPARE(descriptor.value("version").toString(), QString::fromUtf8(XIPS_TEST_VERSION));
    QCOMPARE(descriptor.value("processId").toInteger(), QCoreApplication::applicationPid());
    QVERIFY(SuiteApp::validateAppDescriptor(descriptor, &error));
    const QString uri =
        IntegrationService::assetUri(asset.asset.id).toString() + "?revision=1";
    const auto resolved = call([&] { return client.resolveResource(uri); });
    QVERIFY2(resolved.value("ok").toBool(),
             qPrintable(QJsonDocument(resolved).toJson()));
    const auto metadata = resolved.value("result").toObject();
    QCOMPARE(metadata.value("revision").toString(), firstRevision);
    QCOMPARE(metadata.value("contentVerified"), QJsonValue(false));
    QVERIFY(!metadata.contains("path"));
    QVERIFY(!metadata.contains("files"));
    const auto surface =
        call([&] { return client.describeSurface("xips.library", uri); });
    QVERIFY(surface.value("ok").toBool());
    const auto surfaceResult = surface.value("result").toObject();
    QCOMPARE(surfaceResult.value("mode").toString(), QString("native"));
    QCOMPARE(surfaceResult.value("fallback").toString(), QString("external"));
    const auto native = surfaceResult.value("native").toObject();
    QCOMPARE(native.value("abi").toInt(), 1);
    QCOMPARE(native.value("factory").toString(), QString("xips_create_browser_v1"));
    QVERIFY(QFileInfo::exists(native.value("path").toString()));
    const auto opened =
        call([&] { return client.invokeAction("xips.asset.open", {}, uri); });
    QVERIFY(opened.value("ok").toBool());
    QTRY_COMPARE(panel.saveState().value("revision").toString(), firstRevision);
    const auto surfaceOpened =
        call([&] { return client.openSurface("xips.library", uri); });
    QVERIFY(surfaceOpened.value("ok").toBool());
    QVERIFY(surfaceOpened.value("result").toObject().value("opened").toBool());
    const QString assetUri = IntegrationService::assetUri(asset.asset.id).toString();
    const auto exact = call([&] {
      return client.resolveResource(assetUri + "?revision=" + firstRevision);
    });
    QVERIFY(exact.value("ok").toBool());
    QCOMPARE(exact.value("result").toObject().value("revision").toString(), firstRevision);
    const auto latest = call([&] { return client.invokeAction("xips.asset.open", {}, assetUri); });
    QVERIFY(latest.value("ok").toBool());
    QCOMPARE(latest.value("result").toObject().value("resource").toObject().value("revision").toString(),
             asset.snapshot.id);
    QTRY_COMPARE(panel.saveState().value("revision").toString(), asset.snapshot.id);
    const auto missing =
        call([&] { return client.resolveResource("xips://asset/missing"); });
    QVERIFY(!missing.value("ok").toBool());
    QCOMPARE(missing.value("error").toObject().value("code").toString(), QString("resource_not_found"));
    const auto invalidRevision = call([&] {
      return client.resolveResource(IntegrationService::assetUri(asset.asset.id).toString() + "?revision=999");
    });
    QVERIFY(!invalidRevision.value("ok").toBool());
    QCOMPARE(invalidRevision.value("error").toObject().value("code").toString(), QString("revision_not_found"));
    const auto invalidUri = call([&] { return client.resolveResource(uri + "#invalid"); });
    QVERIFY(!invalidUri.value("ok").toBool());
    QCOMPARE(invalidUri.value("error").toObject().value("code").toString(), QString("invalid_resource"));
    for (const auto *action : {"xips.asset.delete", "xips.asset.collect", "xips.asset.use", "xips.asset.update"}) {
      const auto rejected = call([&] { return client.invokeAction(action, {}, uri); });
      QVERIFY(!rejected.value("ok").toBool());
    }
    QCOMPARE(
        SnapshotLibrary::scan(tmp.filePath("library")).assets.first().document,
        baseline);
    QFile revisionFile(asset.asset.root + "/.xips/revisions/" + firstRevision + ".json");
    QVERIFY(revisionFile.open(QIODevice::ReadOnly));
    auto duplicate = QJsonDocument::fromJson(revisionFile.readAll()).object();
    revisionFile.close();
    const QString duplicateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    duplicate.insert("id", duplicateId);
    QFile duplicateFile(asset.asset.root + "/.xips/revisions/" + duplicateId + ".json");
    QVERIFY(duplicateFile.open(QIODevice::WriteOnly));
    QVERIFY(duplicateFile.write(QJsonDocument(duplicate).toJson()) > 0);
    duplicateFile.close();
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    const auto ambiguous = call([&] { return client.resolveResource(uri); });
    QVERIFY(!ambiguous.value("ok").toBool());
    QCOMPARE(ambiguous.value("error").toObject().value("code").toString(), QString("revision_ambiguous"));
    const auto exactAfterBranch = call([&] {
      return client.invokeAction("xips.asset.open", {}, assetUri + "?revision=" + firstRevision);
    });
    QVERIFY(exactAfterBranch.value("ok").toBool());
    QTRY_COMPARE(panel.saveState().value("revision").toString(), firstRevision);
    integration.reset();
    const auto afterExit = client.listProviders().response;
    QVERIFY(afterExit.value("ok").toBool());
    QVERIFY(afterExit.value("result").toObject().value("providers").toArray().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(runtime.state(), QProcess::NotRunning, 5000);
    QCOMPARE(runtime.exitStatus(), QProcess::NormalExit);
    QCOMPARE(runtime.exitCode(), 0);
  }
  void remainsUsableWithoutRuntimeAndCanRetry() {
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath("library")));
    BrowserPanel panel;
    panel.setContext(tmp.filePath("library"), {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto integration = std::make_unique<SuiteIntegration>(&panel, &panel);
    SuiteApp::RuntimeStartOptions options;
    options.endpoint = "suiteapp.xips.unavailable." +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
    options.startIfMissing = false;
    options.probeTimeoutMs = 20;
    QString error;
    QVERIFY(!integration->start(&error, options));
    QVERIFY(!error.isEmpty());
    QVERIFY(panel.isEnabled());
    panel.refresh();
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(panel.saveState().value("library").toString(), tmp.filePath("library"));
    QProcess runtime;
    runtime.start(QString::fromUtf8(XIPS_RUNTIME_PATH), {"--endpoint", options.endpoint});
    const auto cleanup = qScopeGuard([&] { stopOwnedRuntime(runtime); });
    QVERIFY(runtime.waitForStarted());
    QTRY_VERIFY(SuiteApp::ensureRuntime(options).available);
    QVERIFY2(integration->start(&error, options), qPrintable(error));
    SuiteApp::Client client(options.endpoint);
    const auto resolved = call([&] { return client.resolveResource("xips://show"); });
    QVERIFY(resolved.value("ok").toBool());
    QCOMPARE(resolved.value("result").toObject().value("kind").toString(), QString("library"));
    integration.reset();
    QTRY_COMPARE_WITH_TIMEOUT(runtime.state(), QProcess::NotRunning, 5000);
    QCOMPARE(runtime.exitCode(), 0);
  }
};
QTEST_MAIN(SuiteTest)
#include "tst_suite.moc"
