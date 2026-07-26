#pragma once

#include "app/AssetTableModel.h"
#include "app/LibraryController.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"

#include <QMainWindow>

#include <optional>

class QAction;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QTableView;
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
    void runSearch();
    void updateDetails(const AssetRecord *asset);
    void populateFiles(const AssetRecord &asset);
    void populateVersions(const AssetRecord &asset);
    void selectFirstRow();
    bool selectAssetById(const QString &assetId);

    void chooseLibrary();
    void addFolder();
    void addFile();
    void importAsset(const QString &sourcePath, const QString &dialogTitle);
    void editCurrentAsset();
    void createCurrentVersion();
    void exportCurrentVersion();
    void openCurrentFolder();
    void saveLibrarySetting();

    [[nodiscard]] const AssetRecord *currentRecord() const;
    [[nodiscard]] QString selectedVersion() const;

    QString m_libraryRoot;
    LibraryController *m_controller = nullptr;
    AssetLibraryService m_libraryService;
    std::optional<ActivationRequest> m_pendingActivation;
    bool m_loaded = false;

    QLineEdit *m_searchEdit = nullptr;
    QTableView *m_assetTable = nullptr;
    AssetTableModel *m_tableModel = nullptr;
    QSortFilterProxyModel *m_proxyModel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QTreeWidget *m_infoTree = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    QTreeWidget *m_versionTree = nullptr;
    QTimer *m_searchTimer = nullptr;

    QAction *m_editAction = nullptr;
    QAction *m_versionAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_openFolderAction = nullptr;
};

} // namespace xips
