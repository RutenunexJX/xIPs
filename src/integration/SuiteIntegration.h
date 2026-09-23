#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <memory>
#include <suiteapp/provider.h>
#include <suiteapp/runtime.h>
class QWidget;
namespace xips
{
class BrowserPanel;
class SuiteIntegration final : public QObject
{
  public:
    SuiteIntegration(BrowserPanel *panel, QWidget *window);
    bool start(QString *error = nullptr, const SuiteApp::RuntimeStartOptions &options = {});
    static QJsonObject descriptor(const QString &version);
    QJsonObject processRequest(const QJsonObject &request);

  private:
    QPointer<BrowserPanel> panel;
    QPointer<QWidget> window;
    std::unique_ptr<SuiteApp::Provider> provider;
};
} // namespace xips
