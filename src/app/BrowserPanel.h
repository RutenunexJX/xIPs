#pragma once

#include "library/SnapshotLibrary.h"
#include <QPointer>
#include <QWidget>
#include <functional>
#include <optional>

class ElaComboBox;
class ElaLineEdit;
class ElaPushButton;
class ElaText;
class ElaListView;
class ElaProgressRing;
class QStringListModel;
class QSplitter;
class QTimer;
template <class T> class QFutureWatcher;

namespace xips
{
class CatalogModel;
class BrowserPanel final : public QWidget
{
    Q_OBJECT
  public:
    explicit BrowserPanel(QWidget *parent = nullptr, QObject *host = nullptr);
    Q_INVOKABLE void setContext(const QString &library, const QString &workspace);
    Q_INVOKABLE void collectPaths(const QStringList &paths);
    Q_INVOKABLE void revealAsset(const QString &id);
    Q_INVOKABLE QVariantMap saveState() const;
    Q_INVOKABLE void restoreState(const QVariantMap &state);
    Q_INVOKABLE void refresh();
    bool isCatalogBusy() const
    {
        return m_busy;
    }
    std::optional<CatalogAsset> catalogAsset(const QString &id) const;

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void filter();
    void selectCurrent();
    void readLegacyDetails(const CatalogAsset &asset);
    void showDetails(const CatalogAsset &asset);
    void selectVersion();
    void chooseLibrary();
    void addSources();
    void updateAsset();
    void exportAsset();
    void more();
    void detailsDialog();
    void run(const QString &message, std::function<SnapshotResult()> work,
             std::function<void(const SnapshotResult &)> finished = {});
    void setBusy(bool value, const QString &message = {});
    void notice(const QString &message, bool error = false);
    void applyTheme();
    void updateActivity();
    void restoreSplit();
    double splitRatio() const;
    bool applyPendingContext();
    QStringList pickSources();
    QString m_library;
    QString m_workspace;
    QString m_category;
    QString m_pendingId;
    QString m_pendingRevision;
    QStringList m_problems;
    QList<CatalogAsset> m_assets;
    CatalogAsset m_selected;
    Snapshot m_snapshot;
    QPointer<QObject> m_host;
    int m_generation = 0;
    bool m_busy = false;
    bool m_loadingDetails = false;
    bool m_noticeError = false;
    double m_horizontalRatio = 0.5;
    double m_verticalRatio = 0.45;
    std::optional<QPair<QString, QString>> m_pendingContext;
    std::optional<CatalogAsset> m_pendingDetail;
    QFutureWatcher<SnapshotResult> *m_detailWatcher = nullptr;
    ElaProgressRing *m_activity = nullptr;
    ElaText *m_title = nullptr;
    ElaText *m_status = nullptr;
    ElaText *m_name = nullptr;
    ElaText *m_summary = nullptr;
    ElaText *m_description = nullptr;
    ElaLineEdit *m_search = nullptr;
    ElaComboBox *m_versions = nullptr;
    ElaListView *m_list = nullptr;
    ElaListView *m_files = nullptr;
    CatalogModel *m_model = nullptr;
    QStringListModel *m_fileModel = nullptr;
    ElaPushButton *m_add = nullptr;
    ElaPushButton *m_take = nullptr;
    ElaPushButton *m_update = nullptr;
    QWidget *m_details = nullptr;
    QWidget *m_filters = nullptr;
    QSplitter *m_splitter = nullptr;
    QTimer *m_searchTimer = nullptr;
};

void initializeEla();
} // namespace xips
