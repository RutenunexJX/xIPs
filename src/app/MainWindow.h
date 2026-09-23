#pragma once
#include "ElaWidget.h"
#include "integration/IntegrationService.h"
namespace xips
{
class BrowserPanel;
class MainWindow final : public ElaWidget
{
    Q_OBJECT
  public:
    explicit MainWindow(QString libraryRoot, QWidget *parent = nullptr);
    void applyActivation(const ActivationRequest &request);

  private:
    BrowserPanel *m_browser;
};
} // namespace xips