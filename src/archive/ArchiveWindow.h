#pragma once
#include "ArchiveState.h"
#include <QWidget>
namespace xips::archive {
class ArchivePage;
class ArchiveWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ArchiveWindow(QWidget* owner, ArchiveState state = ArchiveState());
    ~ArchiveWindow() override;
    bool isBusy() const;
    ArchivePage* page() const { return page_; }
    void requestCancel();
    void setAppearancePalette(const QPalette& palette);
signals:
    void busyChanged(bool busy);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void persist();
    ArchiveState state_;
    ArchivePage* page_;
};
}
