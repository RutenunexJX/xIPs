#include "ArchiveBatchPanel.h"
#include "VivadoWorkspace.h"
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace xips::archive {
ArchiveBatchPanel::ArchiveBatchPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("archiveBatchPanel");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(9);
    auto* row = new QHBoxLayout;
    folder_ = new QLineEdit(this); folder_->setObjectName("archiveScanFolder"); folder_->setPlaceholderText("Choose a folder to find all .xpr projects"); folder_->setMinimumHeight(36);
    folder_->setAccessibleName("Project scan folder"); row->addWidget(folder_, 1);
    choose_ = new QPushButton("Choose folder...", this); choose_->setObjectName("archiveChooseFolder"); choose_->setMinimumHeight(34); row->addWidget(choose_);
    scanButton_ = new QPushButton("Scan", this); scanButton_->setObjectName("archiveScan"); scanButton_->setMinimumHeight(34); row->addWidget(scanButton_); layout->addLayout(row);
    table_ = new QTableWidget(0, 5, this); table_->setObjectName("archiveProjects");
    table_->setHorizontalHeaderLabels({"Use", "Project (.xpr)", "Vivado", "Archive name", "Status"});
    table_->verticalHeader()->hide(); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setMinimumHeight(190); table_->setMaximumHeight(270);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(table_);
    summary_ = new QLabel("Edit each archive name. Blank names are generated automatically. Checked projects run one at a time.", this);
    summary_->setObjectName("archiveScanSummary"); summary_->setWordWrap(true); summary_->setTextFormat(Qt::PlainText); layout->addWidget(summary_);
    connect(choose_, &QPushButton::clicked, this, [this] {
        const auto folder = QFileDialog::getExistingDirectory(this, "Find Vivado projects", folder_->text());
        if (!folder.isEmpty()) scanFolder(folder);
    });
    connect(scanButton_, &QPushButton::clicked, this, [this] { scanFolder(folder_->text()); });
    connect(folder_, &QLineEdit::editingFinished, this, &ArchiveBatchPanel::stateChanged);
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (locked_) return;
        if (item->column() == 3) {
            const auto error = archiveNameError(item->text());
            setRowStatus(item->row(), error.isEmpty() ? "Ready" : "Invalid name", error);
        }
        emit stateChanged();
    });
}
ArchiveBatchPanel::~ArchiveBatchPanel() { if (scan_) { scan_->requestInterruption(); scan_->wait(); } }
void ArchiveBatchPanel::cancelScan() { if (scan_) scan_->requestInterruption(); }
void ArchiveBatchPanel::scanFolder(const QString& folder)
{
    if (locked_ || scan_) return;
    const auto previous = saveState();
    folder_->setText(QDir::fromNativeSeparators(folder.trimmed()));
    if (folder_->text().isEmpty()) { summary_->setText("Choose a project folder."); return; }
    scan_ = new ArchiveScan(folder_->text(), this);
    summary_->setText("Scanning for .xpr projects..."); setLocked(true); emit scanningChanged(true);
    connect(scan_, &ArchiveScan::progress, this, [this](int found, const QString& directory) {
        summary_->setText(QString("Scanning: %1 projects found. %2").arg(found).arg(directory));
    });
    connect(scan_, &QThread::finished, this, [this, previous] {
        if (scan_->cancelled) summary_->setText("Scan cancelled. The previous project list was retained.");
        else if (!scan_->error.isEmpty()) summary_->setText(scan_->error);
        else {
            setProjects(scan_->projects, previous);
            summary_->setText(QString("%1 projects found; %2 inaccessible or linked folders skipped. Edit names and check the projects to archive.")
                                 .arg(table_->rowCount()).arg(scan_->skippedDirectories));
        }
        scan_->deleteLater(); scan_ = nullptr; setLocked(false); emit scanningChanged(false); emit stateChanged();
    });
    scan_->start();
}
void ArchiveBatchPanel::setProjects(const QList<DiscoveredProject>& projects, const QVariantMap& previous)
{
    QMap<QString, QVariantMap> saved;
    for (const auto& value : previous.value("projects").toList()) {
        const auto entry = value.toMap(); saved.insert(entry.value("xpr").toString().toCaseFolded(), entry);
    }
    const QSignalBlocker blocker(table_); table_->setRowCount(0);
    for (const auto& project : projects) {
        const int row = table_->rowCount(); table_->insertRow(row);
        for (int column = 0; column < 5; ++column) {
            auto* item = new QTableWidgetItem; item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); table_->setItem(row, column, item);
        }
        const auto stored = saved.value(project.xpr.toCaseFolded());
        table_->item(row, 0)->setCheckState(project.error.isEmpty() && stored.value("checked", true).toBool() ? Qt::Checked : Qt::Unchecked);
        table_->item(row, 1)->setText(QDir(folder_->text()).relativeFilePath(project.xpr));
        table_->item(row, 1)->setData(Qt::UserRole, project.xpr); table_->item(row, 1)->setToolTip(project.xpr.toHtmlEscaped());
        table_->item(row, 2)->setText(project.version); table_->item(row, 2)->setData(Qt::UserRole, project.error);
        table_->item(row, 3)->setText(stored.value("archiveName").toString());
        const auto nameError = archiveNameError(table_->item(row, 3)->text());
        setRowStatus(row, !project.error.isEmpty() ? "Invalid project" : nameError.isEmpty() ? "Ready" : "Invalid name",
                     project.error.isEmpty() ? nameError : project.error);
    }
    setLocked(locked_);
}
void ArchiveBatchPanel::setLocked(bool locked)
{
    locked_ = locked; folder_->setEnabled(!locked); choose_->setEnabled(!locked); scanButton_->setEnabled(!locked);
    const QSignalBlocker blocker(table_);
    for (int row = 0; row < table_->rowCount(); ++row) {
        const bool valid = table_->item(row, 2)->data(Qt::UserRole).toString().isEmpty();
        table_->item(row, 0)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (!locked && valid ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
        table_->item(row, 3)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (!locked && valid ? Qt::ItemIsEditable : Qt::NoItemFlags));
    }
}
QList<int> ArchiveBatchPanel::selectedRows() const
{
    QList<int> rows;
    for (int row = 0; row < table_->rowCount(); ++row) if (table_->item(row, 0)->checkState() == Qt::Checked) rows.append(row);
    return rows;
}
QList<Request> ArchiveBatchPanel::requests(const QString& output, const QList<Installation>& installations, QString& error) const
{
    error.clear();
    QList<Request> result;
    QSet<QString> destinations;
    for (int row : selectedRows()) {
        const auto xpr = table_->item(row, 1)->data(Qt::UserRole).toString();
        const auto version = table_->item(row, 2)->text();
        const auto name = table_->item(row, 3)->text().trimmed();
        auto reason = table_->item(row, 2)->data(Qt::UserRole).toString();
        if (reason.isEmpty()) reason = archiveNameError(name);
        Installation selected;
        for (const auto& install : installations) if (install.version == version && QFileInfo(install.launcher).isFile()) { selected = install; break; }
        if (reason.isEmpty() && selected.launcher.isEmpty()) reason = "Configure Vivado " + version + " in Manage.";
        const auto root = QFileInfo(xpr).absolutePath();
        const auto absoluteOutput = QFileInfo(output).absoluteFilePath();
        if (reason.isEmpty() && (isWithin(root, absoluteOutput) || isWithin(absoluteOutput, root))) reason = "The output folder and source project must not contain one another.";
        if (reason.isEmpty() && !name.isEmpty()) {
            for (const auto& entry : QStringList{name, name + ".7z"}) {
                const auto key = entry.toCaseFolded();
                if (destinations.contains(key) || QFileInfo::exists(QDir(output).filePath(entry))) {
                    reason = "Archive name conflicts with another selected project or an existing output: " + entry; break;
                }
                destinations.insert(key);
            }
        }
        if (!reason.isEmpty()) { error = QString("Project %1: %2\n%3").arg(row + 1).arg(reason, xpr); return {}; }
        result.append({xpr, output, selected, {}, name});
    }
    if (result.isEmpty()) error = "Check at least one project to archive.";
    return result;
}
void ArchiveBatchPanel::setRowStatus(int row, const QString& status, const QString& detail)
{
    const QSignalBlocker blocker(table_);
    table_->item(row, 4)->setText(status); table_->item(row, 4)->setToolTip(detail.toHtmlEscaped());
    if (status == "Running") table_->scrollToItem(table_->item(row, 1));
}
QVariantMap ArchiveBatchPanel::saveState() const
{
    QVariantList projects;
    for (int row = 0; row < table_->rowCount(); ++row) projects.append(QVariantMap{
        {"xpr", table_->item(row, 1)->data(Qt::UserRole)}, {"version", table_->item(row, 2)->text()},
        {"error", table_->item(row, 2)->data(Qt::UserRole)}, {"archiveName", table_->item(row, 3)->text()},
        {"checked", table_->item(row, 0)->checkState() == Qt::Checked}});
    return {{"folder", folder_->text()}, {"projects", projects}};
}
void ArchiveBatchPanel::restoreState(const QVariantMap& state)
{
    folder_->setText(state.value("folder").toString());
    QList<DiscoveredProject> projects;
    for (const auto& value : state.value("projects").toList()) {
        const auto entry = value.toMap();
        const auto xpr = entry.value("xpr").toString();
        if (!xpr.isEmpty()) projects.append({xpr, entry.value("version").toString(), entry.value("error").toString()});
    }
    setProjects(projects, state);
}
}
