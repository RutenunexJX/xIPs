#pragma once

#include "app/AssetTableModel.h"
#include "app/LibraryController.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"

#include <QMainWindow>

#include <optional>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableView;
class QTabWidget;
class QTimer;
class QTreeWidget;

namespace xips {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString libraryRoot, QWidget *parent = nullptr);
    void applyActivation(const ActivationRequest &request);

private:
    void buildUi();
    void buildMenus();
    void runSearch();
    void refreshTagFilter();
    void updateDetails(const AssetRecord *asset);
    void populateFiles(const AssetRecord &asset);
    void populateVersions(const AssetRecord &asset);
    void selectFirstRow();
    bool selectAssetById(const QString &assetId);

    void chooseLibrary();
    void addIp();
    void editCurrentIp();
    void createCurrentVersion();
    void exportCurrentVersion();
    void openCurrentFolder();
    void copyCurrentLink();
    void saveLibrarySetting();

    [[nodiscard]] const AssetRecord *currentRecord() const;
    [[nodiscard]] QString selectedVersion() const;

    QString m_libraryRoot;
    LibraryController *m_controller = nullptr;
    AssetLibraryService m_libraryService;
    std::optional<ActivationRequest> m_pendingActivation;

    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_tagFilter = nullptr;
    QTableView *m_assetTable = nullptr;
    AssetTableModel *m_tableModel = nullptr;
    AssetFilterProxyModel *m_proxyModel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QTreeWidget *m_infoTree = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    QPlainTextEdit *m_sourcePreview = nullptr;
    QTreeWidget *m_versionTree = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTimer *m_searchTimer = nullptr;

    QAction *m_editAction = nullptr;
    QAction *m_versionAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_openFolderAction = nullptr;
    QAction *m_copyLinkAction = nullptr;

    QList<AssetRecord> m_assets;
    QList<ScanIssue> m_scanIssues;
};

} // namespace xips
