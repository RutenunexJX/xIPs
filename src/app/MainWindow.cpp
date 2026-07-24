#include "app/MainWindow.h"

#include "assetindex/AssetScanner.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace xips {
namespace {

constexpr int FilePathRole = Qt::UserRole;
constexpr int VersionRole = Qt::UserRole;

bool looksBinary(const QByteArray &contents)
{
    if (contents.contains('\0')) {
        return true;
    }
    qsizetype controls = 0;
    for (const char byte : contents) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x20 && value != '\t' && value != '\n'
            && value != '\r' && value != '\f') {
            ++controls;
        }
    }
    return !contents.isEmpty() && controls * 100 > contents.size();
}

QString readPreview(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Cannot open %1\n%2").arg(path, file.errorString());
    }
    constexpr qint64 limit = 512 * 1024;
    const QByteArray contents = file.read(limit + 1);
    const QByteArray preview = contents.first(
        std::min(contents.size(), static_cast<qsizetype>(limit)));
    if (looksBinary(preview)) {
        return QStringLiteral("[Binary preview unavailable]\n\nFile: %1\nSize: %2 bytes")
            .arg(path)
            .arg(file.size());
    }
    QString text = QString::fromUtf8(preview);
    if (contents.size() > limit) {
        text += QStringLiteral("\n\n[Preview truncated at 512 KiB]");
    }
    return text;
}

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
    auto *version = new QLineEdit(metadata.version, &dialog);
    version->setPlaceholderText(QStringLiteral("optional, for example 1.0.0"));
    auto *tags = new QLineEdit(metadata.tags.join(QStringLiteral(", ")), &dialog);
    tags->setPlaceholderText(QStringLiteral("comma-separated"));
    auto *description = new QPlainTextEdit(metadata.description, &dialog);
    description->setPlaceholderText(QStringLiteral("What this IP provides"));
    description->setMaximumBlockCount(100);

    form->addRow(QStringLiteral("ID"), id);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Current version"), version);
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
    metadata.version = version->text().trimmed();
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
    buildMenus();
    m_controller->setLibraryRoot(m_libraryRoot);

    connect(m_controller, &LibraryController::indexingStarted, this, [this] {
        statusBar()->showMessage(QStringLiteral("Refreshing IP library..."));
    });
    connect(m_controller,
            &LibraryController::indexingFinished,
            this,
            [this](const QList<AssetRecord> &assets,
                   const QList<ScanIssue> &issues) {
                m_assets = assets;
                m_scanIssues = issues;
                refreshTagFilter();
                runSearch();
                int errors = 0;
                int warnings = 0;
                for (const ScanIssue &issue : issues) {
                    errors += issue.severity == Diagnostic::Severity::Error ? 1 : 0;
                    warnings += issue.severity == Diagnostic::Severity::Warning ? 1 : 0;
                }
                statusBar()->showMessage(
                    QStringLiteral("%1 IPs  |  %2 errors  |  %3 warnings  |  %4")
                        .arg(assets.size())
                        .arg(errors)
                        .arg(warnings)
                        .arg(QDir::toNativeSeparators(m_libraryRoot)));
                if (m_pendingActivation) {
                    const ActivationRequest request = *m_pendingActivation;
                    m_pendingActivation.reset();
                    applyActivation(request);
                }
            });
    connect(m_controller,
            &LibraryController::indexingFailed,
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
        QStringLiteral("name, ID, version, tag, description, or path"));
    m_searchEdit->setMinimumWidth(280);
    toolbar->addWidget(m_searchEdit);
    toolbar->addWidget(new QLabel(QStringLiteral("Tag"), toolbar));
    m_tagFilter = new QComboBox(toolbar);
    m_tagFilter->setObjectName(QStringLiteral("tagFilter"));
    m_tagFilter->setMinimumContentsLength(12);
    toolbar->addWidget(m_tagFilter);
    QAction *refreshAction = toolbar->addAction(QStringLiteral("Refresh"));
    connect(refreshAction, &QAction::triggered, m_controller, &LibraryController::rebuild);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] {
        m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(m_tagFilter,
            &QComboBox::currentTextChanged,
            this,
            [this](const QString &tag) {
                m_proxyModel->setTagFilter(m_tagFilter->currentIndex() <= 0
                                               ? QString()
                                               : tag);
                selectFirstRow();
            });

    m_tableModel = new AssetTableModel(this);
    m_proxyModel = new AssetFilterProxyModel(this);
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

    m_tabs = new QTabWidget(details);
    m_tabs->setObjectName(QStringLiteral("detailTabs"));
    m_tabs->setDocumentMode(true);
    auto *filesPage = new QWidget(m_tabs);
    auto *filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(0, 0, 0, 0);
    auto *filesSplitter = new QSplitter(Qt::Vertical, filesPage);
    m_fileTree = new QTreeWidget(filesSplitter);
    m_fileTree->setObjectName(QStringLiteral("fileTree"));
    m_fileTree->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Size")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->header()->setStretchLastSection(false);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sourcePreview = new QPlainTextEdit(filesSplitter);
    m_sourcePreview->setObjectName(QStringLiteral("sourcePreview"));
    m_sourcePreview->setReadOnly(true);
    m_sourcePreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_sourcePreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    filesSplitter->setSizes({190, 250});
    filesLayout->addWidget(filesSplitter);

    m_versionTree = new QTreeWidget(m_tabs);
    m_versionTree->setObjectName(QStringLiteral("versionTree"));
    m_versionTree->setHeaderLabels(
        {QStringLiteral("Version"), QStringLiteral("Created"), QStringLiteral("Hash")});
    m_versionTree->setRootIsDecorated(false);
    m_versionTree->setAlternatingRowColors(true);
    m_versionTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versionTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_versionTree->header()->setStretchLastSection(true);
    m_tabs->addTab(filesPage, QStringLiteral("Files"));
    m_tabs->addTab(m_versionTree, QStringLiteral("Versions"));
    detailsLayout->addWidget(m_tabs, 1);
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
                m_copyLinkAction->setEnabled(selected);
                updateDetails(currentRecord());
            });
    connect(m_assetTable, &QTableView::doubleClicked,
            this, &MainWindow::openCurrentFolder);
    connect(m_fileTree,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem *current) {
                if (!current) {
                    m_sourcePreview->clear();
                    return;
                }
                m_sourcePreview->setPlainText(
                    readPreview(current->data(0, FilePathRole).toString()));
            });
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

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *choose = fileMenu->addAction(QStringLiteral("Choose IP library..."),
                                          QKeySequence::Open);
    connect(choose, &QAction::triggered, this, &MainWindow::chooseLibrary);
    QAction *add = fileMenu->addAction(QStringLiteral("Add IP..."));
    connect(add, &QAction::triggered, this, &MainWindow::addIp);
    fileMenu->addAction(m_editAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_exportAction);
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QStringLiteral("Exit"), QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu *assetMenu = menuBar()->addMenu(QStringLiteral("&IP"));
    assetMenu->addAction(m_versionAction);
    assetMenu->addAction(m_openFolderAction);
    m_copyLinkAction = assetMenu->addAction(QStringLiteral("Copy xIPs link"));
    m_copyLinkAction->setEnabled(false);
    connect(m_copyLinkAction,
            &QAction::triggered,
            this,
            &MainWindow::copyCurrentLink);

    QMenu *libraryMenu = menuBar()->addMenu(QStringLiteral("&Library"));
    QAction *refresh = libraryMenu->addAction(QStringLiteral("Refresh"),
                                              QKeySequence::Refresh);
    connect(refresh, &QAction::triggered, m_controller, &LibraryController::rebuild);
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

void MainWindow::refreshTagFilter()
{
    const QString selected = m_tagFilter->currentIndex() > 0
                                 ? m_tagFilter->currentText()
                                 : QString();
    QStringList tags;
    for (const AssetRecord &asset : m_assets) {
        tags.append(asset.manifest.tags);
    }
    tags.removeDuplicates();
    std::sort(tags.begin(), tags.end(), [](const QString &left, const QString &right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });

    const QSignalBlocker blocker(m_tagFilter);
    m_tagFilter->clear();
    m_tagFilter->addItem(QStringLiteral("All tags"));
    m_tagFilter->addItems(tags);
    const int restored = selected.isEmpty() ? 0 : m_tagFilter->findText(selected);
    m_tagFilter->setCurrentIndex(std::max(0, restored));
    m_proxyModel->setTagFilter(m_tagFilter->currentIndex() > 0
                                   ? m_tagFilter->currentText()
                                   : QString());
}

void MainWindow::updateDetails(const AssetRecord *asset)
{
    m_infoTree->clear();
    m_description->clear();
    m_fileTree->clear();
    m_sourcePreview->clear();
    m_versionTree->clear();
    if (!asset) {
        m_nameLabel->setText(QStringLiteral("No IP selected"));
        return;
    }

    m_nameLabel->setText(asset->manifest.name);
    addInfoRow(m_infoTree, QStringLiteral("ID"), asset->manifest.id);
    addInfoRow(m_infoTree,
               QStringLiteral("Current version"),
               asset->manifest.version.isEmpty() ? QStringLiteral("working")
                                                 : asset->manifest.version);
    addInfoRow(m_infoTree, QStringLiteral("Language"), asset->manifest.language);
    if (!asset->manifest.top.isEmpty()) {
        addInfoRow(m_infoTree, QStringLiteral("Top"), asset->manifest.top);
    }
    if (!asset->manifest.tools.isEmpty()) {
        QStringList tools;
        for (const QString &name : asset->manifest.tools.keys()) {
            tools.append(name + u' ' + asset->manifest.tools.value(name).toVariant().toString());
        }
        addInfoRow(m_infoTree,
                   QStringLiteral("Tools"),
                   tools.join(QStringLiteral(", ")));
    }
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
    addInfoRow(m_infoTree, QStringLiteral("Content hash"), asset->contentHash);
    m_description->setPlainText(asset->manifest.description);
    populateFiles(*asset);
    populateVersions(*asset);
}

void MainWindow::populateFiles(const AssetRecord &asset)
{
    const QStringList files = AssetScanner::assetFiles(asset.assetRoot);
    for (const QString &relative : files) {
        const QString absolute = QDir(asset.assetRoot).absoluteFilePath(relative);
        const QFileInfo info(absolute);
        auto *item = new QTreeWidgetItem(
            m_fileTree,
            {QDir::toNativeSeparators(relative), formatBytes(info.size())});
        item->setData(0, FilePathRole, absolute);
        item->setToolTip(0, absolute);
    }
    if (m_fileTree->topLevelItemCount() > 0) {
        m_fileTree->setCurrentItem(m_fileTree->topLevelItem(0));
    } else {
        m_sourcePreview->setPlainText(QStringLiteral("No payload files."));
    }
}

void MainWindow::populateVersions(const AssetRecord &asset)
{
    const auto shortHash = [](QString hash) {
        hash.remove(QStringLiteral("sha256:"));
        return hash.left(16);
    };
    auto *working = new QTreeWidgetItem(
        m_versionTree,
        {QStringLiteral("Working copy"),
         asset.lastModified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
         shortHash(asset.contentHash)});
    working->setData(0, VersionRole, QString());
    working->setToolTip(2, asset.contentHash);

    QString error;
    const QList<VersionInfo> versions = m_libraryService.versions(asset.assetRoot, &error);
    if (!error.isEmpty()) {
        statusBar()->showMessage(error);
    }
    for (const VersionInfo &version : versions) {
        auto *item = new QTreeWidgetItem(
            m_versionTree,
            {version.version,
             version.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             shortHash(version.contentHash)});
        item->setData(0, VersionRole, version.version);
        item->setToolTip(2, version.contentHash);
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
        .version = asset->manifest.version,
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

void MainWindow::copyCurrentLink()
{
    const AssetRecord *asset = currentRecord();
    if (!asset) {
        return;
    }
    const QString link = IntegrationService::assetUri(asset->manifest.id)
                             .toString(QUrl::FullyEncoded);
    QApplication::clipboard()->setText(link);
    statusBar()->showMessage(QStringLiteral("Copied %1").arg(link));
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
        if (m_assets.isEmpty() && m_tableModel->rowCount() == 0) {
            m_pendingActivation = request;
            return;
        }
        m_tagFilter->setCurrentIndex(0);
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
