#pragma once
#include "ArchiveEngine.h"
#include "ArchiveScan.h"
#include <QPointer>
#include <QVariantMap>
#include <QWidget>
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace xips::archive {
class ArchiveBatchPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ArchiveBatchPanel(QWidget* parent = nullptr);
    ~ArchiveBatchPanel() override;
    bool isScanning() const { return !scan_.isNull(); }
    void scanFolder(const QString& folder);
    void cancelScan();
    void setLocked(bool locked);
    QList<int> selectedRows() const;
    QList<Request> requests(const QString& output, const QList<Installation>& installations, QString& error) const;
    void setRowStatus(int row, const QString& status, const QString& detail = {});
    QVariantMap saveState() const;
    void restoreState(const QVariantMap& state);
signals:
    void stateChanged();
    void scanningChanged(bool scanning);
private:
    void setProjects(const QList<DiscoveredProject>& projects, const QVariantMap& previous);
    QLineEdit* folder_;
    QPushButton* choose_;
    QPushButton* scanButton_;
    QLabel* summary_;
    QTableWidget* table_;
    QPointer<ArchiveScan> scan_;
    bool locked_ = false;
};
}
