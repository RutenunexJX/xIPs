#include "ArchiveWindow.h"
#include "ArchivePage.h"
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QLabel>
#include <QIcon>
namespace xips::archive {
ArchiveWindow::ArchiveWindow(QWidget* owner, ArchiveState state)
    : QWidget(owner, Qt::Window), state_(std::move(state)), page_(new ArchivePage(this))
{
    setObjectName("xipsArchiveWindow");
    setWindowTitle("Archive project — xIPs");
    setWindowIcon(owner ? owner->windowIcon() : QIcon());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_WindowPropagation);
    setAutoFillBackground(true);
    resize(760, 740);
    setMinimumSize(420, 320);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(page_);
    page_->restoreState(state_.load());
    connect(page_, &ArchivePage::stateChanged, this, &ArchiveWindow::persist);
    connect(page_, &ArchivePage::busyChanged, this, &ArchiveWindow::busyChanged);
}
ArchiveWindow::~ArchiveWindow()
{
    persist();
    disconnect(page_, nullptr, this, nullptr);
    delete page_;
}
bool ArchiveWindow::isBusy() const { return page_->isBusy(); }
void ArchiveWindow::setAppearancePalette(const QPalette& palette)
{
    setPalette(palette);
    page_->setAppearancePalette(palette);
}
void ArchiveWindow::requestCancel() { page_->requestCancel(); }
void ArchiveWindow::persist()
{
    if (!state_.save(page_->saveState()))
        if (auto* label = page_->findChild<QLabel*>("archiveStatus")) {
            label->setText("Cannot save archive settings: " + state_.fileName());
            label->show();
        }
}
void ArchiveWindow::closeEvent(QCloseEvent* event)
{
    if (isBusy()) {
        event->ignore();
        if (auto* label = page_->findChild<QLabel*>("archiveStatus")) {
            label->setText(page_->closeBlockReason());
            label->show();
        }
        return;
    }
    persist();
    QWidget::closeEvent(event);
}
}
