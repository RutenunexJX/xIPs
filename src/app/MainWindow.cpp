#include "app/MainWindow.h"

#include "assetcore/JsonUtil.h"
#include "assetindex/AssetIndex.h"
#include "assetindex/AssetScanner.h"
#include "diff/DiffService.h"
#include "integration/IntegrationService.h"
#include "managed/ManagedAssetService.h"
#include "manifest/ManifestService.h"
#include "semantic/SlangService.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTableView>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QtConcurrent>

#include <algorithm>

namespace xips {
namespace {

constexpr int FilterKindRole = Qt::UserRole;
constexpr int FilterValueRole = Qt::UserRole + 1;

QTreeWidgetItem *addFilter(QTreeWidgetItem *parent,
                           const QString &label,
                           const QString &kind,
                           const QString &value)
{
    auto *item = new QTreeWidgetItem(parent, {label});
    item->setData(0, FilterKindRole, kind);
    item->setData(0, FilterValueRole, value);
    return item;
}

void addInspectorRow(QTreeWidget *tree, const QString &field, const QString &value)
{
    auto *item = new QTreeWidgetItem(tree, {field, value.isEmpty() ? QStringLiteral("—") : value});
    item->setFirstColumnSpanned(false);
}

QString dependencyLabel(const DependencySpec &dependency)
{
    QString result = dependency.id;
    if (!dependency.versionConstraint.isEmpty()) {
        result += QStringLiteral("  ") + dependency.versionConstraint;
    }
    if (dependency.optional) {
        result += QStringLiteral("  (optional)");
    }
    return result;
}

bool looksBinary(const QByteArray &contents)
{
    if (contents.contains('\0')) {
        return true;
    }
    qsizetype controlCharacters = 0;
    for (const char byte : contents) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x20 && value != '\t' && value != '\n'
            && value != '\r' && value != '\f') {
            controlCharacters += 1;
        }
    }
    return !contents.isEmpty()
           && controlCharacters * 100 > contents.size();
}

QString readPreview(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Cannot open %1\n%2").arg(path, file.errorString());
    }
    constexpr qint64 PreviewLimit = 512 * 1024;
    const QByteArray contents = file.read(PreviewLimit + 1);
    const qsizetype previewSize =
        std::min(contents.size(), static_cast<qsizetype>(PreviewLimit));
    const QByteArray preview = contents.first(previewSize);
    if (looksBinary(preview)) {
        return QStringLiteral(
                   "[Binary preview unavailable]\n\nFile: %1\nSize: %2 bytes")
            .arg(path)
            .arg(file.size());
    }
    QString text = QString::fromUtf8(preview);
    if (contents.size() > PreviewLimit) {
        text += QStringLiteral("\n\n[Preview truncated at 512 KiB]");
    }
    return text;
}

QStringList projectNames(const QList<AssetRecord> &assets)
{
    QStringList result;
    for (const AssetRecord &asset : assets) {
        const QString project =
            asset.manifest.rawObject.value(QStringLiteral("project")).toString();
        if (!project.isEmpty()) {
            result.append(project);
        }
    }
    result.removeDuplicates();
    std::sort(result.begin(), result.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return result;
}

struct UiImportPreview {
    ImportPlan plan;
    QList<AssetUpgrade> upgrades;
};

struct UiDiffResult {
    AssetDifference difference;
    bool semanticCompared = false;
    QString error;
};

struct TestPublishOutcome {
    TestPublishStatus status = TestPublishStatus::Error;
    QString error;
};

struct RegistrationDiscovery {
    SemanticResult semantic;
    QString error;
};

struct UiReferenceInspection {
    QString path;
    QString targetRoot;
    QString expectedContentHash;
    QJsonObject document;
    QList<ReferenceState> states;
    QString error;

    [[nodiscard]] bool needsRepair() const
    {
        return std::any_of(states.cbegin(), states.cend(), [](const ReferenceState &state) {
            return !state.available || !state.hashMatches;
        });
    }

    [[nodiscard]] bool canRepair() const
    {
        return needsRepair()
               && std::all_of(states.cbegin(),
                              states.cend(),
                              [](const ReferenceState &state) {
                                  return (state.available && state.hashMatches)
                                         || state.repairableById;
                              });
    }
};

QString referenceInspectionDetails(const UiReferenceInspection &inspection)
{
    QStringList lines{
        QStringLiteral("Configuration: %1").arg(inspection.path),
        QStringLiteral("Target project: %1").arg(inspection.targetRoot),
        QString(),
    };
    for (const ReferenceState &state : inspection.states) {
        QString status;
        if (state.available && state.hashMatches) {
            status = QStringLiteral("valid");
        } else if (state.repairableById) {
            status = state.available ? QStringLiteral("hash/path mismatch; repairable by ID and hash")
                                     : QStringLiteral("missing; repairable by ID and hash");
        } else {
            status = state.available ? QStringLiteral("hash mismatch; no compatible catalog asset")
                                     : QStringLiteral("missing; no compatible catalog asset");
        }
        lines.append(QStringLiteral("[%1] %2").arg(status, state.assetId));
        lines.append(QStringLiteral("  resolved source: %1")
                         .arg(state.resolvedSourcePath));
    }
    return lines.join(u'\n');
}

QString assetUpgradeKind(const AssetUpgradeKind kind)
{
    switch (kind) {
    case AssetUpgradeKind::Added:
        return QStringLiteral("added");
    case AssetUpgradeKind::Removed:
        return QStringLiteral("removed");
    case AssetUpgradeKind::Changed:
        return QStringLiteral("changed");
    case AssetUpgradeKind::Unchanged:
        return QStringLiteral("unchanged");
    }
    return QStringLiteral("unchanged");
}

QString importPlanDetails(const UiImportPreview &preview)
{
    const ImportPlan &plan = preview.plan;
    QStringList lines{
        QStringLiteral("Mode: %1")
            .arg(plan.mode == ImportMode::Reference ? QStringLiteral("Reference")
                                                    : QStringLiteral("Vendor")),
        QStringLiteral("Target: %1").arg(plan.targetRoot),
        QStringLiteral("Output: %1").arg(plan.outputPath),
        QStringLiteral("Dependency order: %1")
            .arg(plan.dependencies.orderedIds().join(QStringLiteral(" → "))),
    };
    if (plan.mode == ImportMode::Vendor) {
        const QMap<PlannedFileAction, int> counts = plan.actionCounts();
        lines.append(
            QStringLiteral("Files: %1 add, %2 overwrite, %3 remove, %4 conflict, %5 skip")
                .arg(counts.value(PlannedFileAction::Add))
                .arg(counts.value(PlannedFileAction::Overwrite))
                .arg(counts.value(PlannedFileAction::Remove))
                .arg(counts.value(PlannedFileAction::Conflict))
                .arg(counts.value(PlannedFileAction::Skip)));
        lines.append(QString());
        for (const PlannedFile &file : plan.files) {
            lines.append(QStringLiteral("[%1] %2")
                             .arg(plannedFileActionToString(file.action),
                                  file.destinationPath));
        }
    }
    if (!preview.upgrades.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("Asset version/content changes:"));
        for (const AssetUpgrade &upgrade : preview.upgrades) {
            lines.append(
                QStringLiteral("[%1] %2  %3 → %4")
                    .arg(assetUpgradeKind(upgrade.kind),
                         upgrade.assetId,
                         upgrade.beforeVersion.isEmpty()
                             ? QStringLiteral("not installed")
                             : upgrade.beforeVersion,
                         upgrade.afterVersion.isEmpty()
                             ? QStringLiteral("not installed")
                             : upgrade.afterVersion));
        }
    }
    if (!plan.dependencies.issues.isEmpty() || !plan.issues.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("Issues:"));
        for (const DependencyIssue &issue : plan.dependencies.issues) {
            lines.append(QStringLiteral("- %1").arg(issue.message));
        }
        for (const ImportIssue &issue : plan.issues) {
            lines.append(QStringLiteral("- %1: %2").arg(issue.message, issue.path));
        }
    }
    return lines.join(u'\n');
}

QString testCommandDisplay(const TestCommandSpec &command)
{
    QStringList parts{command.program};
    parts.append(command.arguments);
    for (QString &part : parts) {
        if (part.contains(u' ') || part.contains(u'\t') || part.contains(u'"')) {
            part.replace(u'"', QStringLiteral("\\\""));
            part = u'"' + part + u'"';
        }
    }
    return parts.join(u' ');
}

QString diffDetails(const UiDiffResult &result)
{
    QStringList lines;
    for (const DifferenceEntry &entry : result.difference.entries) {
        lines.append(QStringLiteral("[%1/%2] %3")
                         .arg(differenceCategoryToString(entry.category),
                              differenceKindToString(entry.kind),
                              entry.key));
        if (!entry.before.isEmpty()) {
            lines.append(QStringLiteral("  before: %1").arg(entry.before));
        }
        if (!entry.after.isEmpty()) {
            lines.append(QStringLiteral("  after:  %1").arg(entry.after));
        }
    }
    return lines.join(u'\n');
}

QString managedPlanDetails(const ManagedAssetPlan &plan)
{
    QStringList lines{
        QStringLiteral("Target: %1").arg(plan.targetRoot),
        QStringLiteral("Manifest: %1").arg(plan.manifestPath),
        QStringLiteral("Top: %1").arg(plan.manifest.top),
        QStringLiteral("Files:"),
    };
    for (const ManagedPlannedFile &file : plan.files) {
        lines.append(
            QStringLiteral("[%1] %2%3")
                .arg(file.generated() ? QStringLiteral("generate")
                                      : QStringLiteral("copy"),
                     file.destinationPath,
                     file.sourcePath.isEmpty()
                         ? QString()
                         : QStringLiteral(" ← ") + file.sourcePath));
    }
    if (!plan.issues.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("Issues:"));
        for (const ManagedAssetIssue &issue : plan.issues) {
            lines.append(QStringLiteral("- [%1] %2: %3")
                             .arg(diagnosticSeverityToString(issue.severity),
                                  issue.message,
                                  issue.path));
        }
    }
    return lines.join(u'\n');
}

bool pathIsWithin(const QString &path, const QString &root)
{
    const QString normalizedPath =
        QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QString normalizedRoot =
        QDir::fromNativeSeparators(QFileInfo(root).absoluteFilePath());
    return normalizedPath.compare(normalizedRoot, Qt::CaseInsensitive) == 0
           || normalizedPath.startsWith(normalizedRoot + u'/',
                                        Qt::CaseInsensitive);
}

} // namespace

MainWindow::MainWindow(QString primaryLibrary,
                       QStringList externalLibraries,
                       QString indexPath,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_primaryLibrary(std::move(primaryLibrary))
    , m_externalLibraries(std::move(externalLibraries))
    , m_controller(new LibraryController(std::move(indexPath), this))
    , m_testRunner(new TestRunner(this))
{
    buildUi();
    buildMenus();

    QList<LibraryRoot> roots{
        LibraryRoot{.path = m_primaryLibrary, .origin = AssetOrigin::Managed},
    };
    for (const QString &external : m_externalLibraries) {
        roots.append(LibraryRoot{.path = external, .origin = AssetOrigin::External});
    }
    m_controller->setRoots(roots);

    connect(m_controller, &LibraryController::indexingStarted, this, [this] {
        statusBar()->showMessage(QStringLiteral("Indexing asset manifests…"));
    });
    connect(m_controller,
            &LibraryController::incrementalRefreshStarted,
            this,
            [this](const QStringList &assetIds) {
                statusBar()->showMessage(
                    QStringLiteral("Refreshing changed assets: %1")
                        .arg(assetIds.join(QStringLiteral(", "))));
            });
    connect(m_controller,
            &LibraryController::indexingFinished,
            this,
            [this](const QList<AssetRecord> &assets,
                   const QList<ScanIssue> &issues,
                   const qint64 generation) {
                m_assets = assets;
                m_scanIssues = issues;
                populateFilterTree(assets);
                runSearch();
                int errors = 0;
                int warnings = 0;
                for (const ScanIssue &issue : issues) {
                    errors += issue.severity == Diagnostic::Severity::Error ? 1 : 0;
                    warnings += issue.severity == Diagnostic::Severity::Warning ? 1 : 0;
                }
                statusBar()->showMessage(
                    QStringLiteral("%1 assets · generation %2 · %3 errors · %4 warnings")
                        .arg(assets.size())
                        .arg(generation)
                        .arg(errors)
                        .arg(warnings));
            });
    connect(m_controller, &LibraryController::indexingFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message);
        if (message != QStringLiteral("Indexing cancelled")) {
            QMessageBox::critical(this, QStringLiteral("Indexing failed"), message);
        }
    });
    connect(m_testRunner,
            &TestRunner::outputReceived,
            this,
            [this](const QString &text, const bool standardError) {
                m_tests->moveCursor(QTextCursor::End);
                m_tests->insertPlainText(standardError ? QStringLiteral("[stderr] ") + text
                                                       : text);
                m_tests->moveCursor(QTextCursor::End);
            });
    connect(m_testRunner, &TestRunner::finished, this, [this](const TestResult &result) {
        m_tests->appendPlainText(
            QStringLiteral("\nResult: %1 · exit %2 · %3 ms")
                .arg(result.status)
                .arg(result.exitCode)
                .arg(result.durationMs));
        const qint64 generation = m_activeTestGeneration;
        m_activeTestGeneration = -1;
        const QString activeIndexPath = m_controller->indexPath();
        auto *watcher = new QFutureWatcher<TestPublishOutcome>(this);
        connect(watcher,
                &QFutureWatcher<TestPublishOutcome>::finished,
                this,
                [this, watcher, result] {
                    const TestPublishOutcome outcome = watcher->result();
                    watcher->deleteLater();
                    if (outcome.status == TestPublishStatus::Published) {
                        statusBar()->showMessage(
                            QStringLiteral("Test result published for %1: %2")
                                .arg(result.assetId, result.status));
                        runSearch();
                    } else if (outcome.status == TestPublishStatus::Stale) {
                        statusBar()->showMessage(
                            QStringLiteral("Test completed, but source changed; result is stale"));
                    } else {
                        statusBar()->showMessage(
                            QStringLiteral("Test result could not be cached: %1")
                                .arg(outcome.error));
                    }
                });
        watcher->setFuture(QtConcurrent::run(
            [activeIndexPath, result, generation] {
                TestPublishOutcome outcome;
                outcome.status = AssetIndex(activeIndexPath).publishTestResult(
                    result.assetId,
                    result.contentHash,
                    generation,
                    result,
                    &outcome.error);
                return outcome;
            }));
    });

    m_controller->rebuild();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("xIPs — FPGA Asset Library"));
    resize(1460, 860);
    setMinimumSize(980, 620);

    auto *toolbar = addToolBar(QStringLiteral("Library"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    auto *libraryLabel = new QLabel(QStringLiteral("Search  "), toolbar);
    toolbar->addWidget(libraryLabel);
    m_searchEdit = new QLineEdit(toolbar);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("name, id, type, symbol, port, parameter, tag, dependency, tool, path"));
    m_searchEdit->setMinimumWidth(420);
    toolbar->addWidget(m_searchEdit);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] {
        m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::runSearch);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    auto *horizontalSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    m_filterTree = new QTreeWidget(horizontalSplitter);
    m_filterTree->setHeaderLabel(QStringLiteral("Filters"));
    m_filterTree->setMinimumWidth(150);
    m_filterTree->setMaximumWidth(360);

    m_tableModel = new AssetTableModel(this);
    m_proxyModel = new AssetFilterProxyModel(this);
    m_proxyModel->setSourceModel(m_tableModel);

    m_assetTable = new QTableView(horizontalSplitter);
    m_assetTable->setModel(m_proxyModel);
    m_assetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_assetTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_assetTable->setSortingEnabled(true);
    m_assetTable->sortByColumn(AssetTableModel::NameColumn, Qt::AscendingOrder);
    m_assetTable->setAlternatingRowColors(true);
    m_assetTable->setShowGrid(false);
    m_assetTable->setWordWrap(false);
    m_assetTable->verticalHeader()->setDefaultSectionSize(24);
    m_assetTable->verticalHeader()->hide();
    m_assetTable->horizontalHeader()->setStretchLastSection(false);
    m_assetTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_assetTable->setColumnWidth(AssetTableModel::NameColumn, 190);
    m_assetTable->setColumnWidth(AssetTableModel::TypeColumn, 88);
    m_assetTable->setColumnWidth(AssetTableModel::VersionColumn, 82);
    m_assetTable->setColumnWidth(AssetTableModel::TopColumn, 125);
    m_assetTable->setColumnWidth(AssetTableModel::LanguageColumn, 105);
    m_assetTable->setColumnWidth(AssetTableModel::TestColumn, 92);
    m_assetTable->setColumnWidth(AssetTableModel::DiagnosticsColumn, 112);
    m_assetTable->setColumnWidth(AssetTableModel::ModifiedColumn, 92);
    m_assetTable->setColumnWidth(AssetTableModel::RepositoryColumn, 145);
    m_assetTable->setColumnWidth(AssetTableModel::LastUsedColumn, 128);

    m_inspector = new QTreeWidget(horizontalSplitter);
    m_inspector->setColumnCount(2);
    m_inspector->setHeaderLabels({QStringLiteral("Inspector"), QStringLiteral("Value")});
    m_inspector->setRootIsDecorated(false);
    m_inspector->setAlternatingRowColors(true);
    m_inspector->setMinimumWidth(250);
    m_inspector->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_inspector->header()->setStretchLastSection(true);

    horizontalSplitter->setSizes({190, 900, 330});
    horizontalSplitter->setCollapsible(0, false);
    horizontalSplitter->setCollapsible(1, false);

    m_detailsTabs = new QTabWidget(verticalSplitter);
    m_detailsTabs->setDocumentMode(true);
    auto *sourcePage = new QWidget(m_detailsTabs);
    auto *sourceLayout = new QVBoxLayout(sourcePage);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(2);
    m_sourceSelector = new QComboBox(sourcePage);
    m_sourceSelector->setObjectName(QStringLiteral("sourceSelector"));
    m_sourceSelector->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_sourceSelector->setMinimumContentsLength(28);
    sourceLayout->addWidget(m_sourceSelector);
    m_sourcePreview = new QPlainTextEdit(sourcePage);
    m_sourcePreview->setObjectName(QStringLiteral("sourcePreview"));
    m_sourcePreview->setReadOnly(true);
    m_sourcePreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_sourcePreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    sourceLayout->addWidget(m_sourcePreview, 1);
    m_dependencies = new QTreeWidget(m_detailsTabs);
    m_dependencies->setHeaderLabels(
        {QStringLiteral("Asset dependency"), QStringLiteral("Constraint"), QStringLiteral("State")});
    m_dependencies->setRootIsDecorated(false);
    m_versions = new QPlainTextEdit(m_detailsTabs);
    m_versions->setReadOnly(true);
    m_tests = new QPlainTextEdit(m_detailsTabs);
    m_tests->setReadOnly(true);
    m_tests->setMaximumBlockCount(10000);
    m_usage = new QPlainTextEdit(m_detailsTabs);
    m_usage->setReadOnly(true);
    m_detailsTabs->addTab(sourcePage, QStringLiteral("Source Preview"));
    m_detailsTabs->addTab(m_dependencies, QStringLiteral("Dependencies"));
    m_detailsTabs->addTab(m_versions, QStringLiteral("Versions"));
    m_detailsTabs->addTab(m_tests, QStringLiteral("Tests"));
    m_detailsTabs->addTab(m_usage, QStringLiteral("Usage"));
    verticalSplitter->setSizes({610, 230});
    verticalSplitter->setCollapsible(0, false);

    setCentralWidget(verticalSplitter);
    statusBar()->setSizeGripEnabled(true);

    connect(m_assetTable->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex &, const QModelIndex &) {
                updateDetails(currentRecord());
            });
    connect(m_sourceSelector,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](const int index) {
                if (index < 0) {
                    return;
                }
                const QString path =
                    m_sourceSelector->itemData(index).toString();
                if (!path.isEmpty()) {
                    m_sourcePreview->setPlainText(readPreview(path));
                }
            });
    connect(m_filterTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const QString kind = item->data(0, FilterKindRole).toString();
        const QString value = item->data(0, FilterValueRole).toString();
        if (kind == QStringLiteral("all")) {
            m_proxyModel->clearAssetFilter();
        } else if (kind == QStringLiteral("type")) {
            m_proxyModel->setTypeFilter(assetTypeFromString(value));
        } else if (kind == QStringLiteral("tag")) {
            m_proxyModel->setTagFilter(value);
        } else if (kind == QStringLiteral("favorite")) {
            m_proxyModel->setFavoritesOnly(true);
        } else if (kind == QStringLiteral("project")) {
            m_proxyModel->setProjectFilter(value);
        } else if (kind == QStringLiteral("status")) {
            m_proxyModel->setStatusFilter(value);
        }
        selectFirstRow();
    });

    auto *focusSearch = new QShortcut(QKeySequence::Find, this);
    connect(focusSearch, &QShortcut::activated, m_searchEdit, [this] {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *openLibrary =
        fileMenu->addAction(QStringLiteral("Open managed library…"), QKeySequence::Open);
    connect(openLibrary, &QAction::triggered, this, &MainWindow::openPrimaryLibrary);
    QAction *addExternal = fileMenu->addAction(QStringLiteral("Register external library…"));
    connect(addExternal, &QAction::triggered, this, &MainWindow::addExternalLibrary);
    QAction *registerSource =
        fileMenu->addAction(QStringLiteral("Register SystemVerilog source in place…"));
    connect(registerSource,
            &QAction::triggered,
            this,
            &MainWindow::registerSystemVerilogSource);
    QAction *createManaged =
        fileMenu->addAction(QStringLiteral("Create managed module…"));
    connect(createManaged,
            &QAction::triggered,
            this,
            &MainWindow::createManagedAsset);
    fileMenu->addSeparator();
    QAction *referenceImport =
        fileMenu->addAction(QStringLiteral("Reference selected asset into project…"));
    connect(referenceImport, &QAction::triggered, this, [this] {
        importCurrentAsset(ImportMode::Reference);
    });
    QAction *vendorImport =
        fileMenu->addAction(QStringLiteral("Vendor selected asset into project…"));
    connect(vendorImport, &QAction::triggered, this, [this] {
        importCurrentAsset(ImportMode::Vendor);
    });
    QAction *inspectReference =
        fileMenu->addAction(QStringLiteral("Inspect or repair Reference configuration…"));
    connect(inspectReference,
            &QAction::triggered,
            this,
            &MainWindow::inspectReferences);
    fileMenu->addSeparator();
    QAction *exitAction = fileMenu->addAction(QStringLiteral("Exit"), QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *libraryMenu = menuBar()->addMenu(QStringLiteral("&Library"));
    QAction *rebuild = libraryMenu->addAction(QStringLiteral("Rebuild index"), QKeySequence::Refresh);
    connect(rebuild, &QAction::triggered, m_controller, &LibraryController::rebuild);
    QAction *cancel = libraryMenu->addAction(QStringLiteral("Cancel indexing"));
    connect(cancel, &QAction::triggered, m_controller, &LibraryController::cancel);
    QAction *cancelFileOperation =
        libraryMenu->addAction(QStringLiteral("Cancel file operation"));
    connect(cancelFileOperation,
            &QAction::triggered,
            this,
            &MainWindow::cancelFileOperation);

    QMenu *versionsMenu = menuBar()->addMenu(QStringLiteral("&Versions"));
    QAction *compare =
        versionsMenu->addAction(QStringLiteral("Compare selected asset with manifest…"));
    connect(compare, &QAction::triggered, this, &MainWindow::compareCurrentAsset);

    QMenu *testsMenu = menuBar()->addMenu(QStringLiteral("&Tests"));
    QAction *runTest = testsMenu->addAction(QStringLiteral("Run selected asset test…"));
    connect(runTest, &QAction::triggered, this, &MainWindow::runCurrentTest);
    QAction *cancelTest = testsMenu->addAction(QStringLiteral("Cancel running test"));
    connect(cancelTest, &QAction::triggered, m_testRunner, &TestRunner::cancel);

    QMenu *integrationMenu = menuBar()->addMenu(QStringLiteral("&Integration"));
    QAction *openInZeroSlack =
        integrationMenu->addAction(QStringLiteral("Open selected asset in ZeroSlack"));
    connect(openInZeroSlack,
            &QAction::triggered,
            this,
            &MainWindow::openCurrentInZeroSlack);
    QAction *sendCodeBlock =
        integrationMenu->addAction(QStringLiteral("Send selected Code Block to ZeroSlack…"));
    connect(sendCodeBlock,
            &QAction::triggered,
            this,
            &MainWindow::sendCurrentCodeBlock);
}

void MainWindow::populateFilterTree(const QList<AssetRecord> &assets)
{
    m_filterTree->clear();
    auto *types = new QTreeWidgetItem(m_filterTree, {QStringLiteral("Asset type")});
    addFilter(types, QStringLiteral("All assets"), QStringLiteral("all"), QString());
    addFilter(types, QStringLiteral("Code Block"), QStringLiteral("type"), QStringLiteral("code-block"));
    addFilter(types, QStringLiteral("Module"), QStringLiteral("type"), QStringLiteral("module"));
    addFilter(types, QStringLiteral("IP"), QStringLiteral("type"), QStringLiteral("ip"));

    auto *tags = new QTreeWidgetItem(m_filterTree, {QStringLiteral("Tags")});
    QStringList tagValues;
    for (const AssetRecord &asset : assets) {
        tagValues.append(asset.manifest.tags);
    }
    tagValues.removeDuplicates();
    std::sort(tagValues.begin(), tagValues.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    for (const QString &tag : tagValues) {
        addFilter(tags, tag, QStringLiteral("tag"), tag);
    }

    auto *favorites = new QTreeWidgetItem(m_filterTree, {QStringLiteral("Favorites")});
    addFilter(favorites, QStringLiteral("Favorite assets"), QStringLiteral("favorite"), QString());

    auto *projects = new QTreeWidgetItem(m_filterTree, {QStringLiteral("Projects")});
    for (const QString &project : projectNames(assets)) {
        addFilter(projects, project, QStringLiteral("project"), project);
    }

    auto *status = new QTreeWidgetItem(m_filterTree, {QStringLiteral("Status")});
    addFilter(status, QStringLiteral("Modified"), QStringLiteral("status"), QStringLiteral("modified"));
    addFilter(status,
              QStringLiteral("Diagnostics errors"),
              QStringLiteral("status"),
              QStringLiteral("diagnostics"));
    addFilter(status,
              QStringLiteral("Test failed"),
              QStringLiteral("status"),
              QStringLiteral("test-failed"));
    m_filterTree->expandAll();
    m_filterTree->setCurrentItem(types->child(0));
}

void MainWindow::runSearch()
{
    QString error;
    const QList<SearchHit> hits = m_controller->search(m_searchEdit->text(),
                                                       AssetType::Unknown,
                                                       1000,
                                                       &error);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
        return;
    }
    const bool searching = !m_searchEdit->text().trimmed().isEmpty();
    if (searching) {
        m_assetTable->setSortingEnabled(false);
        m_proxyModel->sort(-1);
    } else {
        m_assetTable->setSortingEnabled(true);
        m_assetTable->sortByColumn(AssetTableModel::NameColumn,
                                   Qt::AscendingOrder);
    }
    m_tableModel->setHits(hits);
    selectFirstRow();
}

void MainWindow::updateDetails(const AssetRecord *record)
{
    const QSignalBlocker sourceSelectorBlocker(m_sourceSelector);
    m_sourceSelector->clear();
    m_sourceSelector->hide();
    m_inspector->clear();
    m_dependencies->clear();
    m_sourcePreview->clear();
    m_versions->clear();
    m_tests->clear();
    m_usage->clear();
    if (!record) {
        return;
    }

    const Manifest &manifest = record->manifest;
    addInspectorRow(m_inspector, QStringLiteral("ID"), manifest.id);
    addInspectorRow(m_inspector, QStringLiteral("Description"), manifest.description);
    addInspectorRow(m_inspector, QStringLiteral("Source files"), manifest.sources.join(u'\n'));
    QStringList parameters;
    QStringList ports;
    for (const SemanticUnit &unit : record->semantic.units) {
        for (const SemanticParameter &parameter : unit.parameters) {
            parameters.append(parameter.name + QStringLiteral(" : ") + parameter.type);
        }
        for (const SemanticPort &port : unit.ports) {
            const QString dimensions =
                (port.packedDimensions + u' ' + port.unpackedDimensions).trimmed();
            ports.append(port.direction + u' ' + port.name + QStringLiteral(" : ") + port.type
                         + (dimensions.isEmpty() ? QString()
                                                 : QStringLiteral(" ") + dimensions));
        }
    }
    addInspectorRow(m_inspector, QStringLiteral("Parameters"), parameters.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Ports"), ports.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Include dirs"), manifest.includeDirs.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Defines"), manifest.defines.join(u'\n'));
    addInspectorRow(m_inspector,
                    QStringLiteral("Slang includes"),
                    record->semantic.includes.join(u'\n'));
    addInspectorRow(m_inspector,
                    QStringLiteral("Slang defines"),
                    record->semantic.defines.join(u'\n'));
    QStringList imports;
    for (const SemanticUnit &unit : record->semantic.units) {
        imports.append(unit.imports);
    }
    imports.removeDuplicates();
    addInspectorRow(m_inspector, QStringLiteral("Package imports"), imports.join(u'\n'));
    QStringList dependencies;
    for (const DependencySpec &dependency : manifest.dependencies) {
        dependencies.append(dependencyLabel(dependency));
    }
    addInspectorRow(m_inspector, QStringLiteral("Dependencies"), dependencies.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Constraints"), manifest.constraints.join(u'\n'));
    QStringList tools;
    for (const QString &tool : manifest.tools.keys()) {
        tools.append(tool + QStringLiteral(": ")
                     + manifest.tools.value(tool).toVariant().toString());
    }
    addInspectorRow(m_inspector, QStringLiteral("Tool compatibility"), tools.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Examples"), manifest.examples.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Tests"), manifest.tests.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Git commit/tag"),
                    (record->gitCommit + u' ' + record->gitTag).trimmed());
    addInspectorRow(m_inspector,
                    QStringLiteral("Linked documentation"),
                    manifest.documentation.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Content hash"), record->contentHash);
    addInspectorRow(m_inspector,
                    QStringLiteral("Semantic engine"),
                    record->semantic.engineVersion);
    QStringList semanticDiagnostics;
    for (const Diagnostic &diagnostic : record->semantic.diagnostics) {
        semanticDiagnostics.append(
            QStringLiteral("%1: %2")
                .arg(diagnosticSeverityToString(diagnostic.severity), diagnostic.message));
    }
    addInspectorRow(m_inspector,
                    QStringLiteral("Diagnostics detail"),
                    semanticDiagnostics.join(u'\n'));
    addInspectorRow(m_inspector, QStringLiteral("Origin"), assetOriginToString(record->origin));
    m_inspector->resizeColumnToContents(0);

    if (manifest.type == AssetType::CodeBlock) {
        m_sourcePreview->setPlainText(manifest.templateText);
    } else if (!manifest.sources.isEmpty()) {
        for (const QString &source : manifest.sources) {
            const QString path =
                QDir::isAbsolutePath(source)
                    ? source
                    : QDir(record->assetRoot).absoluteFilePath(source);
            m_sourceSelector->addItem(
                QDir::toNativeSeparators(source),
                QFileInfo(path).absoluteFilePath());
        }
        m_sourceSelector->setCurrentIndex(0);
        m_sourceSelector->setVisible(m_sourceSelector->count() > 1);
        m_sourcePreview->setPlainText(
            readPreview(m_sourceSelector->currentData().toString()));
    } else {
        m_sourcePreview->setPlainText(QStringLiteral("No source file declared."));
    }

    for (const DependencySpec &dependency : manifest.dependencies) {
        new QTreeWidgetItem(m_dependencies,
                            {dependency.id,
                             dependency.versionConstraint,
                             dependency.optional ? QStringLiteral("optional")
                                                 : QStringLiteral("required")});
    }
    QSet<QString> semanticDependencies;
    for (const SemanticUnit &unit : record->semantic.units) {
        for (const QString &instance : unit.instances) {
            semanticDependencies.insert(instance);
        }
    }
    for (const QString &dependency : semanticDependencies) {
        new QTreeWidgetItem(m_dependencies,
                            {dependency,
                             QStringLiteral("Slang instance"),
                             record->stale ? QStringLiteral("stale")
                                           : QStringLiteral("discovered")});
    }
    m_dependencies->resizeColumnToContents(0);
    m_dependencies->resizeColumnToContents(1);

    m_versions->setPlainText(
        QStringLiteral("Working state: %1\nSemantic version: %2\nCommit: %3\nTag: %4\n"
                       "Content hash: %5\nGeneration: %6\nStale: %7\nSlang: %8")
            .arg(record->modifiedStatus,
                 manifest.version.isEmpty() ? QStringLiteral("not declared") : manifest.version,
                 record->gitCommit.isEmpty() ? QStringLiteral("not available") : record->gitCommit,
                 record->gitTag.isEmpty() ? QStringLiteral("not available") : record->gitTag,
                 record->contentHash)
            .arg(record->generation)
            .arg(record->stale ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(record->semantic.engineVersion.isEmpty()
                     ? QStringLiteral("unavailable")
                     : record->semantic.engineVersion));

    QStringList testCommands;
    for (const TestCommandSpec &command : manifest.testCommands) {
        testCommands.append(QStringLiteral("%1: %2")
                                .arg(command.name, testCommandDisplay(command)));
    }
    m_tests->setPlainText(
        QStringLiteral("Status: %1\n\nDeclared test files:\n%2\n\nCommands:\n%3")
            .arg(record->testStatus,
                 manifest.tests.isEmpty() ? QStringLiteral("—") : manifest.tests.join(u'\n'),
                 testCommands.isEmpty() ? QStringLiteral("—") : testCommands.join(u'\n')));
    const QString testAssetKey =
        QStringLiteral("%1|%2|%3")
            .arg(manifest.id, record->contentHash)
            .arg(record->generation);
    m_tests->setProperty("xipsAssetKey", testAssetKey);
    m_tests->setProperty("xipsTestResultsLoaded", false);
    if (!m_testRunner->isRunning()) {
        loadTestResults(*record);
    }

    if (manifest.type == AssetType::CodeBlock) {
        QStringList slotLines;
        for (const SlotDefinition &slot : manifest.slotDefinitions) {
            slotLines.append(QStringLiteral("${%1}  %2")
                             .arg(slot.name, slot.required ? QStringLiteral("required")
                                                          : QStringLiteral("optional")));
        }
        m_usage->setPlainText(
            QStringLiteral("Allowed scope: %1\nWorkspace override: %2\n\nSlots:\n%3"
                           "\n\nExample input:\n%4\n\nExample output:\n%5")
                .arg(manifest.scope.join(QStringLiteral(", ")),
                     manifest.allowWorkspaceOverride ? QStringLiteral("yes")
                                                     : QStringLiteral("no"),
                     slotLines.join(u'\n'),
                     manifest.exampleInput.isUndefined()
                         ? QStringLiteral("—")
                         : QString::fromUtf8(
                               json::canonicalJson(manifest.exampleInput)),
                     manifest.exampleOutput.isUndefined()
                         ? QStringLiteral("—")
                         : QString::fromUtf8(
                               json::canonicalJson(manifest.exampleOutput))));
    } else {
        m_usage->setPlainText(
            QStringLiteral("Reference keeps this source in place.\nAsset root: %1\nTop: %2")
                .arg(record->assetRoot, manifest.top));
    }
}

void MainWindow::openPrimaryLibrary()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Open managed asset library"),
        m_primaryLibrary);
    if (selected.isEmpty()) {
        return;
    }
    m_primaryLibrary = QFileInfo(selected).absoluteFilePath();
    QList<LibraryRoot> roots{
        LibraryRoot{.path = m_primaryLibrary, .origin = AssetOrigin::Managed},
    };
    for (const QString &external : m_externalLibraries) {
        roots.append(LibraryRoot{.path = external, .origin = AssetOrigin::External});
    }
    m_controller->setRoots(roots);
    saveLibrarySettings();
    m_controller->rebuild();
}

void MainWindow::addExternalLibrary()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Register external asset library"),
        m_primaryLibrary);
    if (selected.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(selected).absoluteFilePath();
    if (!m_externalLibraries.contains(absolute, Qt::CaseInsensitive)
        && absolute.compare(m_primaryLibrary, Qt::CaseInsensitive) != 0) {
        m_externalLibraries.append(absolute);
    }

    QList<LibraryRoot> roots{
        LibraryRoot{.path = m_primaryLibrary, .origin = AssetOrigin::Managed},
    };
    for (const QString &external : m_externalLibraries) {
        roots.append(LibraryRoot{.path = external, .origin = AssetOrigin::External});
    }
    m_controller->setRoots(roots);
    saveLibrarySettings();
    m_controller->rebuild();
}

void MainWindow::createManagedAsset()
{
    if (m_fileOperationCancelled
        && !m_fileOperationCancelled->load(std::memory_order_relaxed)) {
        QMessageBox::information(
            this,
            QStringLiteral("Create managed module"),
            QStringLiteral("Another file operation is already running."));
        return;
    }
    const AssetRecord *selectedAsset = currentRecord();
    QStringList seedChoices{QStringLiteral("Empty module")};
    if (selectedAsset && selectedAsset->manifest.type == AssetType::Module) {
        seedChoices.append(QStringLiteral("Selected module"));
    }
    seedChoices.append(QStringLiteral("Source file"));
    seedChoices.append(QStringLiteral("Existing directory"));
    bool accepted = false;
    const QString seedLabel = QInputDialog::getItem(
        this,
        QStringLiteral("Create managed module"),
        QStringLiteral("Starting point"),
        seedChoices,
        0,
        false,
        &accepted);
    if (!accepted) {
        return;
    }
    ManagedAssetSeed seed = ManagedAssetSeed::EmptyModule;
    QString sourceFile;
    QString existingDirectory;
    if (seedLabel == QStringLiteral("Selected module")) {
        seed = ManagedAssetSeed::ExistingDirectory;
        existingDirectory = selectedAsset->assetRoot;
    } else if (seedLabel == QStringLiteral("Source file")) {
        seed = ManagedAssetSeed::SourceFile;
        sourceFile = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select SystemVerilog source"),
            m_primaryLibrary,
            QStringLiteral("HDL files (*.sv *.v);;All files (*)"));
        if (sourceFile.isEmpty()) {
            return;
        }
    } else if (seedLabel == QStringLiteral("Existing directory")) {
        seed = ManagedAssetSeed::ExistingDirectory;
        existingDirectory = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Select seed directory"),
            m_primaryLibrary);
        if (existingDirectory.isEmpty()) {
            return;
        }
    }

    QString suggestedId;
    if (!sourceFile.isEmpty()) {
        suggestedId = QFileInfo(sourceFile).completeBaseName();
    } else if (!existingDirectory.isEmpty()) {
        suggestedId = QFileInfo(existingDirectory).fileName();
    }
    const QString id = QInputDialog::getText(
        this,
        QStringLiteral("Create managed module"),
        QStringLiteral("Stable asset ID"),
        QLineEdit::Normal,
        suggestedId,
        &accepted);
    if (!accepted) {
        return;
    }
    const QString name = QInputDialog::getText(
        this,
        QStringLiteral("Create managed module"),
        QStringLiteral("Display name"),
        QLineEdit::Normal,
        id,
        &accepted);
    if (!accepted) {
        return;
    }
    const QString top = QInputDialog::getText(
        this,
        QStringLiteral("Create managed module"),
        QStringLiteral("Top module/interface/package"),
        QLineEdit::Normal,
        id,
        &accepted);
    if (!accepted) {
        return;
    }
    const QString version = QInputDialog::getText(
        this,
        QStringLiteral("Create managed module"),
        QStringLiteral("Semantic version (optional)"),
        QLineEdit::Normal,
        QStringLiteral("0.1.0"),
        &accepted);
    if (!accepted) {
        return;
    }

    QStringList existingAssetIds;
    for (const AssetRecord &asset : m_assets) {
        existingAssetIds.append(asset.manifest.id);
    }
    const ManagedAssetRequest request{
        .libraryRoot = m_primaryLibrary,
        .id = id,
        .name = name,
        .top = top,
        .version = version,
        .seed = seed,
        .sourceFile = sourceFile,
        .existingDirectory = existingDirectory,
        .existingAssetIds = existingAssetIds,
    };
    statusBar()->showMessage(QStringLiteral("Planning managed asset creation…"));
    auto *watcher = new QFutureWatcher<ManagedAssetPlan>(this);
    connect(watcher, &QFutureWatcher<ManagedAssetPlan>::finished, this, [this, watcher] {
        const ManagedAssetPlan plan = watcher->result();
        watcher->deleteLater();
        QMessageBox preview(this);
        preview.setWindowTitle(QStringLiteral("Managed asset creation plan"));
        preview.setIcon(plan.canExecute() ? QMessageBox::Question
                                          : QMessageBox::Warning);
        preview.setText(
            plan.canExecute()
                ? QStringLiteral("Review the complete creation plan.")
                : QStringLiteral("The creation plan contains errors."));
        preview.setInformativeText(
            QStringLiteral("Files are built in a staging directory and the asset directory is published only after all steps succeed."));
        preview.setDetailedText(managedPlanDetails(plan));
        preview.setStandardButtons(plan.canExecute()
                                       ? QMessageBox::Ok | QMessageBox::Cancel
                                       : QMessageBox::Close);
        preview.setDefaultButton(plan.canExecute() ? QMessageBox::Cancel
                                                   : QMessageBox::Close);
        if (preview.exec() != QMessageBox::Ok || !plan.canExecute()) {
            statusBar()->showMessage(QStringLiteral("Managed asset creation cancelled"));
            return;
        }

        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        m_fileOperationCancelled = cancellation;
        statusBar()->showMessage(QStringLiteral("Creating managed asset…"));
        auto *execution =
            new QFutureWatcher<ManagedAssetExecutionResult>(this);
        connect(execution,
                &QFutureWatcher<ManagedAssetExecutionResult>::finished,
                this,
                [this, execution, cancellation] {
                    const ManagedAssetExecutionResult result = execution->result();
                    execution->deleteLater();
                    if (m_fileOperationCancelled == cancellation) {
                        m_fileOperationCancelled.reset();
                    }
                    if (!result.success) {
                        QMessageBox::critical(
                            this,
                            QStringLiteral("Managed asset creation failed"),
                            result.error
                                + (result.rolledBack
                                       ? QStringLiteral("\nThe staging directory was removed.")
                                       : QString()));
                        statusBar()->showMessage(
                            QStringLiteral("Managed asset creation failed"));
                        return;
                    }
                    statusBar()->showMessage(
                        QStringLiteral("Managed asset created: %1").arg(result.targetRoot));
                    m_controller->rebuild();
                });
        execution->setFuture(QtConcurrent::run([plan, cancellation] {
            return ManagedAssetService().execute(
                plan,
                ManagedAssetExecutionOptions{
                    .confirmed = true,
                    .cancelled = cancellation.get(),
                });
        }));
    });
    watcher->setFuture(QtConcurrent::run([request] {
        return ManagedAssetService().plan(request);
    }));
}

void MainWindow::registerSystemVerilogSource()
{
    bool accepted = false;
    const QString inputType = QInputDialog::getItem(
        this,
        QStringLiteral("Register SystemVerilog in place"),
        QStringLiteral("Input"),
        {QStringLiteral("Source file"), QStringLiteral("Directory or Git checkout")},
        0,
        false,
        &accepted);
    if (!accepted) {
        return;
    }
    QString assetRoot;
    QStringList sources;
    QStringList includeDirectories;
    if (inputType == QStringLiteral("Source file")) {
        const QString source = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Register SystemVerilog source in place"),
            m_primaryLibrary,
            QStringLiteral("SystemVerilog files (*.sv *.v);;All files (*)"));
        if (source.isEmpty()) {
            return;
        }
        sources.append(QFileInfo(source).absoluteFilePath());
        assetRoot = QFileInfo(source).absolutePath();
    } else {
        assetRoot = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Register SystemVerilog directory in place"),
            m_primaryLibrary);
        if (assetRoot.isEmpty()) {
            return;
        }
        QDirIterator iterator(
            assetRoot,
            {QStringLiteral("*.sv"),
             QStringLiteral("*.v"),
             QStringLiteral("*.svh"),
             QStringLiteral("*.vh")},
            QDir::Files,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QString relative =
                QDir::fromNativeSeparators(QDir(assetRoot).relativeFilePath(path));
            const QString relativeLower = relative.toLower();
            if (relativeLower.startsWith(QStringLiteral(".git/"))
                || relativeLower.startsWith(QStringLiteral(".xips/"))
                || relativeLower.startsWith(QStringLiteral("build/"))
                || relativeLower.startsWith(QStringLiteral("build-"))
                || relativeLower.contains(QStringLiteral("/build/"))
                || relativeLower.contains(QStringLiteral("/build-"))) {
                continue;
            }
            const QFileInfo fileInfo(path);
            const QString suffix = fileInfo.suffix().toLower();
            if (suffix == QStringLiteral("svh") || suffix == QStringLiteral("vh")) {
                QString directory =
                    QDir::fromNativeSeparators(
                        QDir(assetRoot).relativeFilePath(fileInfo.absolutePath()));
                if (directory == QStringLiteral(".")) {
                    directory.clear();
                }
                if (!directory.isEmpty()) {
                    includeDirectories.append(directory);
                }
            } else {
                sources.append(fileInfo.absoluteFilePath());
            }
        }
        std::sort(sources.begin(), sources.end());
        includeDirectories.removeDuplicates();
        std::sort(includeDirectories.begin(), includeDirectories.end());
        if (sources.isEmpty()) {
            QMessageBox::warning(
                this,
                QStringLiteral("Register SystemVerilog in place"),
                QStringLiteral("The selected directory contains no .sv or .v source."));
            return;
        }
    }
    AssetRecord candidate;
    candidate.assetRoot = assetRoot;
    candidate.manifestPath =
        QDir(assetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    candidate.manifest.id = sources.size() == 1
                                ? QFileInfo(sources.first()).completeBaseName()
                                : QFileInfo(assetRoot).fileName();
    candidate.manifest.type = AssetType::Module;
    candidate.manifest.name = candidate.manifest.id;
    for (const QString &source : sources) {
        QString relative = QDir(assetRoot).relativeFilePath(source);
        relative = QDir::fromNativeSeparators(QDir::cleanPath(relative));
        candidate.manifest.sources.append(relative);
    }
    candidate.manifest.includeDirs = includeDirectories;
    candidate.contentHash =
        AssetScanner::contentHash(candidate.manifest, candidate.assetRoot);

    statusBar()->showMessage(QStringLiteral("Discovering units with Slang…"));
    auto *watcher = new QFutureWatcher<RegistrationDiscovery>(this);
    connect(watcher,
            &QFutureWatcher<RegistrationDiscovery>::finished,
            this,
            [this, watcher, sources, includeDirectories, assetRoot] {
                const RegistrationDiscovery discovery = watcher->result();
                watcher->deleteLater();
                if (!discovery.error.isEmpty()) {
                    QMessageBox::warning(this,
                                         QStringLiteral("Registration discovery failed"),
                                         discovery.error);
                    statusBar()->showMessage(
                        QStringLiteral("SystemVerilog registration blocked"));
                    return;
                }
                QStringList candidates = discovery.semantic.topCandidates;
                if (candidates.isEmpty()) {
                    for (const SemanticUnit &unit : discovery.semantic.units) {
                        if (unit.kind == QStringLiteral("module")
                            || unit.kind == QStringLiteral("interface")
                            || unit.kind == QStringLiteral("package")) {
                            candidates.append(unit.name);
                        }
                    }
                }
                candidates.removeDuplicates();
                std::sort(candidates.begin(), candidates.end());
                if (candidates.isEmpty()) {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Registration discovery failed"),
                        QStringLiteral("Slang reported no module, interface, or package candidate."));
                    return;
                }

                bool selectionAccepted = false;
                const QString top = QInputDialog::getItem(
                    this,
                    QStringLiteral("Register SystemVerilog source"),
                    QStringLiteral("Asset top"),
                    candidates,
                    0,
                    false,
                    &selectionAccepted);
                if (!selectionAccepted) {
                    return;
                }
                QStringList discoveredSymbols;
                for (const SemanticUnit &unit : discovery.semantic.units) {
                    if (unit.name != top) {
                        continue;
                    }
                    discoveredSymbols.append(unit.instances);
                    for (const QString &importName : unit.imports) {
                        const qsizetype separator =
                            importName.indexOf(QStringLiteral("::"));
                        discoveredSymbols.append(
                            separator < 0 ? importName
                                          : importName.first(separator));
                    }
                }
                discoveredSymbols.removeAll(top);
                discoveredSymbols.removeDuplicates();
                std::sort(discoveredSymbols.begin(),
                          discoveredSymbols.end());

                QStringList suggestedDependencyIds;
                for (const QString &symbol : discoveredSymbols) {
                    QStringList matchingIds;
                    for (const AssetRecord &asset : m_assets) {
                        bool matches =
                            asset.manifest.top == symbol
                            || asset.manifest.id == symbol;
                        for (const SemanticUnit &unit : asset.semantic.units) {
                            matches |= unit.name == symbol;
                        }
                        if (matches) {
                            matchingIds.append(asset.manifest.id);
                        }
                    }
                    matchingIds.removeDuplicates();
                    if (matchingIds.size() == 1) {
                        suggestedDependencyIds.append(matchingIds.first());
                    }
                }
                suggestedDependencyIds.removeDuplicates();
                std::sort(suggestedDependencyIds.begin(),
                          suggestedDependencyIds.end());

                const QString dependenciesText = QInputDialog::getText(
                    this,
                    QStringLiteral("Register SystemVerilog source"),
                    QStringLiteral("Dependency asset IDs, comma-separated (Slang units: %1)")
                        .arg(discoveredSymbols.isEmpty()
                                 ? QStringLiteral("none")
                                 : discoveredSymbols.join(
                                       QStringLiteral(", "))),
                    QLineEdit::Normal,
                    suggestedDependencyIds.join(QStringLiteral(", ")),
                    &selectionAccepted);
                if (!selectionAccepted) {
                    return;
                }
                QList<DependencySpec> dependencies;
                for (QString dependencyText :
                     dependenciesText.split(u',', Qt::SkipEmptyParts)) {
                    dependencyText = dependencyText.trimmed();
                    DependencySpec dependency;
                    if (dependencyText.startsWith(u'?')) {
                        dependency.optional = true;
                        dependencyText.remove(0, 1);
                    }
                    const qsizetype separator = dependencyText.indexOf(u'@');
                    dependency.id =
                        (separator < 0 ? dependencyText
                                       : dependencyText.first(separator))
                            .trimmed();
                    dependency.versionConstraint =
                        separator < 0
                            ? QString()
                            : dependencyText.mid(separator + 1).trimmed();
                    if (!dependency.id.isEmpty()) {
                        dependencies.append(dependency);
                    }
                }
                const QString id = QInputDialog::getText(
                    this,
                    QStringLiteral("Register SystemVerilog source"),
                    QStringLiteral("Stable asset ID"),
                    QLineEdit::Normal,
                    top,
                    &selectionAccepted);
                if (!selectionAccepted) {
                    return;
                }
                const QString name = QInputDialog::getText(
                    this,
                    QStringLiteral("Register SystemVerilog source"),
                    QStringLiteral("Display name"),
                    QLineEdit::Normal,
                    top,
                    &selectionAccepted);
                if (!selectionAccepted) {
                    return;
                }

                QStringList existingAssetIds;
                for (const AssetRecord &asset : m_assets) {
                    existingAssetIds.append(asset.manifest.id);
                }
                IntegrationService service;
                const ModuleRegistrationPlan plan =
                    service.planModuleRegistration(ModuleRegistrationRequest{
                        .assetRoot = assetRoot,
                        .sourcePaths = sources,
                        .id = id,
                        .name = name,
                        .top = top,
                        .includeDirs = includeDirectories,
                        .dependencies = dependencies,
                        .existingAssetIds = existingAssetIds,
                    });
                QMessageBox preview(this);
                preview.setWindowTitle(QStringLiteral("Registration plan"));
                preview.setIcon(plan.canExecute() ? QMessageBox::Question
                                                  : QMessageBox::Warning);
                preview.setText(
                    plan.canExecute()
                        ? QStringLiteral("Create this manifest without moving or copying source?")
                        : QStringLiteral("The registration plan contains errors."));
                preview.setDetailedText(
                    QString::fromUtf8(
                        QJsonDocument(plan.toJson()).toJson(QJsonDocument::Indented)));
                preview.setStandardButtons(plan.canExecute()
                                               ? QMessageBox::Ok | QMessageBox::Cancel
                                               : QMessageBox::Close);
                preview.setDefaultButton(plan.canExecute() ? QMessageBox::Cancel
                                                           : QMessageBox::Close);
                if (preview.exec() != QMessageBox::Ok || !plan.canExecute()) {
                    return;
                }
                QString error;
                if (!service.executeModuleRegistration(plan, true, &error)) {
                    QMessageBox::critical(this,
                                          QStringLiteral("Registration failed"),
                                          error);
                    return;
                }

                bool covered = pathIsWithin(assetRoot, m_primaryLibrary);
                for (const QString &external : m_externalLibraries) {
                    covered |= pathIsWithin(assetRoot, external);
                }
                if (!covered) {
                    m_externalLibraries.append(assetRoot);
                    saveLibrarySettings();
                    QList<LibraryRoot> roots{
                        LibraryRoot{
                            .path = m_primaryLibrary,
                            .origin = AssetOrigin::Managed,
                        },
                    };
                    for (const QString &external : m_externalLibraries) {
                        roots.append(LibraryRoot{
                            .path = external,
                            .origin = AssetOrigin::External,
                        });
                    }
                    m_controller->setRoots(roots);
                }
                statusBar()->showMessage(
                    QStringLiteral("Registered %1 source file(s) in place")
                        .arg(sources.size()));
                m_controller->rebuild();
            });
    watcher->setFuture(QtConcurrent::run([candidate] {
        RegistrationDiscovery discovery;
        discovery.semantic = SlangService().analyze(SlangService::Request{
            .asset = candidate,
            .generation = 1,
        });
        if (!discovery.semantic.available) {
            QStringList diagnostics;
            for (const Diagnostic &diagnostic : discovery.semantic.diagnostics) {
                diagnostics.append(diagnostic.message);
            }
            discovery.error =
                diagnostics.isEmpty()
                    ? QStringLiteral("Slang is unavailable")
                    : diagnostics.join(u'\n');
        }
        return discovery;
    }));
}

void MainWindow::cancelFileOperation()
{
    if (!m_fileOperationCancelled) {
        statusBar()->showMessage(QStringLiteral("No file operation is running"));
        return;
    }
    m_fileOperationCancelled->store(true, std::memory_order_relaxed);
    statusBar()->showMessage(QStringLiteral("Cancellation requested"));
}

void MainWindow::importCurrentAsset(const ImportMode mode)
{
    const AssetRecord *record = currentRecord();
    if (!record) {
        QMessageBox::information(this,
                                 QStringLiteral("Import asset"),
                                 QStringLiteral("Select an asset before importing."));
        return;
    }
    QStringList blockingScanIssues;
    for (const ScanIssue &issue : m_scanIssues) {
        if (issue.severity == Diagnostic::Severity::Error
            && issue.assetId == record->manifest.id) {
            blockingScanIssues.append(issue.message + QStringLiteral(": ") + issue.path);
        }
    }
    if (!blockingScanIssues.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Import asset"),
            QStringLiteral("The selected asset has catalog errors and cannot be imported:\n\n%1")
                .arg(blockingScanIssues.join(u'\n')));
        return;
    }
    const QString targetRoot = QFileDialog::getExistingDirectory(
        this,
        mode == ImportMode::Reference ? QStringLiteral("Select Reference target project")
                                      : QStringLiteral("Select Vendor target project"),
        QDir::currentPath());
    if (targetRoot.isEmpty()) {
        return;
    }

    const QList<AssetRecord> catalog = m_assets;
    const QStringList roots{record->manifest.id};
    statusBar()->showMessage(QStringLiteral("Planning import…"));
    auto *watcher = new QFutureWatcher<UiImportPreview>(this);
    connect(watcher, &QFutureWatcher<UiImportPreview>::finished, this, [this, watcher] {
        const UiImportPreview result = watcher->result();
        watcher->deleteLater();
        const ImportPlan &plan = result.plan;

        QMessageBox preview(this);
        preview.setWindowTitle(QStringLiteral("Import plan"));
        preview.setIcon(plan.canExecute() ? QMessageBox::Question : QMessageBox::Warning);
        preview.setText(
            plan.canExecute()
                ? QStringLiteral("Review the import plan before execution.")
                : QStringLiteral("The import plan contains errors or conflicts."));
        preview.setInformativeText(
            plan.mode == ImportMode::Reference
                ? QStringLiteral("Reference writes configuration only; source files remain in place.")
                : QStringLiteral("Vendor copies a deterministic dependency closure and writes a lockfile."));
        preview.setDetailedText(importPlanDetails(result));
        preview.setStandardButtons(plan.canExecute()
                                       ? QMessageBox::Ok | QMessageBox::Cancel
                                       : QMessageBox::Close);
        preview.setDefaultButton(plan.canExecute() ? QMessageBox::Cancel
                                                   : QMessageBox::Close);
        if (preview.exec() == QMessageBox::Ok && plan.canExecute()) {
            executeImportPlan(plan);
        } else {
            statusBar()->showMessage(plan.canExecute() ? QStringLiteral("Import cancelled")
                                                       : QStringLiteral("Import plan blocked"));
        }
    });
    watcher->setFuture(QtConcurrent::run([catalog, roots, targetRoot, mode] {
        ImportService service;
        UiImportPreview preview;
        if (mode == ImportMode::Reference) {
            preview.plan = service.planReference(catalog, roots, targetRoot);
        } else {
            const VendorUpgradePlan upgrade =
                service.planVendorUpgrade(catalog, roots, targetRoot);
            preview.plan = upgrade.importPlan;
            preview.upgrades = upgrade.assets;
        }
        return preview;
    }));
}

void MainWindow::inspectReferences()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Inspect Reference configuration"),
        QDir::currentPath(),
        QStringLiteral("xIPs Reference configuration (references.json);;JSON files (*.json)"));
    if (path.isEmpty()) {
        return;
    }

    const QList<AssetRecord> catalog = m_assets;
    statusBar()->showMessage(QStringLiteral("Inspecting Reference configuration…"));
    auto *watcher = new QFutureWatcher<UiReferenceInspection>(this);
    connect(watcher,
            &QFutureWatcher<UiReferenceInspection>::finished,
            this,
            [this, watcher, catalog] {
                const UiReferenceInspection inspection = watcher->result();
                watcher->deleteLater();
                if (!inspection.error.isEmpty()) {
                    QMessageBox::critical(this,
                                          QStringLiteral("Reference inspection failed"),
                                          inspection.error);
                    statusBar()->showMessage(
                        QStringLiteral("Reference inspection failed"));
                    return;
                }

                QMessageBox dialog(this);
                dialog.setWindowTitle(QStringLiteral("Reference state"));
                dialog.setIcon(inspection.needsRepair() ? QMessageBox::Warning
                                                        : QMessageBox::Information);
                if (!inspection.needsRepair()) {
                    dialog.setText(
                        QStringLiteral("All Reference entries resolve to the expected asset content."));
                    dialog.setStandardButtons(QMessageBox::Close);
                } else if (inspection.canRepair()) {
                    dialog.setText(
                        QStringLiteral("One or more Reference entries can be repaired from the registered catalog."));
                    dialog.setInformativeText(
                        QStringLiteral("Repair rewrites paths and source/include metadata; it does not copy source files."));
                    dialog.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
                    dialog.setDefaultButton(QMessageBox::Cancel);
                } else {
                    dialog.setText(
                        QStringLiteral("One or more Reference entries cannot be repaired from the registered catalog."));
                    dialog.setStandardButtons(QMessageBox::Close);
                }
                dialog.setDetailedText(referenceInspectionDetails(inspection));
                const int choice = dialog.exec();
                if (!inspection.canRepair() || choice != QMessageBox::Ok) {
                    statusBar()->showMessage(
                        inspection.needsRepair()
                            ? QStringLiteral("Reference repair was not applied")
                            : QStringLiteral("Reference configuration is valid"));
                    return;
                }

                statusBar()->showMessage(
                    QStringLiteral("Repairing Reference configuration…"));
                auto *repair =
                    new QFutureWatcher<QPair<bool, QString>>(this);
                connect(repair,
                        &QFutureWatcher<QPair<bool, QString>>::finished,
                        this,
                        [this, repair, path = inspection.path] {
                            const auto [success, error] = repair->result();
                            repair->deleteLater();
                            if (!success) {
                                QMessageBox::critical(
                                    this,
                                    QStringLiteral("Reference repair failed"),
                                    error);
                                statusBar()->showMessage(
                                    QStringLiteral("Reference repair failed"));
                                return;
                            }
                            statusBar()->showMessage(
                                QStringLiteral("Reference configuration repaired: %1")
                                    .arg(path));
                        });
                repair->setFuture(QtConcurrent::run(
                    [inspection, catalog] {
                        QJsonObject repaired = inspection.document;
                        QString error;
                        ImportService service;
                        const bool success =
                            service.repairReferences(repaired,
                                                     catalog,
                                                     inspection.targetRoot,
                                                     &error)
                            && service.writeReferenceDocumentIfUnchanged(
                                inspection.path,
                                repaired,
                                inspection.expectedContentHash,
                                &error);
                        return qMakePair(success, error);
                    }));
            });
    watcher->setFuture(QtConcurrent::run([path, catalog] {
        UiReferenceInspection inspection;
        inspection.path = QFileInfo(path).absoluteFilePath();
        QFile file(inspection.path);
        if (!file.open(QIODevice::ReadOnly)) {
            inspection.error =
                QStringLiteral("Cannot open Reference configuration: %1")
                    .arg(file.errorString());
            return inspection;
        }
        const QByteArray encoded = file.readAll();
        inspection.expectedContentHash =
            QStringLiteral("sha256:")
            + QString::fromLatin1(
                QCryptographicHash::hash(encoded, QCryptographicHash::Sha256)
                    .toHex());
        QJsonParseError parseError;
        const QJsonDocument parsed =
            QJsonDocument::fromJson(encoded, &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
            inspection.error =
                QStringLiteral("Invalid Reference JSON at byte %1: %2")
                    .arg(parseError.offset)
                    .arg(parseError.errorString());
            return inspection;
        }
        inspection.document = parsed.object();
        if (inspection.document.value(QStringLiteral("schemaVersion")).toInt() != 1
            || inspection.document.value(QStringLiteral("mode")).toString()
                   != QStringLiteral("reference")
            || !inspection.document.value(QStringLiteral("assets")).isArray()) {
            inspection.error = QStringLiteral("Unsupported Reference document");
            return inspection;
        }
        const QString metadataDirectory =
            QFileInfo(inspection.path).absolutePath();
        inspection.targetRoot =
            QFileInfo(metadataDirectory).fileName() == QStringLiteral(".xips")
                ? QFileInfo(QDir(metadataDirectory).absoluteFilePath(QStringLiteral("..")))
                      .absoluteFilePath()
                : metadataDirectory;
        inspection.states =
            ImportService().inspectReferences(inspection.document,
                                              catalog,
                                              inspection.targetRoot);
        return inspection;
    }));
}

void MainWindow::executeImportPlan(const ImportPlan &plan)
{
    if (m_fileOperationCancelled
        && !m_fileOperationCancelled->load(std::memory_order_relaxed)) {
        QMessageBox::information(
            this,
            QStringLiteral("File operation"),
            QStringLiteral("Another file operation is already running."));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Executing import…"));
    const QStringList importedAssetIds =
        plan.dependencies.orderedIds();
    if (plan.mode == ImportMode::Reference) {
        auto *watcher = new QFutureWatcher<QPair<bool, QString>>(this);
        connect(watcher,
                &QFutureWatcher<QPair<bool, QString>>::finished,
                this,
                [this,
                 watcher,
                 outputPath = plan.outputPath,
                 importedAssetIds] {
                    const auto [success, error] = watcher->result();
                    watcher->deleteLater();
                    if (!success) {
                        statusBar()->showMessage(QStringLiteral("Reference import failed"));
                        QMessageBox::critical(this,
                                              QStringLiteral("Reference import failed"),
                                              error);
                        return;
                    }
                    statusBar()->showMessage(
                        QStringLiteral("Reference configuration written: %1").arg(outputPath));
                    for (const QString &assetId : importedAssetIds) {
                        markAssetUsed(assetId);
                    }
                });
        watcher->setFuture(QtConcurrent::run([plan] {
            QString error;
            const bool success = ImportService().writeReference(plan, &error);
            return qMakePair(success, error);
        }));
        return;
    }

    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_fileOperationCancelled = cancellation;
    auto *watcher = new QFutureWatcher<ImportExecutionResult>(this);
    connect(watcher,
            &QFutureWatcher<ImportExecutionResult>::finished,
            this,
            [this,
             watcher,
             outputPath = plan.outputPath,
             cancellation,
             importedAssetIds] {
                const ImportExecutionResult result = watcher->result();
                watcher->deleteLater();
                if (m_fileOperationCancelled == cancellation) {
                    m_fileOperationCancelled.reset();
                }
                if (!result.success) {
                    statusBar()->showMessage(QStringLiteral("Vendor import failed"));
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Vendor import failed"),
                        result.error
                            + (result.rolledBack
                                   ? QStringLiteral("\nThe transaction was rolled back.")
                                   : QString()));
                    return;
                }
                statusBar()->showMessage(
                    QStringLiteral("Vendor import complete: %1 added, %2 overwritten, %3 removed, %4 skipped · %5")
                        .arg(result.added)
                        .arg(result.overwritten)
                        .arg(result.removed)
                        .arg(result.skipped)
                        .arg(outputPath));
                for (const QString &assetId : importedAssetIds) {
                    markAssetUsed(assetId);
                }
            });
    watcher->setFuture(QtConcurrent::run([plan, cancellation] {
        return ImportService().executeVendor(
            plan,
            ImportExecutionOptions{
                .confirmed = true,
                .cancelled = cancellation.get(),
            });
    }));
}

void MainWindow::runCurrentTest()
{
    if (m_testRunner->isRunning()) {
        QMessageBox::information(this,
                                 QStringLiteral("Run test"),
                                 QStringLiteral("A test is already running."));
        return;
    }
    const AssetRecord *record = currentRecord();
    if (!record) {
        QMessageBox::information(this,
                                 QStringLiteral("Run test"),
                                 QStringLiteral("Select an asset before running a test."));
        return;
    }
    if (record->manifest.testCommands.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Run test"),
            QStringLiteral("The selected manifest declares no structured test command."));
        return;
    }

    TestCommandSpec command = record->manifest.testCommands.first();
    if (record->manifest.testCommands.size() > 1) {
        QStringList names;
        for (const TestCommandSpec &candidate : record->manifest.testCommands) {
            names.append(candidate.name);
        }
        bool selected = false;
        const QString name = QInputDialog::getItem(
            this,
            QStringLiteral("Run test"),
            QStringLiteral("Command"),
            names,
            0,
            false,
            &selected);
        if (!selected) {
            return;
        }
        const auto iterator = std::find_if(
            record->manifest.testCommands.cbegin(),
            record->manifest.testCommands.cend(),
            [&name](const TestCommandSpec &candidate) {
                return candidate.name == name;
            });
        if (iterator != record->manifest.testCommands.cend()) {
            command = *iterator;
        }
    }

    const QString configuredWorkingDirectory =
        command.workingDirectory.isEmpty()
            ? record->assetRoot
            : (QDir::isAbsolutePath(command.workingDirectory)
                   ? command.workingDirectory
                   : QDir(record->assetRoot)
                         .absoluteFilePath(command.workingDirectory));
    const QString confirmation =
        QStringLiteral("Execute this repository-provided command?\n\n"
                       "Command: %1\nWorking directory: %2\n"
                       "Asset: %3\nContent hash: %4")
            .arg(testCommandDisplay(command),
                 configuredWorkingDirectory,
                 record->manifest.id,
                 record->contentHash);
    if (QMessageBox::question(this,
                              QStringLiteral("Confirm test execution"),
                              confirmation,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    m_detailsTabs->setCurrentWidget(m_tests);
    m_tests->setPlainText(
        QStringLiteral("Running %1\nCommand: %2\nWorking directory: %3\n\n")
            .arg(command.name,
                 testCommandDisplay(command),
                 configuredWorkingDirectory));
    m_activeTestGeneration = record->generation;
    QString error;
    if (!m_testRunner->start(TestRunRequest{
                                 .asset = *record,
                                 .command = command,
                                 .confirmed = true,
                             },
                             &error)) {
        m_activeTestGeneration = -1;
        QMessageBox::critical(this, QStringLiteral("Cannot start test"), error);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Test running: %1").arg(command.name));
    markAssetUsed(record->manifest.id);
}

void MainWindow::loadTestResults(const AssetRecord &record)
{
    const QString assetKey =
        QStringLiteral("%1|%2|%3")
            .arg(record.manifest.id, record.contentHash)
            .arg(record.generation);
    const QString indexPath = m_controller->indexPath();
    const QString assetId = record.manifest.id;
    const QString contentHash = record.contentHash;
    const QString gitCommit = record.gitCommit;
    auto *watcher = new QFutureWatcher<QList<TestResult>>(this);
    connect(watcher,
            &QFutureWatcher<QList<TestResult>>::finished,
            this,
            [this, watcher, assetKey, contentHash, gitCommit] {
                const QList<TestResult> results = watcher->result();
                watcher->deleteLater();
                if (m_tests->property("xipsAssetKey").toString() != assetKey
                    || m_tests->property("xipsTestResultsLoaded").toBool()
                    || m_testRunner->isRunning()) {
                    return;
                }
                m_tests->setProperty("xipsTestResultsLoaded", true);
                m_tests->appendPlainText(QStringLiteral("\nCached results:"));
                if (results.isEmpty()) {
                    m_tests->appendPlainText(QStringLiteral("—"));
                    return;
                }
                for (const TestResult &result : results) {
                    m_tests->appendPlainText(
                        QStringLiteral("%1: %2 · %3 ms · %4 · stale: %5")
                            .arg(result.commandName,
                                 result.status,
                                 QString::number(result.durationMs),
                                 result.startedAt.toLocalTime().toString(Qt::ISODate),
                                 result.isStale(contentHash, gitCommit)
                                     ? QStringLiteral("yes")
                                     : QStringLiteral("no")));
                }
            });
    watcher->setFuture(QtConcurrent::run([indexPath, assetId] {
        return AssetIndex(indexPath).testResults(assetId);
    }));
}

void MainWindow::compareCurrentAsset()
{
    const AssetRecord *selected = currentRecord();
    if (!selected) {
        QMessageBox::information(this,
                                 QStringLiteral("Compare asset"),
                                 QStringLiteral("Select an asset before comparing."));
        return;
    }
    const QString manifestPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select another xIPs manifest"),
        selected->assetRoot,
        QStringLiteral("xIPs manifest (.xips.json);;JSON files (*.json)"));
    if (manifestPath.isEmpty()) {
        return;
    }

    const AssetRecord before = *selected;
    statusBar()->showMessage(QStringLiteral("Computing asset differences…"));
    auto *watcher = new QFutureWatcher<UiDiffResult>(this);
    connect(watcher, &QFutureWatcher<UiDiffResult>::finished, this, [this, watcher] {
        const UiDiffResult result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Asset comparison failed"));
            QMessageBox::critical(this,
                                  QStringLiteral("Asset comparison failed"),
                                  result.error);
            return;
        }
        const QMap<DifferenceCategory, int> counts = result.difference.counts();
        QMessageBox dialog(this);
        dialog.setWindowTitle(QStringLiteral("Asset differences"));
        dialog.setIcon(QMessageBox::Information);
        dialog.setText(
            result.difference.identical()
                ? QStringLiteral("The compared assets are identical in the available data.")
                : QStringLiteral("%1 file, %2 manifest, %3 semantic, and %4 dependency differences.")
                      .arg(counts.value(DifferenceCategory::File))
                      .arg(counts.value(DifferenceCategory::Manifest))
                      .arg(counts.value(DifferenceCategory::Semantic))
                      .arg(counts.value(DifferenceCategory::Dependency)));
        if (!result.semanticCompared) {
            dialog.setInformativeText(
                QStringLiteral("Semantic differences were omitted because Slang results were unavailable for one or both versions."));
        }
        dialog.setDetailedText(result.difference.identical()
                                   ? QStringLiteral("No differences.")
                                   : diffDetails(result));
        dialog.exec();
        statusBar()->showMessage(QStringLiteral("Asset comparison complete"));
    });
    watcher->setFuture(QtConcurrent::run([before, manifestPath] {
        UiDiffResult result;
        const ManifestLoadResult loaded = ManifestService().load(manifestPath);
        if (!loaded.ok()) {
            QStringList messages;
            for (const Diagnostic &diagnostic : loaded.diagnostics) {
                messages.append(diagnostic.message);
            }
            result.error = messages.join(u'\n');
            return result;
        }

        AssetRecord left = before;
        AssetRecord right;
        right.manifest = *loaded.manifest;
        right.assetRoot = QFileInfo(manifestPath).absolutePath();
        right.manifestPath = QFileInfo(manifestPath).absoluteFilePath();
        right.contentHash =
            AssetScanner::contentHash(right.manifest, right.assetRoot);
        right.generation = qMax<qint64>(1, before.generation);

        SlangService slang;
        if (SlangService::supports(left) && SlangService::supports(right)) {
            if (!left.semantic.available || left.stale) {
                left.semantic = slang.analyze(SlangService::Request{
                    .asset = left,
                    .generation = qMax<qint64>(1, left.generation),
                });
            }
            right.semantic = slang.analyze(SlangService::Request{
                .asset = right,
                .generation = right.generation,
            });
            result.semanticCompared =
                left.semantic.available && right.semantic.available;
        }
        if (!result.semanticCompared) {
            left.semantic = {};
            right.semantic = {};
        }
        result.difference = DiffService().compare(left, right);
        return result;
    }));
}

void MainWindow::openCurrentInZeroSlack()
{
    const AssetRecord *record = currentRecord();
    if (!record) {
        QMessageBox::information(this,
                                 QStringLiteral("Open in ZeroSlack"),
                                 QStringLiteral("Select an asset first."));
        return;
    }
    const QUrl uri = IntegrationService::zeroSlackOpenUri(*record);
    if (!QDesktopServices::openUrl(uri)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Open in ZeroSlack"),
            QStringLiteral("No application accepted the URI:\n%1")
                .arg(uri.toString(QUrl::FullyEncoded)));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Sent ZeroSlack open request for %1").arg(record->manifest.id));
    markAssetUsed(record->manifest.id);
}

void MainWindow::sendCurrentCodeBlock()
{
    const AssetRecord *record = currentRecord();
    if (!record || record->manifest.type != AssetType::CodeBlock) {
        QMessageBox::information(
            this,
            QStringLiteral("Send Code Block"),
            QStringLiteral("Select a Code Block asset first."));
        return;
    }
    const QString documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggested = QDir(documents).absoluteFilePath(
        record->manifest.id + QStringLiteral(".zeroslack-code-block.json"));
    const QString handoff = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save ZeroSlack Code Block handoff"),
        suggested,
        QStringLiteral("JSON files (*.json)"));
    if (handoff.isEmpty()) {
        return;
    }
    if (QMessageBox::question(
            this,
            QStringLiteral("Send Code Block"),
            QStringLiteral("Write the versioned Code Block payload and open it in ZeroSlack?\n\n%1")
                .arg(QFileInfo(handoff).absoluteFilePath()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!IntegrationService::writeCodeBlockHandoff(*record, handoff, &error)) {
        QMessageBox::critical(this, QStringLiteral("Code Block handoff failed"), error);
        return;
    }
    const QUrl uri = IntegrationService::zeroSlackCodeBlockUri(*record, handoff);
    if (!QDesktopServices::openUrl(uri)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Send Code Block"),
            QStringLiteral("The handoff file was written, but no application accepted:\n%1")
                .arg(uri.toString(QUrl::FullyEncoded)));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Sent Code Block handoff for %1").arg(record->manifest.id));
    markAssetUsed(record->manifest.id);
}

void MainWindow::markAssetUsed(const QString &assetId)
{
    const QDateTime usedAt = QDateTime::currentDateTimeUtc();
    QString error;
    if (!m_controller->markUsed(assetId, usedAt, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not update last-used time for %1: %2")
                .arg(assetId, error));
        return;
    }
    m_tableModel->setLastUsed(assetId, usedAt);
    for (AssetRecord &asset : m_assets) {
        if (asset.manifest.id == assetId) {
            asset.lastUsed = usedAt;
            break;
        }
    }
}

void MainWindow::saveLibrarySettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("library/primary"), m_primaryLibrary);
    settings.setValue(QStringLiteral("library/external"), m_externalLibraries);
}

const AssetRecord *MainWindow::currentRecord() const
{
    const QModelIndex proxyIndex = m_assetTable->currentIndex();
    if (!proxyIndex.isValid()) {
        return nullptr;
    }
    const QModelIndex sourceIndex = m_proxyModel->mapToSource(proxyIndex);
    return m_tableModel->recordAt(sourceIndex.row());
}

void MainWindow::selectFirstRow()
{
    if (m_proxyModel->rowCount() > 0) {
        m_assetTable->selectRow(0);
        m_assetTable->setCurrentIndex(m_proxyModel->index(0, 0));
    } else {
        updateDetails(nullptr);
    }
}

} // namespace xips
