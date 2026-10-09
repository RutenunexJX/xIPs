#include "ArchivePage.h"
#include "ArchiveBatchPanel.h"
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResource>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
#include <stdexcept>

static void initializeArchiveResources()
{
    static const bool initialized = [] { Q_INIT_RESOURCE(xips_archive_resources); return true; }();
    Q_UNUSED(initialized)
}

namespace xips::archive {
namespace {
QLabel* label(const QString& text, QWidget* parent)
{
    auto* value = new QLabel(text, parent);
    value->setWordWrap(true);
    value->setTextFormat(Qt::PlainText);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return value;
}
QPushButton* button(const QString& caption, const QString& name, QWidget* parent)
{
    auto* value = new QPushButton(caption, parent);
    value->setObjectName(name);
    value->setMinimumHeight(32);
    value->setCursor(Qt::PointingHandCursor);
    return value;
}
QString droppedXpr(const QMimeData* data)
{
    if (!data->hasUrls() || data->urls().size() != 1 || !data->urls().first().isLocalFile()) return {};
    const auto path = data->urls().first().toLocalFile();
    return QFileInfo(path).suffix().compare("xpr", Qt::CaseInsensitive) == 0 ? path : QString{};
}
}
ArchivePage::ArchivePage(QWidget* parent, InstallationProvider installed) : QWidget(parent), installed_(std::move(installed))
{
    initializeArchiveResources();
    appearancePalette_ = palette();
    setObjectName("projectArchivePage");
    setAcceptDrops(true);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("archiveScroll");
    scroll->viewport()->setObjectName("archiveViewport");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget(scroll);
    content->setObjectName("archiveContent");
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(16);
    auto* header = new QHBoxLayout;
    auto* icon = new QLabel(content); icon->setPixmap(QIcon(":/xips/archive/archive.svg").pixmap(48, 48));
    icon->setFixedSize(48, 48); header->addWidget(icon); header->addSpacing(4);
    auto* heading = new QVBoxLayout;
    auto* title = label("Archive project", content);
    auto font = title->font(); font.setPointSize(22); font.setBold(true); title->setFont(font);
    heading->addWidget(title);
    heading->addWidget(label("A clean copy and a 7z package. Your original stays unchanged.", content));
    header->addLayout(heading, 1); layout->addLayout(header);
    auto* inputs = new QFrame(content); inputs->setObjectName("archiveInputs");
    auto* form = new QVBoxLayout(inputs); form->setContentsMargins(20, 18, 20, 18); form->setSpacing(9);
    inputMode_ = new QComboBox(inputs); inputMode_->setObjectName("archiveInputMode"); inputMode_->addItems({"Single project", "Project folder"});
    inputMode_->setMinimumHeight(34); inputMode_->setAccessibleName("Archive input mode"); form->addWidget(inputMode_);
    singleProject_ = new QWidget(inputs);
    auto* projectForm = new QVBoxLayout(singleProject_); projectForm->setContentsMargins(0, 0, 0, 0); projectForm->setSpacing(9);
    auto* projectLabel = label("Project", inputs); auto fieldFont = projectLabel->font(); fieldFont.setBold(true); projectLabel->setFont(fieldFont);
    projectForm->addWidget(projectLabel);
    project_ = new QLineEdit(inputs); project_->setObjectName("archiveProject"); project_->setPlaceholderText("Drop a .xpr file here or choose a project"); project_->setAcceptDrops(false); project_->setMinimumHeight(36);
    project_->setAccessibleName("Vivado project file");
    browse_ = button("Choose...", "archiveBrowse", inputs);
    auto* projectRow = new QHBoxLayout; projectRow->addWidget(project_, 1); projectRow->addWidget(browse_);
    projectForm->addLayout(projectRow);
    projectInfo_ = label({}, inputs); projectInfo_->setObjectName("archiveProjectInfo"); projectInfo_->hide(); projectForm->addWidget(projectInfo_);
    form->addWidget(singleProject_);
    batch_ = new ArchiveBatchPanel(inputs); batch_->hide(); form->addWidget(batch_);
    form->addSpacing(6);
    auto* outputLabel = label("Output folder", inputs); outputLabel->setFont(fieldFont); form->addWidget(outputLabel);
    output_ = new QLineEdit(QDir::tempPath() + "/xIPs/archives", inputs); output_->setObjectName("archiveOutput"); output_->setAcceptDrops(false); output_->setMinimumHeight(36);
    output_->setAccessibleName("Archive output folder");
    outputBrowse_ = button("Change...", "archiveOutputBrowse", inputs);
    auto* outputRow = new QHBoxLayout; outputRow->addWidget(output_, 1); outputRow->addWidget(outputBrowse_);
    form->addLayout(outputRow); form->addSpacing(6);
    singleName_ = new QWidget(inputs);
    auto* nameForm = new QVBoxLayout(singleName_); nameForm->setContentsMargins(0, 0, 0, 0); nameForm->setSpacing(9);
    auto* nameLabel = label("Archive name (optional)", inputs); nameLabel->setFont(fieldFont); nameForm->addWidget(nameLabel);
    name_ = new QLineEdit(inputs); name_->setObjectName("archiveName"); name_->setMinimumHeight(36); name_->setAcceptDrops(false);
    name_->setPlaceholderText("Automatic: project name + timestamp + unique ID"); name_->setAccessibleName("Archive name (optional)");
    nameLabel->setBuddy(name_); nameForm->addWidget(name_);
    nameHint_ = label({}, inputs); nameHint_->setObjectName("archiveNameHint"); nameForm->addWidget(nameHint_); form->addWidget(singleName_); form->addSpacing(6);
    auto* versionRow = new QHBoxLayout;
    auto* versionLabel = label("Vivado", inputs); versionLabel->setFont(fieldFont); versionRow->addWidget(versionLabel); versionRow->addSpacing(10);
    versions_ = new QComboBox(inputs); versions_->setObjectName("archiveVersion"); versions_->setMinimumContentsLength(16); versions_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); versions_->setMinimumHeight(34); versions_->setAccessibleName("Vivado release");
    versionRow->addWidget(versions_, 1);
    batchVersions_ = label("Each project uses its exact required release.", inputs); batchVersions_->hide(); versionRow->addWidget(batchVersions_, 1);
    configure_ = button("Manage...", "archiveConfigure", inputs); versionRow->addWidget(configure_);
    form->addLayout(versionRow); layout->addWidget(inputs);
    status_ = label({}, content); status_->setObjectName("archiveStatus"); status_->hide(); layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    start_ = button("Create archive + 7z", "archiveStart", content); start_->setMinimumHeight(40); start_->setMinimumWidth(188); actions->addWidget(start_);
    start_->setToolTip("Close this project in Vivado before archiving.");
    cancel_ = button("Cancel", "archiveCancel", content); cancel_->setEnabled(false); cancel_->hide(); actions->addWidget(cancel_);
    actions->addStretch();
    open_ = button("Open output", "archiveOpen", content); open_->hide(); actions->addWidget(open_);
    layout->addLayout(actions);
    progressPanel_ = new QFrame(content); progressPanel_->setObjectName("archiveProgress");
    auto* progressLayout = new QVBoxLayout(progressPanel_); progressLayout->setContentsMargins(0, 8, 0, 8); progressLayout->setSpacing(12);
    auto* progressHeader = new QHBoxLayout;
    progressTitle_ = label("Preparing archive", progressPanel_); progressTitle_->setFont(fieldFont); progressHeader->addWidget(progressTitle_, 1);
    percent_ = label("0%", progressPanel_); percent_->setObjectName("archivePercent"); percent_->setFont(fieldFont); progressHeader->addWidget(percent_);
    progressLayout->addLayout(progressHeader);
    overall_ = new QProgressBar(progressPanel_); overall_->setObjectName("archiveOverall"); overall_->setValue(0); overall_->setTextVisible(false); overall_->setFixedHeight(8);
    progressLayout->addWidget(overall_);
    auto* stageRow = new QHBoxLayout; stageRow->setSpacing(12);
    const QStringList names{"Inspect", "Copy", "Artifacts", "Reset", "Clean", "7z"};
    for (int i = 0; i < names.size(); ++i) {
        auto* column = new QVBoxLayout;
        auto* name = label(names[i], progressPanel_); auto stageFont = name->font(); stageFont.setPointSize(9); name->setFont(stageFont); stageNames_.append(name); column->addWidget(name);
        auto* bar = new QProgressBar(progressPanel_); bar->setObjectName("archiveStage" + QString::number(i)); bar->setRange(0, 100); bar->setValue(0); bar->setFormat("Pending"); bar->setTextVisible(false); bar->setFixedHeight(4); bar->setMinimumWidth(24); bar->setAccessibleName(names[i]);
        stages_.append(bar); column->addWidget(bar); stageRow->addLayout(column, 1);
    }
    progressLayout->addLayout(stageRow); progressPanel_->hide(); layout->addWidget(progressPanel_);
    details_ = button("Details", "archiveDetails", content); details_->setFlat(true); details_->setCheckable(true); details_->hide(); layout->addWidget(details_, 0, Qt::AlignLeft);
    detailsPanel_ = new QWidget(content); detailsPanel_->setObjectName("archiveDetailsPanel");
    auto* detailsLayout = new QVBoxLayout(detailsPanel_); detailsLayout->setContentsMargins(0, 0, 0, 0);
    result_ = label({}, detailsPanel_); result_->setObjectName("archiveResult"); detailsLayout->addWidget(result_);
    log_ = new QPlainTextEdit(detailsPanel_); log_->setReadOnly(true); log_->setObjectName("archiveLog"); log_->setMaximumBlockCount(1500); log_->setPlaceholderText("Process output"); log_->setFixedHeight(160);
    detailsLayout->addWidget(log_); detailsPanel_->hide(); layout->addWidget(detailsPanel_);
    connect(details_, &QPushButton::toggled, detailsPanel_, &QWidget::setVisible);
    layout->addStretch();
    applyAppearance();
    connect(browse_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Select Vivado project", project_->text(), "Vivado project (*.xpr)");
        if (!path.isEmpty()) selectProject(path);
    });
    connect(project_, &QLineEdit::editingFinished, this, [this] { selectProject(project_->text()); });
    connect(outputBrowse_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "Select archive output folder", output_->text());
        if (!path.isEmpty()) { output_->setText(path); emit stateChanged(); }
    });
    connect(output_, &QLineEdit::textChanged, this, &ArchivePage::refreshActions);
    connect(output_, &QLineEdit::editingFinished, this, &ArchivePage::stateChanged);
    connect(name_, &QLineEdit::textChanged, this, &ArchivePage::refreshActions);
    connect(name_, &QLineEdit::editingFinished, this, &ArchivePage::stateChanged);
    connect(inputMode_, &QComboBox::currentIndexChanged, this, [this](int mode) {
        singleProject_->setVisible(mode == 0); singleName_->setVisible(mode == 0); versions_->setVisible(mode == 0);
        batch_->setVisible(mode == 1); batchVersions_->setVisible(mode == 1);
        if (mode == 0 && !project_->text().isEmpty()) selectProject(project_->text());
        refreshActions(); emit stateChanged();
    });
    connect(batch_, &ArchiveBatchPanel::stateChanged, this, [this] { refreshActions(); emit stateChanged(); });
    connect(batch_, &ArchiveBatchPanel::scanningChanged, this, [this](bool scanning) {
        setBusy(scanning); setStatus({});
        if (scanning) { progressPanel_->hide(); details_->setChecked(false); open_->hide(); }
    });
    connect(versions_, &QComboBox::currentIndexChanged, this, [this] { refreshActions(); emit stateChanged(); });
    connect(configure_, &QPushButton::clicked, this, &ArchivePage::configure);
    connect(start_, &QPushButton::clicked, this, &ArchivePage::startArchive);
    connect(cancel_, &QPushButton::clicked, this, &ArchivePage::requestCancel);
    connect(open_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(resultDirectory_)); });
    refreshVersions();
    QTimer::singleShot(0, this, [this] { if (installations_.isEmpty()) discover(); });
}
void ArchivePage::setAppearancePalette(const QPalette& value)
{
    appearancePalette_ = value;
    setPalette(value);
    applyAppearance();
}
void ArchivePage::applyAppearance()
{
    // Explicit colors refresh stylesheet caches when an embedding host changes palette.
    const auto css = QString("QWidget#projectArchivePage, QWidget#archiveContent, QWidget#archiveViewport, QScrollArea#archiveScroll { background: %5; color: %4; }"
        "QLabel { color: %4; background: transparent; }"
        "QFrame#archiveInputs { background: %1; border: 1px solid %2; border-radius: 12px; }"
        "QLabel#archiveProjectInfo, QLabel#archiveNameHint { color: %3; }"
        "QLineEdit, QPlainTextEdit, QComboBox, QAbstractItemView { background: %5; color: %4; border: 1px solid %2; border-radius: 6px; padding: 4px 8px; selection-background-color: #4269d5; selection-color: white; }"
        "QTableWidget#archiveProjects { padding: 0; }"
        "QHeaderView { background: %1; padding: 0; border: 0; border-radius: 0; }"
        "QHeaderView::section { background: %1; color: %4; padding: 6px 8px; border: 0; border-bottom: 1px solid %2; }"
        "QLineEdit:focus, QComboBox:focus { border-color: #4269d5; }"
        "QLineEdit:disabled, QComboBox:disabled { color: %3; }"
        "QPushButton { background: %1; color: %4; border: 1px solid %2; border-radius: 6px; padding: 0 12px; }"
        "QPushButton:hover, QPushButton:focus { border-color: #4269d5; }"
        "QPushButton:disabled { color: %3; }"
        "QPushButton#archiveDetails { background: transparent; border: 0; padding: 0; }"
        "QPushButton#archiveStart { background: #4269d5; color: white; border: 0; border-radius: 7px; padding: 0 16px; font-weight: bold; }"
        "QPushButton#archiveStart:hover { background: #365cbd; }"
        "QPushButton#archiveStart:disabled { background: %2; color: %3; }"
        "QProgressBar { border: 0; border-radius: 2px; background: %2; }"
        "QProgressBar::chunk { background: #4269d5; border-radius: 2px; }")
        .arg(appearancePalette_.color(QPalette::Base).name(), appearancePalette_.color(QPalette::Mid).name(),
             appearancePalette_.color(QPalette::PlaceholderText).name(), appearancePalette_.color(QPalette::Text).name(),
             appearancePalette_.color(QPalette::Window).name());
    if (styleSheet() != css) setStyleSheet(css);
}
void ArchivePage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (start_ && event->type() == QEvent::PaletteChange) applyAppearance();
}
ArchivePage::~ArchivePage()
{
    if (scanner_) { scanner_->requestInterruption(); scanner_->wait(); }
    if (job_) { job_->requestInterruption(); job_->wait(); }
}
void ArchivePage::requestCancel()
{
    cancelQueue_ = true;
    batch_->cancelScan();
    if (scanner_) scanner_->requestInterruption();
    if (job_) job_->requestInterruption();
    cancel_->setEnabled(false);
    if (isBusy()) setStatus("Cancelling...");
}
void ArchivePage::setStatus(const QString& text, bool error)
{
    status_->setText(text);
    status_->setStyleSheet(error ? "color: #d45454; font-weight: bold;" : "font-weight: bold;");
    status_->setVisible(!text.isEmpty());
}
void ArchivePage::refreshActions()
{
    const int selected = versions_->currentIndex();
    const bool matching = selected >= 0 && selected < installations_.size() && installations_[selected].version == detectedVersion_;
    const auto error = archiveNameError(name_->text());
    nameHint_->setText(error.isEmpty() ? "Used for the folder and .7z file. Leave blank for an automatic name." : error);
    nameHint_->setStyleSheet(error.isEmpty() ? QString{} : "color: #d45454;");
    const bool batchMode = inputMode_->currentIndex() == 1;
    const bool ready = batchMode ? !batch_->selectedRows().isEmpty() : !detectedVersion_.isEmpty() && matching && error.isEmpty();
    start_->setText(batchMode ? "Archive selected projects" : "Create archive + 7z");
    start_->setEnabled(!busy_ && !scanner_ && ready && !output_->text().trimmed().isEmpty());
    versions_->setToolTip(versions_->currentData().toString());
}
void ArchivePage::selectProject(const QString& path)
{
    if (busy_) return;
    project_->setText(QDir::fromNativeSeparators(path.trimmed()));
    detectedVersion_.clear();
    projectInfo_->hide();
    if (project_->text().isEmpty()) { setStatus({}); refreshActions(); emit stateChanged(); return; }
    try {
        const auto info = inspectProject(project_->text());
        detectedVersion_ = info.version;
        projectInfo_->setText("Requires Vivado " + info.version); projectInfo_->show();
        project_->setToolTip(info.xpr);
        refreshVersions(info.version);
        setStatus({});
    } catch (const std::exception& ex) {
        setStatus(QString::fromUtf8(ex.what()), true);
    }
    refreshActions();
    emit stateChanged();
}
void ArchivePage::refreshVersions(const QString& preferred)
{
    if (installed_) for (const auto& install : installed_()) {
        bool found = false;
        for (const auto& existing : installations_) if (existing.launcher.compare(install.launcher, Qt::CaseInsensitive) == 0) found = true;
        if (!found) installations_.append(install);
    }
    const auto old = versions_->currentData().toString();
    const QSignalBlocker blocker(versions_);
    versions_->clear();
    for (const auto& install : installations_) {
        versions_->addItem("Vivado " + install.version, install.launcher);
        versions_->setItemData(versions_->count() - 1, install.launcher, Qt::ToolTipRole);
    }
    int chosen = -1;
    for (int i = 0; i < installations_.size(); ++i) {
        if ((!preferred.isEmpty() && installations_[i].version == preferred) || (preferred.isEmpty() && installations_[i].launcher == old)) { chosen = i; break; }
    }
    for (int i = 0; i < installations_.size(); ++i)
        if (installations_[i].launcher == old && (preferred.isEmpty() || installations_[i].version == preferred)) chosen = i;
    versions_->setCurrentIndex(chosen);
    versions_->setPlaceholderText(preferred.isEmpty() ? "Select a Vivado release" : "Vivado " + preferred + " is not configured");
    refreshActions();
}
void ArchivePage::discover()
{
    if (scanner_ || busy_) return;
    configure_->setEnabled(false); start_->setEnabled(false);
    cancel_->setEnabled(true); cancel_->show();
    versions_->setPlaceholderText("Finding Vivado...");
    auto found = std::make_shared<QList<Installation>>();
    scanner_ = QThread::create([found] { *found = discoverInstallations(); });
    scanner_->setParent(this);
    connect(scanner_, &QThread::finished, this, [this, found] {
        for (const auto& install : *found) {
            bool exists = false;
            for (const auto& saved : installations_) if (saved.launcher.compare(install.launcher, Qt::CaseInsensitive) == 0) exists = true;
            if (!exists) installations_.append(install);
        }
        scanner_->deleteLater(); scanner_ = nullptr;
        emit busyChanged(isBusy());
        configure_->setEnabled(!busy_);
        cancel_->setVisible(busy_); cancel_->setEnabled(busy_);
        refreshVersions(detectedVersion_);
        if (installations_.isEmpty()) setStatus("Add a Vivado installation in Manage to continue.");
        emit installationsChanged();
        emit stateChanged();
    });
    emit busyChanged(true);
    scanner_->start();
}
void ArchivePage::configure()
{
    QDialog dialog(this); dialog.setWindowTitle("Vivado installation paths"); dialog.resize(780, 380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(label("Installation paths are saved on this device.", &dialog));
    auto* table = new QTableWidget(0, 2, &dialog); table->setObjectName("vivadoPathTable"); table->setHorizontalHeaderLabels({"Release", "Launcher path"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents); table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (const auto& install : installations_) {
        const int row = table->rowCount(); table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(install.version)); table->setItem(row, 1, new QTableWidgetItem(install.launcher));
    }
    layout->addWidget(table);
    auto* row = new QHBoxLayout;
    auto* scan = button("Find installed", "archiveDiscover", &dialog); row->addWidget(scan); scan->setEnabled(!scanner_);
    connect(scan, &QPushButton::clicked, &dialog, [this, scan] { scan->setEnabled(false); discover(); });
    connect(this, &ArchivePage::installationsChanged, &dialog, [this, table, scan] {
        for (const auto& install : installations_) {
            bool exists = false;
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 1) && table->item(r, 1)->text().compare(install.launcher, Qt::CaseInsensitive) == 0) exists = true;
            if (!exists) {
                const int r = table->rowCount(); table->insertRow(r);
                table->setItem(r, 0, new QTableWidgetItem(install.version));
                table->setItem(r, 1, new QTableWidgetItem(install.launcher));
            }
        }
        scan->setEnabled(true);
    });
    auto* add = button("Add installation...", "vivadoAdd", &dialog); row->addWidget(add);
    auto* remove = button("Remove selected", "vivadoRemove", &dialog); row->addWidget(remove); row->addStretch(); layout->addLayout(row);
    auto* message = label({}, &dialog); layout->addWidget(message);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    connect(add, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, "Select Vivado launcher", {}, "Vivado launcher (vivado.bat vivado.exe vivado);;All files (*)");
        if (file.isEmpty()) return;
        const int r = table->rowCount(); table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(installationVersion(file))); table->setItem(r, 1, new QTableWidgetItem(QDir::fromNativeSeparators(file)));
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QList<Installation> updated;
        for (int r = 0; r < table->rowCount(); ++r) {
            const auto version = table->item(r, 0) ? table->item(r, 0)->text().trimmed() : QString{};
            const auto path = table->item(r, 1) ? table->item(r, 1)->text().trimmed() : QString{};
            if (!QRegularExpression("^[0-9]{4}\\.[0-9]+(?:\\.[0-9]+)?$").match(version).hasMatch() || !QFileInfo(path).isFile()) {
                message->setText(QString("Row %1 needs a release such as 2023.1 and an existing launcher.").arg(r + 1)); return;
            }
            updated.append({version, QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath())});
        }
        installations_ = updated; dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted) { refreshVersions(detectedVersion_); emit stateChanged(); }
}
void ArchivePage::setBusy(bool busy)
{
    busy_ = busy;
    emit busyChanged(isBusy());
    for (auto* widget : QList<QWidget*>{project_, output_, name_, versions_, browse_, outputBrowse_, configure_, inputMode_}) widget->setEnabled(!busy);
    batch_->setLocked(busy);
    if (scanner_) configure_->setEnabled(false);
    cancel_->setEnabled(busy); cancel_->setVisible(busy);
    refreshActions();
}
void ArchivePage::startArchive()
{
    if (busy_) return;
    if (output_->text().trimmed().isEmpty()) { setStatus("Choose an output folder.", true); return; }
    batchActive_ = inputMode_->currentIndex() == 1;
    if (batchActive_) {
        refreshVersions();
        QString error;
        requests_ = batch_->requests(output_->text().trimmed(), installations_, error);
        if (!error.isEmpty()) { setStatus(error, true); return; }
        batchRows_ = batch_->selectedRows();
        for (int row : batchRows_) batch_->setRowStatus(row, "Queued");
    } else {
        const auto selectedPath = versions_->currentData();
        selectProject(project_->text());
        versions_->setCurrentIndex(versions_->findData(selectedPath));
        const int selected = versions_->currentIndex();
        if (detectedVersion_.isEmpty()) return;
        if (selected < 0 || selected >= installations_.size()) { setStatus("Add the required Vivado release in Manage.", true); return; }
        if (installations_[selected].version != detectedVersion_) { setStatus("Select the required Vivado release.", true); return; }
        const auto nameError = archiveNameError(name_->text());
        if (!nameError.isEmpty()) { setStatus(nameError, true); return; }
        requests_ = {{project_->text(), output_->text().trimmed(), installations_[selected], {}, name_->text()}};
        batchRows_.clear();
    }
    cancelQueue_ = false; queueIndex_ = 0; successes_ = 0; failures_ = 0;
    log_->clear(); result_->clear(); resultDirectory_.clear(); open_->hide();
    progressPanel_->show(); details_->show(); details_->setChecked(false); setStatus({});
    emit stateChanged(); setBusy(true); startNextArchive();
}
void ArchivePage::startNextArchive()
{
    if (cancelQueue_ || queueIndex_ >= requests_.size()) { finishQueue(); return; }
    for (auto* stage : stages_) { stage->setRange(0, 100); stage->setValue(0); stage->setFormat("Pending"); }
    activeStage_ = -1;
    const auto& request = requests_.at(queueIndex_);
    if (batchActive_) {
        batch_->setRowStatus(batchRows_.at(queueIndex_), "Running");
        log_->appendPlainText(QString("Project %1/%2: %3").arg(queueIndex_ + 1).arg(requests_.size()).arg(request.xpr));
    }
    progressTitle_->setText("Preparing archive");
    job_ = new ArchiveJob(request, this);
    connect(job_, &ArchiveJob::progress, this, [this](int stage, int percent, const QString& detail) {
        if (stage < 0 || stage >= stages_.size()) return;
        activeStage_ = stage;
        auto* bar = stages_[stage]; bar->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) bar->setValue(percent);
        bar->setFormat(percent == 100 ? "Complete" : "%p%");
        const int starts[]{0, 5, 35, 45, 65, 70}; const int weights[]{5, 30, 10, 20, 5, 30};
        const int overall = starts[stage] + (percent < 0 ? 0 : percent * weights[stage] / 100);
        const int total = (queueIndex_ * 100 + overall) / requests_.size();
        overall_->setValue(total); percent_->setText(QString::number(total) + '%');
        const QStringList titles{"Inspecting project", "Copying project", "Saving artifacts", "Resetting project", "Cleaning archive", "Creating 7z package"};
        progressTitle_->setText(stage == 5 && percent >= 90 ? "Checking 7z package" : titles[stage]);
        if (batchActive_) progressTitle_->setText(QString("Project %1/%2: %3").arg(queueIndex_ + 1).arg(requests_.size()).arg(progressTitle_->text()));
        for (int i = 0; i < stageNames_.size(); ++i) {
            auto font = stageNames_[i]->font(); font.setBold(i == stage); stageNames_[i]->setFont(font);
        }
        bar->setToolTip(detail); result_->setText(detail);
    });
    connect(job_, &ArchiveJob::logMessage, log_, &QPlainTextEdit::appendPlainText);
    connect(job_, &ArchiveJob::completed, this, [this](bool success, bool cancelled, const QString& directory, const QString& diagnostics, const QString& message) {
        if (success) ++successes_; else if (!cancelled) ++failures_;
        if (cancelled) cancelQueue_ = true;
        if (batchActive_) {
            batch_->setRowStatus(batchRows_.at(queueIndex_), success ? "Complete" : cancelled ? "Cancelled" : "Failed",
                                 message + '\n' + (success ? directory : diagnostics));
            log_->appendPlainText(message + '\n' + (success ? directory : diagnostics));
        }
        for (auto* stage : stages_) if (stage->maximum() == 0) { stage->setRange(0, 100); stage->setValue(0); stage->setFormat(cancelled ? "Cancelled" : "Failed"); }
        if (!success && activeStage_ >= 0) {
            stages_[activeStage_]->setRange(0, 100);
            stages_[activeStage_]->setFormat(cancelled ? "Cancelled" : "Failed");
        }
        setStatus(message, !success && !cancelled);
        progressTitle_->setText(success ? "Complete" : cancelled ? "Cancelled" : "Archive failed");
        resultDirectory_ = success ? QFileInfo(directory).absolutePath() : diagnostics;
        result_->setText(success ? "Folder: " + directory + "\n7z: " + directory + ".7z" : diagnostics);
        open_->setText(success ? "Open output" : "Open diagnostics"); open_->setVisible(!resultDirectory_.isEmpty());
    });
    connect(job_, &QThread::finished, this, [this] {
        job_->deleteLater(); job_ = nullptr; ++queueIndex_;
        QTimer::singleShot(0, this, &ArchivePage::startNextArchive);
    });
    job_->start();
}
void ArchivePage::finishQueue()
{
    if (batchActive_) {
        for (int index = queueIndex_; index < batchRows_.size(); ++index) batch_->setRowStatus(batchRows_.at(index), "Not started");
        const auto summary = QString("%1 projects archived; %2 failed.%3").arg(successes_).arg(failures_)
                                 .arg(cancelQueue_ ? " Queue cancelled." : "");
        setStatus(summary, failures_ > 0); progressTitle_->setText(cancelQueue_ ? "Queue cancelled" : "Queue complete");
        if (!cancelQueue_) { overall_->setValue(100); percent_->setText("100%"); }
        if (successes_ > 0) { resultDirectory_ = output_->text().trimmed(); open_->setText("Open output"); open_->show(); }
        result_->setText(summary + '\n' + resultDirectory_);
    }
    requests_.clear(); batchRows_.clear(); batchActive_ = false; setBusy(false);
}
QVariantMap ArchivePage::saveState() const
{
    QVariantList paths;
    for (const auto& install : installations_) paths.append(QVariantMap{{"version", install.version}, {"launcher", install.launcher}});
    return {{"schema", 1}, {"installations", paths}, {"project", project_->text()}, {"output", output_->text()}, {"archiveName", name_->text()},
            {"inputMode", inputMode_->currentIndex()}, {"batch", batch_->saveState()}, {"selected", versions_->currentData().toString()}};
}
void ArchivePage::restoreState(const QVariantMap& state)
{
    if (state.value("schema").toInt() != 1) return;
    installations_.clear();
    for (const auto& item : state.value("installations").toList()) {
        const auto map = item.toMap();
        if (!map.value("version").toString().isEmpty() && !map.value("launcher").toString().isEmpty()) installations_.append({map.value("version").toString(), map.value("launcher").toString()});
    }
    if (!state.value("output").toString().isEmpty()) output_->setText(state.value("output").toString());
    name_->setText(state.value("archiveName").toString());
    batch_->restoreState(state.value("batch").toMap());
    inputMode_->setCurrentIndex(state.value("inputMode").toInt() == 1 ? 1 : 0);
    project_->setText(state.value("project").toString());
    refreshVersions();
    const auto index = versions_->findData(state.value("selected")); versions_->setCurrentIndex(index);
    if (inputMode_->currentIndex() == 0 && !project_->text().isEmpty()) selectProject(project_->text());
}
QString ArchivePage::closeBlockReason() const { return isBusy() ? "An archive or project scan is running. Cancel it and wait for it to stop before closing." : QString{}; }
void ArchivePage::dragEnterEvent(QDragEnterEvent* event) { if (!busy_ && !droppedXpr(event->mimeData()).isEmpty()) event->acceptProposedAction(); }
void ArchivePage::dropEvent(QDropEvent* event)
{
    const auto path = droppedXpr(event->mimeData());
    if (!busy_ && !path.isEmpty()) { inputMode_->setCurrentIndex(0); selectProject(path); event->acceptProposedAction(); }
}
}
