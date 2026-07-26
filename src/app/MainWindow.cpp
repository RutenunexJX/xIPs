#include "app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace xips {
namespace {

constexpr int VersionRole = Qt::UserRole;

QString formatBytes(const qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KiB").arg(
            static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MiB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

void addInfoRow(QTreeWidget *tree, const QString &field, const QString &value)
{
    new QTreeWidgetItem(tree, {field, value.isEmpty() ? QStringLiteral("-") : value});
}

bool editMetadata(QWidget *parent,
                  const QString &title,
                  IpMetadata &metadata,
                  const bool idEditable)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("metadataDialog"));
    dialog.setWindowTitle(title);
    dialog.resize(520, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *id = new QLineEdit(metadata.id, &dialog);
    id->setObjectName(QStringLiteral("ipIdEdit"));
    id->setEnabled(idEditable);
    auto *name = new QLineEdit(metadata.name, &dialog);
    name->setObjectName(QStringLiteral("ipNameEdit"));
    auto *tags = new QLineEdit(metadata.tags.join(QStringLiteral(", ")), &dialog);
    tags->setPlaceholderText(QStringLiteral("comma-separated"));
    auto *description = new QPlainTextEdit(metadata.description, &dialog);
    description->setPlaceholderText(QStringLiteral("What this IP provides"));
    description->setMaximumBlockCount(100);

    form->addRow(QStringLiteral("ID"), id);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Tags"), tags);
    form->addRow(QStringLiteral("Description"), description);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                            | QDialogButtonBox::Cancel,
                                        &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (id->text().trimmed().isEmpty() || name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Incomplete metadata"),
                                 QStringLiteral("ID and name are required."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    metadata.id = id->text().trimmed();
    metadata.name = name->text().trimmed();
    metadata.description = description->toPlainText().trimmed();
    QString tagText = tags->text();
    tagText.replace(u';', u',');
    metadata.tags = tagText.split(u',', Qt::SkipEmptyParts);
    for (QString &tag : metadata.tags) {
        tag = tag.trimmed();
    }
    return true;
}

} // namespace

MainWindow::MainWindow(QString libraryRoot, QWidget *parent)
    : QMainWindow(parent)
    , m_libraryRoot(QFileInfo(libraryRoot).absoluteFilePath())
    , m_controller(new LibraryController(this))
{
    buildUi();
    m_controller->setLibraryRoot(m_libraryRoot);

    connect(m_controller, &LibraryController::refreshStarted, this, [this] {
        statusBar()->showMessage(QStringLiteral("Refreshing IP library..."));
    });
    connect(m_controller,
            &LibraryController::refreshFinished,
            this,
            [this](const QList<AssetRecord> &assets,
                   const QStringList &errors) {
                m_loaded = true;
                runSearch();
                QString status = QStringLiteral("%1 IPs  |  %2")
                                     .arg(assets.size())
                                     .arg(QDir::toNativeSeparators(m_libraryRoot));
                if (!errors.isEmpty()) {
                    status += QStringLiteral("  |  %1 skipped: %2")
                                  .arg(errors.size())
                                  .arg(errors.first());
                }
                statusBar()->showMessage(status);
                if (m_pendingActivation) {
                    const ActivationRequest request = *m_pendingActivation;
                    m_pendingActivation.reset();
                    applyActivation(request);
                }
            });
    connect(m_controller,
            &LibraryController::refreshFailed,
            this,
            [this](const QString &message) {
                statusBar()->showMessage(message);
            });
    m_controller->rebuild();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("xIPs - IP Library"));
    resize(1180, 760);
    setMinimumSize(900, 600);

    auto *toolbar = addToolBar(QStringLiteral("IP Library"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    QAction *libraryAction = toolbar->addAction(QStringLiteral("Library..."));
    connect(libraryAction,
            &QAction::triggered,
            this,
            &MainWindow::chooseLibrary);
    toolbar->addSeparator();
    QAction *addAction = toolbar->addAction(QStringLiteral("Add IP"));
    connect(addAction, &QAction::triggered, this, &MainWindow::addIp);
    m_editAction = toolbar->addAction(QStringLiteral("Edit"));
    connect(m_editAction, &QAction::triggered, this, &MainWindow::editCurrentIp);
    m_versionAction = toolbar->addAction(QStringLiteral("Save version"));
    connect(m_versionAction,
            &QAction::triggered,
            this,
            &MainWindow::createCurrentVersion);
    m_exportAction = toolbar->addAction(QStringLiteral("Export"));
    connect(m_exportAction,
            &QAction::triggered,
            this,
            &MainWindow::exportCurrentVersion);
    m_openFolderAction = toolbar->addAction(QStringLiteral("Open folder"));
    connect(m_openFolderAction,
            &QAction::triggered,
            this,
            &MainWindow::openCurrentFolder);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral("Search"), toolbar));
    m_searchEdit = new QLineEdit(toolbar);
    m_searchEdit->setObjectName(QStringLiteral("searchEdit"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("name, ID, version, tag, or description"));
    m_searchEdit->setMinimumWidth(280);
    toolbar->addWidget(m_searchEdit);
    QAction *refreshAction = toolbar->addAction(QStringLiteral("Refresh"));
    connect(refreshAction, &QAction::triggered, m_controller, &LibraryController::rebuild);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] {
        m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::runSearch);
    m_tableModel = new AssetTableModel(this);
    m_proxyModel = new QSortFilterProxyModel(this);
    m_proxyModel->setSortRole(Qt::UserRole);
    m_proxyModel->setDynamicSortFilter(true);
    m_proxyModel->setSourceModel(m_tableModel);
    m_assetTable = new QTableView(this);
    m_assetTable->setObjectName(QStringLiteral("assetTable"));
    m_assetTable->setModel(m_proxyModel);
    m_assetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_assetTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_assetTable->setSortingEnabled(true);
    m_assetTable->sortByColumn(AssetTableModel::NameColumn, Qt::AscendingOrder);
    m_assetTable->setAlternatingRowColors(true);
    m_assetTable->setShowGrid(false);
    m_assetTable->setWordWrap(false);
    m_assetTable->verticalHeader()->hide();
    m_assetTable->verticalHeader()->setDefaultSectionSize(25);
    m_assetTable->horizontalHeader()->setStretchLastSection(true);
    m_assetTable->setColumnWidth(AssetTableModel::NameColumn, 190);
    m_assetTable->setColumnWidth(AssetTableModel::VersionColumn, 90);
    m_assetTable->setColumnWidth(AssetTableModel::TagsColumn, 170);
    m_assetTable->setColumnWidth(AssetTableModel::FilesColumn, 55);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->addWidget(m_assetTable);
    auto *details = new QWidget(mainSplitter);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(8, 4, 0, 0);
    detailsLayout->setSpacing(6);
    m_nameLabel = new QLabel(QStringLiteral("No IP selected"), details);
    QFont heading = m_nameLabel->font();
    heading.setBold(true);
    heading.setPointSize(heading.pointSize() + 2);
    m_nameLabel->setFont(heading);
    detailsLayout->addWidget(m_nameLabel);

    m_infoTree = new QTreeWidget(details);
    m_infoTree->setObjectName(QStringLiteral("infoTree"));
    m_infoTree->setColumnCount(2);
    m_infoTree->setHeaderLabels({QStringLiteral("Field"), QStringLiteral("Value")});
    m_infoTree->setRootIsDecorated(false);
    m_infoTree->setAlternatingRowColors(true);
    m_infoTree->setMaximumHeight(190);
    m_infoTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_infoTree->header()->setStretchLastSection(true);
    detailsLayout->addWidget(m_infoTree);

    m_description = new QPlainTextEdit(details);
    m_description->setObjectName(QStringLiteral("descriptionView"));
    m_description->setReadOnly(true);
    m_description->setPlaceholderText(QStringLiteral("No description"));
    m_description->setMaximumHeight(90);
    detailsLayout->addWidget(m_description);

    auto *tabs = new QTabWidget(details);
    tabs->setObjectName(QStringLiteral("detailTabs"));
    tabs->setDocumentMode(true);
    auto *filesPage = new QWidget(tabs);
    auto *filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(0, 0, 0, 0);
    m_fileTree = new QTreeWidget(filesPage);
    m_fileTree->setObjectName(QStringLiteral("fileTree"));
    m_fileTree->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Size")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->header()->setStretchLastSection(false);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    filesLayout->addWidget(m_fileTree);

    m_versionTree = new QTreeWidget(tabs);
    m_versionTree->setObjectName(QStringLiteral("versionTree"));
    m_versionTree->setHeaderLabels(
        {QStringLiteral("Version"), QStringLiteral("Created")});
    m_versionTree->setRootIsDecorated(false);
    m_versionTree->setAlternatingRowColors(true);
    m_versionTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versionTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_versionTree->header()->setStretchLastSection(true);
    tabs->addTab(filesPage, QStringLiteral("Files"));
    tabs->addTab(m_versionTree, QStringLiteral("Versions"));
    detailsLayout->addWidget(tabs, 1);
    mainSplitter->setSizes({560, 620});
    mainSplitter->setCollapsible(0, false);
    mainSplitter->setCollapsible(1, false);
    setCentralWidget(mainSplitter);
    statusBar()->setSizeGripEnabled(true);

    connect(m_assetTable->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex &, const QModelIndex &) {
                const bool selected = currentRecord() != nullptr;
                m_editAction->setEnabled(selected);
                m_versionAction->setEnabled(selected);
                m_exportAction->setEnabled(selected);
                m_openFolderAction->setEnabled(selected);
                updateDetails(currentRecord());
            });
    connect(m_assetTable, &QTableView::doubleClicked,
            this, &MainWindow::openCurrentFolder);
    auto *focusSearch = new QShortcut(QKeySequence::Find, this);
    connect(focusSearch, &QShortcut::activated, m_searchEdit, [this] {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });

    m_editAction->setEnabled(false);
    m_versionAction->setEnabled(false);
    m_exportAction->setEnabled(false);
    m_openFolderAction->setEnabled(false);
}

void MainWindow::runSearch()
{
    const AssetRecord *selected = currentRecord();
    const QString selectedId = selected ? selected->manifest.id : QString();
    const bool searching = !m_searchEdit->text().trimmed().isEmpty();
    if (searching) {
        m_assetTable->setSortingEnabled(false);
        m_proxyModel->sort(-1);
    } else {
        m_assetTable->setSortingEnabled(true);
    }
    m_tableModel->setHits(m_controller->search(m_searchEdit->text()));
    if (!searching) {
        m_assetTable->sortByColumn(AssetTableModel::NameColumn, Qt::AscendingOrder);
    }
    if (selectedId.isEmpty() || !selectAssetById(selectedId)) {
        selectFirstRow();
    }
}

void MainWindow::updateDetails(const AssetRecord *asset)
{
    m_infoTree->clear();
    m_description->clear();
    m_fileTree->clear();
    m_versionTree->clear();
    if (!asset) {
        m_nameLabel->setText(QStringLiteral("No IP selected"));
        return;
    }

    m_nameLabel->setText(asset->manifest.name);
    addInfoRow(m_infoTree, QStringLiteral("ID"), asset->manifest.id);
    addInfoRow(m_infoTree,
               QStringLiteral("Last saved version"),
               asset->manifest.version.isEmpty() ? QStringLiteral("None")
                                                 : asset->manifest.version);
    addInfoRow(m_infoTree,
               QStringLiteral("Tags"),
               asset->manifest.tags.join(QStringLiteral(", ")));
    addInfoRow(m_infoTree,
               QStringLiteral("Files"),
               QStringLiteral("%1 (%2)")
                   .arg(asset->fileCount)
                   .arg(formatBytes(asset->totalBytes)));
    addInfoRow(m_infoTree,
               QStringLiteral("Modified"),
               asset->lastModified.toLocalTime().toString(
                   QStringLiteral("yyyy-MM-dd HH:mm")));
    addInfoRow(m_infoTree,
               QStringLiteral("Path"),
               QDir::toNativeSeparators(asset->assetRoot));
    m_description->setPlainText(asset->manifest.description);
    populateFiles(*asset);
    populateVersions(*asset);
}

void MainWindow::populateFiles(const AssetRecord &asset)
{
    for (const QString &relative : asset.files) {
        const QString absolute = QDir(asset.assetRoot).absoluteFilePath(relative);
        const QFileInfo info(absolute);
        auto *item = new QTreeWidgetItem(
            m_fileTree,
            {QDir::toNativeSeparators(relative), formatBytes(info.size())});
        item->setToolTip(0, absolute);
    }
    if (m_fileTree->topLevelItemCount() > 0) {
        m_fileTree->setCurrentItem(m_fileTree->topLevelItem(0));
    }
}

void MainWindow::populateVersions(const AssetRecord &asset)
{
    auto *working = new QTreeWidgetItem(
        m_versionTree,
        {QStringLiteral("Working copy"),
         asset.lastModified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
    working->setData(0, VersionRole, QString());

    QString error;
    const QList<VersionInfo> versions = m_libraryService.versions(asset.assetRoot, &error);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
    }
    for (const VersionInfo &version : versions) {
        auto *item = new QTreeWidgetItem(
            m_versionTree,
            {version.version,
             version.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
        item->setData(0, VersionRole, version.version);
    }
    m_versionTree->setCurrentItem(working);
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
        QStringLiteral("Choose IP library"),
        m_libraryRoot);
    if (selected.isEmpty()) {
        return;
    }
    m_libraryRoot = QFileInfo(selected).absoluteFilePath();
    m_controller->setLibraryRoot(m_libraryRoot);
    m_loaded = false;
    saveLibrarySetting();
    m_tableModel->setHits({});
    updateDetails(nullptr);
    m_controller->rebuild();
}

void MainWindow::addIp()
{
    const QString source = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select IP directory to copy into the library"),
        QDir::homePath());
    if (source.isEmpty()) {
        return;
    }
    IpMetadata metadata = AssetLibraryService::suggestedMetadata(source);
    if (!editMetadata(this, QStringLiteral("Add IP"), metadata, true)) {
        return;
    }

    QString error;
    AssetRecord created;
    if (!m_libraryService.importIp(
            ImportIpRequest{
                .libraryRoot = m_libraryRoot,
                .sourceDirectory = source,
                .metadata = metadata,
            },
            &created,
            &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot add IP"), error);
        return;
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = created.manifest.id.isEmpty() ? metadata.id : created.manifest.id,
    };
    statusBar()->showMessage(
        QStringLiteral("Added %1 to the IP library").arg(metadata.name));
    m_controller->rebuild();
}

void MainWindow::editCurrentIp()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    IpMetadata metadata{
        .id = asset->manifest.id,
        .name = asset->manifest.name,
        .description = asset->manifest.description,
        .tags = asset->manifest.tags,
    };
    if (!editMetadata(this, QStringLiteral("Edit IP metadata"), metadata, false)) {
        return;
    }
    QString error;
    if (!m_libraryService.updateMetadata(*asset, metadata, &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot update IP"), error);
        return;
    }
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = asset->manifest.id,
    };
    m_controller->rebuild();
}

void MainWindow::createCurrentVersion()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    bool accepted = false;
    const QString version = QInputDialog::getText(
        this,
        QStringLiteral("Save version"),
        QStringLiteral("Version name:"),
        QLineEdit::Normal,
        QString(),
        &accepted).trimmed();
    if (!accepted || version.isEmpty()) {
        return;
    }
    QString error;
    VersionInfo created;
    if (!m_libraryService.createVersion(*asset, version, &created, &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot save version"), error);
        return;
    }
    const QString assetId = asset->manifest.id;
    m_pendingActivation = ActivationRequest{
        .action = ActivationAction::OpenAsset,
        .value = assetId,
    };
    statusBar()->showMessage(
        QStringLiteral("Saved immutable version %1").arg(created.version));
    m_controller->rebuild();
}

void MainWindow::exportCurrentVersion()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const QString version = selectedVersion();
    const QString parent = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Choose export destination"),
        QDir::homePath());
    if (parent.isEmpty()) {
        return;
    }
    const QString suffix = version.isEmpty() ? QStringLiteral("working") : version;
    const QString destination = QDir(parent).absoluteFilePath(
        asset->manifest.id + u'-' + suffix);
    QString error;
    if (!m_libraryService.exportVersion(*asset, version, destination, &error)) {
        QMessageBox::critical(this, QStringLiteral("Cannot export IP"), error);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Exported to %1").arg(QDir::toNativeSeparators(destination)));
}

void MainWindow::openCurrentFolder()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(asset->assetRoot))) {
        QMessageBox::warning(this,
                             QStringLiteral("Cannot open folder"),
                             QDir::toNativeSeparators(asset->assetRoot));
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
    if (request.action == ActivationAction::Search) {
        m_searchEdit->setText(request.value);
        runSearch();
    } else if (request.action == ActivationAction::OpenAsset) {
        if (!m_loaded) {
            m_pendingActivation = request;
            return;
        }
        m_searchEdit->setText(request.value);
        runSearch();
        if (!selectAssetById(request.value)) {
            statusBar()->showMessage(
                QStringLiteral("IP not found: %1").arg(request.value));
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
