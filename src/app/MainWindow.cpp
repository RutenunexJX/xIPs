#include "MainWindow.h"
#include "BrowserPanel.h"
#include "Branding.h"
#include <QVBoxLayout>
#include <QCloseEvent>
#include "archive/ArchiveWindow.h"
namespace xips
{
MainWindow::MainWindow(QString libraryRoot, QWidget *parent) : ElaWidget(parent)
{
    setWindowTitle(QStringLiteral("xIPs"));
    setWindowIcon(applicationIcon());
    setAppBarHeight(32);
    setIsStayTop(false);
    setWindowButtonFlags(ElaAppBarType::MinimizeButtonHint | ElaAppBarType::MaximizeButtonHint |
                        ElaAppBarType::CloseButtonHint);
    resize(720, 420);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_browser = new BrowserPanel(this);
    layout->addWidget(m_browser);
    m_browser->setContext(libraryRoot, {});
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    auto *tool = m_browser->findChild<archive::ArchiveWindow *>();
    if (tool && tool->isBusy()) {
        m_browser->openArchiveProject();
        tool->close();
        event->ignore();
        return;
    }
    ElaWidget::closeEvent(event);
}
void MainWindow::applyActivation(const ActivationRequest &request)
{
    if (request.action == ActivationAction::OpenAsset)
        m_browser->revealAsset(request.value);
    else if (request.action == ActivationAction::Search)
        m_browser->restoreState({{"query", request.value}});
}
} // namespace xips
