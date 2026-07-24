#pragma once

#include "app/AssetTableModel.h"
#include "app/LibraryController.h"
#include "importer/ImportService.h"
#include "testrunner/TestRunner.h"

#include <QMainWindow>

#include <atomic>
#include <memory>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSplitter;
class QTableView;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace xips {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QString primaryLibrary,
               QStringList externalLibraries,
               QString indexPath,
               QWidget *parent = nullptr);

private:
    void buildUi();
    void buildMenus();
    void populateFilterTree(const QList<AssetRecord> &assets);
    void runSearch();
    void updateDetails(const AssetRecord *record);
    void openPrimaryLibrary();
    void addExternalLibrary();
    void importCurrentAsset(ImportMode mode);
    void inspectReferences();
    void executeImportPlan(const ImportPlan &plan);
    void runCurrentTest();
    void loadTestResults(const AssetRecord &record);
    void compareCurrentAsset();
    void openCurrentInZeroSlack();
    void sendCurrentCodeBlock();
    void createManagedAsset();
    void registerSystemVerilogSource();
    void cancelFileOperation();
    void markAssetUsed(const QString &assetId);
    void saveLibrarySettings();
    const AssetRecord *currentRecord() const;
    void selectFirstRow();

    QString m_primaryLibrary;
    QStringList m_externalLibraries;
    LibraryController *m_controller = nullptr;
    TestRunner *m_testRunner = nullptr;
    qint64 m_activeTestGeneration = -1;
    std::shared_ptr<std::atomic_bool> m_fileOperationCancelled;

    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_filterTree = nullptr;
    QTableView *m_assetTable = nullptr;
    QTreeWidget *m_inspector = nullptr;
    QComboBox *m_sourceSelector = nullptr;
    QPlainTextEdit *m_sourcePreview = nullptr;
    QTreeWidget *m_dependencies = nullptr;
    QPlainTextEdit *m_versions = nullptr;
    QPlainTextEdit *m_tests = nullptr;
    QPlainTextEdit *m_usage = nullptr;
    QTabWidget *m_detailsTabs = nullptr;
    QTimer *m_searchTimer = nullptr;
    AssetTableModel *m_tableModel = nullptr;
    AssetFilterProxyModel *m_proxyModel = nullptr;
    QList<AssetRecord> m_assets;
    QList<ScanIssue> m_scanIssues;
};

} // namespace xips
