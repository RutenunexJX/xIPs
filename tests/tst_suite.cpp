#include "app/Branding.h"
#include "app/BrowserPanel.h"
#include "integration/IntegrationService.h"
#include "integration/SuiteIntegration.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <QtTest>
#include <suiteapp/client.h>

using namespace xips;
class SuiteTest final : public QObject {
  Q_OBJECT
private slots:
  void registersAndRoutesWithoutExposingPayload() {
    initializeEla();
    QTemporaryDir tmp;
    QVERIFY(QDir().mkpath(tmp.filePath("library")));
    QFile source(tmp.filePath("module.sv"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("module first;endmodule");
    source.close();
    auto asset = SnapshotLibrary::collect(
        tmp.filePath("library"), {source.fileName()}, "Module", "module");
    QVERIFY2(asset.ok, qPrintable(asset.error));
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
    QVERIFY(runtime.waitForStarted());
    const auto cleanup = qScopeGuard([&] {
      runtime.terminate();
      if (!runtime.waitForFinished(1000)) {
        runtime.kill();
        runtime.waitForFinished(1000);
      }
    });
    QTRY_VERIFY(SuiteApp::ensureRuntime(options).available);
    SuiteIntegration integration(&panel, &panel);
    QString error;
    QVERIFY2(integration.start(&error, options), qPrintable(error));
    SuiteApp::Client client(endpoint);
    const auto call = [](auto work) {
      QFutureWatcher<SuiteApp::TransportResult> watcher;
      QEventLoop loop;
      QObject::connect(&watcher,
                       &QFutureWatcher<SuiteApp::TransportResult>::finished,
                       &loop, &QEventLoop::quit);
      watcher.setFuture(QtConcurrent::run(work));
      if (!watcher.isFinished())
        loop.exec();
      return watcher.result().response;
    };
    const auto listed = client.listProviders().response;
    QVERIFY(listed.value("ok").toBool());
    const auto providers =
        listed.value("result").toObject().value("providers").toArray();
    QCOMPARE(providers.size(), 1);
    const auto descriptor = providers.first().toObject();
    QCOMPARE(descriptor.value("appId").toString(), QString("xips"));
    QVERIFY(SuiteApp::validateAppDescriptor(descriptor, &error));
    const QString uri =
        IntegrationService::assetUri(asset.asset.id).toString() + "?revision=1";
    const auto resolved = call([&] { return client.resolveResource(uri); });
    QVERIFY2(resolved.value("ok").toBool(),
             qPrintable(QJsonDocument(resolved).toJson()));
    const auto metadata = resolved.value("result").toObject();
    QCOMPARE(metadata.value("revision").toString(), QString("1"));
    QVERIFY(!metadata.contains("path"));
    QVERIFY(!metadata.contains("files"));
    const auto surface =
        call([&] { return client.describeSurface("xips.library", uri); });
    QVERIFY(surface.value("ok").toBool());
    QCOMPARE(surface.value("result").toObject().value("mode").toString(),
             QString("native"));
    QCOMPARE(surface.value("result").toObject().value("fallback").toString(),
             QString("external"));
    const auto opened =
        call([&] { return client.invokeAction("xips.asset.open", {}, uri); });
    QVERIFY(opened.value("ok").toBool());
    QTRY_COMPARE(panel.saveState().value("revision").toString(), QString("1"));
    const auto missing =
        call([&] { return client.resolveResource("xips://asset/missing"); });
    QVERIFY(!missing.value("ok").toBool());
    const auto rejected =
        call([&] { return client.invokeAction("xips.asset.delete", {}, uri); });
    QVERIFY(!rejected.value("ok").toBool());
    QCOMPARE(
        SnapshotLibrary::scan(tmp.filePath("library")).assets.first().document,
        baseline);
  }
  void remainsUsableWithoutRuntime() {
    BrowserPanel panel;
    SuiteIntegration integration(&panel, &panel);
    SuiteApp::RuntimeStartOptions options;
    options.endpoint = "suiteapp.xips.unavailable." +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
    options.startIfMissing = false;
    options.probeTimeoutMs = 20;
    QString error;
    QVERIFY(!integration.start(&error, options));
    QVERIFY(!error.isEmpty());
    QVERIFY(panel.isEnabled());
  }
};
QTEST_MAIN(SuiteTest)
#include "tst_suite.moc"
