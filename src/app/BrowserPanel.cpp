#include "BrowserPanel.h"
#include "Branding.h"
#include "ElaApplication.h"
#include "ElaComboBox.h"
#include "ElaContentDialog.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaMenu.h"
#include "ElaPlainTextEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToolButton.h"
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QMimeData>
#include <QResizeEvent>
#include <QSettings>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTextDocument>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <type_traits>

namespace xips
{
namespace
{
template <class Editor> void editingMenu(Editor *editor, const QPoint &position)
{
    ElaMenu menu(editor);
    menu.setObjectName("xipsEditMenu");
    bool selection, undo, redo, empty;
    if constexpr (std::is_base_of_v<QLineEdit, Editor>)
    {
        selection = editor->hasSelectedText();
        undo = editor->isUndoAvailable();
        redo = editor->isRedoAvailable();
        empty = editor->text().isEmpty();
    }
    else
    {
        selection = editor->textCursor().hasSelection();
        undo = editor->document()->isUndoAvailable();
        redo = editor->document()->isRedoAvailable();
        empty = editor->document()->isEmpty();
    }
    const bool writable = !editor->isReadOnly();
    const auto add =
        [&](const QString &text, const QKeySequence &shortcut, auto operation, bool enabled)
    {
        auto *action = menu.addAction(text);
        action->setShortcut(shortcut);
        action->setEnabled(enabled);
        QObject::connect(action, &QAction::triggered, editor, operation);
    };
    add(QStringLiteral("Undo"), QKeySequence::Undo, [editor] { editor->undo(); }, writable && undo);
    add(QStringLiteral("Redo"), QKeySequence::Redo, [editor] { editor->redo(); }, writable && redo);
    menu.addSeparator();
    add(
        QStringLiteral("Cut"), QKeySequence::Cut, [editor] { editor->cut(); },
        writable && selection);
    add(QStringLiteral("Copy"), QKeySequence::Copy, [editor] { editor->copy(); }, selection);
    add(
        QStringLiteral("Paste"), QKeySequence::Paste, [editor] { editor->paste(); },
        writable && !QApplication::clipboard()->text().isEmpty());
    add(
        QStringLiteral("Delete"), {},
        [editor]
        {
            if constexpr (std::is_base_of_v<QLineEdit, Editor>)
                editor->del();
            else
            {
                auto cursor = editor->textCursor();
                cursor.removeSelectedText();
            }
        },
        writable && selection);
    menu.addSeparator();
    add(
        QStringLiteral("Select all"), QKeySequence::SelectAll, [editor] { editor->selectAll(); },
        !empty);
    menu.exec(position);
}
class EnglishLineEdit final : public ElaLineEdit
{
  public:
    using ElaLineEdit::ElaLineEdit;

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editingMenu(this, event->globalPos());
    }
};
class EnglishPlainTextEdit final : public ElaPlainTextEdit
{
  public:
    using ElaPlainTextEdit::ElaPlainTextEdit;

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editingMenu(this, event->globalPos());
    }
};
class Form final : public ElaContentDialog
{
  public:
    QVBoxLayout *body;
    ElaPushButton *acceptButton;
    Form(QWidget *parent, const QString &title, const QString &action)
        : ElaContentDialog(parent->window())
    {
        setObjectName("xipsForm");
        setWindowTitle(title);
        setStandardButtonsVisible(false);
        auto *content = new QWidget(this);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(20, 18, 20, 18);
        layout->setSpacing(12);
        auto *heading = new ElaText(title, 18, content);
        heading->setTextFormat(Qt::PlainText);
        layout->addWidget(heading);
        body = new QVBoxLayout;
        body->setSpacing(10);
        layout->addLayout(body);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new ElaPushButton(QStringLiteral("Cancel"), content);
        acceptButton = new ElaPushButton(action, content);
        acceptButton->setObjectName("formAccept");
        acceptButton->setDefault(true);
        buttons->addWidget(cancel);
        buttons->addWidget(acceptButton);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
        setCentralWidget(content);
        setMinimumWidth(400);
    }
    void message(const QString &text)
    {
        auto *label = new ElaText(text, this);
        label->setTextPixelSize(13);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(label);
    }
};
ElaComboBox *categories(QWidget *parent, const QString &category)
{
    auto *combo = new ElaComboBox(parent);
    combo->setObjectName("categoryCombo");
    for (const auto &type : QStringList{"module", "ip", "artifact", "other"})
        combo->addItem(SnapshotLibrary::categoryLabel(type), type);
    combo->setCurrentIndex(combo->findData(category));
    return combo;
}
QString versionText(const CatalogAsset &asset, const Snapshot &snapshot)
{
    if (asset.legacy)
        return snapshot.id == "working" ? QStringLiteral("Legacy working copy")
                                        : QStringLiteral("Legacy version %1").arg(snapshot.id);
    return QStringLiteral("rev%1").arg(snapshot.id);
}
} // namespace
void initializeEla()
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    const bool siblings = QApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    eApp->init();
    qApp->setFont(font);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, siblings);
    using namespace ElaThemeType;
    const auto set = [](ThemeColor role, const char *color)
    { eTheme->setThemeColor(Dark, role, QColor(color)); };
    set(WindowBase, "#181825");
    set(WindowCentralStackBase, "#1e1e2e");
    set(PopupBase, "#1e1e2e");
    set(DialogBase, "#1e1e2e");
    set(DialogLayoutArea, "#181825");
    set(BasicText, "#cdd6f4");
    set(BasicDetailsText, "#a6adc8");
    set(BasicBase, "#313244");
    set(BasicHover, "#45475a");
    set(BasicBorder, "#45475a");
    set(PrimaryNormal, "#89b4fa");
    set(BasicSelectedAlpha, "#313244");
    set(BasicSelectedHoverAlpha, "#45475a");
    eTheme->setThemeMode(QSettings().value("ui/dark", true).toBool() ? Dark : Light);
}
BrowserPanel::BrowserPanel(QWidget *parent, QObject *host) : QWidget(parent), m_host(host)
{
    setObjectName("xipsBrowser");
    setAcceptDrops(true);
    setMinimumSize(340, 360);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(10);
    auto *header = new QHBoxLayout;
    auto *icon = new ElaText(this);
    icon->setPixmap(applicationIcon().pixmap(20, 20));
    icon->setFixedSize(20, 20);
    header->addWidget(icon);
    m_title = new ElaText(QStringLiteral("Library"), 18, this);
    header->addWidget(m_title);
    header->addStretch();
    m_add = new ElaPushButton(QStringLiteral("Collect"), this);
    m_add->setObjectName("collectButton");
    header->addWidget(m_add);
    auto *menu = new ElaToolButton(this);
    menu->setText(QStringLiteral("···"));
    menu->setToolTip(QStringLiteral("Library and asset actions"));
    menu->setObjectName("moreButton");
    menu->setFixedSize(32, 32);
    header->addWidget(menu);
    layout->addLayout(header);
    m_search = new EnglishLineEdit(this);
    m_search->setObjectName("assetSearch");
    m_search->setPlaceholderText(QStringLiteral("Search names, descriptions or files…"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_filters = new QWidget(this);
    auto *filters = new QHBoxLayout(m_filters);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(4);
    auto *group = new QButtonGroup(this);
    for (const auto &category : QStringList{"", "module", "ip", "artifact", "other"})
    {
        auto *button = new ElaToolButton(m_filters);
        button->setText(category.isEmpty() ? QStringLiteral("All")
                                           : SnapshotLibrary::categoryLabel(category));
        button->setObjectName("filter_" + category);
        button->setCheckable(true);
        button->setChecked(category.isEmpty());
        button->setIsSelected(category.isEmpty());
        group->addButton(button);
        filters->addWidget(button);
        connect(button, &QToolButton::toggled, button, &ElaToolButton::setIsSelected);
        connect(button, &QToolButton::clicked, this,
                [this, category]
                {
                    m_category = category;
                    filter();
                });
    }
    filters->addStretch();
    layout->addWidget(m_filters);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(5);
    m_list = new ElaListView(m_splitter);
    m_list->setObjectName("assetList");
    m_list->setItemHeight(54);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_model = new QStandardItemModel(this);
    m_list->setModel(m_model);
    m_details = new QWidget(m_splitter);
    m_details->setObjectName("assetDetails");
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(12, 0, 0, 0);
    details->setSpacing(8);
    m_name = new ElaText(QStringLiteral("Select an asset"), 18, m_details);
    m_name->setObjectName("assetName");
    m_name->setWordWrap(true);
    m_name->setTextFormat(Qt::PlainText);
    details->addWidget(m_name);
    m_summary = new ElaText(m_details);
    m_summary->setTextPixelSize(13);
    m_summary->setObjectName("assetSummary");
    details->addWidget(m_summary);
    m_description = new ElaText(m_details);
    m_description->setTextPixelSize(13);
    m_description->setWordWrap(true);
    m_description->setTextFormat(Qt::PlainText);
    details->addWidget(m_description);
    m_versions = new ElaComboBox(m_details);
    m_versions->setObjectName("versionCombo");
    details->addWidget(m_versions);
    m_files = new ElaListView(m_details);
    m_files->setObjectName("fileList");
    m_files->setItemHeight(28);
    m_files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileModel = new QStandardItemModel(this);
    m_files->setModel(m_fileModel);
    details->addWidget(m_files, 1);
    auto *actions = new QHBoxLayout;
    m_take = new ElaPushButton(QStringLiteral("Use…"), m_details);
    m_take->setObjectName("takeButton");
    m_update = new ElaPushButton(QStringLiteral("Update"), m_details);
    m_update->setObjectName("updateButton");
    actions->addWidget(m_take, 1);
    actions->addWidget(m_update);
    details->addLayout(actions);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);
    m_status =
        new ElaText(QStringLiteral("Choose a library folder to start collecting assets"), this);
    m_status->setObjectName("browserNotice");
    m_status->setTextPixelSize(12);
    m_status->setMaximumHeight(52);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, &BrowserPanel::filter);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &BrowserPanel::selectCurrent);
    connect(m_versions, &QComboBox::currentIndexChanged, this, &BrowserPanel::selectVersion);
    connect(m_add, &QPushButton::clicked, this, &BrowserPanel::addSources);
    connect(m_take, &QPushButton::clicked, this, &BrowserPanel::exportAsset);
    connect(m_update, &QPushButton::clicked, this, &BrowserPanel::updateAsset);
    connect(menu, &QToolButton::clicked, this, &BrowserPanel::more);
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this] { applyTheme(); });
    applyTheme();
    setBusy(false);
}
void BrowserPanel::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
    const auto background = eTheme->getThemeColor(mode, ElaThemeType::WindowCentralStackBase);
    const auto text = eTheme->getThemeColor(mode, ElaThemeType::BasicText);
    const auto border = eTheme->getThemeColor(mode, ElaThemeType::BasicBorder);
    auto p = palette();
    p.setColor(QPalette::Window, background);
    p.setColor(QPalette::Base, background);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::WindowText, text);
    setPalette(p);
    setAutoFillBackground(true);
    setStyleSheet(
        QStringLiteral(
            "QWidget#assetDetails { background: %1; } QSplitter::handle { background: %2; } "
            "QListView { background: %1; color: %3; border: 0; selection-background-color: %2; }")
            .arg(background.name(), border.name(), text.name()));
}
void BrowserPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto orientation = width() < 660 ? Qt::Vertical : Qt::Horizontal;
    if (m_splitter->orientation() != orientation)
    {
        m_splitter->setOrientation(orientation);
        m_details->layout()->setContentsMargins(orientation == Qt::Horizontal ? 12 : 0,
                                                orientation == Qt::Vertical ? 8 : 0, 0, 0);
        m_splitter->setSizes({250, 300});
    }
}
void BrowserPanel::dragEnterEvent(QDragEnterEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (!m_busy && !urls.isEmpty() &&
        std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); }))
        event->acceptProposedAction();
}
void BrowserPanel::dropEvent(QDropEvent *event)
{
    if (m_busy)
        return;
    QStringList paths;
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    event->acceptProposedAction();
    if (m_library.isEmpty())
        chooseLibrary();
    collectPaths(paths);
}
void BrowserPanel::setContext(const QString &library, const QString &workspace)
{
    m_workspace = workspace;
    m_take->setText(workspace.isEmpty() ? QStringLiteral("Use…")
                                        : QStringLiteral("Use in project…"));
    QString path = library;
    if (path.isEmpty())
    {
        QSettings settings("xIPs", "xIPs");
        path = qEnvironmentVariable("XIPS_LIBRARY");
        if (path.isEmpty())
            path = settings.value("library/root").toString();
    }
    if ((path == m_library && !m_assets.isEmpty()) || m_busy)
        return;
    m_library = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    refresh();
}
void BrowserPanel::refresh()
{
    if (m_busy)
        return;
    if (m_library.isEmpty())
    {
        notice(QStringLiteral("Choose a library from More, or click Collect to get started"));
        return;
    }
    const QString library = m_library,
                  selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    setBusy(true, QStringLiteral("Loading library…"));
    auto *watcher = new QFutureWatcher<CatalogResult>(this);
    connect(watcher, &QFutureWatcher<CatalogResult>::finished, this,
            [this, watcher, selectedId]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_assets = result.assets;
                m_problems = result.problems;
                m_pendingId = selectedId;
                setBusy(false);
                filter();
                m_title->setText(QStringLiteral("Library · %1").arg(m_assets.size()));
                if (!m_problems.isEmpty())
                    notice(QStringLiteral("%1 issues found. Open More → Issues for details.")
                               .arg(m_problems.size()),
                           true);
                else
                    notice(
                        m_assets.isEmpty()
                            ? QStringLiteral(
                                  "Your library is empty. Click Collect to add files or a folder.")
                            : QDir::toNativeSeparators(m_library));
            });
    watcher->setFuture(QtConcurrent::run([library] { return SnapshotLibrary::scan(library); }));
}
void BrowserPanel::filter()
{
    if (m_busy)
        return;
    const QString selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    const auto terms = m_search->text().trimmed().toCaseFolded().split(' ', Qt::SkipEmptyParts);
    m_model->clear();
    int selectedRow = 0;
    for (const auto &asset : m_assets)
    {
        if (!m_category.isEmpty() && asset.category != m_category)
            continue;
        QString haystack = asset.name + ' ' + asset.description + ' ' + asset.tags.join(' ');
        if (!asset.snapshots.isEmpty())
            haystack += ' ' + asset.snapshots.last().files.join(' ');
        haystack = haystack.toCaseFolded();
        if (!std::all_of(terms.begin(), terms.end(),
                         [&](const QString &term) { return haystack.contains(term); }))
            continue;
        QString secondary = SnapshotLibrary::categoryLabel(asset.category);
        secondary += asset.legacy ? QStringLiteral(" · Legacy")
                                  : QStringLiteral(" · rev%1").arg(asset.snapshots.last().id);
        if (!asset.description.isEmpty())
            secondary += " · " + asset.description.simplified();
        auto *item = new QStandardItem(asset.name + '\n' + secondary);
        item->setData(asset.root, Qt::UserRole);
        item->setToolTip(asset.name + '\n' + secondary);
        item->setSizeHint(QSize(200, 54));
        if (asset.id == selectedId)
            selectedRow = m_model->rowCount();
        m_model->appendRow(item);
    }
    m_pendingId.clear();
    if (m_model->rowCount())
        m_list->setCurrentIndex(m_model->index(selectedRow, 0));
    else
    {
        ++m_generation;
        m_selected = {};
        m_snapshot = {};
        m_name->setText(QStringLiteral("No matching assets"));
        m_summary->clear();
        m_description->clear();
        m_versions->clear();
        m_fileModel->clear();
        setBusy(false);
    }
}
void BrowserPanel::selectCurrent()
{
    const QString root = m_list->currentIndex().data(Qt::UserRole).toString();
    const auto found = std::find_if(m_assets.begin(), m_assets.end(),
                                    [&](const auto &asset) { return asset.root == root; });
    if (found == m_assets.end())
        return;
    m_selected = *found;
    const auto asset = m_selected;
    const int generation = ++m_generation;
    m_name->setText(asset.name);
    m_versions->clear();
    m_fileModel->clear();
    m_snapshot = {};
    m_take->setEnabled(false);
    m_update->setEnabled(false);
    m_summary->setText(QStringLiteral("Loading revisions…"));
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this,
            [this, watcher, generation]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation)
                    return;
                if (!result.ok)
                {
                    notice(result.error, true);
                    return;
                }
                showDetails(result.asset);
            });
    watcher->setFuture(QtConcurrent::run([asset] { return SnapshotLibrary::describe(asset); }));
}
void BrowserPanel::showDetails(const CatalogAsset &asset)
{
    m_selected = asset;
    m_name->setText(asset.name);
    m_description->setText(asset.description);
    m_description->setVisible(!asset.description.isEmpty());
    const QSignalBlocker blocker(m_versions);
    m_versions->clear();
    for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
    {
        QString label = versionText(asset, *it);
        if (it->created.isValid())
            label += " · " + it->created.toLocalTime().toString("yyyy-MM-dd");
        if (!it->note.isEmpty())
            label += " · " + it->note;
        m_versions->addItem(label, it->id);
    }
    const int requested = m_versions->findData(m_pendingRevision);
    if (requested >= 0)
        m_versions->setCurrentIndex(requested);
    m_pendingRevision.clear();
    m_update->setText(asset.legacy ? QStringLiteral("Convert legacy asset")
                                   : QStringLiteral("Update"));
    selectVersion();
}
void BrowserPanel::selectVersion()
{
    m_snapshot = {};
    for (const auto &snapshot : m_selected.snapshots)
        if (snapshot.id == m_versions->currentData().toString())
            m_snapshot = snapshot;
    m_fileModel->clear();
    for (const auto &file : m_snapshot.files)
        m_fileModel->appendRow(new QStandardItem(file));
    m_summary->setText((m_snapshot.files.size() == 1 ? QStringLiteral("%1 · %2 file")
                                                     : QStringLiteral("%1 · %2 files"))
                           .arg(SnapshotLibrary::categoryLabel(m_selected.category))
                           .arg(m_snapshot.files.size()));
    m_take->setEnabled(!m_busy && !m_snapshot.id.isEmpty());
    m_update->setEnabled(!m_busy && !m_selected.id.isEmpty());
}
void BrowserPanel::setBusy(bool value, const QString &message)
{
    m_busy = value;
    m_add->setEnabled(!value);
    m_search->setEnabled(!value);
    m_filters->setEnabled(!value);
    m_list->setEnabled(!value);
    m_versions->setEnabled(!value);
    m_take->setEnabled(!value && !m_snapshot.id.isEmpty());
    m_update->setEnabled(!value && !m_selected.id.isEmpty());
    if (!message.isEmpty())
        notice(message);
}
void BrowserPanel::notice(const QString &message, bool error)
{
    m_status->setText(message);
    m_status->setToolTip(message);
    m_status->setProperty("error", error);
}
void BrowserPanel::run(const QString &message, std::function<SnapshotResult()> work,
                       std::function<void(const SnapshotResult &)> finished)
{
    if (m_busy)
        return;
    ++m_generation;
    setBusy(true, message);
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this,
            [this, watcher, finished]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                if (!result.ok)
                {
                    QString message = result.error;
                    if (!result.retainedPath.isEmpty())
                        message += QStringLiteral("\nRetained at: %1").arg(result.retainedPath);
                    m_problems.append(message);
                    notice(message, true);
                    return;
                }
                if (finished)
                    finished(result);
                else
                {
                    m_pendingId = result.asset.id;
                    refresh();
                }
            });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
void BrowserPanel::chooseLibrary()
{
    if (m_busy)
        return;
    const QString path =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Choose library folder"), m_library);
    if (path.isEmpty())
        return;
    m_library = path;
    m_assets.clear();
    m_selected = {};
    QSettings settings("xIPs", "xIPs");
    settings.setValue("library/root", path);
    refresh();
}
QStringList BrowserPanel::pickSources()
{
    ElaMenu menu(this);
    auto *current = m_host ? menu.addAction(QStringLiteral("Current file")) : nullptr;
    const auto *files = menu.addAction(QStringLiteral("Choose files…"));
    const auto *folder = menu.addAction(QStringLiteral("Choose folder…"));
    const auto *chosen = menu.exec(QCursor::pos());
    if (chosen && chosen == current)
    {
        QStringList paths;
        if (!QMetaObject::invokeMethod(m_host, "collectionSources", Qt::DirectConnection,
                                       Q_RETURN_ARG(QStringList, paths)) ||
            paths.isEmpty())
            notice(QStringLiteral("No saved source file was selected."));
        return paths;
    }
    if (chosen == files)
        return QFileDialog::getOpenFileNames(this, QStringLiteral("Choose files to collect"),
                                             m_workspace);
    if (chosen == folder)
    {
        const auto path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose folder to collect"), m_workspace);
        if (!path.isEmpty())
            return {path};
    }
    return {};
}
void BrowserPanel::addSources()
{
    if (m_busy)
        return;
    if (m_library.isEmpty())
    {
        chooseLibrary();
        return;
    }
    const auto paths = pickSources();
    if (!paths.isEmpty())
        collectPaths(paths);
}
void BrowserPanel::collectPaths(const QStringList &paths)
{
    if (m_busy || paths.isEmpty())
        return;
    if (m_library.isEmpty())
    {
        notice(QStringLiteral("Choose a library folder first"));
        return;
    }
    Form form(this, QStringLiteral("Collect an asset"), QStringLiteral("Collect"));
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("collectName");
    name->setText(QFileInfo(paths.first()).completeBaseName());
    name->setPlaceholderText(QStringLiteral("Asset name"));
    auto *category = categories(&form, SnapshotLibrary::suggestedCategory(paths));
    form.body->addWidget(name);
    form.body->addWidget(category);
    form.message(paths.join('\n'));
    form.message(QStringLiteral("Creates rev1. Module and IP assets exclude build folders; "
                                "Artifact assets include the selected outputs."));
    connect(name, &QLineEdit::textChanged, &form,
            [&] { form.acceptButton->setEnabled(!name->text().trimmed().isEmpty()); });
    if (form.exec() != QDialog::Accepted)
        return;
    const QString library = m_library, title = name->text(),
                  type = category->currentData().toString();
    run(QStringLiteral("Collecting files…"), [library, paths, title, type]
        { return SnapshotLibrary::collect(library, paths, title, type); });
}
void BrowserPanel::updateAsset()
{
    if (m_busy || m_selected.id.isEmpty())
        return;
    const auto asset = m_selected;
    if (asset.legacy)
    {
        Form form(this, QStringLiteral("Convert legacy asset"), QStringLiteral("Convert"));
        form.message(
            QStringLiteral("Convert the saved versions and working copy of %1 to linear revisions. "
                           "The original folder will be retained as a backup.")
                .arg(asset.name));
        if (form.exec() != QDialog::Accepted)
            return;
        run(
            QStringLiteral("Converting legacy asset…"),
            [asset] { return SnapshotLibrary::migrate(asset); },
            [this](const auto &result)
            {
                m_pendingId = result.asset.id;
                if (!result.retainedPath.isEmpty())
                {
                    Form details(this, QStringLiteral("Original files retained"),
                                 QStringLiteral("Done"));
                    details.message(result.retainedPath);
                    details.exec();
                }
                refresh();
            });
        return;
    }
    const auto paths = pickSources();
    if (paths.isEmpty())
        return;
    Form form(this, QStringLiteral("Update %1").arg(asset.name), QStringLiteral("Save revision"));
    form.message(QStringLiteral("Saves a complete snapshot. Existing revisions stay unchanged. "
                                "Identical content is skipped."));
    auto *note = new EnglishLineEdit(&form);
    note->setObjectName("revisionNote");
    note->setPlaceholderText(QStringLiteral("Note (optional), e.g. verified on board"));
    form.body->addWidget(note);
    if (form.exec() != QDialog::Accepted)
        return;
    const auto text = note->text();
    run(
        QStringLiteral("Saving revision…"),
        [asset, paths, text] { return SnapshotLibrary::update(asset, paths, text); },
        [this](const auto &result)
        {
            showDetails(result.asset);
            if (result.unchanged)
                notice(QStringLiteral("Content unchanged. No revision created."));
            else
            {
                m_pendingId = result.asset.id;
                refresh();
            }
        });
}
void BrowserPanel::exportAsset()
{
    if (m_busy || m_snapshot.id.isEmpty())
        return;
    const auto asset = m_selected;
    const auto snapshot = m_snapshot;
    const QString workspace = m_workspace;
    Form form(this, QStringLiteral("Use %1 · %2").arg(asset.name, versionText(asset, snapshot)),
              QStringLiteral("Use"));
    auto *destination = new EnglishLineEdit(&form);
    destination->setObjectName("exportDestination");
    const QString suggested = snapshot.files.size() == 1
                                  ? QFileInfo(snapshot.files.first()).fileName()
                                  : QFileInfo(asset.root).fileName();
    destination->setText(workspace.isEmpty() ? QString()
                                             : QDir(workspace).absoluteFilePath(suggested));
    destination->setPlaceholderText(QStringLiteral("Full destination path"));
    auto *row = new QHBoxLayout;
    row->addWidget(destination, 1);
    auto *browse = new ElaPushButton(QStringLiteral("Browse"), &form);
    row->addWidget(browse);
    form.body->addLayout(row);
    form.message(QStringLiteral("Copies %1 files to a new destination. Later library updates will "
                                "not change this copy.")
                     .arg(snapshot.files.size()));
    connect(browse, &QPushButton::clicked, &form,
            [&]
            {
                const auto directory = QFileDialog::getExistingDirectory(
                    &form, QStringLiteral("Choose destination folder"), workspace);
                if (!directory.isEmpty())
                    destination->setText(QDir(directory).absoluteFilePath(suggested));
            });
    const auto validate = [&]
    {
        const QString path = destination->text().trimmed();
        form.acceptButton->setEnabled(!path.isEmpty() && QDir::isAbsolutePath(path) &&
                                      !QFileInfo::exists(path) &&
                                      QFileInfo(QFileInfo(path).absolutePath()).isDir());
    };
    connect(destination, &QLineEdit::textChanged, &form, validate);
    validate();
    if (form.exec() != QDialog::Accepted)
        return;
    const QString target = QFileInfo(destination->text().trimmed()).absoluteFilePath();
    if (m_host)
    {
        QString error;
        if (!QMetaObject::invokeMethod(m_host, "destinationError", Qt::DirectConnection,
                                       Q_RETURN_ARG(QString, error), Q_ARG(QString, target)))
        {
            notice(QStringLiteral("The host did not provide destination validation."), true);
            return;
        }
        if (!error.isEmpty())
        {
            notice(error, true);
            return;
        }
    }
    run(
        QStringLiteral("Copying selected revision…"), [asset, snapshot, target]
        { return SnapshotLibrary::exportSnapshot(asset, snapshot.id, target); },
        [this, workspace](const auto &result)
        {
            const QVariantMap provenance{{"schema", "xips.use/v1"},
                                         {"assetId", result.asset.id},
                                         {"name", result.asset.name},
                                         {"revision", result.snapshot.id},
                                         {"contentHash", result.snapshot.hash},
                                         {"path", result.exportedPath},
                                         {"workspace", workspace},
                                         {"files", result.snapshot.files},
                                         {"category", result.asset.category}};
            if (m_host)
            {
                QString error;
                const bool recorded = QMetaObject::invokeMethod(
                    m_host, "exportCompleted", Qt::DirectConnection, Q_RETURN_ARG(QString, error),
                    Q_ARG(QVariantMap, provenance));
                if (!recorded || !error.isEmpty())
                {
                    notice(QStringLiteral("Files created at %1. %2")
                               .arg(result.exportedPath,
                                    recorded ? error
                                             : QStringLiteral(
                                                   "The host could not record their origin.")),
                           true);
                    return;
                }
            }
            notice(QStringLiteral("Created %1 from rev%2")
                       .arg(result.exportedPath, result.snapshot.id));
        });
}
void BrowserPanel::detailsDialog()
{
    const auto asset = m_selected;
    Form form(this, QStringLiteral("Edit asset details"), QStringLiteral("Save"));
    auto *name = new EnglishLineEdit(&form);
    name->setText(asset.name);
    auto *category = categories(&form, asset.category);
    auto *description = new EnglishPlainTextEdit(&form);
    description->setPlainText(asset.description);
    description->setMaximumHeight(100);
    description->setPlaceholderText(QStringLiteral("Description (optional)"));
    form.body->addWidget(name);
    form.body->addWidget(category);
    form.body->addWidget(description);
    if (form.exec() != QDialog::Accepted)
        return;
    const QString title = name->text(), type = category->currentData().toString(),
                  text = description->toPlainText();
    run(QStringLiteral("Saving asset details…"),
        [asset, title, type, text] { return SnapshotLibrary::edit(asset, title, type, text); });
}
void BrowserPanel::more()
{
    ElaMenu menu(this);
    auto *choose = menu.addAction(QStringLiteral("Choose library…"));
    auto *refreshAction = menu.addAction(QStringLiteral("Refresh"));
    menu.addSeparator();
    auto *editAction = menu.addAction(QStringLiteral("Edit asset details…"));
    auto *deleteVersion = menu.addAction(QStringLiteral("Delete selected revision…"));
    auto *deleteAsset = menu.addAction(QStringLiteral("Delete asset…"));
    menu.addSeparator();
    auto *problems = menu.addAction(QStringLiteral("Issues (%1)").arg(m_problems.size()));
    auto *theme = menu.addAction(QStringLiteral("Toggle light / dark theme"));
    theme->setVisible(!m_host);
    for (auto *action : {choose, refreshAction, editAction, deleteVersion, deleteAsset})
        action->setEnabled(!m_busy);
    editAction->setEnabled(!m_busy && !m_selected.id.isEmpty() && !m_selected.legacy);
    deleteVersion->setEnabled(!m_busy && !m_selected.legacy && m_selected.snapshots.size() > 1 &&
                              !m_snapshot.id.isEmpty());
    deleteAsset->setEnabled(!m_busy && !m_selected.id.isEmpty());
    const auto *action = menu.exec(QCursor::pos());
    if (action == choose)
        chooseLibrary();
    else if (action == refreshAction)
        refresh();
    else if (action == editAction)
        detailsDialog();
    else if (action == theme)
    {
        const bool dark = eTheme->getThemeMode() != ElaThemeType::Dark;
        eTheme->setThemeMode(dark ? ElaThemeType::Dark : ElaThemeType::Light);
        QSettings().setValue("ui/dark", dark);
    }
    else if (action == problems)
    {
        Form form(this, QStringLiteral("Library issues"), QStringLiteral("Done"));
        form.message(m_problems.isEmpty() ? QStringLiteral("No issues found")
                                          : m_problems.join("\n\n"));
        form.exec();
    }
    else if (action == deleteVersion || action == deleteAsset)
    {
        const auto asset = m_selected;
        const auto snapshot = m_snapshot;
        const bool whole = action == deleteAsset;
        Form form(this, whole ? QStringLiteral("Delete asset") : QStringLiteral("Delete revision"),
                  QStringLiteral("Move to Recycle Bin"));
        form.message(whole ? QStringLiteral("Move %1 and all its revisions to the Recycle Bin? "
                                            "Existing project copies stay unchanged.")
                                 .arg(asset.name)
                           : QStringLiteral("Move %1, rev%2 to the Recycle Bin? Other revisions "
                                            "and project copies stay unchanged.")
                                 .arg(asset.name, snapshot.id));
        if (form.exec() != QDialog::Accepted)
            return;
        run(QStringLiteral("Moving to Recycle Bin…"),
            [asset, snapshot, whole]
            {
                return whole ? SnapshotLibrary::eraseAsset(asset)
                             : SnapshotLibrary::eraseSnapshot(asset, snapshot.id);
            });
    }
}
std::optional<CatalogAsset> BrowserPanel::catalogAsset(const QString &id) const
{
    for (const auto &asset : m_assets)
        if (asset.id.compare(id, Qt::CaseInsensitive) == 0)
            return asset;
    return std::nullopt;
}
void BrowserPanel::revealAsset(const QString &id)
{
    m_pendingId = id;
    if (!m_busy)
        filter();
}
QVariantMap BrowserPanel::saveState() const
{
    return {{"query", m_search->text()},
            {"category", m_category},
            {"assetId", m_selected.id},
            {"revision", m_snapshot.id}};
}
void BrowserPanel::restoreState(const QVariantMap &state)
{
    m_search->setText(state.value("query").toString());
    m_category = state.value("category").toString();
    if (auto *button = findChild<ElaToolButton *>("filter_" + m_category))
        button->setChecked(true);
    m_pendingId = state.value("assetId").toString();
    m_pendingRevision = state.value("revision").toString();
    if (!m_busy)
        filter();
}
} // namespace xips
