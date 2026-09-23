#include "app/BrowserPanel.h"
#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

class BrowserPerformance final : public QObject
{
    Q_OBJECT
    QWidget *observed = nullptr;
    int paints = 0;
    int layouts = 0;
    bool eventFilter(QObject *target, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(target);
        if (observed && widget && (widget == observed || observed->isAncestorOf(widget)))
        {
            paints += event->type() == QEvent::Paint;
            layouts += event->type() == QEvent::LayoutRequest;
        }
        return false;
    }
  private slots:
    void largeCatalogFiltering()
    {
        xips::initializeEla();
        QTemporaryDir fixture;
        constexpr int count = 2000;
        QJsonArray files;
        for (int i = 0; i < 64; ++i)
            files.append(QStringLiteral("rtl/channel_%1.sv").arg(i));
        const QJsonObject revision{{"id", "1"}, {"note", "Synthetic catalog fixture"},
            {"created", "2026-09-24T00:00:00.000Z"}, {"hash", "sha256:fixture"}, {"files", files}};
        for (int i = 0; i < count; ++i)
        {
            const QString name = QStringLiteral("block_%1").arg(i, 4, 10, QChar('0'));
            QVERIFY(QDir().mkpath(fixture.filePath(name)));
            QFile manifest(fixture.filePath(name + "/.xips.json"));
            QVERIFY(manifest.open(QIODevice::WriteOnly));
            const QJsonObject object{{"schemaVersion", 2}, {"id", name}, {"name", name},
                {"category", "module"}, {"description", "Reusable module with channel interface"},
                {"nextRevision", 2}, {"revisions", QJsonArray{revision}}};
            manifest.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        }
        xips::BrowserPanel panel;
        panel.resize(1000, 700);
        panel.show();
        panel.setContext(fixture.path(), {});
        auto *list = panel.findChild<QAbstractItemView *>("assetList");
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(!panel.isCatalogBusy(), 30000);
        QCOMPARE(list->model()->rowCount(), count);
        observed = &panel;
        qApp->installEventFilter(this);
        QList<double> samples;
        QJsonArray runs;
        const QStringList queries{"channel_63", "block_1", "absent", "interface", ""};
        for (int round = 0; round < 4; ++round)
        {
            for (const auto &query : queries)
            {
                paints = layouts = 0;
                QElapsedTimer timer;
                timer.start();
                panel.restoreState({{"query", query}});
                const double elapsed = timer.nsecsElapsed() / 1000000.0;
                samples.append(elapsed);
                QTest::qWait(200);
                const int expected = query == "absent" ? 0 : query == "block_1" ? 1000 : count;
                QCOMPARE(list->model()->rowCount(), expected);
                runs.append(QJsonObject{{"query", query}, {"dispatchMs", elapsed},
                    {"paintEvents", paints}, {"layoutRequests", layouts}});
            }
        }
        qApp->removeEventFilter(this);
        observed = nullptr;
        std::sort(samples.begin(), samples.end());
        const QJsonObject report{{"assets", count}, {"filesPerRevision", 64},
            {"width", panel.width()}, {"height", panel.height()},
            {"devicePixelRatio", panel.devicePixelRatioF()}, {"platform", QGuiApplication::platformName()},
            {"medianDispatchMs", samples.at(samples.size() / 2)},
            {"p95DispatchMs", samples.at(samples.size() * 95 / 100)}, {"runs", runs}};
        const auto output = qEnvironmentVariable("XIPS_PERF_OUTPUT");
        if (!output.isEmpty())
        {
            QFile file(output);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(report).toJson());
        }
        qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
    }
};
QTEST_MAIN(BrowserPerformance)
#include "tst_browser_performance.moc"
