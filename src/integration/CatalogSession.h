#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
namespace SuiteApp
{
class JsonServer;
}
namespace xips
{
class BrowserPanel;
class CatalogSession final : public QObject
{
  public:
    explicit CatalogSession(BrowserPanel *panel);
    ~CatalogSession() override;

  private:
    QJsonObject handle(const QJsonObject &request);
    QJsonObject state();
    void syncRegistration();
    QString registeredLibrary, processStart;
    BrowserPanel *panel;
    SuiteApp::JsonServer *server = nullptr;
    QString id, recordPath, lease, expected, outcome = "none";
    QJsonObject focus;
    qint64 ownerPid = 0;
};
} // namespace xips
