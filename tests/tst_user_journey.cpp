#include "ElaComboBox.h"
#include "ElaContentDialog.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "app/BrowserPanel.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace xips;
class Host final : public QObject
{
    Q_OBJECT
  public:
    QVariantMap receipt;
    Q_INVOKABLE QString destinationError(const QString &)
    {
        return {};
    }
    Q_INVOKABLE QString exportCompleted(const QVariantMap &value)
    {
        receipt = value;
        return {};
    }
};
class UserJourneyTest : public QObject
{
    Q_OBJECT
  private slots:
    void collectChooseAndUsePinnedVersion();
};
void UserJourneyTest::collectChooseAndUsePinnedVersion()
{
    initializeEla();
    QTemporaryDir tmp;
    const QString library = tmp.filePath("library"), workspace = tmp.filePath("project"),
                  source = tmp.filePath("uart.sv");
    QDir().mkpath(library);
    QDir().mkpath(workspace);
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("first version");
    file.close();
    Host host;
    BrowserPanel panel(nullptr, &host);
    panel.resize(850, 600);
    panel.show();
    panel.setContext(library, workspace);
    auto *take = panel.findChild<ElaPushButton *>("takeButton");
    auto *collect = panel.findChild<ElaPushButton *>("collectButton");
    auto *versions = panel.findChild<ElaComboBox *>("versionCombo");
    QTRY_VERIFY(collect->isEnabled());
    QTimer::singleShot(0, &panel,
                       [&]
                       {
                           auto *form = panel.findChild<ElaContentDialog *>("xipsForm");
                           QVERIFY(form);
                           form->findChild<ElaPushButton *>("formAccept")->click();
                       });
    panel.collectPaths({source});
    QTRY_COMPARE(versions->count(), 1);
    QTRY_VERIFY(take->isEnabled());
    QTimer::singleShot(0, &panel,
                       [&]
                       {
                           auto *form = panel.findChild<ElaContentDialog *>("xipsForm");
                           QVERIFY(form);
                           form->findChild<ElaPushButton *>("formAccept")->click();
                       });
    take->click();
    QTRY_COMPARE(host.receipt.value("revision").toString(), QString("1"));
    QCOMPARE(host.receipt.value("workspace").toString(), workspace);
    QFile project(workspace + "/uart.sv");
    QVERIFY(project.open(QIODevice::ReadOnly));
    QCOMPARE(project.readAll(), QByteArray("first version"));
    project.close();
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("second version");
    file.close();
    auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.size(), 1);
    QVERIFY(SnapshotLibrary::update(catalog.assets.first(), {source}, "verified").ok);
    QTRY_VERIFY(collect->isEnabled());
    panel.refresh();
    QTRY_COMPARE(versions->count(), 2);
    QVERIFY(project.open(QIODevice::ReadOnly));
    QCOMPARE(project.readAll(), QByteArray("first version"));
    QCOMPARE(host.receipt.value("revision").toString(), QString("1"));
}
QTEST_MAIN(UserJourneyTest)
#include "tst_user_journey.moc"
