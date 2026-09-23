#include "MainWindow.h"
#include "BrowserPanel.h"
#include "Branding.h"
#include <QVBoxLayout>
namespace xips
{
MainWindow::MainWindow(QString libraryRoot, QWidget *parent) : ElaWidget(parent)
{
    setWindowTitle(QStringLiteral("xIPs"));
    setWindowIcon(applicationIcon());
    setAppBarHeight(36);
    resize(900, 640);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 36, 0, 0);
    m_browser = new BrowserPanel(this);
    layout->addWidget(m_browser);
    m_browser->setContext(libraryRoot, {});
}
void MainWindow::applyActivation(const ActivationRequest &request)
{
    if (request.action == ActivationAction::OpenAsset)
        m_browser->revealAsset(request.value);
    else if (request.action == ActivationAction::Search)
        m_browser->restoreState({{"query", request.value}});
}
} // namespace xips
