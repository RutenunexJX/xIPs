#pragma once

#include "library/SnapshotLibrary.h"
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QWidget>
#include <functional>
#include <optional>
#include <memory>

class ElaComboBox;
class ElaLineEdit;
class ElaPushButton;
class ElaText;
class ElaListView;
class ElaTreeView;
class ElaTableView;
class ElaProgressRing;
class ElaToolButton;
class ElaFlowLayout;
class ElaTabWidget;
class ElaCheckBox;
class ElaScrollArea;
class QBoxLayout;
class QStringListModel;
class QStandardItemModel;
class QSplitter;
class QTimer;
template <class T> class QFutureWatcher;

namespace xips
{
class CatalogModel;
class OperationControl;
class WorkingFilesModel;
class BrowserPanel final : public QWidget
{
    Q_OBJECT
  public:
    explicit BrowserPanel(QWidget *parent = nullptr, QObject *host = nullptr, bool embedded = false);
    ~BrowserPanel() override;
    Q_INVOKABLE void setContext(const QString &library, const QString &workspace);
    Q_INVOKABLE void collectPaths(const QStringList &paths);
    Q_INVOKABLE void importWorkingFiles(const QStringList &paths);
    Q_INVOKABLE void revealAsset(const QString &id);
    Q_INVOKABLE QVariantMap saveState() const;
    Q_INVOKABLE void restoreState(const QVariantMap &state);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setDarkTheme(bool dark);
    Q_INVOKABLE bool isCatalogBusy() const
    {
        return m_busy;
    }
    std::optional<CatalogAsset> catalogAsset(const QString &id) const;

  signals:
    void contextChanging();

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void filter();
    void selectCurrent();
    void readLegacyDetails(const CatalogAsset &asset);
    void showDetails(const CatalogAsset &asset);
    void selectVersion();
    void rememberChecks();
    QString workingKey() const;
    void chooseLibrary();
    void addSources();
    void createAsset();
    void createGroup();
    void renameGroup(const QString &id);
    void groupMenu(const QPoint &position);
    void runGroup(std::function<GroupResult()> work, const QString &selectedAsset = {});
    void referenceAsset();
    void refreshIndexes();
    void updateAsset();
    void exportAsset();
    void exportPrepared(const SnapshotResult &prepared);
    void savePrepared(const SnapshotResult &prepared, const QStringList &sources);
    void changeReference();
    void saveOrigin();
    void beginOperation();
    void openFile();
    void openSourceFolder();
    void updateActions();
    void removeMembership();
    void deleteAsset(bool whole);
    void showIssues();
    void detailsDialog();
    void run(const QString &message, std::function<SnapshotResult()> work,
             std::function<void(const SnapshotResult &)> finished = {});
    void setBusy(bool value, const QString &message = {});
    void notice(const QString &message, bool error = false);
    void applyTheme();
    void updateActivity();
    void restoreSplit();
    void adaptEmbeddedLayout();
    void updateVersionHeight();
    double splitRatio() const;
    bool applyPendingContext();
    void handleFileDrop(QDropEvent *event, bool working, bool import);
    bool overWorkingPage(const QPoint &position) const;
    QStringList pickSources();
    QString m_library;
    QString m_workspace;
    QString m_category;
    QString m_pendingId;
    QString m_pendingRevision;
    QString m_indexTerm;
    QString m_activeGroup;
    QString m_displayedAsset;
    QHash<QString, QSet<QString>> m_checkedFiles;
    QSet<QString> m_collapsedGroups;
    QStringList m_problems;
    QList<CatalogAsset> m_assets;
    CatalogAsset m_selected;
    Snapshot m_snapshot;
    QPointer<QObject> m_host;
    const bool m_embedded;
    bool m_contextInitialized = false;
    int m_pendingPage = -1;
    int m_generation = 0;
    bool m_busy = false;
    bool m_loadingDetails = false;
    bool m_noticeError = false;
    std::shared_ptr<OperationControl> m_operation;
    ElaPushButton *m_cancel = nullptr;
    QTimer *m_progressTimer = nullptr;
    SnapshotResult m_lastExport;
    double m_horizontalRatio = 0.27;
    double m_verticalRatio = 0.28;
    std::optional<QPair<QString, QString>> m_pendingContext;
    std::optional<QVariantMap> m_pendingState;
    std::optional<CatalogAsset> m_pendingDetail;
    QFutureWatcher<SnapshotResult> *m_detailWatcher = nullptr;
    ElaProgressRing *m_activity = nullptr;
    ElaText *m_status = nullptr;
    ElaText *m_name = nullptr;
    ElaText *m_description = nullptr;
    ElaText *m_fileHeading = nullptr;
    ElaLineEdit *m_search = nullptr;
    ElaTableView *m_versions = nullptr;
    QStandardItemModel *m_versionModel = nullptr;
    ElaComboBox *m_indexes = nullptr;
    ElaComboBox *m_types = nullptr;
    ElaToolButton *m_filterToggle = nullptr;
    ElaToolButton *m_collect = nullptr;
    ElaToolButton *m_refresh = nullptr;
    ElaToolButton *m_theme = nullptr;
    ElaToolButton *m_issues = nullptr;
    ElaToolButton *m_receipt = nullptr;
    ElaToolButton *m_openFile = nullptr;
    ElaToolButton *m_openFolder = nullptr;
    ElaToolButton *m_reference = nullptr;
    ElaToolButton *m_edit = nullptr;
    ElaToolButton *m_remove = nullptr;
    ElaToolButton *m_changeReference = nullptr;
    ElaToolButton *m_deleteRevision = nullptr;
    ElaToolButton *m_deleteAsset = nullptr;
    ElaToolButton *m_renameGroup = nullptr;
    ElaToolButton *m_deleteGroup = nullptr;
    ElaFlowLayout *m_actionLayout = nullptr;
    ElaTreeView *m_list = nullptr;
    ElaToolButton *m_newGroup = nullptr;
    ElaListView *m_groupItems = nullptr;
    ElaTabWidget *m_pages = nullptr;
    QWidget *m_workingPage = nullptr;
    QWidget *m_historyPage = nullptr;
    ElaTreeView *m_workingFiles = nullptr;
    WorkingFilesModel *m_workingModel = nullptr;
    ElaCheckBox *m_checkAll = nullptr;
    ElaToolButton *m_addFiles = nullptr;
    ElaToolButton *m_addFolder = nullptr;
    ElaToolButton *m_openWorking = nullptr;
    ElaText *m_workingEmpty = nullptr;
    ElaText *m_historyEmpty = nullptr;
    ElaListView *m_files = nullptr;
    CatalogModel *m_model = nullptr;
    QStringListModel *m_fileModel = nullptr;
    ElaPushButton *m_new = nullptr;
    ElaToolButton *m_folder = nullptr;
    ElaPushButton *m_take = nullptr;
    ElaToolButton *m_update = nullptr;
    QWidget *m_catalog = nullptr;
    QWidget *m_details = nullptr;
    ElaScrollArea *m_detailScroll = nullptr;
    QBoxLayout *m_identityLayout = nullptr;
    QBoxLayout *m_identityActions = nullptr;
    QBoxLayout *m_workingActions = nullptr;
    QBoxLayout *m_versionHeader = nullptr;
    QWidget *m_versionSection = nullptr;
    QWidget *m_fileSection = nullptr;
    QWidget *m_feedback = nullptr;
    QWidget *m_filters = nullptr;
    QSplitter *m_splitter = nullptr;
    QTimer *m_searchTimer = nullptr;
};

void initializeEla(bool embedded = false);
} // namespace xips
