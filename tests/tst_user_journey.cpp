#include "RevisionDriver.h"
#include "ElaComboBox.h"
#include "DialogDriver.h"
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
    auto *create = panel.findChild<ElaPushButton *>("newAssetButton");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    QTRY_VERIFY(create->isEnabled());
    whenVisible(&panel, "xipsForm",
                       [&](QWidget *form)
                       {
                           QVERIFY(form);
                           form->findChild<ElaPushButton *>("formAccept")->click();
                       });
    whenVisible(&panel, "payloadReviewForm", [](QWidget *form)
        { form->findChild<ElaPushButton *>("formAccept")->click(); });
    panel.collectPaths({source});
    QTRY_COMPARE(versions->model()->rowCount(), 1);
    QTRY_VERIFY(take->isEnabled());
    const auto firstRevision = versions->currentIndex().data(Qt::UserRole).toString();
    whenVisible(&panel, "xipsForm",
                       [&](QWidget *form)
                       {
                           QVERIFY(form);
                           form->findChild<ElaPushButton *>("formAccept")->click();
                       });
    take->click();
    QTRY_COMPARE(host.receipt.value("revision").toString(), firstRevision);
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
    QTRY_VERIFY(create->isEnabled());
    panel.refresh();
    QTRY_COMPARE(versions->model()->rowCount(), 2);
    QVERIFY(project.open(QIODevice::ReadOnly));
    QCOMPARE(project.readAll(), QByteArray("first version"));
    QCOMPARE(host.receipt.value("revision").toString(), firstRevision);
}
QTEST_MAIN(UserJourneyTest)
#include "tst_user_journey.moc"
