#pragma once

#include "app/AssetTableModel.h"
#include "app/LibraryController.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"

#include <QMainWindow>

#include <optional>

class QAction;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace xips {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString libraryRoot, QWidget *parent = nullptr);
    void applyActivation(const ActivationRequest &request);
    [[nodiscard]] static QString openTarget(const AssetRecord &asset);

protected:
    bool event(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void runSearch();
    void rebuildGroups(const QList<AssetRecord> &assets);
    void updateDetails(const AssetRecord *asset);
    void populateFiles(const AssetRecord &asset);
    void populateVersions(const AssetRecord &asset);
    void selectFirstRow();
    bool selectAssetById(const QString &assetId);

    void chooseLibrary();
    void addFolder();
    void addFiles();
    void importPaths(const QStringList &sourcePaths);
    void editCurrentAsset();
    void createCurrentVersion();
    void copyCurrentVersion();
    void deleteSelectedVersion();
    void openCurrent();
    void openSelectedFile();
    void assignNewGroup();
    void renameCurrentGroup();
    void removeCurrentGroup();
    void showProblems();
    void setLibraryReady(bool ready);
    void saveLibrarySetting();

    [[nodiscard]] const AssetRecord *currentRecord() const;
    [[nodiscard]] QList<AssetRecord> selectedRecords() const;
    [[nodiscard]] QString currentGroup() const;
    [[nodiscard]] QString selectedVersion() const;

    QString m_libraryRoot;
    LibraryController *m_controller = nullptr;
    AssetLibraryService m_libraryService;
    std::optional<ActivationRequest> m_pendingActivation;
    bool m_loaded = false;
    int m_stateGeneration = 0;
    QStringList m_lastProblems;

    QStackedWidget *m_contentStack = nullptr;
    QWidget *m_libraryPage = nullptr;
    QWidget *m_welcomePage = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_groupTree = nullptr;
    QTableView *m_assetTable = nullptr;
    AssetTableModel *m_tableModel = nullptr;
    QSortFilterProxyModel *m_proxyModel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QTreeWidget *m_infoTree = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    QTreeWidget *m_versionTree = nullptr;
    QTreeWidgetItem *m_workingStateItem = nullptr;
    QTimer *m_searchTimer = nullptr;
    QTimer *m_focusRefreshTimer = nullptr;

    QAction *m_addFilesAction = nullptr;
    QAction *m_addFolderAction = nullptr;
    QAction *m_editAction = nullptr;
    QAction *m_versionAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_problemAction = nullptr;
    QAction *m_deleteVersionAction = nullptr;
};

} // namespace xips
