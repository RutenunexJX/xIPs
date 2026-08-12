#include "app/MainWindow.h"

#include "library/AssetScanner.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDirIterator>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace xips {
namespace {

constexpr int VersionRole = Qt::UserRole;
constexpr int GroupRole = Qt::UserRole;
constexpr int FilePathRole = Qt::UserRole;

QString ungroupedFilterKey()
{
    return QString(1, QChar(0x1f));
}

bool isUngroupedFilter(const QString &group)
{
    return group == ungroupedFilterKey();
}

QString displayedGroup(const QString &group)
{
    return isUngroupedFilter(group) ? QStringLiteral("Ungrouped") : group;
}

bool isReservedGroupName(const QString &group)
{
    const QString name = group.trimmed();
    return name.compare(QStringLiteral("All assets"), Qt::CaseInsensitive) == 0
           || name.compare(QStringLiteral("Ungrouped"),
                           Qt::CaseInsensitive) == 0;
}

void setPreviewFilesWritable(const QString &root, const bool writable)
{
    QDirIterator iterator(root,
                          QDir::Files | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        QFileDevice::Permissions permissions = QFileInfo(path).permissions();
        if (writable) {
            permissions |= QFileDevice::WriteOwner | QFileDevice::WriteUser;
        } else {
            permissions &= ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                             | QFileDevice::WriteGroup
                             | QFileDevice::WriteOther);
        }
        QFile::setPermissions(path, permissions);
    }
}

struct GroupSummary {
    QString name;
    int count = 0;
};

void addInfoRow(QTreeWidget *tree, const QString &field, const QString &value)
{
    new QTreeWidgetItem(tree, {field, value.isEmpty() ? QStringLiteral("-") : value});
}

bool hasGroup(const AssetRecord &asset, const QString &group)
{
    for (const QString &tag : asset.manifest.tags) {
        if (tag.trimmed().compare(group, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool editMetadata(QWidget *parent,
                  const QString &title,
                  const QStringList &availableGroups,
                  AssetMetadata &metadata)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("metadataDialog"));
    dialog.setWindowTitle(title);
    dialog.resize(520, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *name = new QLineEdit(metadata.name, &dialog);
    name->setObjectName(QStringLiteral("ipNameEdit"));
    auto *groups = new QListWidget(&dialog);
    groups->setObjectName(QStringLiteral("groupChecklist"));
    groups->setMaximumHeight(130);
    for (const QString &group : availableGroups) {
        auto *item = new QListWidgetItem(group, groups);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(metadata.tags.contains(group, Qt::CaseInsensitive)
                                ? Qt::Checked
                                : Qt::Unchecked);
    }
    auto *newGroup = new QLineEdit(&dialog);
    newGroup->setObjectName(QStringLiteral("newGroupEdit"));
    newGroup->setPlaceholderText(QStringLiteral("Optional new group"));
    auto *description = new QPlainTextEdit(metadata.description, &dialog);
    description->setPlaceholderText(QStringLiteral("What this asset provides"));
    description->setMaximumBlockCount(100);

    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Groups"), groups);
    form->addRow(QStringLiteral("New group"), newGroup);
    form->addRow(QStringLiteral("Description"), description);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                            | QDialogButtonBox::Cancel,
                                        &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Incomplete metadata"),
                                 QStringLiteral("Name is required."));
            return;
        }
        if (isReservedGroupName(newGroup->text())) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Reserved group name"),
                QStringLiteral("'All assets' and 'Ungrouped' are automatic filters. Choose another group name."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    metadata.name = name->text().trimmed();
    metadata.description = description->toPlainText().trimmed();
    metadata.tags.clear();
    for (int index = 0; index < groups->count(); ++index) {
        const QListWidgetItem *item = groups->item(index);
        if (item->checkState() == Qt::Checked) {
            metadata.tags.append(item->text());
        }
    }
    const QString createdGroup = newGroup->text().trimmed();
    if (!createdGroup.isEmpty()
        && !metadata.tags.contains(createdGroup, Qt::CaseInsensitive)) {
        metadata.tags.append(createdGroup);
    }
    return true;
}

QStringList groupNames(const QList<AssetRecord> &assets)
{
    QStringList groups;
    for (const AssetRecord &asset : assets) {
        for (const QString &rawGroup : asset.manifest.tags) {
            const QString group = rawGroup.trimmed();
            if (!group.isEmpty()
                && !groups.contains(group, Qt::CaseInsensitive)) {
                groups.append(group);
            }
        }
    }
    std::sort(groups.begin(), groups.end(), [](const QString &left,
                                                const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });
    return groups;
}

QString updatePreviewText(const UpdatePreview &preview)
{
    QString text = QStringLiteral("Add %1  |  Replace %2  |  Remove %3  |  Unchanged %4")
                       .arg(preview.addedFiles.size())
                       .arg(preview.replacedFiles.size())
                       .arg(preview.removedFiles.size())
                       .arg(preview.unchangedCount);
    if (!preview.removedFiles.isEmpty()) {
        QStringList examples = preview.removedFiles.mid(0, 4);
        for (QString &path : examples) {
            path = QDir::toNativeSeparators(path);
        }
        text += QStringLiteral("\nFiles removed from the working copy: %1")
                    .arg(examples.join(QStringLiteral(", ")));
        if (preview.removedFiles.size() > examples.size()) {
            text += QStringLiteral(" and %1 more")
                        .arg(preview.removedFiles.size() - examples.size());
        }
    }
    return text;
}

} // namespace

MainWindow::MainWindow(QString libraryRoot,
                       QWidget *parent,
                       const RemovalMode removalMode)
    : QMainWindow(parent)
    , m_libraryRoot(libraryRoot.trimmed().isEmpty()
                        ? QString()
                        : QFileInfo(libraryRoot).absoluteFilePath())
    , m_controller(new LibraryController(this))
    , m_removalMode(removalMode)
{
    buildUi();

    connect(m_controller, &LibraryController::refreshStarted, this, [this] {
        statusBar()->showMessage(QStringLiteral("Refreshing asset library..."));
    });
    connect(m_controller,
            &LibraryController::refreshFinished,
            this,
            [this](const QList<AssetRecord> &assets,
                   const QStringList &errors) {
                m_loaded = true;
                m_lastProblems.append(errors);
                m_lastProblems.removeDuplicates();
                m_problemAction->setEnabled(!m_lastProblems.isEmpty());
                rebuildGroups(assets);
                runSearch();
                QString status = QStringLiteral("%1 assets  |  %2")
                                     .arg(assets.size())
                                     .arg(QDir::toNativeSeparators(m_libraryRoot));
                if (!m_lastProblems.isEmpty()) {
                    status += QStringLiteral("  |  %1 problem(s); open More > Problems")
                                  .arg(m_lastProblems.size());
                }
                statusBar()->showMessage(status);
                if (!m_undoImportAssets.isEmpty()) {
                    QString message = QStringLiteral(
                                          "Added %1 ungrouped asset(s) to the library; showing All assets")
                                          .arg(m_undoImportAssets.size());
                    if (!errors.isEmpty()) {
                        message += QStringLiteral("; %1 item(s) were skipped")
                                       .arg(errors.size());
                    }
                    showNotice(message,
                               QStringLiteral("Undo"),
                               [this] { undoLastImport(); });
                } else if (!errors.isEmpty()) {
                    showPassiveReview(
                        QStringLiteral("%1 library problem(s) need review")
                            .arg(errors.size()));
                }
                if (m_pendingActivation) {
                    const ActivationRequest request = *m_pendingActivation;
                    m_pendingActivation.reset();
                    applyActivation(request);
                }
                if (!m_pendingVersionSelection.isEmpty()) {
                    for (int index = 0;
                         index < m_versionTree->topLevelItemCount();
                         ++index) {
                        QTreeWidgetItem *item = m_versionTree->topLevelItem(index);
                        if (item->data(0, VersionRole).toString()
                            == m_pendingVersionSelection) {
                            m_versionTree->setCurrentItem(item);
                            m_versionTree->scrollToItem(item);
                            m_detailTabs->setCurrentWidget(
                                m_versionTree->parentWidget());
                            break;
                        }
                    }
                    if (!m_pendingVersionMessage.isEmpty()) {
                        showNotice(m_pendingVersionMessage);
                    }
                    m_pendingVersionSelection.clear();
                    m_pendingVersionMessage.clear();
                }
            });
    connect(m_controller,
            &LibraryController::refreshFailed,
            this,
            [this](const QString &message) {
                if (!m_lastProblems.contains(message)) {
                    m_lastProblems.append(message);
                }
                m_problemAction->setEnabled(true);
                statusBar()->showMessage(message);
                showPassiveReview(message);
            });
    if (m_libraryRoot.isEmpty()) {
        setLibraryReady(false);
        statusBar()->showMessage(QStringLiteral("Choose an asset library to begin"));
    } else {
        m_controller->setLibraryRoot(m_libraryRoot);
        setLibraryReady(true);
        reportUnfinishedOperationPaths();
        m_controller->rebuild();
    }
}

MainWindow::~MainWindow()
{
    clearNotice();
    for (const auto &preview : m_versionPreviews) {
        if (preview) {
            setPreviewFilesWritable(preview->path(), true);
        }
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("xIPs - Asset Library"));
    resize(1180, 760);
    setMinimumSize(900, 600);

    auto *toolbar = addToolBar(QStringLiteral("Asset Library"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    auto *addButton = new QToolButton(toolbar);
    addButton->setObjectName(QStringLiteral("addButton"));
    addButton->setText(QStringLiteral("Add"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(addButton);
    m_addFilesAction = addMenu->addAction(QStringLiteral("Files..."));
    m_addFilesAction->setObjectName(QStringLiteral("addFilesAction"));
    connect(m_addFilesAction, &QAction::triggered, this, &MainWindow::addFiles);
    m_addFolderAction = addMenu->addAction(QStringLiteral("Folder..."));
    m_addFolderAction->setObjectName(QStringLiteral("addFolderAction"));
    connect(m_addFolderAction, &QAction::triggered, this, &MainWindow::addFolder);
    addButton->setMenu(addMenu);
    toolbar->addWidget(addButton);

    m_copyAction = toolbar->addAction(QStringLiteral("Copy working copy..."));
    m_copyAction->setObjectName(QStringLiteral("copyAction"));
    connect(m_copyAction,
            &QAction::triggered,
            this,
            &MainWindow::copyCurrentVersion);
    toolbar->addSeparator();
    m_searchEdit = new QLineEdit(toolbar);
    m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("Search assets, groups, or file names"));
    m_searchEdit->setMinimumWidth(340);
    toolbar->addWidget(m_searchEdit);
    m_searchScopeLabel = new QLabel(QStringLiteral("Scope: All assets"), toolbar);
    m_searchScopeLabel->setObjectName(QStringLiteral("searchScopeLabel"));
    m_searchScopeLabel->setMinimumWidth(110);
    toolbar->addWidget(m_searchScopeLabel);

    auto *moreButton = new QToolButton(toolbar);
    moreButton->setObjectName(QStringLiteral("moreButton"));
    moreButton->setText(QStringLiteral("More"));
    moreButton->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(moreButton);
    QAction *libraryAction = moreMenu->addAction(QStringLiteral("Choose library..."));
    connect(libraryAction, &QAction::triggered, this, &MainWindow::chooseLibrary);
    m_refreshAction = moreMenu->addAction(QStringLiteral("Refresh now"));
    m_refreshAction->setObjectName(QStringLiteral("refreshAction"));
    connect(m_refreshAction,
            &QAction::triggered,
            this,
            &MainWindow::refreshLibrary);
    moreMenu->addSeparator();
    m_updateAction = moreMenu->addAction(QStringLiteral("Update selected asset from..."));
    m_updateAction->setObjectName(QStringLiteral("updateAssetAction"));
    connect(m_updateAction,
            &QAction::triggered,
            this,
            &MainWindow::updateCurrentAsset);
    m_deleteAssetAction = moreMenu->addAction(QStringLiteral("Delete selected asset..."));
    m_deleteAssetAction->setObjectName(QStringLiteral("deleteAssetAction"));
    connect(m_deleteAssetAction,
            &QAction::triggered,
            this,
            &MainWindow::deleteCurrentAsset);
    moreMenu->addSeparator();
    m_problemAction = moreMenu->addAction(QStringLiteral("Problems..."));
    m_problemAction->setObjectName(QStringLiteral("problemAction"));
    m_problemAction->setEnabled(false);
    connect(m_problemAction, &QAction::triggered, this, &MainWindow::showProblems);
    moreButton->setMenu(moreMenu);
    toolbar->addWidget(moreButton);

    m_openAction = new QAction(QStringLiteral("Open"), this);
    m_openAction->setObjectName(QStringLiteral("openAction"));
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openCurrent);
    m_openMatchedFileAction = new QAction(
        QStringLiteral("Open matched file"),
        this);
    m_openMatchedFileAction->setObjectName(
        QStringLiteral("openMatchedFileAction"));
    connect(m_openMatchedFileAction,
            &QAction::triggered,
            this,
            &MainWindow::openMatchedFile);
    m_editAction = new QAction(QStringLiteral("Edit details"), this);
    m_editAction->setObjectName(QStringLiteral("editAction"));
    connect(m_editAction, &QAction::triggered, this, &MainWindow::editCurrentAsset);
    m_versionAction = new QAction(QStringLiteral("Save version"), this);
    m_versionAction->setObjectName(QStringLiteral("saveVersionAction"));
    connect(m_versionAction,
            &QAction::triggered,
            this,
            &MainWindow::createCurrentVersion);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        const bool empty = text.trimmed().isEmpty();
        if (m_searchWasEmpty && !empty && !currentGroup().isEmpty()
            && m_groupTree->topLevelItemCount() > 0) {
            m_groupTree->setCurrentItem(m_groupTree->topLevelItem(0));
        }
        m_searchWasEmpty = empty;
        m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::runSearch);
    m_tableModel = new AssetTableModel(this);
    m_proxyModel = new QSortFilterProxyModel(this);
    m_proxyModel->setSortRole(AssetTableModel::SortRole);
    m_proxyModel->setDynamicSortFilter(true);
    m_proxyModel->setSourceModel(m_tableModel);
    m_assetTable = new QTableView(this);
    m_assetTable->setObjectName(QStringLiteral("assetTable"));
    m_assetTable->setModel(m_proxyModel);
    m_assetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_assetTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_assetTable->setSortingEnabled(true);
    m_assetTable->sortByColumn(AssetTableModel::NameColumn, Qt::AscendingOrder);
    m_assetTable->setAlternatingRowColors(true);
    m_assetTable->setShowGrid(false);
    m_assetTable->setWordWrap(true);
    m_assetTable->verticalHeader()->hide();
    m_assetTable->verticalHeader()->setDefaultSectionSize(25);
    m_assetTable->horizontalHeader()->setStretchLastSection(true);
    m_assetTable->setColumnWidth(AssetTableModel::NameColumn, 240);
    m_assetTable->setColumnWidth(AssetTableModel::GroupsColumn, 130);
    m_assetTable->setColumnWidth(AssetTableModel::VersionColumn, 100);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    auto *groupPane = new QWidget(mainSplitter);
    auto *groupLayout = new QVBoxLayout(groupPane);
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->setSpacing(4);
    m_groupTree = new QTreeWidget(groupPane);
    m_groupTree->setObjectName(QStringLiteral("groupTree"));
    m_groupTree->setColumnCount(2);
    m_groupTree->setHeaderLabels(
        {QStringLiteral("Group"), QStringLiteral("Assets")});
    m_groupTree->setRootIsDecorated(false);
    m_groupTree->setAlternatingRowColors(true);
    m_groupTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupTree->setMinimumWidth(150);
    m_groupTree->setMaximumWidth(240);
    m_groupTree->setToolTip(
        QStringLiteral("Select a group to filter assets. Starting a new search uses all assets."));
    m_groupTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_groupTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    groupLayout->addWidget(m_groupTree, 1);
    auto *groupButton = new QToolButton(groupPane);
    groupButton->setObjectName(QStringLiteral("groupMenuButton"));
    groupButton->setText(QStringLiteral("Manage groups"));
    groupButton->setPopupMode(QToolButton::InstantPopup);
    auto *groupMenu = new QMenu(groupButton);
    QAction *assignGroupAction = groupMenu->addAction(
        QStringLiteral("Add selected assets to group..."));
    connect(assignGroupAction,
            &QAction::triggered,
            this,
            &MainWindow::assignNewGroup);
    QAction *removeSelectedGroupAction = groupMenu->addAction(
        QStringLiteral("Remove selected assets from current group"));
    connect(removeSelectedGroupAction,
            &QAction::triggered,
            this,
            &MainWindow::removeSelectedFromCurrentGroup);
    groupMenu->addSeparator();
    QAction *renameGroupAction = groupMenu->addAction(
        QStringLiteral("Rename current group..."));
    connect(renameGroupAction,
            &QAction::triggered,
            this,
            &MainWindow::renameCurrentGroup);
    QAction *removeGroupAction = groupMenu->addAction(
        QStringLiteral("Remove current group..."));
    connect(removeGroupAction,
            &QAction::triggered,
            this,
            &MainWindow::removeCurrentGroup);
    groupButton->setMenu(groupMenu);
    groupLayout->addWidget(groupButton);
    mainSplitter->addWidget(groupPane);
    auto *resultsPane = new QWidget(mainSplitter);
    auto *resultsLayout = new QVBoxLayout(resultsPane);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_resultsStack = new QStackedWidget(resultsPane);
    m_resultsStack->setObjectName(QStringLiteral("resultsStack"));
    m_emptyResultsLabel = new QLabel(resultsPane);
    m_emptyResultsLabel->setObjectName(QStringLiteral("emptyResultsLabel"));
    m_emptyResultsLabel->setAlignment(Qt::AlignCenter);
    m_emptyResultsLabel->setWordWrap(true);
    m_emptyResultsLabel->setText(QStringLiteral("No assets in this library"));
    m_resultsStack->addWidget(m_assetTable);
    m_resultsStack->addWidget(m_emptyResultsLabel);
    resultsLayout->addWidget(m_resultsStack);
    mainSplitter->addWidget(resultsPane);
    auto *details = new QWidget(mainSplitter);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(8, 4, 0, 0);
    detailsLayout->setSpacing(6);
    m_nameLabel = new QLabel(QStringLiteral("No asset selected"), details);
    QFont heading = m_nameLabel->font();
    heading.setBold(true);
    heading.setPointSize(heading.pointSize() + 2);
    m_nameLabel->setFont(heading);
    detailsLayout->addWidget(m_nameLabel);

    auto *detailActions = new QHBoxLayout;
    auto addDetailAction = [details, detailActions](QAction *action) {
        auto *button = new QToolButton(details);
        button->setDefaultAction(action);
        detailActions->addWidget(button);
        return button;
    };
    addDetailAction(m_openAction);
    m_openMatchedFileButton = addDetailAction(m_openMatchedFileAction);
    addDetailAction(m_editAction);
    addDetailAction(m_versionAction);
    detailActions->addStretch(1);
    detailsLayout->addLayout(detailActions);

    m_infoTree = new QTreeWidget(details);
    m_infoTree->setObjectName(QStringLiteral("infoTree"));
    m_infoTree->setColumnCount(2);
    m_infoTree->setHeaderLabels({QStringLiteral("Field"), QStringLiteral("Value")});
    m_infoTree->setRootIsDecorated(false);
    m_infoTree->setAlternatingRowColors(true);
    m_infoTree->setMaximumHeight(150);
    m_infoTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_infoTree->header()->setStretchLastSection(true);
    detailsLayout->addWidget(m_infoTree);

    m_description = new QPlainTextEdit(details);
    m_description->setObjectName(QStringLiteral("descriptionView"));
    m_description->setReadOnly(true);
    m_description->setPlaceholderText(QStringLiteral("No description"));
    m_description->setMaximumHeight(90);
    detailsLayout->addWidget(m_description);

    m_detailTabs = new QTabWidget(details);
    m_detailTabs->setObjectName(QStringLiteral("detailTabs"));
    m_detailTabs->setDocumentMode(true);
    auto *filesPage = new QWidget(m_detailTabs);
    auto *filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(0, 0, 0, 0);
    m_fileTree = new QTreeWidget(filesPage);
    m_fileTree->setObjectName(QStringLiteral("fileTree"));
    m_fileTree->setColumnCount(1);
    m_fileTree->setHeaderLabels({QStringLiteral("File")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->header()->setStretchLastSection(true);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    filesLayout->addWidget(m_fileTree);

    auto *versionsPage = new QWidget(m_detailTabs);
    auto *versionsLayout = new QVBoxLayout(versionsPage);
    versionsLayout->setContentsMargins(0, 0, 0, 0);
    m_versionTree = new QTreeWidget(versionsPage);
    m_versionTree->setObjectName(QStringLiteral("versionTree"));
    m_versionTree->setHeaderLabels(
        {QStringLiteral("Version"), QStringLiteral("Created")});
    m_versionTree->setRootIsDecorated(false);
    m_versionTree->setAlternatingRowColors(true);
    m_versionTree->setToolTip(
        QStringLiteral("Double-click or press Enter to open a temporary preview copy"));
    m_versionTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versionTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_versionTree->header()->setStretchLastSection(true);
    versionsLayout->addWidget(m_versionTree, 1);
    m_restoreVersionAction = new QAction(
        QStringLiteral("Restore to working copy..."), this);
    m_restoreVersionAction->setObjectName(
        QStringLiteral("restoreVersionAction"));
    connect(m_restoreVersionAction,
            &QAction::triggered,
            this,
            &MainWindow::restoreSelectedVersion);
    m_deleteVersionAction = new QAction(QStringLiteral("Delete saved version..."),
                                        this);
    m_deleteVersionAction->setObjectName(QStringLiteral("deleteVersionAction"));
    connect(m_deleteVersionAction,
            &QAction::triggered,
            this,
            &MainWindow::deleteSelectedVersion);
    auto *versionActions = new QHBoxLayout;
    versionActions->setContentsMargins(0, 0, 0, 0);
    auto *restoreVersionButton = new QToolButton(versionsPage);
    restoreVersionButton->setObjectName(QStringLiteral("restoreVersionButton"));
    restoreVersionButton->setDefaultAction(m_restoreVersionAction);
    versionActions->addWidget(restoreVersionButton);
    auto *deleteVersionButton = new QToolButton(versionsPage);
    deleteVersionButton->setObjectName(QStringLiteral("deleteVersionButton"));
    deleteVersionButton->setDefaultAction(m_deleteVersionAction);
    versionActions->addWidget(deleteVersionButton);
    versionActions->addStretch(1);
    versionsLayout->addLayout(versionActions);
    m_detailTabs->addTab(filesPage, QStringLiteral("Files"));
    m_detailTabs->addTab(versionsPage, QStringLiteral("Versions"));
    detailsLayout->addWidget(m_detailTabs, 1);
    mainSplitter->setSizes({180, 460, 540});
    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(1, false);
    mainSplitter->setCollapsible(2, false);
    m_libraryPage = mainSplitter;
    m_contentStack = new QStackedWidget(this);
    m_welcomePage = new QWidget(m_contentStack);
    m_welcomePage->setObjectName(QStringLiteral("welcomePage"));
    auto *welcomeLayout = new QVBoxLayout(m_welcomePage);
    welcomeLayout->setContentsMargins(80, 60, 80, 60);
    welcomeLayout->addStretch(1);
    auto *welcomeHeading = new QLabel(QStringLiteral("Choose your asset library"),
                                      m_welcomePage);
    QFont welcomeFont = welcomeHeading->font();
    welcomeFont.setBold(true);
    welcomeFont.setPointSize(welcomeFont.pointSize() + 4);
    welcomeHeading->setFont(welcomeFont);
    welcomeHeading->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(welcomeHeading);
    auto *welcomeText = new QLabel(
        QStringLiteral("Select an existing folder, including a Jianguoyun-synced folder. "
                       "Assets added later are copied into this folder; source files are left unchanged."),
        m_welcomePage);
    welcomeText->setWordWrap(true);
    welcomeText->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(welcomeText);
    auto *chooseButton = new QToolButton(m_welcomePage);
    chooseButton->setObjectName(QStringLiteral("chooseLibraryButton"));
    chooseButton->setText(QStringLiteral("Choose library folder..."));
    chooseButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(chooseButton, &QToolButton::clicked, this, &MainWindow::chooseLibrary);
    welcomeLayout->addWidget(chooseButton, 0, Qt::AlignHCenter);
    welcomeLayout->addStretch(1);
    m_contentStack->addWidget(m_welcomePage);
    m_contentStack->addWidget(m_libraryPage);
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    m_noticeFrame = new QFrame(central);
    m_noticeFrame->setObjectName(QStringLiteral("noticeBanner"));
    m_noticeFrame->setFrameShape(QFrame::StyledPanel);
    auto *noticeLayout = new QHBoxLayout(m_noticeFrame);
    noticeLayout->setContentsMargins(10, 5, 6, 5);
    m_noticeLabel = new QLabel(m_noticeFrame);
    m_noticeLabel->setObjectName(QStringLiteral("noticeLabel"));
    m_noticeLabel->setWordWrap(true);
    noticeLayout->addWidget(m_noticeLabel, 1);
    m_noticeActionButton = new QToolButton(m_noticeFrame);
    m_noticeActionButton->setObjectName(QStringLiteral("noticeActionButton"));
    noticeLayout->addWidget(m_noticeActionButton);
    m_noticeDismissButton = new QToolButton(m_noticeFrame);
    m_noticeDismissButton->setObjectName(
        QStringLiteral("dismissNoticeButton"));
    m_noticeDismissButton->setText(QStringLiteral("Close"));
    noticeLayout->addWidget(m_noticeDismissButton);
    connect(m_noticeActionButton, &QToolButton::clicked, this, [this] {
        std::function<void()> callback = std::move(m_noticeCallback);
        m_noticeDismissCallback = {};
        clearNotice();
        if (callback) {
            callback();
        }
    });
    connect(m_noticeDismissButton, &QToolButton::clicked, this, [this] {
        m_allowChangedLiveForNoticeDismiss = true;
        const bool cleared = clearNotice();
        m_allowChangedLiveForNoticeDismiss = false;
        if (cleared) {
            m_undoImportAssets.clear();
        }
    });
    m_noticeFrame->hide();
    centralLayout->addWidget(m_noticeFrame);
    centralLayout->addWidget(m_contentStack, 1);
    setCentralWidget(central);
    statusBar()->setSizeGripEnabled(true);

    connect(m_assetTable->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex &, const QModelIndex &) {
                const bool selected = currentRecord() != nullptr;
                m_editAction->setEnabled(selected);
                m_versionAction->setEnabled(selected);
                m_copyAction->setEnabled(selected);
                m_openAction->setEnabled(selected);
                m_updateAction->setEnabled(selected);
                m_deleteAssetAction->setEnabled(selected);
                updateDetails(currentRecord());
            });
    connect(m_assetTable, &QTableView::doubleClicked, this, [this] {
        if (currentMatchedFile().isEmpty()) {
            openCurrent();
        } else {
            openMatchedFile();
        }
    });
    connect(m_fileTree, &QTreeWidget::itemActivated,
            this, &MainWindow::openSelectedFile);
    connect(m_versionTree,
            &QTreeWidget::itemActivated,
            this,
            [this](QTreeWidgetItem *item, int) {
                if (item) {
                    openVersion(item->data(0, VersionRole).toString());
                }
            });
    connect(m_versionTree,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) {
                const QString version = selectedVersion();
                m_restoreVersionAction->setEnabled(!version.isEmpty());
                m_deleteVersionAction->setEnabled(!version.isEmpty());
                m_copyAction->setText(
                    version.isEmpty()
                        ? QStringLiteral("Copy working copy...")
                        : QStringLiteral("Copy version %1...").arg(version));
            });
    connect(m_groupTree,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) {
                runSearch();
            });
    auto *focusSearch = new QShortcut(QKeySequence::Find, this);
    connect(focusSearch, &QShortcut::activated, m_searchEdit, [this] {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });

    m_focusRefreshTimer = new QTimer(this);
    m_focusRefreshTimer->setSingleShot(true);
    m_focusRefreshTimer->setInterval(500);
    connect(m_focusRefreshTimer,
            &QTimer::timeout,
            this,
            &MainWindow::refreshLibrary);
    setAcceptDrops(true);

    m_editAction->setEnabled(false);
    m_versionAction->setEnabled(false);
    m_copyAction->setEnabled(false);
    m_openAction->setEnabled(false);
    m_openMatchedFileAction->setEnabled(false);
    m_openMatchedFileAction->setVisible(false);
    m_openMatchedFileButton->hide();
    m_updateAction->setEnabled(false);
    m_deleteAssetAction->setEnabled(false);
    m_restoreVersionAction->setEnabled(false);
    m_restoreVersionAction->setVisible(false);
    m_deleteVersionAction->setEnabled(false);
    m_deleteVersionAction->setVisible(false);
}

void MainWindow::runSearch()
{
    const AssetRecord *selected = currentRecord();
    const QString selectedId = selected ? selected->manifest.id : QString();
    const bool hasQuery = !m_searchEdit->text().trimmed().isEmpty();
    const QString query = m_searchEdit->text().trimmed();
    if (hasQuery) {
        m_assetTable->setSortingEnabled(false);
        m_proxyModel->sort(-1);
    } else {
        m_assetTable->setSortingEnabled(true);
    }
    QList<SearchHit> hits = m_controller->search(m_searchEdit->text());
    const QString group = currentGroup();
    if (!group.isEmpty()) {
        QList<SearchHit> grouped;
        grouped.reserve(hits.size());
        for (SearchHit &hit : hits) {
            if ((isUngroupedFilter(group)
                 && hit.asset.manifest.tags.isEmpty())
                || (!isUngroupedFilter(group)
                    && hasGroup(hit.asset, group))) {
                grouped.append(std::move(hit));
            }
        }
        hits.swap(grouped);
    }
    m_searchScopeLabel->setText(
        group.isEmpty()
            ? QStringLiteral("Scope: All assets")
            : QStringLiteral("Scope: %1").arg(displayedGroup(group)));
    const bool hasResults = !hits.isEmpty();
    m_tableModel->setHits(std::move(hits));
    m_assetTable->verticalHeader()->setDefaultSectionSize(hasQuery ? 44 : 25);
    if (hasQuery && hasResults) {
        m_assetTable->resizeRowsToContents();
    }
    if (hasResults) {
        m_resultsStack->setCurrentWidget(m_assetTable);
    } else {
        QString emptyText;
        if (hasQuery) {
            emptyText = group.isEmpty()
                            ? QStringLiteral("No assets match ‘%1’ in all assets")
                                  .arg(query)
                            : QStringLiteral("No assets match ‘%1’ in group %2")
                                  .arg(query, displayedGroup(group));
        } else if (!group.isEmpty()) {
            emptyText = QStringLiteral("No assets in group %1")
                            .arg(displayedGroup(group));
        } else {
            emptyText = QStringLiteral(
                "No assets in this library\nUse Add to copy a file or folder into it");
        }
        m_emptyResultsLabel->setText(emptyText);
        m_resultsStack->setCurrentWidget(m_emptyResultsLabel);
    }
    if (!hasQuery) {
        m_assetTable->sortByColumn(AssetTableModel::NameColumn, Qt::AscendingOrder);
    }
    if (selectedId.isEmpty() || !selectAssetById(selectedId)) {
        selectFirstRow();
    }
}

void MainWindow::rebuildGroups(const QList<AssetRecord> &assets)
{
    const QString previous = currentGroup();
    const QSignalBlocker blocker(m_groupTree);
    m_groupTree->clear();

    auto *all = new QTreeWidgetItem(
        m_groupTree,
        {QStringLiteral("All assets"), QString::number(assets.size())});
    all->setData(0, GroupRole, QString());
    all->setToolTip(0, QStringLiteral("Automatic filter; not a custom group"));
    all->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    QTreeWidgetItem *selection = all;

    const qsizetype ungroupedCount = std::count_if(
        assets.cbegin(), assets.cend(), [](const AssetRecord &asset) {
            return asset.manifest.tags.isEmpty();
        });
    if (ungroupedCount > 0) {
        auto *ungrouped = new QTreeWidgetItem(
            m_groupTree,
            {QStringLiteral("Ungrouped"), QString::number(ungroupedCount)});
        ungrouped->setData(0, GroupRole, ungroupedFilterKey());
        ungrouped->setToolTip(
            0,
            QStringLiteral("Automatic filter for assets with no groups"));
        ungrouped->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (isUngroupedFilter(previous)) {
            selection = ungrouped;
        }
    }

    QMap<QString, GroupSummary> groups;
    for (const AssetRecord &asset : assets) {
        QSet<QString> counted;
        for (const QString &rawTag : asset.manifest.tags) {
            const QString name = rawTag.trimmed();
            const QString key = name.toCaseFolded();
            if (name.isEmpty() || isReservedGroupName(name)
                || counted.contains(key)) {
                continue;
            }
            counted.insert(key);
            GroupSummary &summary = groups[key];
            if (summary.name.isEmpty()) {
                summary.name = name;
            }
            ++summary.count;
        }
    }

    for (auto iterator = groups.cbegin(); iterator != groups.cend(); ++iterator) {
        const GroupSummary &summary = iterator.value();
        auto *item = new QTreeWidgetItem(
            m_groupTree,
            {summary.name, QString::number(summary.count)});
        item->setData(0, GroupRole, summary.name);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (summary.name.compare(previous, Qt::CaseInsensitive) == 0) {
            selection = item;
        }
    }
    m_groupTree->setCurrentItem(selection);
}

void MainWindow::updateDetails(const AssetRecord *asset)
{
    const int generation = ++m_stateGeneration;
    m_workingStateItem = nullptr;
    m_infoTree->clear();
    m_description->clear();
    m_fileTree->clear();
    m_versionTree->clear();
    m_openMatchedFileAction->setData({});
    m_openMatchedFileAction->setEnabled(false);
    m_openMatchedFileAction->setVisible(false);
    m_openMatchedFileButton->hide();
    if (!asset) {
        m_nameLabel->setText(QStringLiteral("No asset selected"));
        m_versionAction->setEnabled(false);
        m_versionAction->setToolTip({});
        m_openAction->setText(QStringLiteral("Open"));
        m_copyAction->setText(QStringLiteral("Copy working copy..."));
        m_restoreVersionAction->setVisible(false);
        m_deleteVersionAction->setVisible(false);
        return;
    }

    m_nameLabel->setText(asset->manifest.name);
    m_versionAction->setEnabled(false);
    m_versionAction->setToolTip(
        QStringLiteral("Checking whether the working copy changed..."));
    m_openAction->setText(asset->files.size() == 1
                              ? QStringLiteral("Open file")
                              : QStringLiteral("Open folder"));
    const QString matchedFile = currentMatchedFile();
    if (!matchedFile.isEmpty() && asset->files.contains(matchedFile)) {
        const QString matchedPath = QDir(asset->assetRoot).absoluteFilePath(
            matchedFile);
        m_openMatchedFileAction->setData(matchedPath);
        m_openMatchedFileAction->setToolTip(
            QDir::toNativeSeparators(matchedFile));
        m_openMatchedFileAction->setEnabled(true);
        m_openMatchedFileAction->setVisible(true);
        m_openMatchedFileButton->show();
    }
    m_workingStateItem = new QTreeWidgetItem(
        m_infoTree,
        {QStringLiteral("Working copy"), QStringLiteral("Checking...")});
    addInfoRow(m_infoTree,
               QStringLiteral("Groups"),
               asset->manifest.tags.isEmpty()
                   ? QStringLiteral("None")
                   : asset->manifest.tags.join(QStringLiteral(", ")));
    addInfoRow(m_infoTree,
               QStringLiteral("Modified"),
               asset->lastModified.toLocalTime().toString(
                   QStringLiteral("yyyy-MM-dd HH:mm")));
    m_description->setPlainText(asset->manifest.description);
    populateFiles(*asset, matchedFile);
    populateVersions(*asset);

    const AssetRecord selectedAsset = *asset;
    auto *watcher = new QFutureWatcher<WorkingCopyState>(this);
    connect(watcher,
            &QFutureWatcher<WorkingCopyState>::finished,
            this,
            [this, watcher, generation, assetId = asset->manifest.id] {
                const WorkingCopyState state = watcher->result();
                watcher->deleteLater();
                const AssetRecord *selected = currentRecord();
                if (generation != m_stateGeneration || !m_workingStateItem
                    || !selected || selected->manifest.id != assetId) {
                    return;
                }
                if (!state.error.isEmpty()) {
                    m_workingStateItem->setText(
                        1,
                        QStringLiteral("Could not check: %1").arg(state.error));
                    m_versionAction->setEnabled(false);
                    m_versionAction->setToolTip(
                        QStringLiteral("Cannot save a version until the working copy can be checked"));
                } else if (!state.hasSavedVersion) {
                    m_workingStateItem->setText(1, QStringLiteral("Not saved yet"));
                    m_versionAction->setEnabled(true);
                    m_versionAction->setToolTip({});
                } else if (state.changed) {
                    m_workingStateItem->setText(
                        1,
                        QStringLiteral("Changed since latest saved version %1")
                            .arg(state.latestVersion));
                    m_versionAction->setEnabled(true);
                    m_versionAction->setToolTip({});
                } else {
                    m_workingStateItem->setText(
                        1,
                        QStringLiteral("Matches latest saved version %1")
                            .arg(state.latestVersion));
                    m_versionAction->setEnabled(false);
                    m_versionAction->setToolTip(
                        QStringLiteral("The working copy already matches version %1")
                            .arg(state.latestVersion));
                }
            });
    watcher->setFuture(QtConcurrent::run([selectedAsset] {
        return AssetLibraryService().workingCopyState(selectedAsset);
    }));
}

void MainWindow::populateFiles(const AssetRecord &asset,
                               const QString &matchedFile)
{
    QTreeWidgetItem *selection = nullptr;
    for (const QString &relative : asset.files) {
        const QString absolute = QDir(asset.assetRoot).absoluteFilePath(relative);
        auto *item = new QTreeWidgetItem(
            m_fileTree,
            {QDir::toNativeSeparators(relative)});
        item->setToolTip(0, absolute);
        item->setData(0, FilePathRole, absolute);
        if (relative == matchedFile) {
            selection = item;
        }
    }
    if (m_fileTree->topLevelItemCount() > 0) {
        m_fileTree->setCurrentItem(selection ? selection
                                             : m_fileTree->topLevelItem(0));
        if (selection) {
            m_fileTree->scrollToItem(selection);
        }
    }
}

void MainWindow::populateVersions(const AssetRecord &asset)
{
    const VersionInventoryResult inventory = m_libraryService.versionInventory(
        asset.assetRoot);
    QStringList problems = inventory.problems;
    if (!inventory.fatalError.isEmpty()) {
        problems.prepend(inventory.fatalError);
    }
    for (const QString &problem : problems) {
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    if (!problems.isEmpty()) {
        m_problemAction->setEnabled(true);
        showPassiveReview(
            inventory.fatalError.isEmpty()
                ? QStringLiteral("%1 saved version(s) are unavailable; healthy versions remain listed")
                      .arg(inventory.problems.size())
                : inventory.fatalError);
    }
    for (const VersionInfo &version : inventory.validVersions) {
        auto *item = new QTreeWidgetItem(
            m_versionTree,
            {version.version,
             version.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
        item->setData(0, VersionRole, version.version);
    }
    m_versionTree->setCurrentItem(nullptr);
    m_restoreVersionAction->setVisible(!inventory.validVersions.isEmpty());
    m_restoreVersionAction->setEnabled(false);
    m_deleteVersionAction->setVisible(!inventory.validVersions.isEmpty());
    m_deleteVersionAction->setEnabled(false);
    m_copyAction->setText(QStringLiteral("Copy working copy..."));
}

void MainWindow::selectFirstRow()
{
    if (m_proxyModel->rowCount() > 0) {
        m_assetTable->selectRow(0);
        m_assetTable->setCurrentIndex(m_proxyModel->index(0, 0));
    } else {
        m_assetTable->clearSelection();
        updateDetails(nullptr);
    }
}

bool MainWindow::selectAssetById(const QString &assetId)
{
    for (int sourceRow = 0; sourceRow < m_tableModel->rowCount(); ++sourceRow) {
        const AssetRecord *asset = m_tableModel->recordAt(sourceRow);
        if (!asset || asset->manifest.id.compare(assetId, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QModelIndex proxy = m_proxyModel->mapFromSource(
            m_tableModel->index(sourceRow, AssetTableModel::NameColumn));
        if (!proxy.isValid()) {
            return false;
        }
        m_assetTable->selectRow(proxy.row());
        m_assetTable->setCurrentIndex(proxy);
        m_assetTable->scrollTo(proxy);
        return true;
    }
    return false;
}

void MainWindow::chooseLibrary()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Choose asset library"),
        m_libraryRoot.isEmpty() ? QDir::homePath() : m_libraryRoot);
    if (selected.isEmpty()) {
        return;
    }
    const QStringList previousProblems = std::exchange(m_lastProblems, {});
    if (!clearNotice()) {
        m_lastProblems.append(previousProblems);
        m_lastProblems.removeDuplicates();
        m_problemAction->setEnabled(!m_lastProblems.isEmpty());
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; library was not changed"));
        return;
    }
    m_undoImportAssets.clear();
    m_libraryRoot = QFileInfo(selected).absoluteFilePath();
    m_controller->setLibraryRoot(m_libraryRoot);
    m_loaded = false;
    m_problemAction->setEnabled(!m_lastProblems.isEmpty());
    saveLibrarySetting();
    setLibraryReady(true);
    m_tableModel->setHits({});
    rebuildGroups({});
    updateDetails(nullptr);
    reportUnfinishedOperationPaths();
    m_controller->rebuild();
}

void MainWindow::addFolder()
{
    const QString source = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select asset folder to copy into the library"),
        QDir::homePath());
    if (source.isEmpty()) {
        return;
    }
    importPaths({source});
}

void MainWindow::addFiles()
{
    const QStringList sources = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("Select files to copy into the library"),
        QDir::homePath(),
        QStringLiteral("FPGA files (*.v *.vh *.sv *.svh *.vhd *.vhdl *.xdc *.sdc *.tcl *.qsf *.qip *.mif *.mem *.coe);;All files (*)"));
    if (sources.isEmpty()) {
        return;
    }
    importPaths(sources);
}

void MainWindow::importPaths(const QStringList &sourcePaths)
{
    if (m_libraryRoot.isEmpty() || sourcePaths.isEmpty()) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; import was not started"));
        return;
    }
    m_undoImportAssets.clear();
    const ImportBatchResult result = m_libraryService.importAssets(
        m_libraryRoot,
        sourcePaths);
    m_lastProblems.append(result.errors);
    m_lastProblems.removeDuplicates();
    m_problemAction->setEnabled(!m_lastProblems.isEmpty());
    if (result.created.isEmpty()) {
        QMessageBox::critical(
            this,
            QStringLiteral("Cannot add assets"),
            result.errors.isEmpty()
                ? QStringLiteral("No assets were added")
                : result.errors.join(u'\n'));
        return;
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = result.created.first().manifest.id,
    };
    m_undoImportAssets = result.created;
    m_undoImportProofs = result.createdProofs;
    const bool undoVerified = m_undoImportProofs.size()
                                  == m_undoImportAssets.size()
                              && std::all_of(
                                  m_undoImportProofs.cbegin(),
                                  m_undoImportProofs.cend(),
                                  [](const AssetDeletionProof &proof) {
                                      return proof.ok();
                                  });
    if (!undoVerified) {
        m_undoImportAssets.clear();
        m_undoImportProofs.clear();
        const QString problem = QStringLiteral(
            "Import completed, but a verified Undo proof could not be recorded; imported assets were kept");
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
    }
    if (m_groupTree->topLevelItemCount() > 0) {
        m_groupTree->setCurrentItem(m_groupTree->topLevelItem(0));
    }
    QString status = QStringLiteral(
                         "Added %1 ungrouped asset(s); showing All assets")
                         .arg(result.created.size());
    if (!result.errors.isEmpty()) {
        status += QStringLiteral("; %1 skipped; open More > Problems")
                      .arg(result.errors.size());
    }
    if (!undoVerified) {
        status += QStringLiteral("; Undo unavailable; open More > Problems");
        showPassiveReview(
            QStringLiteral("Import completed, but Undo is unavailable; review Problems"));
    }
    statusBar()->showMessage(status);
    m_controller->rebuild();
}

void MainWindow::undoLastImport()
{
    const QList<AssetRecord> imported = std::exchange(m_undoImportAssets, {});
    const QList<AssetDeletionProof> proofs = std::exchange(
        m_undoImportProofs, {});
    if (imported.isEmpty()) {
        return;
    }
    int removed = 0;
    QStringList errors;
    for (qsizetype index = 0; index < imported.size(); ++index) {
        const AssetRecord &asset = imported.at(index);
        QString error;
        if (index < proofs.size()
            && m_libraryService.deleteAsset(m_libraryRoot,
                                            asset,
                                            proofs.at(index),
                                            m_removalMode,
                                            nullptr,
                                            &error)) {
            ++removed;
        } else {
            if (index >= proofs.size()) {
                error = QStringLiteral(
                    "The import Undo proof is unavailable; the asset was kept");
            }
            errors.append(QStringLiteral("%1: %2")
                              .arg(asset.manifest.name, error));
        }
    }
    if (!errors.isEmpty()) {
        m_lastProblems.append(errors);
        m_lastProblems.removeDuplicates();
        m_problemAction->setEnabled(true);
    }
    if (!errors.isEmpty()) {
        showNotice(
            QStringLiteral("Removed %1 imported asset(s); %2 could not be removed")
                .arg(removed)
                .arg(errors.size()),
            QStringLiteral("Review"),
            [this] { showProblems(); });
    } else if (removed > 0) {
        showNotice(
            QStringLiteral("Removed %1 imported asset(s); source files were unchanged")
                .arg(removed));
    } else {
        showNotice(QStringLiteral("The import could not be undone"),
                   QStringLiteral("Review"),
                   [this] { showProblems(); });
    }
    m_controller->rebuild();
}

void MainWindow::editCurrentAsset()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    AssetMetadata metadata{
        .id = selectedAsset.manifest.id,
        .name = selectedAsset.manifest.name,
        .description = selectedAsset.manifest.description,
        .tags = selectedAsset.manifest.tags,
    };
    if (!editMetadata(this,
                      QStringLiteral("Edit details"),
                      groupNames(m_controller->assets()),
                      metadata)) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; details were not changed"));
        return;
    }
    m_undoImportAssets.clear();
    QString error;
    MetadataUpdateResult updated;
    if (!m_libraryService.updateMetadata(selectedAsset,
                                         metadata,
                                         &updated,
                                         &error)) {
        QString problem = QStringLiteral("Cannot update %1: %2")
                              .arg(selectedAsset.manifest.name, error);
        if (!updated.retainedPath.isEmpty()) {
            problem += QStringLiteral("; recovery data remains at %1")
                           .arg(QDir::toNativeSeparators(
                               updated.retainedPath));
        }
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        QMessageBox::critical(this, QStringLiteral("Cannot update asset"), error);
        m_controller->rebuild();
        return;
    }
    if (!updated.warning.isEmpty() || !updated.retainedPath.isEmpty()) {
        QString problem = QStringLiteral("%1: %2")
                              .arg(selectedAsset.manifest.name,
                                   updated.warning.isEmpty()
                                       ? QStringLiteral(
                                             "manifest recovery data remains at %1")
                                             .arg(QDir::toNativeSeparators(
                                                 updated.retainedPath))
                                       : updated.warning);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        showPassiveReview(
            QStringLiteral("Asset details were updated; manifest recovery data needs review"));
    } else {
        showNotice(updated.changed
                       ? QStringLiteral("Updated details for %1")
                             .arg(metadata.name)
                       : QStringLiteral("Asset details were unchanged"));
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = selectedAsset.manifest.id,
    };
    m_controller->rebuild();
}

void MainWindow::updateCurrentAsset()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("updateAssetDialog"));
    dialog.setWindowTitle(QStringLiteral("Update working copy"));
    dialog.resize(650, 260);
    auto *layout = new QVBoxLayout(&dialog);
    auto *explanation = new QLabel(
        QStringLiteral("Choose a file or folder that should replace the working copy. "
                       "Asset details and saved versions remain unchanged. "
                       "The completed update can be undone from its banner."),
        &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto *sourceRow = new QHBoxLayout;
    auto *source = new QLineEdit(&dialog);
    source->setObjectName(QStringLiteral("updateSourceEdit"));
    source->setPlaceholderText(QStringLiteral("Replacement file or folder"));
    sourceRow->addWidget(source, 1);
    auto *browseFile = new QToolButton(&dialog);
    browseFile->setObjectName(QStringLiteral("updateBrowseFileButton"));
    browseFile->setText(QStringLiteral("File..."));
    sourceRow->addWidget(browseFile);
    auto *browseFolder = new QToolButton(&dialog);
    browseFolder->setObjectName(QStringLiteral("updateBrowseFolderButton"));
    browseFolder->setText(QStringLiteral("Folder..."));
    sourceRow->addWidget(browseFolder);
    layout->addLayout(sourceRow);

    auto *preview = new QLabel(
        QStringLiteral("Choose a replacement source to preview the change."),
        &dialog);
    preview->setObjectName(QStringLiteral("updatePreviewLabel"));
    preview->setWordWrap(true);
    preview->setMinimumHeight(70);
    layout->addWidget(preview);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                            | QDialogButtonBox::Cancel,
                                        &dialog);
    buttons->setObjectName(QStringLiteral("updateDialogButtons"));
    QPushButton *updateButton = buttons->button(QDialogButtonBox::Ok);
    updateButton->setText(QStringLiteral("Update"));
    updateButton->setEnabled(false);
    layout->addWidget(buttons);

    const auto refreshPreview = [&] {
        const QString path = source->text().trimmed();
        if (path.isEmpty()) {
            preview->setText(
                QStringLiteral("Choose a replacement source to preview the change."));
            updateButton->setEnabled(false);
            return;
        }
        const UpdatePreview change = m_libraryService.previewUpdate(selectedAsset,
                                                                    path);
        if (!change.ok()) {
            preview->setText(QStringLiteral("Cannot use this source: %1")
                                 .arg(change.error));
            updateButton->setEnabled(false);
            return;
        }
        const bool changed = !change.addedFiles.isEmpty()
                             || !change.replacedFiles.isEmpty()
                             || !change.removedFiles.isEmpty();
        preview->setText(changed
                             ? updatePreviewText(change)
                             : QStringLiteral("No file changes were found."));
        updateButton->setEnabled(changed);
    };
    connect(source, &QLineEdit::textChanged, &dialog, refreshPreview);
    connect(browseFile, &QToolButton::clicked, &dialog, [&] {
        const QString selected = QFileDialog::getOpenFileName(
            &dialog,
            QStringLiteral("Choose replacement file"),
            QDir::homePath(),
            QStringLiteral("FPGA files (*.v *.vh *.sv *.svh *.vhd *.vhdl *.xdc *.sdc *.tcl *.qsf *.qip *.mif *.mem *.coe);;All files (*)"));
        if (!selected.isEmpty()) {
            source->setText(QFileInfo(selected).absoluteFilePath());
        }
    });
    connect(browseFolder, &QToolButton::clicked, &dialog, [&] {
        const QString selected = QFileDialog::getExistingDirectory(
            &dialog,
            QStringLiteral("Choose replacement folder"),
            QDir::homePath());
        if (!selected.isEmpty()) {
            source->setText(QFileInfo(selected).absoluteFilePath());
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the previous Undo backup; update was not started"));
        return;
    }
    m_undoImportAssets.clear();

    UpdateAssetResult result;
    QString error;
    if (!m_libraryService.updateAsset(selectedAsset,
                                      source->text().trimmed(),
                                      WorkingCopyRecoveryMode::RetainForUndo,
                                      &result,
                                      &error)) {
        const QString problem = QStringLiteral("Cannot update %1: %2")
                                    .arg(selectedAsset.manifest.name, error);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        QMessageBox::critical(this,
                              QStringLiteral("Cannot update asset"),
                              error);
        return;
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = selectedAsset.manifest.id,
    };
    const QString message = QStringLiteral(
                                "Updated %1: %2 added, %3 replaced, %4 removed")
                                .arg(selectedAsset.manifest.name)
                                .arg(result.preview.addedFiles.size())
                                .arg(result.preview.replacedFiles.size())
                                .arg(result.preview.removedFiles.size());
    offerWorkingCopyUndo(selectedAsset, result, message);
    m_controller->rebuild();
}

void MainWindow::deleteCurrentAsset()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    const VersionInventoryResult versionInventory =
        m_libraryService.versionInventory(selectedAsset.assetRoot);
    if (!versionInventory.fatalError.isEmpty()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot inspect asset"),
                              versionInventory.fatalError);
        return;
    }
    const qsizetype savedVersions = versionInventory.validVersions.size()
                                       + versionInventory.problems.size();
    const AssetDeletionProof deletionProof =
        m_libraryService.assetDeletionProof(selectedAsset);
    if (!deletionProof.ok()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot inspect asset"),
                              deletionProof.error);
        return;
    }
    QString deleteQuestion = QStringLiteral(
        "Move '%1' to the recycle bin?\n\n"
        "This removes %2 working file(s) and %3 saved version(s) "
        "from the library. Original import sources are not changed.")
                                 .arg(selectedAsset.manifest.name)
                                 .arg(selectedAsset.fileCount)
                                 .arg(savedVersions);
    if (!versionInventory.problems.isEmpty()) {
        deleteQuestion += QStringLiteral(
            "\n\n%1 saved version(s) could not be verified; they will remain inside the asset moved to the recycle bin.")
                              .arg(versionInventory.problems.size());
    }
    if (m_noticeDismissCallback) {
        deleteQuestion += QStringLiteral(
            "\n\nThe pending Undo for the last working-copy change will also be discarded.");
    }
    if (QMessageBox::question(this,
                              QStringLiteral("Delete asset"),
                              deleteQuestion) != QMessageBox::Yes) {
        return;
    }
    m_allowChangedLiveForNoticeDismiss = true;
    const bool noticeCleared = clearNotice();
    m_allowChangedLiveForNoticeDismiss = false;
    if (!noticeCleared) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; asset was not deleted"));
        return;
    }
    m_undoImportAssets.clear();
    QString error;
    if (!m_libraryService.deleteAsset(m_libraryRoot,
                                      selectedAsset,
                                      deletionProof,
                                      m_removalMode,
                                      nullptr,
                                      &error)) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot delete asset"),
                              error);
        return;
    }
    m_assetTable->clearSelection();
    updateDetails(nullptr);
    showNotice(QStringLiteral("Moved '%1' and its saved versions to the recycle bin")
                   .arg(selectedAsset.manifest.name));
    m_controller->rebuild();
}

void MainWindow::createCurrentVersion()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    QString currentVersion = selectedAsset.manifest.version;
    QString versionsError;
    const QList<VersionInfo> savedVersions = m_libraryService.versions(
        selectedAsset.assetRoot,
        &versionsError);
    if (versionsError.isEmpty() && !savedVersions.isEmpty()) {
        currentVersion = savedVersions.first().version;
    }
    bool accepted = false;
    const QString version = QInputDialog::getText(
        this,
        QStringLiteral("Save version"),
        QStringLiteral("Version name:"),
        QLineEdit::Normal,
        AssetLibraryService::suggestedNextVersion(currentVersion),
        &accepted).trimmed();
    if (!accepted || version.isEmpty()) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; version was not saved"));
        return;
    }
    m_undoImportAssets.clear();
    QString error;
    VersionInfo created;
    if (!m_libraryService.createVersion(selectedAsset, version, &created, &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot save version"), error);
        return;
    }
    const QString assetId = selectedAsset.manifest.id;
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = assetId,
    };
    const QString savedMessage = QStringLiteral("Saved immutable version %1")
                                     .arg(created.version);
    m_pendingVersionSelection = created.version;
    m_pendingVersionMessage = created.warning.isEmpty()
                                  ? savedMessage
                                  : QString();
    statusBar()->showMessage(savedMessage);
    if (!created.warning.isEmpty()) {
        const QString problem = QStringLiteral("%1: %2")
                                    .arg(selectedAsset.manifest.name,
                                         created.warning);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        showPassiveReview(savedMessage
                          + QStringLiteral("; open Problems to review the version-marker warning"));
    }
    m_controller->rebuild();
}

void MainWindow::copyCurrentVersion()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    QString versionsError;
    const QList<VersionInfo> savedVersions = m_libraryService.versions(
        selectedAsset.assetRoot,
        &versionsError);
    if (!versionsError.isEmpty()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot inspect versions"),
                              versionsError);
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("copyDialog"));
    dialog.setWindowTitle(QStringLiteral("Copy asset files"));
    dialog.resize(650, 280);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *version = new QComboBox(&dialog);
    version->setObjectName(QStringLiteral("copyVersionCombo"));
    version->addItem(QStringLiteral("Working copy"), QString());
    for (const VersionInfo &saved : savedVersions) {
        version->addItem(QStringLiteral("Saved version %1").arg(saved.version),
                         saved.version);
    }
    const QString requestedVersion = selectedVersion();
    const int requestedIndex = version->findData(requestedVersion);
    if (requestedIndex >= 0) {
        version->setCurrentIndex(requestedIndex);
    }
    form->addRow(QStringLiteral("Source"), version);

    auto *destinationRow = new QWidget(&dialog);
    auto *destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    auto *destination = new QLineEdit(QDir::homePath(), destinationRow);
    destination->setObjectName(QStringLiteral("copyDestinationEdit"));
    destinationLayout->addWidget(destination, 1);
    auto *browse = new QToolButton(destinationRow);
    browse->setObjectName(QStringLiteral("copyBrowseButton"));
    browse->setText(QStringLiteral("Browse..."));
    destinationLayout->addWidget(browse);
    form->addRow(QStringLiteral("Destination folder"), destinationRow);

    auto *name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("copyNameEdit"));
    form->addRow(QStringLiteral("Final name"), name);
    layout->addLayout(form);
    auto *finalPath = new QLabel(&dialog);
    finalPath->setObjectName(QStringLiteral("copyFinalPathLabel"));
    finalPath->setWordWrap(true);
    finalPath->setMinimumHeight(60);
    layout->addWidget(finalPath);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                            | QDialogButtonBox::Cancel,
                                        &dialog);
    buttons->setObjectName(QStringLiteral("copyDialogButtons"));
    QPushButton *copyButton = buttons->button(QDialogButtonBox::Ok);
    copyButton->setText(QStringLiteral("Copy"));
    layout->addWidget(buttons);

    const auto refreshCopy = [&] {
        const QString versionName = version->currentData().toString();
        const CopyPlan plan = m_libraryService.copyPlan(selectedAsset,
                                                        versionName);
        const QString folder = destination->text().trimmed();
        const QString outputName = name->text().trimmed();
        if (!plan.ok()) {
            finalPath->setText(QStringLiteral("Cannot copy this source: %1")
                                   .arg(plan.error));
            copyButton->setEnabled(false);
            return;
        }
        if (!QFileInfo(folder).isDir()) {
            finalPath->setText(QStringLiteral("Choose an existing destination folder."));
            copyButton->setEnabled(false);
            return;
        }
        if (outputName.isEmpty() || outputName == QStringLiteral(".")
            || outputName == QStringLiteral("..")
            || outputName.contains(u'/') || outputName.contains(u'\\')) {
            finalPath->setText(QStringLiteral("Enter one file or folder name."));
            copyButton->setEnabled(false);
            return;
        }
        const QString target = QDir(folder).absoluteFilePath(outputName);
        const QString sourceText = versionName.isEmpty()
                                       ? QStringLiteral("working copy")
                                       : QStringLiteral("saved version %1")
                                             .arg(versionName);
        if (QFileInfo::exists(target)) {
            finalPath->setText(
                QStringLiteral("Source: %1\nAlready exists: %2")
                    .arg(sourceText, QDir::toNativeSeparators(target)));
            copyButton->setEnabled(false);
            return;
        }
        finalPath->setText(QStringLiteral("Source: %1\nFinal path: %2")
                               .arg(sourceText,
                                    QDir::toNativeSeparators(target)));
        copyButton->setEnabled(true);
    };
    const auto resetSuggestedName = [&] {
        const CopyPlan plan = m_libraryService.copyPlan(
            selectedAsset,
            version->currentData().toString());
        name->setText(plan.ok() ? plan.suggestedName : QString());
        refreshCopy();
    };
    connect(version,
            &QComboBox::currentIndexChanged,
            &dialog,
            resetSuggestedName);
    connect(destination, &QLineEdit::textChanged, &dialog, refreshCopy);
    connect(name, &QLineEdit::textChanged, &dialog, refreshCopy);
    connect(browse, &QToolButton::clicked, &dialog, [&] {
        const QString selected = QFileDialog::getExistingDirectory(
            &dialog,
            QStringLiteral("Choose copy destination"),
            destination->text().trimmed().isEmpty()
                ? QDir::homePath()
                : destination->text().trimmed());
        if (!selected.isEmpty()) {
            destination->setText(QFileInfo(selected).absoluteFilePath());
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    resetSuggestedName();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const bool preserveWorkingCopyUndo =
        static_cast<bool>(m_noticeDismissCallback);
    if (!preserveWorkingCopyUndo) {
        if (!clearNotice()) {
            statusBar()->showMessage(
                QStringLiteral("Cannot retire the current Undo backup; copy was not started"));
            return;
        }
        m_undoImportAssets.clear();
    }

    const QString versionName = version->currentData().toString();
    const QString targetPath = QDir(destination->text().trimmed())
                                   .absoluteFilePath(name->text().trimmed());
    QString error;
    QString copiedPath;
    if (!m_libraryService.copyVersionPayload(
            selectedAsset, versionName, targetPath, &copiedPath, &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot copy asset"), error);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Copied to %1")
            .arg(QDir::toNativeSeparators(copiedPath)));
    const QString destinationToOpen = QFileInfo(copiedPath).isDir()
                                          ? copiedPath
                                          : QFileInfo(copiedPath).absolutePath();
    if (!preserveWorkingCopyUndo) {
        showNotice(QStringLiteral("Copied %1 to %2")
                       .arg(versionName.isEmpty()
                                ? QStringLiteral("working copy")
                                : QStringLiteral("version %1").arg(versionName),
                            QDir::toNativeSeparators(copiedPath)),
                   QStringLiteral("Open destination"),
                   [this, destinationToOpen] {
                       if (!QDesktopServices::openUrl(
                               QUrl::fromLocalFile(destinationToOpen))) {
                           QMessageBox::warning(
                               this,
                               QStringLiteral("Cannot open destination"),
                               QDir::toNativeSeparators(destinationToOpen));
                       }
                   });
    }
}

void MainWindow::deleteSelectedVersion()
{
    const AssetRecord *asset = currentRecord();
    const QString version = selectedVersion();
    if (!asset || version.isEmpty()) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    if (QMessageBox::question(
            this,
            QStringLiteral("Delete saved version"),
            QStringLiteral("Move saved version %1 to the recycle bin?\n"
                           "The working copy will not be changed.")
                .arg(version)) != QMessageBox::Yes) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; version was not deleted"));
        return;
    }
    m_undoImportAssets.clear();
    QString error;
    DeleteVersionResult deleted;
    if (!m_libraryService.deleteVersion(selectedAsset,
                                        version,
                                        m_removalMode,
                                        &deleted,
                                        &error)) {
        for (const QString &path : deleted.retainedPaths) {
            const QString problem = QStringLiteral(
                "%1: saved-version recovery data remains at %2")
                                        .arg(selectedAsset.manifest.name,
                                             QDir::toNativeSeparators(path));
            if (!m_lastProblems.contains(problem)) {
                m_lastProblems.append(problem);
            }
        }
        if (!deleted.warning.isEmpty()) {
            const QString problem = QStringLiteral("%1: %2")
                                        .arg(selectedAsset.manifest.name,
                                             deleted.warning);
            if (!m_lastProblems.contains(problem)) {
                m_lastProblems.append(problem);
            }
        }
        if (!deleted.retainedPaths.isEmpty()
            || !deleted.warning.isEmpty()) {
            m_problemAction->setEnabled(true);
        }
        QMessageBox::critical(this,
                              QStringLiteral("Cannot delete saved version"),
                              error);
        return;
    }
    for (const QString &path : deleted.retainedPaths) {
        const QString problem = QStringLiteral(
            "%1: saved-version transaction data remains at %2")
                                    .arg(selectedAsset.manifest.name,
                                         QDir::toNativeSeparators(path));
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    if (!deleted.warning.isEmpty()) {
        const QString problem = QStringLiteral("%1: %2")
                                    .arg(selectedAsset.manifest.name,
                                         deleted.warning);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    if (!deleted.retainedPaths.isEmpty() || !deleted.warning.isEmpty()) {
        m_problemAction->setEnabled(true);
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = selectedAsset.manifest.id,
    };
    const QString deletedMessage =
        m_removalMode == RemovalMode::MoveToTrash
            ? QStringLiteral("Moved saved version %1 to the recycle bin")
                  .arg(version)
            : QStringLiteral("Deleted saved version %1").arg(version);
    if (!deleted.retainedPaths.isEmpty() || !deleted.warning.isEmpty()) {
        showPassiveReview(
            deletedMessage
            + QStringLiteral("; open Problems to review transaction data"));
    } else {
        showNotice(deletedMessage);
    }
    m_controller->rebuild();
}

void MainWindow::restoreSelectedVersion()
{
    const AssetRecord *asset = currentRecord();
    const QString version = selectedVersion();
    if (!asset || version.isEmpty()) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    const UpdatePreview preview = m_libraryService.previewRestore(
        selectedAsset, version);
    if (!preview.ok()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot inspect saved version"),
                              preview.error);
        return;
    }
    if (preview.addedFiles.isEmpty()
        && preview.replacedFiles.isEmpty()
        && preview.removedFiles.isEmpty()) {
        const QString message = QStringLiteral(
            "Working copy already matches version %1")
                                    .arg(version);
        statusBar()->showMessage(message);
        showNotice(message);
        return;
    }

    const QString recoveryText = QStringLiteral(
        "The current working copy will be kept temporarily so this action can be undone.");
    QString removedFiles;
    if (!preview.removedFiles.isEmpty()) {
        QStringList examples = preview.removedFiles.mid(0, 4);
        for (QString &path : examples) {
            path = QDir::toNativeSeparators(path);
        }
        removedFiles = QStringLiteral("\nWill remove: %1")
                           .arg(examples.join(QStringLiteral(", ")));
        if (preview.removedFiles.size() > examples.size()) {
            removedFiles += QStringLiteral(" and %1 more")
                                .arg(preview.removedFiles.size()
                                     - examples.size());
        }
    }
    const QString question = QStringLiteral(
        "Restore saved version %1 to the working copy?\n\n"
        "Files added: %2\nFiles replaced: %3\nFiles removed: %4%5\n\n"
        "%6\nSaved versions will not be changed.")
                                 .arg(version)
                                 .arg(preview.addedFiles.size())
                                 .arg(preview.replacedFiles.size())
                                 .arg(preview.removedFiles.size())
                                 .arg(removedFiles,
                                      recoveryText);
    if (QMessageBox::question(this,
                              QStringLiteral("Restore saved version"),
                              question) != QMessageBox::Yes) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the previous Undo backup; restore was not started"));
        return;
    }
    m_undoImportAssets.clear();

    UpdateAssetResult restored;
    QString error;
    if (!m_libraryService.restoreVersion(selectedAsset,
                                         version,
                                         WorkingCopyRecoveryMode::RetainForUndo,
                                         &restored,
                                         &error)) {
        const QString problem = QStringLiteral("Cannot restore %1: %2")
                                    .arg(selectedAsset.manifest.name, error);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        QMessageBox::critical(this,
                              QStringLiteral("Cannot restore saved version"),
                              error);
        return;
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = selectedAsset.manifest.id,
    };
    QString message = QStringLiteral(
        "Restored version %1 to the working copy; saved versions were kept")
                                .arg(version);
    if (!restored.publishedAsIntended || !restored.warning.isEmpty()) {
        message += QStringLiteral("; review Problems before using the result");
    }
    statusBar()->showMessage(message);
    offerWorkingCopyUndo(selectedAsset, restored, message);
    m_controller->rebuild();
}

QString MainWindow::openTarget(const AssetRecord &asset)
{
    return asset.files.size() == 1
               ? QDir(asset.assetRoot).absoluteFilePath(asset.files.first())
               : asset.assetRoot;
}

void MainWindow::openCurrent()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const QString target = openTarget(*asset);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open asset"),
                             QDir::toNativeSeparators(target));
    }
}

void MainWindow::openVersion(const QString &version)
{
    const AssetRecord *asset = currentRecord();
    if (!asset || version.isEmpty()) {
        return;
    }
    const AssetRecord selectedAsset = *asset;
    const CopyPlan plan = m_libraryService.copyPlan(selectedAsset, version);
    if (!plan.ok()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot open saved version"),
                              plan.error);
        return;
    }
    auto preview = std::make_unique<QTemporaryDir>(
        QDir(QDir::tempPath()).absoluteFilePath(
            QStringLiteral("xips-version-preview-XXXXXX")));
    if (!preview->isValid()) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot open saved version"),
                              QStringLiteral("Cannot create a temporary preview directory"));
        return;
    }
    const QString destination = QDir(preview->path()).absoluteFilePath(
        plan.suggestedName);
    QString target;
    QString copyError;
    if (!m_libraryService.copyVersionPayload(selectedAsset,
                                              version,
                                              destination,
                                              &target,
                                              &copyError)) {
        QMessageBox::critical(this,
                              QStringLiteral("Cannot open saved version"),
                              copyError);
        return;
    }
    const QFileInfo targetInfo(target);
    if ((!plan.isSingleFile() && !targetInfo.isDir())
        || (plan.isSingleFile() && !targetInfo.isFile())) {
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open saved version"),
                             QDir::toNativeSeparators(target));
        return;
    }
    setPreviewFilesWritable(preview->path(), false);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        setPreviewFilesWritable(preview->path(), true);
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open saved version"),
                             QDir::toNativeSeparators(target));
        return;
    }
    m_versionPreviews.push_back(std::move(preview));
    statusBar()->showMessage(
        QStringLiteral("Opened a temporary preview of version %1; edits do not change the saved version")
            .arg(version),
        10000);
}

void MainWindow::openMatchedFile()
{
    const QString target = m_openMatchedFileAction->data().toString();
    if (!QFileInfo(target).isFile()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Matched file is unavailable"),
            QStringLiteral("The matched file no longer exists. Refresh the library and search again."));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open matched file"),
                             QDir::toNativeSeparators(target));
    }
}

void MainWindow::openSelectedFile()
{
    const QTreeWidgetItem *item = m_fileTree->currentItem();
    const QString path = item ? item->data(0, FilePathRole).toString() : QString();
    if (path.isEmpty()) {
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open file"),
                             QDir::toNativeSeparators(path));
    }
}

void MainWindow::assignNewGroup()
{
    const QList<AssetRecord> assets = selectedRecords();
    if (assets.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Select one or more assets first"));
        return;
    }
    bool accepted = false;
    const QString group = QInputDialog::getText(
        this,
        QStringLiteral("Add to group"),
        QStringLiteral("Group name:"),
        QLineEdit::Normal,
        isUngroupedFilter(currentGroup()) ? QString() : currentGroup(),
        &accepted).trimmed();
    if (!accepted || group.isEmpty()) {
        return;
    }
    if (isReservedGroupName(group)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Reserved group name"),
            QStringLiteral("'All assets' and 'Ungrouped' are automatic filters. Choose another group name."));
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; group was not changed"));
        return;
    }
    m_undoImportAssets.clear();
    GroupChangeResult changed;
    QString error;
    const bool complete = m_libraryService.changeGroupMembership(
        assets, QString(), group, &changed, &error);
    const int problemsBefore = m_lastProblems.size();
    recordGroupChangeProblems(changed);
    if (!complete) {
        const QString detail = changed.updated > 0
                                   ? QStringLiteral("%1\n\n%2 asset(s) were updated before the remaining conflicts.")
                                         .arg(error)
                                         .arg(changed.updated)
                                   : error;
        QMessageBox::critical(this,
                              QStringLiteral("Cannot update all groups"),
                              detail);
        m_controller->rebuild();
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Added %1 asset(s) to %2")
            .arg(changed.updated)
            .arg(group));
    if (m_lastProblems.size() > problemsBefore) {
        showPassiveReview(
            QStringLiteral("Groups were updated; manifest recovery data needs review"));
    }
    m_controller->rebuild();
}

void MainWindow::removeSelectedFromCurrentGroup()
{
    const QString group = currentGroup();
    const QList<AssetRecord> assets = selectedRecords();
    if (group.isEmpty() || isUngroupedFilter(group)) {
        statusBar()->showMessage(QStringLiteral("Select a named group first"));
        return;
    }
    if (assets.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Select one or more assets first"));
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; groups were not changed"));
        return;
    }
    m_undoImportAssets.clear();
    GroupChangeResult changed;
    QString error;
    const bool complete = m_libraryService.changeGroupMembership(
        assets, group, QString(), &changed, &error);
    const int problemsBefore = m_lastProblems.size();
    recordGroupChangeProblems(changed);
    if (!complete) {
        const QString detail = changed.updated > 0
                                   ? QStringLiteral("%1\n\n%2 asset(s) were updated before the remaining conflicts.")
                                         .arg(error)
                                         .arg(changed.updated)
                                   : error;
        QMessageBox::critical(this,
                              QStringLiteral("Cannot update all groups"),
                              detail);
        m_controller->rebuild();
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Removed %1 selected asset(s) from %2")
            .arg(changed.updated)
            .arg(group));
    if (m_lastProblems.size() > problemsBefore) {
        showPassiveReview(
            QStringLiteral("Groups were updated; manifest recovery data needs review"));
    }
    m_controller->rebuild();
}

void MainWindow::renameCurrentGroup()
{
    const QString oldGroup = currentGroup();
    if (oldGroup.isEmpty() || isUngroupedFilter(oldGroup)) {
        statusBar()->showMessage(QStringLiteral("Select a named group first"));
        return;
    }
    bool accepted = false;
    const QString newGroup = QInputDialog::getText(
        this,
        QStringLiteral("Rename group"),
        QStringLiteral("New group name:"),
        QLineEdit::Normal,
        oldGroup,
        &accepted).trimmed();
    if (!accepted || newGroup.isEmpty() || newGroup == oldGroup) {
        return;
    }
    if (isReservedGroupName(newGroup)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Reserved group name"),
            QStringLiteral("'All assets' and 'Ungrouped' are automatic filters. Choose another group name."));
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; group was not renamed"));
        return;
    }
    m_undoImportAssets.clear();
    GroupChangeResult changed;
    QString error;
    const bool complete = m_libraryService.changeGroupMembership(
        m_controller->assets(), oldGroup, newGroup, &changed, &error);
    const int problemsBefore = m_lastProblems.size();
    recordGroupChangeProblems(changed);
    if (!complete) {
        const QString detail = changed.updated > 0
                                   ? QStringLiteral("%1\n\n%2 asset(s) were updated before the remaining conflicts.")
                                         .arg(error)
                                         .arg(changed.updated)
                                   : error;
        QMessageBox::critical(this,
                              QStringLiteral("Cannot rename group everywhere"),
                              detail);
        m_controller->rebuild();
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Renamed group for %1 asset(s)")
            .arg(changed.updated));
    if (m_lastProblems.size() > problemsBefore) {
        showPassiveReview(
            QStringLiteral("Groups were updated; manifest recovery data needs review"));
    }
    m_controller->rebuild();
}

void MainWindow::removeCurrentGroup()
{
    const QString group = currentGroup();
    if (group.isEmpty() || isUngroupedFilter(group)) {
        statusBar()->showMessage(QStringLiteral("Select a named group first"));
        return;
    }
    if (QMessageBox::question(
            this,
            QStringLiteral("Remove group"),
            QStringLiteral("Remove group '%1' from every asset?\n"
                           "No asset files will be deleted.")
                .arg(group)) != QMessageBox::Yes) {
        return;
    }
    if (!clearNotice()) {
        statusBar()->showMessage(
            QStringLiteral("Cannot retire the current Undo backup; group was not removed"));
        return;
    }
    m_undoImportAssets.clear();
    GroupChangeResult changed;
    QString error;
    const bool complete = m_libraryService.changeGroupMembership(
        m_controller->assets(), group, QString(), &changed, &error);
    const int problemsBefore = m_lastProblems.size();
    recordGroupChangeProblems(changed);
    if (!complete) {
        const QString detail = changed.updated > 0
                                   ? QStringLiteral("%1\n\n%2 asset(s) were updated before the remaining conflicts.")
                                         .arg(error)
                                         .arg(changed.updated)
                                   : error;
        QMessageBox::critical(this,
                              QStringLiteral("Cannot remove group everywhere"),
                              detail);
        m_controller->rebuild();
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Removed group from %1 asset(s)")
            .arg(changed.updated));
    if (m_lastProblems.size() > problemsBefore) {
        showPassiveReview(
            QStringLiteral("Groups were updated; manifest recovery data needs review"));
    }
    m_controller->rebuild();
}

void MainWindow::showProblems()
{
    if (m_lastProblems.isEmpty()) {
        return;
    }
    QMessageBox::information(this,
                             QStringLiteral("Asset library problems"),
                             m_lastProblems.join(u'\n'));
    m_lastProblems.clear();
    m_problemAction->setEnabled(false);
}

void MainWindow::refreshLibrary()
{
    if (m_libraryRoot.isEmpty()) {
        return;
    }
    reportUnfinishedOperationPaths();
    m_controller->rebuild();
}

void MainWindow::reportUnfinishedOperationPaths()
{
    const int problemsBefore = m_lastProblems.size();
    const ManifestTransactionRecoveryResult recovery =
        m_libraryService.recoverManifestTransactions(m_libraryRoot);
    if (!recovery.fatalError.isEmpty()) {
        const QString problem = QStringLiteral(
            "Manifest recovery could not inspect the library: %1")
                                    .arg(recovery.fatalError);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }

    QSet<QString> reportedRecoveryPaths;
    for (const ManifestTransactionRecoveryItem &item : recovery.items) {
        const QFileInfo transaction(item.transactionPath);
        const bool remains =
            item.outcome == ManifestTransactionRecoveryOutcome::Retained
            || transaction.exists() || transaction.isSymLink();
        if (!remains) {
            continue;
        }
        const QString path = QFileInfo(item.transactionPath).absoluteFilePath();
        reportedRecoveryPaths.insert(path);
        const QString problem = QStringLiteral(
            "Interrupted manifest update needs review at %1: %2")
                                    .arg(QDir::toNativeSeparators(path),
                                         item.message);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }

    const QStringList paths = AssetScanner::unfinishedOperationPaths(
        m_libraryRoot);
    for (const QString &path : paths) {
        if (reportedRecoveryPaths.contains(
                QFileInfo(path).absoluteFilePath())) {
            continue;
        }
        const QString problem = QStringLiteral(
            "Unfinished xIPs operation data remains at %1")
                                    .arg(QDir::toNativeSeparators(path));
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }

    const int recovered = recovery.restoredOriginal
                          + recovery.cleanedExpected
                          + recovery.cleanedReplacement;
    const int newProblems = m_lastProblems.size() - problemsBefore;
    if (newProblems <= 0) {
        if (recovered > 0) {
            const QString message = QStringLiteral(
                "Resolved %1 interrupted manifest update(s); verified asset metadata was preserved")
                                        .arg(recovered);
            if (hasPendingUndo()) {
                statusBar()->showMessage(message, 10000);
            } else {
                showNotice(message);
            }
        }
        return;
    }
    m_problemAction->setEnabled(true);
    const QString message = QStringLiteral(
        "%1 unfinished xIPs operation item(s) need review")
                                .arg(newProblems);
    if (hasPendingUndo()) {
        statusBar()->showMessage(message, 10000);
    } else {
        showPassiveReview(message);
    }
}

void MainWindow::recordGroupChangeProblems(const GroupChangeResult &result)
{
    for (const GroupChangeItemResult &item : result.items) {
        if (item.outcome != GroupChangeOutcome::Conflict
            && item.outcome != GroupChangeOutcome::Failed
            && item.warning.isEmpty() && item.retainedPath.isEmpty()) {
            continue;
        }
        QString detail = !item.message.isEmpty() ? item.message : item.warning;
        if (detail.isEmpty()) {
            detail = QStringLiteral("Group metadata could not be updated safely");
        }
        if (!item.retainedPath.isEmpty()
            && !detail.contains(item.retainedPath, Qt::CaseInsensitive)) {
            detail += QStringLiteral("; recovery data remains at %1")
                          .arg(QDir::toNativeSeparators(item.retainedPath));
        }
        const QString problem = QStringLiteral("%1: %2")
                                    .arg(item.assetName.isEmpty()
                                             ? item.assetId
                                             : item.assetName,
                                         detail);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    if (!m_lastProblems.isEmpty()) {
        m_problemAction->setEnabled(true);
    }
}

void MainWindow::recordUpdateProblems(const AssetRecord &asset,
                                      const UpdateAssetResult &result)
{
    if (!result.warning.isEmpty()) {
        const QString problem = QStringLiteral("%1: %2")
                                    .arg(asset.manifest.name, result.warning);
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    for (const QString &path : result.retainedPaths) {
        const QString problem = QStringLiteral(
            "%1: recovery data remains at %2")
                                    .arg(asset.manifest.name,
                                         QDir::toNativeSeparators(path));
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
    }
    if (!result.warning.isEmpty() || !result.retainedPaths.isEmpty()) {
        m_problemAction->setEnabled(true);
    }
}

void MainWindow::offerWorkingCopyUndo(const AssetRecord &asset,
                                      const UpdateAssetResult &result,
                                      const QString &message)
{
    recordUpdateProblems(asset, result);
    if (!result.undoToken.isValid()) {
        if (!result.warning.isEmpty() || !result.retainedPaths.isEmpty()) {
            showNotice(message + QStringLiteral("; Undo is unavailable"),
                       QStringLiteral("Review"),
                       [this] { showProblems(); });
        } else {
            showNotice(message);
        }
        return;
    }
    const WorkingCopyUndoToken token = result.undoToken;
    showNotice(
        message,
        QStringLiteral("Undo"),
        [this, asset, token] {
            undoWorkingCopyChange(asset, token);
        },
        [this, asset, token] {
            return discardWorkingCopyRecovery(asset, token);
        });
}

void MainWindow::undoWorkingCopyChange(const AssetRecord &asset,
                                       const WorkingCopyUndoToken &token)
{
    UpdateAssetResult undone;
    QString error;
    if (!m_libraryService.undoWorkingCopyChange(asset,
                                                token,
                                                &undone,
                                                &error,
                                                m_removalMode)) {
        recordUpdateProblems(asset, undone);
        const QString problem = QStringLiteral(
            "Cannot undo %1: %2. Recovery remains at %3")
                                    .arg(asset.manifest.name,
                                         error,
                                         QDir::toNativeSeparators(
                                             token.recoveryPath));
        if (!m_lastProblems.contains(problem)) {
            m_lastProblems.append(problem);
        }
        m_problemAction->setEnabled(true);
        showNotice(
            QStringLiteral(
                "Undo was not applied; current files and the recovery backup were kept. Open More > Problems for its path."),
            {},
            {},
            [this, asset, token] {
                return discardWorkingCopyRecovery(asset, token);
            });
        return;
    }
    recordUpdateProblems(asset, undone);
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = asset.manifest.id,
    };
    const QString message = QStringLiteral(
        "Restored the previous working copy for %1")
                                .arg(asset.manifest.name);
    statusBar()->showMessage(message);
    showNotice(message);
    m_controller->rebuild();
}

bool MainWindow::discardWorkingCopyRecovery(
    const AssetRecord &asset,
    const WorkingCopyUndoToken &token)
{
    RecoveryDiscardResult discarded;
    QString error;
    if (m_libraryService.discardWorkingCopyRecovery(asset,
                                                    token,
                                                    m_removalMode,
                                                    &discarded,
                                                    &error,
                                                    m_allowChangedLiveForNoticeDismiss
                                                        ? WorkingCopyDiscardPolicy::AllowVerifiedCurrentCopy
                                                        : WorkingCopyDiscardPolicy::RequirePublishedCopy)) {
        if (!discarded.warning.isEmpty()
            || !discarded.retainedPath.isEmpty()) {
            const QString detail = !discarded.warning.isEmpty()
                                       ? discarded.warning
                                       : QStringLiteral(
                                             "Recovery data remains at %1")
                                             .arg(QDir::toNativeSeparators(
                                                 discarded.retainedPath));
            const QString problem = QStringLiteral("%1: %2")
                                        .arg(asset.manifest.name,
                                             detail);
            if (!m_lastProblems.contains(problem)) {
                m_lastProblems.append(problem);
            }
            m_problemAction->setEnabled(true);
        }
        return true;
    }
    const QString problem = QStringLiteral(
        "Cannot discard recovery for %1: %2. Recovery remains at %3")
                                .arg(asset.manifest.name,
                                     error,
                                     QDir::toNativeSeparators(
                                         token.recoveryPath));
    if (!m_lastProblems.contains(problem)) {
        m_lastProblems.append(problem);
    }
    m_problemAction->setEnabled(true);
    return false;
}

void MainWindow::showNotice(const QString &message,
                            const QString &actionText,
                            std::function<void()> action,
                            std::function<bool()> dismiss)
{
    if (!clearNotice()) {
        return;
    }
    m_noticeLabel->setText(message);
    m_noticeCallback = std::move(action);
    m_noticeDismissCallback = std::move(dismiss);
    m_noticeDismissButton->setText(
        m_noticeDismissCallback ? QStringLiteral("Discard Undo")
                                : QStringLiteral("Close"));
    const bool hasAction = !actionText.isEmpty()
                           && static_cast<bool>(m_noticeCallback);
    m_noticeActionButton->setText(actionText);
    m_noticeActionButton->setVisible(hasAction);
    m_noticeFrame->show();
}

void MainWindow::showPassiveReview(const QString &message)
{
    if (hasPendingUndo()) {
        return;
    }
    showNotice(message,
               QStringLiteral("Review"),
               [this] { showProblems(); });
}

bool MainWindow::hasPendingUndo() const
{
    return !m_undoImportAssets.isEmpty()
           || static_cast<bool>(m_noticeDismissCallback);
}

bool MainWindow::clearNotice()
{
    if (m_noticeDismissCallback && !m_noticeDismissCallback()) {
        return false;
    }
    m_noticeDismissCallback = {};
    m_noticeCallback = {};
    m_noticeDismissButton->setText(QStringLiteral("Close"));
    m_noticeFrame->hide();
    return true;
}

void MainWindow::setLibraryReady(const bool ready)
{
    m_contentStack->setCurrentWidget(ready ? m_libraryPage : m_welcomePage);
    m_addFilesAction->setEnabled(ready);
    m_addFolderAction->setEnabled(ready);
    m_refreshAction->setEnabled(ready);
    m_searchEdit->setEnabled(ready);
    if (!ready) {
        m_editAction->setEnabled(false);
        m_versionAction->setEnabled(false);
        m_copyAction->setEnabled(false);
        m_openAction->setEnabled(false);
        m_openMatchedFileAction->setEnabled(false);
        m_openMatchedFileAction->setVisible(false);
        m_openMatchedFileButton->hide();
        m_updateAction->setEnabled(false);
        m_deleteAssetAction->setEnabled(false);
        m_restoreVersionAction->setEnabled(false);
        m_deleteVersionAction->setEnabled(false);
    }
}

bool MainWindow::event(QEvent *event)
{
    const bool handled = QMainWindow::event(event);
    if (event->type() == QEvent::WindowActivate && m_loaded
        && !m_libraryRoot.isEmpty()) {
        m_focusRefreshTimer->start();
    }
    return handled;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_libraryRoot.isEmpty() || !event->mimeData()->hasUrls()) {
        return;
    }
    const QList<QUrl> urls = event->mimeData()->urls();
    const bool hasLocalPath = std::any_of(
        urls.cbegin(),
        urls.cend(),
        [](const QUrl &url) { return url.isLocalFile(); });
    if (hasLocalPath) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        importPaths(paths);
        event->acceptProposedAction();
    }
}

void MainWindow::saveLibrarySetting()
{
    QSettings().setValue(QStringLiteral("library/root"), m_libraryRoot);
}

const AssetRecord *MainWindow::currentRecord() const
{
    const QModelIndex proxy = m_assetTable->currentIndex();
    if (!proxy.isValid()) {
        return nullptr;
    }
    const QModelIndex source = m_proxyModel->mapToSource(proxy);
    return m_tableModel->recordAt(source.row());
}

QList<AssetRecord> MainWindow::selectedRecords() const
{
    QList<AssetRecord> records;
    const QModelIndexList selected = m_assetTable->selectionModel()->selectedRows(
        AssetTableModel::NameColumn);
    for (const QModelIndex &proxy : selected) {
        const QModelIndex source = m_proxyModel->mapToSource(proxy);
        const AssetRecord *record = m_tableModel->recordAt(source.row());
        if (record) {
            records.append(*record);
        }
    }
    if (records.isEmpty()) {
        const AssetRecord *current = currentRecord();
        if (current) {
            records.append(*current);
        }
    }
    return records;
}

QString MainWindow::currentMatchedFile() const
{
    const QModelIndex current = m_assetTable->currentIndex();
    if (!current.isValid()) {
        return {};
    }
    return current.siblingAtColumn(AssetTableModel::NameColumn)
        .data(AssetTableModel::MatchedFileRole)
        .toString();
}

QString MainWindow::currentGroup() const
{
    const QTreeWidgetItem *item = m_groupTree ? m_groupTree->currentItem() : nullptr;
    return item ? item->data(0, GroupRole).toString() : QString();
}

QString MainWindow::selectedVersion() const
{
    const QTreeWidgetItem *item = m_versionTree->currentItem();
    return item ? item->data(0, VersionRole).toString() : QString();
}

void MainWindow::applyActivation(const ActivationRequest &request)
{
    if (!request.isValid()) {
        return;
    }
    if (m_groupTree && m_groupTree->topLevelItemCount() > 0) {
        m_groupTree->setCurrentItem(m_groupTree->topLevelItem(0));
    }
    if (request.action == ActivationAction::Search) {
        m_searchEdit->setText(request.value);
        runSearch();
    } else if (request.action == ActivationAction::OpenAsset) {
        if (!m_loaded) {
            m_pendingActivation = request;
            return;
        }
        m_searchEdit->clear();
        runSearch();
        if (!selectAssetById(request.value)) {
            statusBar()->showMessage(
                QStringLiteral("Asset not found: %1").arg(request.value));
        }
    }
    if (isMinimized()) {
        showNormal();
    }
    show();
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        raise();
        activateWindow();
    }
}

} // namespace xips
