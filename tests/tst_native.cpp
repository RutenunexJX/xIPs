#include "ElaApplication.h"
#include "library/SnapshotLibrary.h"
#include "xips/BrowserApi.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

class NativeTest final : public QObject
{
    Q_OBJECT
  private slots:
    void loadsPublicSurfaceAndRestoresPinnedRevision()
    {
        eApp->init();
        QTemporaryDir temporary;
        const auto root = temporary.filePath("library");
        QVERIFY(QDir().mkpath(root));
        QFile source(temporary.filePath("counter.sv"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("module counter; endmodule");
        source.close();
        auto asset = xips::SnapshotLibrary::collect(root, {source.fileName()}, "Counter", "module");
        QVERIFY2(asset.ok, qPrintable(asset.error));
        QLibrary library(QString::fromUtf8(XIPS_BROWSER_PATH));
        library.setLoadHints(QLibrary::PreventUnloadHint);
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        const auto abi = reinterpret_cast<XipsBrowserAbiV1>(library.resolve("xips_browser_abi_v1"));
        const auto create =
            reinterpret_cast<XipsCreateBrowserV1>(library.resolve("xips_create_browser_v1"));
        QVERIFY(abi);
        QVERIFY(create);
        QCOMPARE(QByteArray(abi()), xipsExpectedBrowserAbi());
        const auto capabilities = reinterpret_cast<XipsBrowserCapabilitiesV1>(library.resolve("xips_browser_capabilities_v1"));
        QVERIFY(capabilities);
        const auto description = QJsonDocument::fromJson(capabilities()).object();
        QCOMPARE(description["elaBaseline"].toString(), QString("454cac2d-p27"));
        QCOMPARE(description["elaSourceSha256"].toString().size(), 64);
        std::unique_ptr<QWidget> panel(create(nullptr, nullptr));
        QVERIFY(panel);
        QVERIFY(QMetaObject::invokeMethod(panel.get(), "setContext", Q_ARG(QString, root),
                                          Q_ARG(QString, QString())));
        const QVariantMap state{
            {"assetId", asset.asset.id}, {"revision", "1"}, {"query", "counter"}};
        QVERIFY(QMetaObject::invokeMethod(panel.get(), "restoreState", Q_ARG(QVariantMap, state)));
        auto *versions = panel->findChild<QComboBox *>("versionCombo");
        QVERIFY(versions);
        QTRY_COMPARE(versions->count(), 1);
        QVariantMap restored;
        QVERIFY(QMetaObject::invokeMethod(panel.get(), "saveState",
                                          Q_RETURN_ARG(QVariantMap, restored)));
        QCOMPARE(restored.value("assetId"), state.value("assetId"));
        QCOMPARE(restored.value("revision"), state.value("revision"));
        QCOMPARE(restored.value("query"), state.value("query"));
    }
};
QTEST_MAIN(NativeTest)
#include "tst_native.moc"
