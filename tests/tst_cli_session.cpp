#include "ElaPushButton.h"
#include "app/BrowserPanel.h"
#include "app/MainWindow.h"
#include "library/CatalogOperations.h"
#include "library/SnapshotLibrary.h"
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace xips;
namespace
{
void put(const QString &path, const QJsonObject &json)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(json).toJson()) < 0)
        qFatal("Cannot write fixture.");
}
struct Result
{
    int exit;
    QJsonObject json;
    QByteArray diagnostics;
};
Result cli(const QStringList &arguments)
{
    QProcess process;
    QEventLoop loop;
    QTimer deadline;
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        process.kill();
        loop.quit();
    });
    deadline.setSingleShot(true);
    deadline.start(30000);
    process.start(QString(XIPS_CLI_PATH), arguments);
    if (!process.waitForStarted(3000))
        return {-1, {}, process.errorString().toUtf8()};
    loop.exec();
    process.waitForFinished(1000);
    return {process.exitCode(), QJsonDocument::fromJson(process.readAllStandardOutput()).object(),
            process.readAllStandardError()};
}
} // namespace
class CliSessionTest : public QObject
{
    Q_OBJECT
  private slots:
    void activeDialogAndAdoption()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto root = fixture.path() + "/library";
        QVERIFY(QDir().mkpath(root));
        const auto registry = fixture.path() + "/sessions";
        qApp->setProperty("xips.sessionDirectory", registry);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path() + "/profile");
        MainWindow window(root);
        window.show();
        auto *panel = window.findChild<BrowserPanel *>();
        QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(!panel->isCatalogBusy(), 10000);
        const auto sessions = cli({"--action", "sessions", "--session-dir", registry});
        QCOMPARE(sessions.exit, 0);
        const auto values = sessions.json.value("data").toObject().value("sessions").toArray();
        QCOMPARE(values.size(), 1);
        const auto id = values.first().toObject().value("id").toString();
        QVERIFY(!id.isEmpty());
        const auto requestFile = fixture.path() + "/request.json";
        put(requestFile, {{"action", "group-create"}, {"library", root}, {"name", "CLI group"}});
        const auto preview = cli({"--request", requestFile, "--dry-run"});
        QCOMPARE(preview.exit, 0);
        const auto planFile = fixture.path() + "/preview.json";
        put(planFile, preview.json);
        const QStringList apply{"--apply", planFile, "--session", id, "--session-dir", registry};
        bool checked = false;
        QTimer::singleShot(200, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>("newAssetName");
            QVERIFY(name);
            name->setText("unsaved_value");
            const auto blocked = cli(apply);
            QCOMPARE(blocked.exit, 3);
            QCOMPARE(blocked.json.value("error").toObject().value("code").toString(),
                     QString("session_conflict"));
            QCOMPARE(name->text(), QString("unsaved_value"));
            QVERIFY(dialog->isVisible());
            QVERIFY(CatalogGroups::scan(root).groups.isEmpty());
            checked = true;
            dialog->reject();
        });
        auto *create = panel->findChild<ElaPushButton *>("newAssetButton");
        QVERIFY(create);
        create->click();
        QVERIFY(checked);
        const auto applied = cli(apply);
        QVERIFY2(applied.exit == 0, QJsonDocument(applied.json).toJson().constData());
        QCOMPARE(applied.json.value("data")
                     .toObject()
                     .value("gui")
                     .toObject()
                     .value("adoption")
                     .toObject()
                     .value("state")
                     .toString(),
                 QString("adopted"));
        QCOMPARE(CatalogGroups::scan(root).groups.size(), 1);
        QCOMPARE(panel->externalState().value("viewToken").toString(),
                 applied.json.value("data").toObject().value("viewTokens").toObject().value(root).toString());
        const auto old = cli(apply);
        QCOMPARE(old.exit, 3);
        QVERIFY(panel->isEnabled());
        const auto screenshot = qEnvironmentVariable("XIPS_SESSION_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));
        window.close();
    }
};
QTEST_MAIN(CliSessionTest)
#include "tst_cli_session.moc"
