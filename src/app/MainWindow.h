#pragma once

#include "app/AssetTableModel.h"
#include "app/LibraryController.h"
#include "integration/IntegrationService.h"
#include "library/AssetLibraryService.h"

#include <QMainWindow>

#include <optional>
#include <functional>

class QAction;
class QDragEnterEvent;
class QDropEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace xips {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QString libraryRoot,
                        QWidget *parent = nullptr,
                        RemovalMode removalMode = RemovalMode::MoveToTrash);
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
    void undoLastImport();
    void editCurrentAsset();
    void updateCurrentAsset();
    void deleteCurrentAsset();
    void createCurrentVersion();
    void copyCurrentVersion();
    void deleteSelectedVersion();
    void openCurrent();
    void openSelectedFile();
    void assignNewGroup();
    void renameCurrentGroup();
    void removeCurrentGroup();
    void showProblems();
    void showNotice(const QString &message,
                    const QString &actionText = {},
                    std::function<void()> action = {});
    void clearNotice();
    void setLibraryReady(bool ready);
    void saveLibrarySetting();

    [[nodiscard]] const AssetRecord *currentRecord() const;
    [[nodiscard]] QList<AssetRecord> selectedRecords() const;
    [[nodiscard]] QString currentGroup() const;
    [[nodiscard]] QString selectedVersion() const;

    QString m_libraryRoot;
    LibraryController *m_controller = nullptr;
    AssetLibraryService m_libraryService;
    RemovalMode m_removalMode = RemovalMode::MoveToTrash;
    std::optional<ActivationRequest> m_pendingActivation;
    bool m_loaded = false;
    bool m_searchWasEmpty = true;
    int m_stateGeneration = 0;
    QStringList m_lastProblems;
    QList<AssetRecord> m_undoImportAssets;
    std::function<void()> m_noticeCallback;

    QStackedWidget *m_contentStack = nullptr;
    QWidget *m_libraryPage = nullptr;
    QWidget *m_welcomePage = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_searchScopeLabel = nullptr;
    QTreeWidget *m_groupTree = nullptr;
    QTableView *m_assetTable = nullptr;
    QStackedWidget *m_resultsStack = nullptr;
    QLabel *m_emptyResultsLabel = nullptr;
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
    QFrame *m_noticeFrame = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QToolButton *m_noticeActionButton = nullptr;

    QAction *m_addFilesAction = nullptr;
    QAction *m_addFolderAction = nullptr;
    QAction *m_editAction = nullptr;
    QAction *m_updateAction = nullptr;
    QAction *m_deleteAssetAction = nullptr;
    QAction *m_versionAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_problemAction = nullptr;
    QAction *m_deleteVersionAction = nullptr;
};

} // namespace xips
