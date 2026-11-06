#include "MainWindow.h"
#include "BrowserPanel.h"
#include "Branding.h"
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QSettings>
#include <QCryptographicHash>
#include <QDir>
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
    connect(m_browser, &BrowserPanel::libraryChanging, this, &MainWindow::saveBrowserState);
    connect(m_browser, &BrowserPanel::libraryChanged, this, &MainWindow::restoreBrowserState);
    restoreGeometry(QSettings("xIPs", "xIPs").value("ui/windowGeometry").toByteArray());
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
    if (event->isAccepted())
    {
        saveBrowserState();
        QSettings("xIPs", "xIPs").setValue("ui/windowGeometry", saveGeometry());
    }
}
namespace
{
QString browserStateKey(const QString &library)
{
    auto path = QDir::cleanPath(library);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return QStringLiteral("ui/libraries/") + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
}
}
void MainWindow::saveBrowserState()
{
    if (m_browser->libraryRoot().isEmpty()) return;
    QSettings("xIPs", "xIPs").setValue(browserStateKey(m_browser->libraryRoot()), m_browser->saveState());
}
void MainWindow::restoreBrowserState()
{
    const auto library = m_browser->libraryRoot();
    if (library.isEmpty()) return;
    auto state = QSettings("xIPs", "xIPs").value(browserStateKey(library)).toMap();
    state.insert("library", library);
    m_browser->restoreState(state);
}
void MainWindow::applyActivation(const ActivationRequest &request)
{
    auto state = m_browser->saveState();
    if (request.action == ActivationAction::OpenAsset)
    {
        state.insert("assetId", request.value); state.insert("groupId", QString());
        state.insert("query", QString()); state.insert("category", QString()); state.insert("index", QString());
        state.insert("page", 0); state.insert("revision", QStringLiteral("current"));
    }
    else if (request.action == ActivationAction::Search) state.insert("query", request.value);
    else return;
    m_browser->restoreState(state);
}
} // namespace xips
