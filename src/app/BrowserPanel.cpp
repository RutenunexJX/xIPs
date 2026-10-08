#include "BrowserPanel.h"
#include "CatalogModel.h"
#include "CatalogWatcher.h"
#include "WorkingFilesModel.h"
#include "library/CatalogIndex.h"
#include "library/OperationControl.h"
#include "UiSupport.h"
#include "ElaFilePicker.h"
#include "ElaFlowLayout.h"
#include "ElaScrollArea.h"
#include "ElaApplication.h"
#include "ElaComboBox.h"
#include "ElaContentDialog.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaTreeView.h"
#include "ElaTableView.h"
#include "ElaTabWidget.h"
#include "ElaCheckBox.h"
#include "ElaMenu.h"
#include "ElaPlainTextEdit.h"
#include "ElaProgressRing.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToolButton.h"
#include "ElaToolBar.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QMimeData>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QSplitter>
#include <QShowEvent>
#include <QStringListModel>
#include <QTextDocument>
#include <QTimer>
#include <QTabBar>
#include <QtConcurrent>
#include <algorithm>
#include <type_traits>

namespace xips
{
namespace
{
struct CatalogReload
{
    CatalogResult catalog;
    bool full = true;
    CatalogWatcher::Plan watches;
    QHash<QString, CatalogWatcher::Plan> assetWatches;
};
bool sameAssetView(const CatalogAsset &a, const CatalogAsset &b)
{
    return a.id == b.id && a.root == b.root && a.document == b.document &&
        a.historyIncomplete == b.historyIncomplete && a.sourceProblem == b.sourceProblem &&
        a.problems == b.problems && a.workingFiles == b.workingFiles &&
        CatalogIndex::generation({a}) == CatalogIndex::generation({b});
}
QFont uiFont(int pixelSize = 13)
{
    QFont result(QStringLiteral("Segoe UI"));
    result.setPixelSize(pixelSize);
    return result;
}
ElaToolButton *actionButton(QWidget *parent, const char *name, UiIcon icon, const QString &text, const QString &tip = {})
{
    auto *button = new ElaToolButton(parent);
    button->setObjectName(name);
    button->setFont(uiFont());
    button->setText(text);
    button->setIcon(uiIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setFixedSize(28, 28);
    button->setAccessibleName(text);
    button->setToolTip(tip.isEmpty() ? text : tip);
    enableToolTip(button);
    return button;
}
template <class Editor> void editingMenu(Editor *editor, const QPoint &position)
{
    auto *menu = new ElaMenu(editor);
    menu->setObjectName("xipsEditMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    prepareMenu(menu, editor);
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
        auto *action = menu->addAction(text);
        action->setShortcut(shortcut);
        action->setEnabled(enabled);
        QObject::connect(action, &QAction::triggered, editor, operation);
    };
    add(QStringLiteral("Undo"), QKeySequence::Undo, [editor] { editor->undo(); }, writable && undo);
    add(QStringLiteral("Redo"), QKeySequence::Redo, [editor] { editor->redo(); }, writable && redo);
    menu->addSeparator();
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
    menu->addSeparator();
    add(
        QStringLiteral("Select all"), QKeySequence::SelectAll, [editor] { editor->selectAll(); },
        !empty);
    menu->popup(position);
}
class EnglishLineEdit final : public ElaLineEdit
{
  public:
    explicit EnglishLineEdit(QWidget *parent = nullptr) : ElaLineEdit(parent)
    {
        setTextMargins(10, 0, 6, 0);
        setFixedHeight(30);
    }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        editingMenu(this, event->globalPos());
    }
};
class EnglishPlainTextEdit final : public ElaPlainTextEdit
{
  public:
    explicit EnglishPlainTextEdit(QWidget *parent = nullptr) : ElaPlainTextEdit(parent)
    {
        enableSmoothScrolling(this);
    }

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
        const auto detach = [this] { reject(); setParent(nullptr); };
        connect(parent, &QObject::destroyed, this, detach);
        if (auto *panel = qobject_cast<BrowserPanel *>(parent))
            connect(panel, &BrowserPanel::contextChanging, this, &QDialog::reject);
        if (parent != parent->window())
            connect(parent->window(), &QObject::destroyed, this, detach);
        setWindowTitle(title);
        setFont(parent->font());
        setStandardButtonsVisible(false);
        auto *content = new QWidget(this);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);
        auto *heading = new ElaText(title, 16, content);
        heading->setTextFormat(Qt::PlainText);
        layout->addWidget(heading);
        body = new QVBoxLayout;
        body->setSpacing(8);
        layout->addLayout(body);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new ElaPushButton(QStringLiteral("Cancel"), content);
        acceptButton = new ElaPushButton(action, content);
        acceptButton->setObjectName("formAccept");
        acceptButton->setDefault(true);
        primaryButton(acceptButton);
        buttons->addWidget(cancel);
        buttons->addWidget(acceptButton);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
        setCentralWidget(content);
        setMinimumWidth(400);
    }
    int exec() override
    {
        for (auto *control : findChildren<QWidget *>())
            if (qobject_cast<ElaPushButton *>(control) || qobject_cast<ElaComboBox *>(control))
            {
                control->setFixedHeight(30);
                control->setFont(font());
                if (auto *button = qobject_cast<ElaPushButton *>(control))
                    button->setMinimumWidth(button->fontMetrics().horizontalAdvance(button->text()) + 28);
            }
        return ElaContentDialog::exec();
    }
    void showEvent(QShowEvent *event) override
    {
        ElaContentDialog::showEvent(event);
        keepDialogOnScreen(this);
        QTimer::singleShot(0, this, [this] { keepDialogOnScreen(this); });
    }
    void resizeEvent(QResizeEvent *event) override
    {
        ElaContentDialog::resizeEvent(event);
        if (isVisible()) keepDialogOnScreen(this);
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
    for (const auto &type : QStringList{"module", "ip", "project", "artifact", "other"})
        combo->addItem(SnapshotLibrary::categoryLabel(type), type);
    combo->setCurrentIndex(combo->findData(category));
    return combo;
}
QString versionText(const CatalogAsset &asset, const Snapshot &snapshot)
{
    if (snapshot.id == "current")
        return QStringLiteral("Current files");
    if (asset.legacy)
        return snapshot.id == "working" ? QStringLiteral("Legacy working copy")
                                        : QStringLiteral("Legacy version %1").arg(snapshot.id);
    QString label = SnapshotLibrary::revisionLabel(snapshot);
    if (std::count_if(asset.snapshots.cbegin(), asset.snapshots.cend(),
            [&](const auto &other) { return SnapshotLibrary::revisionLabel(other).compare(label, Qt::CaseInsensitive) == 0; }) > 1)
        label += " · " + snapshot.id.left(8);
    return label;
}
void previewContents(Form &form, const PayloadPreview &preview, bool exporting = false)
{
    form.setMinimumWidth(520);
    form.message(QStringLiteral("%1 %2 · %3 bytes").arg(preview.files.size())
        .arg(preview.files.size() == 1 ? QStringLiteral("file") : QStringLiteral("files")).arg(preview.bytes));
    if (exporting)
        form.message(preview.files.size() == 1 ? QStringLiteral("Destination: new file") : QStringLiteral("Destination: new directory"));
    if (!preview.comparisonRevision.isEmpty())
        form.message(QStringLiteral("Changes versus %1 · %2 unchanged files")
            .arg(preview.comparisonRevision).arg(preview.unchanged));
    QStringList lines;
    const auto section = [&](const QString &title, const QStringList &items)
    { if (!items.isEmpty()) { lines.append(title); lines.append(items); lines.append(QString()); } };
    section(QStringLiteral("Included files"), preview.files);
    section(QStringLiteral("Added"), preview.added);
    section(QStringLiteral("Modified"), preview.modified);
    section(QStringLiteral("Not included in this version (working files are kept)"), preview.removed);
    section(QStringLiteral("Excluded paths and reasons"), preview.excluded);
    if (preview.heads.size() > 1)
    {
        lines.prepend(QStringLiteral("%1 parallel revision heads\n%2\nSaving adopts these reviewed files with all heads as parents. Source text is not merged.\n")
            .arg(preview.heads.size()).arg(preview.heads.join('\n')));
    }
    auto *files = new EnglishPlainTextEdit(&form);
    files->setObjectName("payloadPreview");
    files->setReadOnly(true);
    files->setPlainText(lines.join('\n'));
    files->setMinimumSize(360, 170);
    form.body->addWidget(files);
}
QString indexTitle(const QString &kind)
{
    if (kind == "category") return QStringLiteral("Category");
    if (kind == "interface") return QStringLiteral("Interface");
    if (kind == "purpose") return QStringLiteral("Purpose");
    return QStringLiteral("Tag");
}
QMap<QString, EnglishLineEdit *> indexEditors(Form &form, const QMap<QString, QStringList> &values = {})
{
    auto *toggle = new ElaToolButton(&form);
    toggle->setObjectName("indexFieldsToggle");
    toggle->setText(QStringLiteral("Indexes (optional)"));
    toggle->setCheckable(true);
    toggle->setFixedHeight(28);
    form.body->addWidget(toggle, 0, Qt::AlignLeft);
    auto *fields = new QWidget(&form);
    fields->setObjectName("indexFields");
    auto *rows = new QFormLayout(fields);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(8);
    form.body->addWidget(fields);
    QMap<QString, EnglishLineEdit *> editors;
    for (const auto &kind : QStringList{"category", "tag", "interface", "purpose"})
    {
        auto *label = new ElaText(indexTitle(kind), 12, fields);
        auto *edit = new EnglishLineEdit(fields);
        edit->setObjectName("index_" + kind);
        edit->setPlaceholderText(QStringLiteral("Separate values with commas"));
        edit->setAccessibleName(indexTitle(kind));
        edit->setToolTip(edit->placeholderText());
        edit->setText(values.value(kind).join(", "));
        label->setBuddy(edit);
        rows->addRow(label, edit);
        editors.insert(kind, edit);
    }
    fields->hide();
    QObject::connect(toggle, &QToolButton::toggled, fields, [fields, toggle](bool expanded)
    {
        fields->setVisible(expanded);
        toggle->setText(expanded ? QStringLiteral("Hide indexes") : QStringLiteral("Indexes (optional)"));
    });
    toggle->setChecked(!values.isEmpty());
    return editors;
}
QMap<QString, QStringList> readIndexes(const QMap<QString, EnglishLineEdit *> &editors)
{
    QMap<QString, QStringList> result;
    for (auto it = editors.cbegin(); it != editors.cend(); ++it)
        result.insert(it.key(), it.value()->text().split(QRegularExpression("[,;，；]"), Qt::SkipEmptyParts));
    return result;
}
} // namespace
void initializeEla(bool embedded)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    const auto hostFont = qApp->font();
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    const bool siblings = QApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    eApp->init();
    qApp->setFont(embedded ? hostFont : font);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, siblings);
    using namespace ElaThemeType;
    struct Colors { ThemeColor role; const char *light; const char *dark; };
    const Colors colors[] = {
        {WindowBase, "#f5f6f8", "#191b1f"},
        {WindowCentralStackBase, "#ffffff", "#202226"},
        {PopupBase, "#ffffff", "#282b30"},
        {PopupBorder, "#dadce0", "#42474f"},
        {PopupBorderHover, "#b8bdc4", "#58616d"},
        {PopupHover, "#edf2f8", "#383d45"},
        {DialogBase, "#ffffff", "#24272c"},
        {DialogLayoutArea, "#f5f6f8", "#202226"},
        {BasicText, "#202124", "#e8eaed"},
        {BasicTextInvert, "#ffffff", "#172235"},
        {BasicDetailsText, "#5f6368", "#aeb4bc"},
        {BasicTextNoFocus, "#5f6368", "#aeb4bc"},
        {BasicTextDisable, "#8a8f98", "#777f88"},
        {BasicTextPress, "#303640", "#d2d8e0"},
        {BasicTextCategory, "#5f6368", "#aeb4bc"},
        {BasicBase, "#ffffff", "#2d3036"},
        {BasicBaseDeep, "#e5e7eb", "#444951"},
        {BasicDisable, "#f2f3f5", "#282b30"},
        {BasicHover, "#edf2f8", "#383d45"},
        {BasicPress, "#e3e7ed", "#414853"},
        {BasicBorder, "#dadce0", "#42474f"},
        {BasicBorderDeep, "#b8bdc4", "#58616d"},
        {BasicBorderHover, "#b8bdc4", "#58616d"},
        {BasicBaseLine, "#dadce0", "#42474f"},
        {BasicHemline, "#dadce0", "#42474f"},
        {BasicIndicator, "#5f6368", "#aeb4bc"},
        {BasicBaseAlpha, "#f8f9fb", "#272a30"},
        {BasicBaseDeepAlpha, "#e5e7eb", "#444951"},
        {BasicHoverAlpha, "#edf2f8", "#383d45"},
        {BasicPressAlpha, "#e3e7ed", "#414853"},
        {BasicSelectedAlpha, "#e8f0fe", "#2f415c"},
        {BasicSelectedHoverAlpha, "#dbe8fd", "#3a506f"},
        {BasicSelectedHover, "#dbe8fd", "#3a506f"},
        {PrimaryNormal, "#2563eb", "#8ab4f8"},
        {PrimaryHover, "#1d4ed8", "#a6c6fb"},
        {PrimaryPress, "#1e40af", "#669ce7"},
        {ScrollBarHandle, "#9aa0a6", "#737d8b"},
    };
    for (const auto &color : colors)
    {
        eTheme->setThemeColor(Light, color.role, QColor(color.light));
        eTheme->setThemeColor(Dark, color.role, QColor(color.dark));
    }
    eTheme->setThemeMode(QSettings("xIPs", "xIPs").value("ui/dark", false).toBool() ? Dark : Light);
}
BrowserPanel::BrowserPanel(QWidget *parent, QObject *host, bool embedded)
    : QWidget(parent), m_host(host), m_embedded(embedded || host)
{
    QFont panelFont(QStringLiteral("Segoe UI"));
    panelFont.setPixelSize(13);
    setFont(panelFont);
    if (host) connect(host, &QObject::destroyed, this, [this]
    {
        if (m_operation) m_operation->cancel();
        emit contextChanging();
        notice(QStringLiteral("The host connection closed. Reopen this panel to copy files to a project."), true);
    });
    setObjectName("xipsBrowser");
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    setMinimumSize(m_embedded ? 280 : 340, 260);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *toolbar = new ElaToolBar(this);
    toolbar->setObjectName("catalogToolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolBarSpacing(4);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->layout()->setContentsMargins(8, 6, 8, 6);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_search = new EnglishLineEdit(toolbar);
    m_search->setObjectName("assetSearch");
    m_search->setPlaceholderText(QStringLiteral("Search IPs…"));
    m_search->setToolTip(QStringLiteral("Search names, files and indexes (Ctrl+F). Esc clears the search.\nExample: tag:uart interface:AXI"));
    enableToolTip(m_search);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search assets"));
    m_search->setMinimumWidth(96);
    m_search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toolbar->addWidget(m_search);
    m_new = new ElaPushButton(QStringLiteral("New"), toolbar);
    m_new->setObjectName("newAssetButton");
    m_new->setFixedSize(90, 30);
    m_new->setIcon(uiIcon(UiIcon::Add));
    m_new->setIconSize(QSize(14, 14));
    m_new->setToolTip(QStringLiteral("Create a Module, IP or Project (Ctrl+N)"));
    enableToolTip(m_new);
    m_new->setFont(font());
    toolbar->addWidget(m_new);
    m_collect = actionButton(toolbar, "collectButton", UiIcon::Collect, QStringLiteral("Collect"),
        QStringLiteral("Collect files or a folder as a saved asset"));
    toolbar->addWidget(m_collect);
    m_filterToggle = actionButton(toolbar, "filterButton", UiIcon::Filter, QStringLiteral("Filter"),
        QStringLiteral("Filter by type and index"));
    m_filterToggle->setCheckable(true);
    toolbar->addWidget(m_filterToggle);
    m_theme = actionButton(toolbar, "themeButton", UiIcon::Theme, QStringLiteral("Dark theme"));
    m_theme->setCheckable(true);
    m_theme->setVisible(!m_embedded);
    toolbar->addWidget(m_theme);
    layout->addWidget(toolbar);
    m_filters = new QWidget(this);
    m_filters->setObjectName("catalogFilters");
    auto *filters = new QHBoxLayout(m_filters);
    filters->setContentsMargins(8, 0, 8, 6);
    filters->setSpacing(8);
    m_types = new ElaComboBox(m_filters);
    m_types->setObjectName("typeCombo");
    m_types->setAccessibleName(QStringLiteral("Asset type"));
    m_types->setFixedHeight(30);
    for (const auto &category : QStringList{"", "module", "ip", "project", "artifact", "other"})
        m_types->addItem(category.isEmpty() ? QStringLiteral("All types")
                                           : SnapshotLibrary::categoryLabel(category), category);
    filters->addWidget(m_types);
    m_indexes = new ElaComboBox(m_filters);
    m_indexes->setObjectName("indexCombo");
    m_indexes->setAccessibleName(QStringLiteral("Index"));
    m_indexes->setFixedHeight(30);
    m_indexes->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_indexes->setMinimumContentsLength(8);
    m_indexes->addItem(QStringLiteral("All indexes"), QString());
    filters->addWidget(m_indexes, 1);
    auto *clearFilters = new ElaToolButton(m_filters);
    clearFilters->setFont(uiFont());
    clearFilters->setObjectName("clearFiltersButton");
    clearFilters->setText(QStringLiteral("Clear"));
    filters->addWidget(clearFilters);
    layout->addWidget(m_filters);
    m_filters->hide();
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName("assetSplitter");
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_catalog = new QWidget(m_splitter);
    m_catalog->setObjectName("catalogSidebar");
    auto *catalogLayout = new QVBoxLayout(m_catalog);
    catalogLayout->setContentsMargins(8, 6, 8, 6);
    catalogLayout->setSpacing(2);
    auto *libraryActions = new QHBoxLayout;
    libraryActions->setSpacing(2);
    m_folder = actionButton(m_catalog, "folderButton", UiIcon::Folder, QStringLiteral("My IPs"),
        QStringLiteral("Choose the folder for your IP catalog"));
    m_folder->setAccessibleName(QStringLiteral("Choose library folder"));
    m_folder->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_folder->setMinimumWidth(72);
    m_folder->setMaximumWidth(QWIDGETSIZE_MAX);
    m_folder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    libraryActions->addWidget(m_folder, 1);
    m_refresh = actionButton(m_catalog, "refreshButton", UiIcon::Refresh, QStringLiteral("Refresh"),
        QStringLiteral("Refresh library (F5)"));
    libraryActions->addWidget(m_refresh);
    catalogLayout->addLayout(libraryActions);
    auto *libraryDivider = new QWidget(m_catalog);
    libraryDivider->setObjectName("libraryDivider");
    libraryDivider->setFixedHeight(1);
    catalogLayout->addWidget(libraryDivider);
    auto *groupHeader = new QHBoxLayout;
    groupHeader->addWidget(new ElaText(QStringLiteral("Groups"), 12, m_catalog));
    groupHeader->addStretch();
    m_hideEmptyGroups = actionButton(m_catalog, "hideEmptyGroupsButton", UiIcon::Filter,
        QStringLiteral("Hide empty groups"));
    m_hideEmptyGroups->setCheckable(true);
    groupHeader->addWidget(m_hideEmptyGroups);
    connect(m_hideEmptyGroups, &QToolButton::toggled, this, [this](bool hidden)
    {
        m_hideEmptyGroups->setIsSelected(hidden);
        m_hideEmptyGroups->setToolTip(hidden ? QStringLiteral("Show empty groups") : QStringLiteral("Hide empty groups"));
        filter();
    });
    m_newGroup = actionButton(m_catalog, "newGroupButton", UiIcon::FolderPlus, QStringLiteral("New group"),
        QStringLiteral("New group (Ctrl+Shift+N)"));
    groupHeader->addWidget(m_newGroup);
    catalogLayout->addLayout(groupHeader);
    m_list = new ElaTreeView(m_catalog);
    m_list->setObjectName("assetList");
    m_list->setItemHeight(28);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::DragDrop);
    m_list->setDefaultDropAction(Qt::CopyAction);
    m_list->setDropIndicatorShown(true);
    m_list->setAutoExpandDelay(600);
    m_list->setUniformRowHeights(true);
    m_list->setHeaderHidden(true);
    m_list->setIndentation(16);
    m_list->setNativeItemContent(true);
    m_list->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet({});
    enableSmoothScrolling(m_list);
    enableToolTip(m_list);
    m_model = new CatalogModel(this);
    m_list->setModel(m_model);
    catalogLayout->addWidget(m_list, 1);
    if (m_embedded)
    {
        m_detailScroll = new ElaScrollArea(m_splitter);
        m_detailScroll->setObjectName("assetDetailScroll");
        m_detailScroll->setFrameShape(QFrame::NoFrame);
        m_detailScroll->setWidgetResizable(true);
        m_detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_detailScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_detailScroll->setIsAnimation(Qt::Vertical, false);
    }
    m_details = new QWidget(m_detailScroll ? nullptr : m_splitter);
    if (m_detailScroll) m_detailScroll->setWidget(m_details);
    m_details->setFont(font());
    m_details->setObjectName("assetDetails");
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(12, 6, 12, 8);
    details->setSpacing(4);
    auto *identity = new QHBoxLayout;
    m_identityLayout = identity;
    m_name = new ElaText(QStringLiteral("Select an asset"), 15, m_details);
    m_name->setObjectName("assetName");
    m_name->setWordWrap(true);
    m_name->setTextFormat(Qt::PlainText);
    auto nameFont = m_name->font();
    nameFont.setWeight(QFont::DemiBold);
    m_name->setFont(nameFont);
    m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    enableToolTip(m_name);
    identity->addWidget(m_name, 1);
    m_update = actionButton(m_details, "updateButton", UiIcon::Archive, QStringLiteral("Create version"));
    m_update->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_update->setFixedWidth(132);
    auto *identityActions = new QWidget(m_details);
    m_identityActions = new QHBoxLayout(identityActions);
    m_identityActions->setContentsMargins(0, 0, 0, 0);
    m_identityActions->addWidget(m_update);
    m_take = new ElaPushButton(QStringLiteral("Copy to project"), m_details);
    m_take->setObjectName("takeButton");
    m_take->setFont(font());
    m_take->setFixedHeight(30);
    m_take->setIcon(uiIcon(UiIcon::Copy, true));
    m_take->setIconSize(QSize(14, 14));
    m_take->setMinimumWidth(m_take->fontMetrics().horizontalAdvance(m_take->text()) + 44);
    primaryButton(m_take);
    enableToolTip(m_take);
    m_identityActions->addWidget(m_take);
    identity->addWidget(identityActions);
    details->addLayout(identity);
    m_description = new ElaText(m_details);
    m_description->setTextPixelSize(12);
    m_description->setWordWrap(true);
    m_description->setTextFormat(Qt::PlainText);
    m_description->setMaximumHeight(36);
    m_description->setThemeColorEnabled(false);
    enableToolTip(m_description);
    details->addWidget(m_description);
    auto *assetActions = new QWidget(m_details);
    assetActions->setObjectName("assetActions");
    assetActions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_actionLayout = new ElaFlowLayout(assetActions, 0, 2, 2);
    m_actionLayout->setIsAnimation(false);
    m_openFolder = actionButton(assetActions, "openSourceButton", UiIcon::Folder, QStringLiteral("Source folder"));
    m_reference = actionButton(assetActions, "referenceButton", UiIcon::Reference, QStringLiteral("Reference"),
        QStringLiteral("Reference this IP in another library or project without copying its files"));
    m_edit = actionButton(assetActions, "editAssetButton", UiIcon::Edit, QStringLiteral("Edit details"));
    m_remove = actionButton(assetActions, "removeSourceButton", UiIcon::Unlink, QStringLiteral("Unregister"));
    m_changeReference = actionButton(assetActions, "changeReferenceButton", UiIcon::Reference, QStringLiteral("Change reference"));
    m_deleteAsset = actionButton(assetActions, "deleteAssetButton", UiIcon::Trash, QStringLiteral("Delete asset"));
    m_renameGroup = actionButton(assetActions, "renameGroupButton", UiIcon::Edit, QStringLiteral("Rename group"));
    m_deleteGroup = actionButton(assetActions, "deleteGroupButton", UiIcon::Trash, QStringLiteral("Delete group"));
    details->addWidget(assetActions);
    m_groupItems = new ElaListView(m_details);
    m_groupItems->setObjectName("groupMembers");
    m_groupItems->setAccessibleName(QStringLiteral("IPs and modules in this group"));
    m_groupItems->setItemHeight(28);
    m_groupItems->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_groupItems->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupItems->setUniformItemSizes(true);
    m_groupItems->setIsTransparent(true);
    m_groupItems->setFrameShape(QFrame::NoFrame);
    m_groupItems->setModel(m_model);
    m_groupItems->setItemDelegate(detailDelegate(false, m_groupItems));
    enableSmoothScrolling(m_groupItems);
    enableToolTip(m_groupItems);
    details->addWidget(m_groupItems, 1);
    const auto openGroupMember = [this](const QModelIndex &index)
    {
        if (m_model->assetIndex(index) < 0) return;
        m_list->expand(index.parent());
        ElaTreeView::finishExpansion(m_list);
        m_list->setCurrentIndex(index);
        m_list->scrollTo(index);
        m_list->setFocus();
    };
    connect(m_groupItems, &QAbstractItemView::clicked, this, openGroupMember);
    connect(m_groupItems, &QAbstractItemView::activated, this, openGroupMember);
    m_pages = new ElaTabWidget(m_details);
    m_pages->setFont(font());
    m_pages->setObjectName("assetPages");
    m_pages->setTabSize(QSize(116, 28));
    m_pages->setTabsClosable(false);
    m_pages->setMovable(false);
    m_pages->setIsTabTransparent(true);
    m_pages->setIsContainerAcceptDrops(false);
    details->addWidget(m_pages, 1);
    m_workingPage = new QWidget(m_pages);
    m_workingPage->setFont(font());
    m_workingPage->setObjectName("workingPage");
    m_workingPage->setAcceptDrops(true);
    m_workingPage->installEventFilter(this);
    auto *workingLayout = new QVBoxLayout(m_workingPage);
    workingLayout->setContentsMargins(0, 4, 0, 0);
    workingLayout->setSpacing(4);
    auto *workingActions = new QHBoxLayout;
    m_workingActions = workingActions;
    workingActions->setSpacing(4);
    auto *imports = new QWidget(m_workingPage);
    auto *importActions = new QHBoxLayout(imports);
    importActions->setContentsMargins(0, 0, 0, 0);
    importActions->setSpacing(4);
    workingActions->addWidget(imports);
    m_addFiles = actionButton(m_workingPage, "addFilesButton", UiIcon::Add, QStringLiteral("Add files"));
    m_addFolder = actionButton(m_workingPage, "addFolderButton", UiIcon::FolderPlus, QStringLiteral("Add folder"));
    for (auto *button : {m_addFiles, m_addFolder})
    {
        button->setFont(font());
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setFixedWidth(button->fontMetrics().horizontalAdvance(button->text()) + 34);
        importActions->addWidget(button);
    }
    workingActions->addStretch();
    auto *selection = new QWidget(m_workingPage);
    auto *selectionActions = new QHBoxLayout(selection);
    selectionActions->setContentsMargins(0, 0, 0, 0);
    selectionActions->setSpacing(4);
    workingActions->addWidget(selection);
    m_checkAll = new ElaCheckBox(QStringLiteral("All"), m_workingPage);
    m_checkAll->setFont(uiFont(15));
    m_checkAll->setObjectName("checkAllFiles");
    selectionActions->addWidget(m_checkAll);
    m_openWorking = actionButton(m_workingPage, "openWorkingButton", UiIcon::Open, QStringLiteral("Edit file"),
        QStringLiteral("Open the original file (double-click or Enter)"));
    selectionActions->addWidget(m_openWorking);
    workingLayout->addLayout(workingActions);
    m_workingEmpty = new ElaText(QStringLiteral("Add files or a folder to begin."), 12, m_workingPage);
    m_workingEmpty->setObjectName("workingEmpty");
    m_workingEmpty->setWordWrap(true);
    m_workingEmpty->setAcceptDrops(true);
    m_workingEmpty->installEventFilter(this);
    workingLayout->addWidget(m_workingEmpty);
    m_workingFiles = new ElaTreeView(m_workingPage);
    m_workingFiles->setObjectName("workingFiles");
    m_workingFiles->setAccessibleName(QStringLiteral("Files to include in the next version"));
    m_workingFiles->setNativeItemContent(true);
    m_workingFiles->setItemHeight(28);
    m_workingFiles->setHeaderHidden(true);
    m_workingFiles->setUniformRowHeights(true);
    m_workingFiles->setIndentation(16);
    m_workingFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_workingFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    m_workingFiles->setFrameShape(QFrame::NoFrame);
    m_workingFiles->setDragDropMode(QAbstractItemView::DropOnly);
    m_workingFiles->setDropIndicatorShown(false);
    m_workingFiles->setAcceptDrops(true);
    m_workingFiles->installEventFilter(this);
    m_workingFiles->viewport()->setAcceptDrops(true);
    m_workingFiles->viewport()->installEventFilter(this);
    m_workingModel = new WorkingFilesModel(this);
    m_workingFiles->setModel(m_workingModel);
    enableSmoothScrolling(m_workingFiles);
    enableToolTip(m_workingFiles);
    workingLayout->addWidget(m_workingFiles, 1);
    m_pages->addTab(m_workingPage, QStringLiteral("Working files"));
    m_historyPage = new QWidget(m_pages);
    m_historyPage->setFont(font());
    auto *historyLayout = new QVBoxLayout(m_historyPage);
    historyLayout->setContentsMargins(0, 4, 0, 0);
    historyLayout->setSpacing(4);
    m_historyEmpty = new ElaText(QStringLiteral("No saved versions."), 12, m_historyPage);
    m_historyEmpty->setObjectName("historyEmpty");
    historyLayout->addWidget(m_historyEmpty);
    m_pages->addTab(m_historyPage, QStringLiteral("Versions"));
    m_versionSection = new QWidget(m_historyPage);
    m_versionSection->setObjectName("versionSection");
    auto *versionLayout = new QVBoxLayout(m_versionSection);
    versionLayout->setContentsMargins(0, 4, 0, 0);
    versionLayout->setSpacing(0);
    auto *versionHeader = new QHBoxLayout;
    m_versionHeader = versionHeader;
    versionHeader->setContentsMargins(7, 0, 0, 0);
    versionHeader->setSpacing(0);
    auto *versionLabels = new QWidget(m_versionSection);
    auto *labels = new QHBoxLayout(versionLabels);
    labels->setContentsMargins(0, 0, 0, 0);
    labels->setSpacing(0);
    auto *versionLabel = new ElaText(QStringLiteral("Version"), 12, m_versionSection);
    versionLabel->setFixedWidth(104);
    labels->addWidget(versionLabel);
    labels->addWidget(new ElaText(QStringLiteral("Status"), 12, m_versionSection), 1);
    versionHeader->addWidget(versionLabels, 1);
    auto *versionActions = new QWidget(m_versionSection);
    versionActions->setObjectName("versionActions");
    auto *versionButtons = new QHBoxLayout(versionActions);
    versionButtons->setContentsMargins(0, 0, 0, 0);
    versionButtons->setSpacing(4);
    m_renameRevision = actionButton(versionActions, "renameRevisionButton", UiIcon::Edit, QStringLiteral("Edit version"),
        QStringLiteral("Edit the selected version name (double-click the version or press F2)"));
    m_deleteRevision = actionButton(versionActions, "deleteRevisionButton", UiIcon::Trash, QStringLiteral("Delete version"));
    for (auto *button : {m_renameRevision, m_deleteRevision})
    {
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setFixedWidth(button->fontMetrics().horizontalAdvance(button->text()) + 34);
        versionButtons->addWidget(button);
    }
    versionHeader->addWidget(versionActions);
    versionLayout->addLayout(versionHeader);
    m_versions = new ElaTableView(m_versionSection);
    m_versions->setObjectName("revisionTable");
    m_versions->setAccessibleName(QStringLiteral("Versions and status"));
    m_versions->setNativeItemContent(true);
    m_versions->setHeaderMargin(0);
    m_versions->setFrameShape(QFrame::NoFrame);
    m_versions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_versions->setSelectionMode(QAbstractItemView::SingleSelection);
    m_versions->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_versions->verticalHeader()->hide();
    m_versions->verticalHeader()->setDefaultSectionSize(28);
    m_versions->verticalHeader()->setMinimumSectionSize(28);
    m_versions->horizontalHeader()->hide();
    m_versions->horizontalHeader()->setMinimumSectionSize(36);
    m_versions->horizontalHeader()->setSectionsClickable(false);
    m_versions->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_versions->horizontalHeader()->setStretchLastSection(true);
    m_versions->setMinimumHeight(28);
    m_versions->setMaximumHeight(168);
    m_versions->setShowGrid(false);
    m_versionModel = new QStandardItemModel(0, 2, this);
    m_versionModel->setHorizontalHeaderLabels({QStringLiteral("Version"), QStringLiteral("Status")});
    m_versions->setModel(m_versionModel);
    m_versions->setColumnWidth(0, 104);
    m_versions->setItemDelegate(detailDelegate(true, m_versions));
    enableSmoothScrolling(m_versions);
    enableToolTip(m_versions);
    versionLayout->addWidget(m_versions);
    historyLayout->addWidget(m_versionSection);
    m_fileSection = new QWidget(m_historyPage);
    m_fileSection->setObjectName("fileSection");
    auto *fileLayout = new QVBoxLayout(m_fileSection);
    fileLayout->setContentsMargins(0, 4, 0, 0);
    fileLayout->setSpacing(0);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(7, 0, 0, 0);
    actions->setSpacing(4);
    m_fileHeading = new ElaText(QStringLiteral("Files"), m_fileSection);
    m_fileHeading->setObjectName("versionHeading");
    m_fileHeading->setTextPixelSize(12);
    m_fileHeading->setTextFormat(Qt::PlainText);
    m_fileHeading->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_fileHeading->setThemeColorEnabled(false);
    enableToolTip(m_fileHeading);
    actions->addWidget(m_fileHeading, 1);
    m_openFile = actionButton(m_fileSection, "openFileButton", UiIcon::Open, QStringLiteral("Open file"),
        QStringLiteral("Open the selected file (double-click or Enter)"));
    actions->addWidget(m_openFile);
    fileLayout->addLayout(actions);
    m_files = new ElaListView(m_fileSection);
    m_files->setObjectName("fileList");
    m_files->setItemHeight(28);
    m_files->setMinimumHeight(28);
    m_files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_files->setUniformItemSizes(true);
    m_files->setIsTransparent(true);
    m_files->setFrameShape(QFrame::NoFrame);
    m_files->setStyleSheet({});
    enableSmoothScrolling(m_files);
    enableToolTip(m_files);
    m_fileModel = new QStringListModel(this);
    m_files->setModel(m_fileModel);
    m_files->setItemDelegate(detailDelegate(false, m_files));
    connect(m_files, &QAbstractItemView::doubleClicked, this, [this] { openFile(); });
    connect(m_files->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]
        { m_openFile->setEnabled(!m_busy && m_files->currentIndex().isValid()); });
    fileLayout->addWidget(m_files, 1);
    historyLayout->addWidget(m_fileSection, 1);
    historyLayout->addStretch();
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);
    m_status =
        new ElaText(QStringLiteral("Choose a catalog folder, then create a Module, IP or Project"), this);
    m_status->setObjectName("browserNotice");
    m_status->setTextPixelSize(12);
    m_status->setFixedHeight(18);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(false);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_status->setToolTip(m_status->text());
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setThemeColorEnabled(false);
    enableToolTip(m_status);
    m_feedback = new QWidget(this);
    m_feedback->setObjectName("operationFeedback");
    auto *feedback = new QHBoxLayout(m_feedback);
    feedback->setContentsMargins(8, 2, 8, 4);
    feedback->setSpacing(4);
    m_activity = new ElaProgressRing(this);
    m_activity->setObjectName("browserActivity");
    m_activity->setFixedSize(16, 16);
    m_activity->setBusyingWidth(2);
    m_activity->setIsDisplayValue(false);
    m_activity->setIsTransparent(true);
    m_activity->setAccessibleName(QStringLiteral("Operation in progress"));
    feedback->addWidget(m_activity);
    feedback->addWidget(m_status, 1);
    m_issues = actionButton(m_feedback, "issuesButton", UiIcon::Warning, QStringLiteral("Issues"));
    feedback->addWidget(m_issues);
    m_receipt = actionButton(m_feedback, "saveReceiptButton", UiIcon::Receipt, QStringLiteral("Save receipt"),
        QStringLiteral("Save the origin and exact version of the last export"));
    feedback->addWidget(m_receipt);
    m_cancel = new ElaPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setObjectName("cancelOperation");
    m_cancel->setFixedHeight(24);
    m_cancel->hide();
    feedback->addWidget(m_cancel);
    connect(m_cancel, &QPushButton::clicked, this, [this]
    {
        if (m_operation && m_operation->cancel()) notice(QStringLiteral("Cancelling before publication…"));
        m_cancel->setEnabled(false);
    });
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(120);
    connect(m_progressTimer, &QTimer::timeout, this, [this]
    {
        if (!m_busy || !m_operation || m_backgroundRefresh) return;
        const auto status = m_operation->status();
        if (!status.isEmpty() && m_operation->state.load() != OperationControl::CancelRequested) notice(status);
        m_cancel->setEnabled(m_operation->state.load() == OperationControl::Running);
    });
    layout->addWidget(m_feedback);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, &BrowserPanel::filter);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &BrowserPanel::selectCurrent);
    connect(m_versions->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &BrowserPanel::selectVersion);
    connect(m_versions, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &index)
    { if (index.column() == 0) renameVersion(); });
    connect(m_renameRevision, &QToolButton::clicked, this, &BrowserPanel::renameVersion);
    connect(m_pages, &QTabWidget::currentChanged, this, [this] { updateActions(); });
    connect(m_workingModel, &WorkingFilesModel::checkedFilesChanged, this, &BrowserPanel::rememberChecks);
    connect(m_checkAll, &QCheckBox::clicked, this, [this](bool checked) { m_workingModel->checkAll(checked); });
    connect(m_workingFiles->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateActions(); });
    connect(m_workingFiles, &QTreeView::collapsed, this, [this](const QModelIndex &index)
    {
        if (!m_restoringWorkingView && !m_displayedAsset.isEmpty())
            m_workingViews[m_displayedAsset].expanded.remove(index.data(Qt::UserRole).toString());
    });
    connect(m_workingFiles, &QTreeView::expanded, this, [this](const QModelIndex &index)
    {
        if (!m_restoringWorkingView && !m_displayedAsset.isEmpty())
            m_workingViews[m_displayedAsset].expanded.insert(index.data(Qt::UserRole).toString());
    });
    connect(m_workingFiles, &QAbstractItemView::doubleClicked, this, [this] { openFile(); });
    connect(m_openWorking, &QToolButton::clicked, this, &BrowserPanel::openFile);
    connect(m_addFiles, &QToolButton::clicked, this, [this]
    {
        const auto paths = ElaFilePicker::getOpenFileNames(this, QStringLiteral("Add files"), m_workspace);
        if (!paths.isEmpty()) importWorkingFiles(paths);
    });
    connect(m_addFolder, &QToolButton::clicked, this, [this]
    {
        const auto path = ElaFilePicker::getExistingDirectory(this, QStringLiteral("Add folder"), m_workspace);
        if (!path.isEmpty()) importWorkingFiles({path});
    });
    connect(m_new, &QPushButton::clicked, this, &BrowserPanel::createAsset);
    connect(m_newGroup, &QToolButton::clicked, this, &BrowserPanel::createGroup);
    connect(m_list, &QWidget::customContextMenuRequested, this, &BrowserPanel::groupMenu);
    connect(m_model, &CatalogModel::groupMembershipRequested, this,
        [this](const QString &group, const QString &asset)
    {
        const auto library = m_library;
        runGroup([library, group, asset] { return CatalogGroups::setMember(library, group, asset, true); }, asset);
    });
    connect(m_list, &QTreeView::collapsed, this, [this](const QModelIndex &index)
    {
        m_collapsedGroups.insert(m_model->groupId(index));
        if (m_list->currentIndex().parent() == index) m_list->setCurrentIndex(index);
    });
    connect(m_list, &QTreeView::expanded, this, [this](const QModelIndex &index)
    { m_collapsedGroups.remove(m_model->groupId(index)); });
    connect(m_filterToggle, &QToolButton::toggled, this, [this](bool expanded)
    {
        m_filters->setVisible(expanded);
        m_filterToggle->setIsSelected(expanded || !m_category.isEmpty() || !m_indexTerm.isEmpty());
    });
    connect(m_types, &QComboBox::currentIndexChanged, this, [this]
    {
        m_category = m_types->currentData().toString();
        filter();
    });
    connect(clearFilters, &QToolButton::clicked, this, [this]
    {
        const QSignalBlocker types(m_types), indexes(m_indexes);
        m_types->setCurrentIndex(0);
        m_indexes->setCurrentIndex(0);
        m_category.clear();
        m_indexTerm.clear();
        filter();
    });
    connect(m_indexes, &QComboBox::currentIndexChanged, this, [this]
    {
        m_indexTerm = m_indexes->currentData().toString();
        filter();
    });
    connect(m_folder, &QToolButton::clicked, this, &BrowserPanel::chooseLibrary);
    connect(m_take, &QPushButton::clicked, this, &BrowserPanel::exportAsset);
    connect(m_update, &QToolButton::clicked, this, &BrowserPanel::updateAsset);
    connect(m_collect, &QToolButton::clicked, this, &BrowserPanel::addSources);
    connect(m_refresh, &QToolButton::clicked, this, &BrowserPanel::refresh);
    connect(m_openFile, &QToolButton::clicked, this, &BrowserPanel::openFile);
    connect(m_openFolder, &QToolButton::clicked, this, &BrowserPanel::openSourceFolder);
    connect(m_reference, &QToolButton::clicked, this, &BrowserPanel::referenceAsset);
    connect(m_edit, &QToolButton::clicked, this, &BrowserPanel::detailsDialog);
    connect(m_remove, &QToolButton::clicked, this, &BrowserPanel::removeMembership);
    connect(m_changeReference, &QToolButton::clicked, this, &BrowserPanel::changeReference);
    connect(m_deleteRevision, &QToolButton::clicked, this, [this] { deleteAsset(false); });
    connect(m_deleteAsset, &QToolButton::clicked, this, [this] { deleteAsset(true); });
    connect(m_issues, &QToolButton::clicked, this, &BrowserPanel::showIssues);
    connect(m_receipt, &QToolButton::clicked, this, &BrowserPanel::saveOrigin);
    connect(m_renameGroup, &QToolButton::clicked, this, [this] { renameGroup(m_activeGroup); });
    connect(m_deleteGroup, &QToolButton::clicked, this, [this]
    {
        const auto library = m_library, group = m_activeGroup;
        if (!m_busy && !group.isEmpty()) runGroup([library, group] { return CatalogGroups::erase(library, group); });
    });
    connect(m_theme, &QToolButton::clicked, this, [this](bool dark)
    {
        eTheme->setThemeMode(dark ? ElaThemeType::Dark : ElaThemeType::Light);
        QSettings("xIPs", "xIPs").setValue("ui/dark", dark);
    });
    const auto shortcut = [this](const QKeySequence &keys, QWidget *scope, auto operation)
    {
        auto *key = new QShortcut(keys, scope);
        key->setContext(Qt::WidgetWithChildrenShortcut);
        key->setAutoRepeat(false);
        connect(key, &QShortcut::activated, this, operation);
    };
    shortcut(QKeySequence::Find, this, [this]
    {
        if (!m_busy) { m_search->setFocus(); m_search->selectAll(); }
    });
    shortcut(QKeySequence::New, this, [this] { m_new->click(); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")), this, [this] { m_newGroup->click(); });
    shortcut(QKeySequence::Save, this, [this] { m_update->click(); });
    shortcut(QKeySequence(Qt::Key_F5), this, [this] { refresh(); });
    shortcut(QKeySequence(Qt::Key_Escape), m_search, [this] { m_search->clear(); });
    shortcut(QKeySequence(Qt::Key_Return), m_files, [this] { openFile(); });
    shortcut(QKeySequence(Qt::Key_Enter), m_files, [this] { openFile(); });
    shortcut(QKeySequence(Qt::Key_Return), m_workingFiles, [this] { openFile(); });
    shortcut(QKeySequence(Qt::Key_Enter), m_workingFiles, [this] { openFile(); });
    shortcut(QKeySequence(Qt::Key_F2), m_versions, [this] { renameVersion(); });
    shortcut(QKeySequence(Qt::Key_F2), m_list, [this]
    {
        const auto index = m_list->currentIndex();
        if (m_model->assetIndex(index) < 0) renameGroup(m_model->groupId(index));
    });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        (m_splitter->orientation() == Qt::Horizontal ? m_horizontalRatio : m_verticalRatio) = splitRatio();
    });
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this] { applyTheme(); });
    m_catalogWatcher = new CatalogWatcher(this);
    connect(m_catalogWatcher, &CatalogWatcher::refreshRequested, this,
        [this](bool full, const QStringList &ids) { refreshCatalog(true, full, ids); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state)
    {
        if (state == Qt::ApplicationActive && isVisible() && m_contextInitialized && !m_library.isEmpty())
            m_catalogWatcher->checkNow();
    });
    adaptEmbeddedLayout();
    applyTheme();
    setBusy(false);
}
BrowserPanel::~BrowserPanel()
{
    if (m_detailOperation) m_detailOperation->cancel();
    if (m_operation) m_operation->cancel();
}
void BrowserPanel::beginOperation()
{
    if (m_detailOperation) m_detailOperation->cancel();
    m_operation = std::make_shared<OperationControl>();
}
void BrowserPanel::setDarkTheme(bool dark)
{
    eTheme->setThemeMode(dark ? ElaThemeType::Dark : ElaThemeType::Light);
}
void BrowserPanel::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
    m_theme->setChecked(mode == ElaThemeType::Dark);
    m_theme->setIsSelected(mode == ElaThemeType::Dark);
    auto background = eTheme->getThemeColor(mode, ElaThemeType::WindowCentralStackBase);
    const auto base = eTheme->getThemeColor(mode, ElaThemeType::WindowBase);
    const auto blend = [alpha = background.alphaF()](int foreground, int behind) {
        return qRound(foreground * alpha + behind * (1.0 - alpha));
    };
    background = QColor(blend(background.red(), base.red()), blend(background.green(), base.green()),
                        blend(background.blue(), base.blue()));
    const auto text = eTheme->getThemeColor(mode, ElaThemeType::BasicText);
    const auto secondary = eTheme->getThemeColor(mode, ElaThemeType::BasicDetailsText);
    const auto border = eTheme->getThemeColor(mode, ElaThemeType::BasicBorder);
    const QColor sidebar(mode == ElaThemeType::Dark ? "#1c1e22" : "#f6f7f9");
    auto p = palette();
    p.setColor(QPalette::Window, background);
    p.setColor(QPalette::Base, background);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::PlaceholderText, secondary);
    p.setColor(QPalette::Highlight, eTheme->getThemeColor(mode, ElaThemeType::BasicSelectedAlpha));
    p.setColor(QPalette::HighlightedText, text);
    setPalette(p);
    if (m_detailScroll)
    {
        m_detailScroll->setPalette(p);
        m_detailScroll->viewport()->setPalette(p);
        m_detailScroll->viewport()->setAutoFillBackground(true);
    }
    setAutoFillBackground(true);
    setStyleSheet(QStringLiteral("QWidget#xipsBrowser, QWidget#assetDetails { background: %1; } "
        "QWidget#catalogSidebar { background: %3; } "
        "QSplitter#assetSplitter::handle, QWidget#libraryDivider { background: %2; } "
        "QWidget#versionSection, QWidget#fileSection { border-top: 1px solid %2; }")
        .arg(background.name(), border.name(), sidebar.name()));
    for (auto *view : {static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_groupItems),
                       static_cast<QAbstractItemView *>(m_files),
                       static_cast<QAbstractItemView *>(m_workingFiles),
                       static_cast<QAbstractItemView *>(m_versions)})
    {
        view->setFont(font());
        view->viewport()->setFont(font());
        auto viewPalette = p;
        if (view == m_list) { viewPalette.setColor(QPalette::Base, sidebar); viewPalette.setColor(QPalette::Window, sidebar); }
        view->setPalette(viewPalette);
        view->viewport()->setPalette(viewPalette);
        view->viewport()->setAutoFillBackground(true);
        view->viewport()->update();
    }
    m_pages->setFont(font());
    m_pages->tabBar()->setFont(font());
    for (auto *label : findChildren<ElaText *>())
    {
        auto labelFont = label->font();
        labelFont.setFamily(font().family());
        label->setFont(labelFont);
    }
    auto headerPalette = p;
    headerPalette.setColor(QPalette::Button, background);
    headerPalette.setColor(QPalette::ButtonText, secondary);
    m_versions->horizontalHeader()->setPalette(headerPalette);
    m_versions->horizontalHeader()->setStyleSheet(QStringLiteral(
        "QHeaderView::section { background: %1; color: %2; border: none; "
        "border-bottom: 1px solid %3; padding: 0 3px; }")
        .arg(background.name(), secondary.name(), border.name()));
    for (auto *label : {m_status, m_description, m_fileHeading})
    {
        auto labelPalette = label->palette();
        labelPalette.setColor(QPalette::WindowText, label == m_status && m_noticeError
            ? QColor(mode == ElaThemeType::Dark ? "#ffaaa0" : "#a12828") : secondary);
        label->setPalette(labelPalette);
    }
}
double BrowserPanel::splitRatio() const
{
    const auto sizes = m_splitter->sizes();
    const int total = sizes.value(0) + sizes.value(1);
    return total > 0 ? qBound(0.1, double(sizes.value(0)) / total, 0.9) : 0.5;
}
void BrowserPanel::restoreSplit()
{
    const double ratio = m_splitter->orientation() == Qt::Horizontal ? m_horizontalRatio : m_verticalRatio;
    m_splitter->setSizes({qRound(ratio * 10000), qRound((1.0 - ratio) * 10000)});
}
void BrowserPanel::adaptEmbeddedLayout()
{
    if (!m_embedded) return;
    const bool narrow = width() < 580;
    const bool stacked = width() < 360;
    m_identityLayout->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_identityLayout->setStretch(0, narrow ? 0 : 1);
    m_identityActions->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_identityLayout->setAlignment(m_identityActions->parentWidget(), narrow ? Qt::AlignLeft : Qt::Alignment());
    m_identityActions->setAlignment(m_update, narrow ? Qt::AlignLeft : Qt::Alignment());
    m_identityActions->setAlignment(m_take, narrow ? Qt::AlignLeft : Qt::Alignment());
    m_workingActions->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_workingActions->setStretch(1, stacked ? 0 : 1);
    m_workingActions->setAlignment(m_workingActions->itemAt(0)->widget(), stacked ? Qt::AlignLeft : Qt::Alignment());
    m_workingActions->setAlignment(m_workingActions->itemAt(2)->widget(), stacked ? Qt::AlignLeft : Qt::Alignment());
    m_renameRevision->setToolButtonStyle(stacked ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    m_renameRevision->setFixedWidth(stacked ? 28 : m_renameRevision->fontMetrics().horizontalAdvance(m_renameRevision->text()) + 34);
    m_versionHeader->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_versionHeader->setStretch(0, stacked ? 0 : 1);
    m_versionHeader->setAlignment(m_deleteRevision->parentWidget(), stacked ? Qt::AlignLeft : Qt::Alignment());
    updateVersionHeight();
}
void BrowserPanel::updateVersionHeight()
{
    m_versionSection->setMaximumHeight(m_versions->maximumHeight() +
        (m_embedded ? m_versionHeader->sizeHint().height() + 4 : 32));
}
void BrowserPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    adaptEmbeddedLayout();
    const auto orientation = width() < 580 ? Qt::Vertical : Qt::Horizontal;
    if (m_splitter->orientation() != orientation)
    {
        m_splitter->setOrientation(orientation);
        restoreSplit();
    }
    m_status->setText(m_status->fontMetrics().elidedText(m_status->toolTip().simplified(),
                                                       Qt::ElideRight, m_status->width()));
}
void BrowserPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    restoreSplit();
    updateActivity();
    if (m_contextInitialized && !m_busy && !m_library.isEmpty()) m_catalogWatcher->checkNow();
}
void BrowserPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    updateActivity();
}
void BrowserPanel::dragEnterEvent(QDragEnterEvent *event)
{
    handleFileDrop(event, overWorkingPage(event->position().toPoint()), false);
}
void BrowserPanel::dragMoveEvent(QDragMoveEvent *event)
{
    handleFileDrop(event, overWorkingPage(event->position().toPoint()), false);
}
void BrowserPanel::dropEvent(QDropEvent *event)
{
    handleFileDrop(event, overWorkingPage(event->position().toPoint()), true);
}
bool BrowserPanel::overWorkingPage(const QPoint &position) const
{
    return m_workingPage->isVisible() &&
        m_workingPage->rect().contains(m_workingPage->mapFrom(this, position));
}
void BrowserPanel::handleFileDrop(QDropEvent *event, bool working, bool import)
{
    event->ignore();
    if (m_busy || !event->possibleActions().testFlag(Qt::CopyAction)) return;
    QStringList paths;
    for (const auto &url : event->mimeData()->urls())
    {
        if (!url.isLocalFile() || url.toLocalFile().isEmpty()) return;
        paths.append(url.toLocalFile());
    }
    if (paths.isEmpty()) return;
    const bool folders = std::any_of(paths.cbegin(), paths.cend(), [](const auto &path) { return QFileInfo(path).isDir(); });
    if (!folders && working && (m_pages->currentIndex() != 0 || !m_addFiles->isEnabled())) return;
    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (!import) return;
    if (folders) importFolders(paths, working ? m_selected.id : QString());
    else if (working) importWorkingFiles(paths);
    else
    {
        if (m_library.isEmpty()) chooseLibrary();
        collectPaths(paths);
    }
}
void BrowserPanel::setContext(const QString &library, const QString &workspace)
{
    QString path = library.isEmpty() ? m_library : library;
    if (path.isEmpty())
    {
        path = qEnvironmentVariable("XIPS_LIBRARY");
        if (path.isEmpty()) path = QSettings("xIPs", "xIPs").value("library/root").toString();
    }
    path = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    if (m_contextInitialized && path == m_library && workspace == m_workspace && !m_pendingContext)
        return;
    emit contextChanging();
    if (m_busy)
    {
        m_pendingContext = qMakePair(path, workspace);
        m_pendingState.reset();
        return;
    }
    const bool sameLibrary = m_contextInitialized && path == m_library;
    m_contextInitialized = true;
    m_workspace = workspace;
    m_take->setToolTip(QStringLiteral("Copy the selected version's source files to a destination you choose; creates an independent copy"));
    if (sameLibrary) return;
    rememberWorkingView();
    m_catalogWatcher->clear();
    m_library = path;
    m_pendingId.clear();
    m_pendingRevision.clear();
    m_pendingPage = -1;
    m_displayedAsset.clear();
    m_activeGroup.clear();
    m_collapsedGroups.clear();
    m_assets.clear();
    m_model->setAssets({});
    m_selected = {};
    m_pendingDetail.reset();
    m_loadingDetails = false;
    ++m_generation;
    filter();
    refresh();
}
bool BrowserPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_workingPage || watched == m_workingEmpty || watched == m_workingFiles ||
        watched == m_workingFiles->viewport())
    {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)
        {
            handleFileDrop(static_cast<QDropEvent *>(event), true, event->type() == QEvent::Drop);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void BrowserPanel::importWorkingFiles(const QStringList &paths)
{
    if (m_busy || paths.isEmpty() || !m_addFiles->isEnabled()) return;
    const auto asset = m_selected;
    run(QStringLiteral("Adding files…"), [asset, paths] { return SnapshotLibrary::importFiles(asset, paths); },
        [this](const auto &result)
    {
        applyAssetResult(result, QStringLiteral("current"));
    }, true);
}
void BrowserPanel::importFolders(const QStringList &paths, const QString &preferredAsset)
{
    if (m_busy || paths.isEmpty()) return;
    if (m_library.isEmpty())
    {
        chooseLibrary();
        if (m_library.isEmpty()) return;
        auto *ready = new QTimer(this);
        connect(ready, &QTimer::timeout, this, [this, ready, paths, preferredAsset, library = m_library]
        {
            if (m_busy) return;
            ready->stop();
            ready->deleteLater();
            if (library == m_library) importFolders(paths, preferredAsset);
        });
        ready->start(50);
        return;
    }
    const auto library = m_library;
    run(QStringLiteral("Reading files to import…"), [paths] { return SnapshotLibrary::previewImport(paths); },
        [this, paths, preferredAsset, library](const auto &prepared)
    {
        if (m_pendingContext || m_pendingState || library != m_library) return;
        Form form(this, QStringLiteral("Import folder"), QStringLiteral("Import and archive"));
        form.setObjectName("folderImportForm");
        auto *destination = new ElaComboBox(&form);
        destination->setObjectName("importDestination");
        destination->addItem(QStringLiteral("Create new Module / IP / Project"), QString());
        QList<CatalogAsset> targets;
        for (const auto &asset : m_assets)
            if (QStringList{"module", "ip", "project"}.contains(asset.category) &&
                asset.discovered && asset.sourceIsDirectory && !asset.historyRoot.isEmpty() &&
                asset.referencePath.isEmpty() && asset.sourceProblem.isEmpty() && !asset.historyIncomplete)
            {
                targets.append(asset);
                destination->addItem(QStringLiteral("%1 (%2) · %3").arg(asset.name,
                    SnapshotLibrary::categoryLabel(asset.category), QDir(library).relativeFilePath(asset.root)), asset.id);
            }
        const int preferred = destination->findData(preferredAsset);
        if (preferred >= 0) destination->setCurrentIndex(preferred);
        form.body->addWidget(destination);
        auto *newEntry = new QWidget(&form);
        auto *newRow = new QHBoxLayout(newEntry);
        newRow->setContentsMargins(0, 0, 0, 0);
        auto *name = new EnglishLineEdit(newEntry);
        name->setObjectName("importName");
        name->setPlaceholderText(QStringLiteral("New entry name"));
        name->setText(QFileInfo(paths.first()).fileName());
        auto *type = new ElaComboBox(newEntry);
        type->setObjectName("importType");
        for (const auto &category : QStringList{"module", "ip", "project"})
            type->addItem(SnapshotLibrary::categoryLabel(category), category);
        newRow->addWidget(name, 1);
        newRow->addWidget(type);
        form.body->addWidget(newEntry);
        form.message(QStringLiteral("Choose files to import and include in this version."));
        auto *all = new ElaCheckBox(QStringLiteral("All files"), &form);
        all->setObjectName("importAllFiles");
        all->setChecked(true);
        form.body->addWidget(all);
        auto *tree = new ElaTreeView(&form);
        tree->setObjectName("importFiles");
        tree->setHeaderHidden(true);
        tree->setMinimumHeight(150);
        tree->setMaximumHeight(300);
        tree->setUniformRowHeights(true);
        auto *files = new WorkingFilesModel(tree);
        files->setFiles(prepared.preview.files,
            QSet<QString>(prepared.preview.files.cbegin(), prepared.preview.files.cend()), true);
        tree->setModel(files);
        tree->expandAll();
        form.body->addWidget(tree, 1);
        auto *transfer = new ElaComboBox(&form);
        transfer->setObjectName("importTransfer");
        transfer->addItem(QStringLiteral("Copy (keep source files)"), false);
        transfer->addItem(QStringLiteral("Move (remove selected source files after archival)"), true);
        form.body->addWidget(transfer);
        auto *versionRow = new QHBoxLayout;
        auto *version = new EnglishLineEdit(&form);
        version->setObjectName("importVersion");
        version->setMaxLength(128);
        version->setAccessibleName(QStringLiteral("Version"));
        version->setToolTip(QStringLiteral("Version name, e.g. v1.0.0. Leave empty for an automatic name."));
        versionRow->addWidget(version, 1);
        auto *note = new EnglishLineEdit(&form);
        note->setObjectName("importNote");
        note->setPlaceholderText(QStringLiteral("Version note (optional)"));
        versionRow->addWidget(note, 1);
        form.body->addLayout(versionRow);
        auto *summary = new ElaText(&form);
        summary->setObjectName("importSummary");
        summary->setTextPixelSize(13);
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        form.body->addWidget(summary);
        const auto update = [&]
        {
            const auto selected = files->checkedFiles();
            const bool creating = destination->currentData().toString().isEmpty();
            newEntry->setVisible(creating);
            qint64 sequence = 1;
            for (const auto &asset : targets)
                if (asset.id == destination->currentData().toString()) { sequence = asset.nextSequence; break; }
            version->setPlaceholderText(QStringLiteral("Version (automatic: rev%1)").arg(sequence));
            form.acceptButton->setEnabled(!selected.isEmpty() && (!creating || !name->text().trimmed().isEmpty()));
            summary->setText(QStringLiteral("%1 of %2 files selected · creates a saved version")
                .arg(selected.size()).arg(prepared.preview.files.size()));
            const QSignalBlocker blocker(all);
            all->setCheckState(selected.isEmpty() ? Qt::Unchecked
                : selected.size() == prepared.preview.files.size() ? Qt::Checked : Qt::PartiallyChecked);
        };
        connect(all, &QCheckBox::clicked, files, &WorkingFilesModel::checkAll);
        connect(files, &WorkingFilesModel::checkedFilesChanged, &form, update);
        connect(destination, &QComboBox::currentIndexChanged, &form, update);
        connect(name, &QLineEdit::textChanged, &form, update);
        update();
        if (form.exec() != QDialog::Accepted || m_pendingContext || m_pendingState) return;
        ImportRequest request;
        request.sources = paths;
        request.move = transfer->currentData().toBool();
        request.note = note->text();
        request.version = version->text();
        request.selection.files = files->checkedFiles();
        for (const auto &file : request.selection.files)
            request.selection.objects.insert(file, prepared.preview.objects.value(file));
        CatalogDefinition definition;
        definition.name = name->text();
        definition.category = type->currentData().toString();
        CatalogAsset target;
        for (const auto &asset : targets)
            if (asset.id == destination->currentData().toString()) { target = asset; break; }
        const auto group = m_activeGroup;
        run(QStringLiteral("Importing and archiving files…"), [library, definition, target, request, group]
        {
            auto result = target.id.isEmpty() ? SnapshotLibrary::create(library, definition, &request)
                                             : SnapshotLibrary::importAndSave(target, request);
            if (result.ok && target.id.isEmpty() && !group.isEmpty())
            {
                const auto membership = CatalogGroups::setMember(library, group, result.asset.id, true);
                if (!membership.ok)
                {
                    result.ok = false;
                    result.error = QStringLiteral("Files archived, but could not add the entry to the group: %1").arg(membership.error);
                    result.retainedPath = result.asset.root;
                }
            }
            return result;
        }, [this](const auto &result)
        {
            if (!result.retainedSources.isEmpty() && !m_pendingContext && !m_pendingState)
            {
                Form retained(this, QStringLiteral("Files archived; some sources retained"), QStringLiteral("Close"));
                retained.message(QStringLiteral("These source files could not be removed or changed during the move:\n%1")
                    .arg(result.retainedSources.join('\n')));
                retained.exec();
            }
            applyAssetResult(result, QStringLiteral("current"));
        }, true);
    });
}
bool BrowserPanel::applyPendingContext()
{
    if (m_busy) return false;
    const auto context = m_pendingContext;
    const auto state = m_pendingState;
    m_pendingContext.reset();
    m_pendingState.reset();
    if (context) setContext(context->first, context->second);
    if (state) restoreState(*state);
    else if (context && !m_busy) filter();
    return bool(context) || bool(state);
}
void BrowserPanel::refresh()
{
    refreshCatalog(false);
}
void BrowserPanel::refreshCatalog(bool automatic, bool full, const QStringList &ids, int attempt)
{
    if (automatic && (m_busy || m_loadingDetails || !isVisible() || QApplication::activeModalWidget()))
    {
        m_catalogWatcher->queue(full, ids);
        return;
    }
    if (m_busy) return;
    if (m_library.isEmpty())
    {
        if (!automatic) notice(QStringLiteral("Choose a catalog folder, then create a Module, IP or Project."));
        return;
    }
    const QString library = m_library,
                  selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    ++m_generation;
    m_pendingDetail.reset();
    m_loadingDetails = false;
    m_backgroundRefresh = automatic;
    beginOperation();
    const auto operation = m_operation;
    setBusy(true, automatic ? QStringLiteral("Updating changed files…") : QStringLiteral("Scanning folder…"));
    auto *watcher = new QFutureWatcher<CatalogReload>(this);
    connect(watcher, &QFutureWatcher<CatalogReload>::finished, this,
            [this, watcher, selectedId, automatic, full, ids, attempt, library]
            {
                const auto reload = watcher->result();
                const auto &result = reload.catalog;
                watcher->deleteLater();
                if (result.cancelled)
                {
                    setBusy(false);
                    m_backgroundRefresh = false;
                    notice(QStringLiteral("Scan cancelled. The previous catalog is still available."));
                    applyPendingContext();
                    return;
                }
                if (automatic && !result.problems.isEmpty() && attempt < 2)
                {
                    setBusy(false);
                    m_backgroundRefresh = false;
                    if (applyPendingContext()) return;
                    QTimer::singleShot(900, this, [this, library, full, ids, attempt]
                    { if (m_library == library) refreshCatalog(true, full, ids, attempt + 1); });
                    return;
                }
                const auto currentId = automatic ? m_selected.id : selectedId;
                bool changed = false, indexesChanged = false, reset = false, selectedChanged = false;
                if (reload.full)
                {
                    m_catalogWatcher->install(reload.watches);
                    bool sameStructure = m_assets.size() == result.assets.size() && m_model->groups() == result.groups;
                    for (int i = 0; sameStructure && i < m_assets.size(); ++i)
                        sameStructure = m_assets[i].id == result.assets[i].id && m_assets[i].root == result.assets[i].root &&
                            m_assets[i].name == result.assets[i].name;
                    if (!sameStructure || !automatic)
                    {
                        const auto oldSelection = std::find_if(m_assets.cbegin(), m_assets.cend(),
                            [&](const auto &asset) { return asset.id == currentId; });
                        const auto newSelection = std::find_if(result.assets.cbegin(), result.assets.cend(),
                            [&](const auto &asset) { return asset.id == currentId; });
                        selectedChanged = oldSelection != m_assets.cend() &&
                            (newSelection == result.assets.cend() || !sameAssetView(*oldSelection, *newSelection));
                        m_assets = result.assets;
                        m_model->setAssets(m_assets, m_library, result.groups);
                        changed = indexesChanged = reset = true;
                    }
                    m_problems = result.problems;
                }
                if (!reload.full) m_catalogWatcher->updateAssets(reload.assetWatches);
                QHash<QString, int> positions;
                if (!reset) for (int i = 0; i < m_assets.size(); ++i) positions.insert(m_assets[i].id, i);
                bool reorder = false;
                if (!reset) for (const auto &asset : result.assets)
                {
                    const int row = positions.value(asset.id, -1);
                    if (row >= 0)
                    {
                        auto &current = m_assets[row];
                        if (!sameAssetView(current, asset))
                        {
                            selectedChanged |= asset.id == currentId;
                            indexesChanged |= current.indexes != asset.indexes;
                            if (!reload.full)
                            {
                                for (const auto &problem : current.problems) m_problems.removeAll(problem);
                                m_problems.append(asset.problems);
                            }
                            current = asset;
                            reorder |= !m_model->updateAsset(asset);
                            changed = true;
                        }
                    }
                }
                if (reorder)
                {
                    std::sort(m_assets.begin(), m_assets.end(), [](const auto &a, const auto &b)
                        { return a.name.localeAwareCompare(b.name) < 0; });
                    m_model->setAssets(m_assets, m_library, m_model->groups());
                }
                if (indexesChanged) refreshIndexes();
                if (changed)
                {
                    if (m_pendingId.isEmpty()) m_pendingId = currentId;
                    if (!automatic || selectedChanged) m_selected = {};
                }
                setBusy(false);
                m_backgroundRefresh = false;
                if (applyPendingContext())
                    return;
                if (changed) filter();
                if (automatic && !m_loadingDetails && !m_selected.id.isEmpty() &&
                    (full || ids.contains(m_selected.id)) && m_selected.discovered)
                {
                    ++m_generation;
                    m_loadingDetails = true;
                    readDetails(m_selected);
                    updateActivity();
                    updateActions();
                }
                if (!m_problems.isEmpty() && (!automatic || !m_noticeError))
                    notice(QStringLiteral("%1 issues found. Select Issues for details.")
                               .arg(m_problems.size()),
                           true);
                else if (!automatic)
                    notice({});
            });
    watcher->setFuture(QtConcurrent::run([library, operation, full, ids, attempt, assets = m_assets]() mutable
    {
        OperationScope scope(operation.get());
        CatalogReload reload; reload.full = full;
        if (!full)
        {
            const auto generation = CatalogIndex::generation(assets);
            QList<CatalogAsset> updates;
            for (auto &asset : assets)
                if (ids.contains(asset.id))
                {
                    const auto result = SnapshotLibrary::describe(asset);
                    if (result.cancelled) { reload.catalog.cancelled = true; return reload; }
                    if (!result.ok) { reload.catalog.problems.append(result.error); continue; }
                    reload.catalog.problems.append(result.asset.problems);
                    reload.assetWatches.insert(asset.id, CatalogWatcher::assetPlan(result.asset));
                    if (!sameAssetView(asset, result.asset))
                    {
                        asset = result.asset;
                        updates.append(asset);
                    }
                    reload.catalog.assets.append(asset);
                }
            if (reload.catalog.problems.isEmpty() && !updates.isEmpty())
                CatalogIndex::updateAssets(library, updates, generation, CatalogIndex::generation(assets));
            if (reload.catalog.problems.isEmpty() || attempt < 2) return reload;
            reload.full = true;
        }
        reload.catalog = SnapshotLibrary::scan(library);
        if (!reload.catalog.cancelled) reload.watches = CatalogWatcher::plan(library, reload.catalog.assets);
        return reload;
    }));
}
void BrowserPanel::filter()
{
    m_searchTimer->stop();
    const int filterCount = int(!m_category.isEmpty()) + int(!m_indexTerm.isEmpty());
    m_filterToggle->setToolTip(filterCount ? QStringLiteral("Filter by type and index · %1 active").arg(filterCount)
                                         : QStringLiteral("Filter by type and index"));
    m_filterToggle->setAccessibleName(filterCount ? QStringLiteral("Filter · %1 active").arg(filterCount)
                                                 : QStringLiteral("Filter"));
    m_filterToggle->setIsSelected(m_filterToggle->isChecked() || filterCount > 0);
    const QString selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    QString queryError;
    auto terms = CatalogIndex::queryTerms(m_search->text(), &queryError);
    if (!queryError.isEmpty()) { notice(queryError, true); return; }
    if (!m_indexTerm.isEmpty()) terms.append(m_indexTerm);
    const QSignalBlocker selectionSignals(m_list->selectionModel());
    const QSignalBlocker treeSignals(m_list);
    m_model->filter(m_category, terms);
    auto selected = m_model->indexForId(selectedId, m_activeGroup);
    if (selected.parent().isValid())
    {
        const auto group = m_model->groupId(selected);
        if (!m_pendingId.isEmpty()) m_collapsedGroups.remove(group);
        else if (m_collapsedGroups.contains(group)) selected = selected.parent();
    }
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
        const auto root = m_model->index(row, 0);
        const bool emptyGroup = m_model->assetIndex(root) < 0 && m_model->rowCount(root) == 0;
        m_list->setRowHidden(row, {}, m_hideEmptyGroups->isChecked() && emptyGroup);
        if (m_model->assetIndex(root) < 0)
            m_list->setExpanded(root, !m_collapsedGroups.contains(m_model->groupId(root)));
    }
    if (selected.isValid() && m_list->isRowHidden(selected.row(), selected.parent()))
    {
        selected = {};
        for (int row = 0; row < m_model->rowCount(); ++row)
            if (!m_list->isRowHidden(row, {})) { selected = m_model->index(row, 0); break; }
    }
    m_pendingId.clear();
    if (selected.isValid())
    {
        m_list->setCurrentIndex(selected);
        selectCurrent();
    }
    else
    {
        ++m_generation;
        if (m_detailOperation) m_detailOperation->cancel();
        m_selected = {};
        m_activeGroup.clear();
        m_snapshot = {};
        m_name->setText(m_library.isEmpty() ? QStringLiteral("Choose your library")
                         : m_assets.isEmpty() ? QStringLiteral("No IPs yet")
                                              : QStringLiteral("No results"));
        m_name->setToolTip({});
        m_description->setText(m_library.isEmpty() ? QStringLiteral("Select the folder that holds your IPs and modules.")
            : m_assets.isEmpty() ? QStringLiteral("Use New to create an IP or register existing sources.")
                                : QStringLiteral("Try another search or clear the filters."));
        m_description->setToolTip(m_description->text());
        m_description->show();
        m_versionModel->removeRows(0, m_versionModel->rowCount());
        m_take->hide();
        m_update->hide();
        m_fileModel->setStringList({});
        m_pendingDetail.reset();
        m_loadingDetails = false;
        updateActivity();
        updateActions();
    }
}
void BrowserPanel::selectCurrent()
{
    const auto current = m_list->currentIndex();
    m_activeGroup = m_model->groupId(current);
    const int index = m_model->assetIndex(current);
    if (index < 0 && !m_activeGroup.isEmpty())
    {
        ++m_generation;
        m_selected = {};
        m_snapshot = {};
        m_pendingDetail.reset();
        m_loadingDetails = false;
        for (const auto &group : m_model->groups())
            if (group.id == m_activeGroup) m_name->setText(group.name);
        const int count = m_model->rowCount(current);
        m_name->setToolTip(QStringLiteral("Group · %1 IPs").arg(count));
        const bool filtered = !m_search->text().trimmed().isEmpty() || !m_category.isEmpty() || !m_indexTerm.isEmpty();
        m_description->setText(filtered
            ? QStringLiteral("No matching IPs or modules in this group. Clear search or filters to show all.")
            : QStringLiteral("Drag an IP onto this group, or use New IP to create one here."));
        m_description->setToolTip(m_description->text());
        m_description->setVisible(count == 0);
        m_groupItems->setRootIndex(current);
        m_groupItems->setCurrentIndex({});
        const QSignalBlocker versions(m_versions->selectionModel());
        m_versionModel->removeRows(0, m_versionModel->rowCount());
        m_take->hide();
        m_update->hide();
        m_fileModel->setStringList({});
        updateActivity();
        updateActions();
        return;
    }
    if (index < 0 || index >= m_assets.size())
        return;
    if (m_selected.root == m_assets[index].root && !m_snapshot.id.isEmpty())
    {
        if (!m_pendingRevision.isEmpty())
            showDetails(m_selected, m_workingClean);
        return;
    }
    m_selected = m_assets[index];
    m_take->show();
    const auto asset = m_selected;
    ++m_generation;
    m_pendingDetail.reset();
    const bool compareWorking = asset.discovered && asset.referencePath.isEmpty() &&
        asset.sourceProblem.isEmpty() && !asset.historyIncomplete && !asset.workingFiles.isEmpty() &&
        std::any_of(asset.snapshots.cbegin(), asset.snapshots.cend(),
            [](const auto &snapshot) { return snapshot.id != "current"; });
    if (!asset.legacy && !compareWorking)
    {
        if (m_detailOperation) m_detailOperation->cancel();
        m_loadingDetails = false;
        showDetails(asset);
        updateActivity();
        return;
    }
    if (m_displayedAsset != workingKey())
    {
        rememberWorkingView();
        m_displayedAsset.clear();
        m_workingModel->setFiles({}, {}, false);
        m_versionModel->removeRows(0, m_versionModel->rowCount());
        m_fileModel->setStringList({});
        m_snapshot = {};
    }
    m_name->setText(asset.name);
    m_take->setEnabled(false);
    m_update->setEnabled(false);
    m_description->setText(asset.legacy ? QStringLiteral("Loading revisions…") : QStringLiteral("Checking working files…"));
    m_description->show();
    m_loadingDetails = true;
    updateActivity();
    updateActions();
    readDetails(asset);
}
void BrowserPanel::readDetails(const CatalogAsset &asset)
{
    if (m_detailWatcher)
    {
        if (m_detailOperation) m_detailOperation->cancel();
        m_pendingDetail = asset;
        return;
    }
    const int generation = m_generation;
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    m_detailWatcher = watcher;
    const auto operation = std::make_shared<OperationControl>();
    m_detailOperation = operation;
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this,
            [this, watcher, generation, asset]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_detailWatcher = nullptr;
                m_detailOperation.reset();
                if (generation == m_generation)
                {
                    m_loadingDetails = false;
                    if (!result.ok)
                    {
                        showDetails(asset);
                        if (!result.cancelled) notice(result.error, true);
                    }
                    else
                        showDetails(result.asset, result.unchanged);
                }
                if (m_pendingDetail)
                {
                    const auto pending = *m_pendingDetail;
                    m_pendingDetail.reset();
                    if (m_selected.root == pending.root)
                        readDetails(pending);
                }
                updateActivity();
                updateActions();
            });
    watcher->setFuture(QtConcurrent::run([asset, operation]
    {
        OperationScope scope(operation.get());
        if (asset.legacy || !asset.discovered || !asset.sourceProblem.isEmpty() ||
            asset.historyIncomplete || asset.workingFiles.isEmpty())
            return SnapshotLibrary::describe(asset);
        auto result = SnapshotLibrary::previewSelected(asset, asset.workingFiles);
        if (result.ok)
            result.unchanged = result.preview.added.isEmpty() && result.preview.modified.isEmpty() &&
                               result.preview.removed.isEmpty() && result.preview.heads.size() <= 1;
        return result;
    }));
}
void BrowserPanel::showDetails(const CatalogAsset &asset, bool workingClean)
{
    rememberWorkingView();
    const QScopedValueRollback<bool> restoring(m_restoringWorkingView, true);
    m_workingClean = workingClean;
    const auto previousRevision = m_snapshot.id;
    const bool sameAsset = m_displayedAsset == m_library + '\n' + asset.id;
    const auto requestedRevision = m_pendingRevision;
    m_selected = asset;
    m_displayedAsset = workingKey();
    m_name->setText(asset.name);
    QStringList detail;
    detail.append(SnapshotLibrary::categoryLabel(asset.category));
    if (!asset.description.isEmpty()) detail.append(asset.description);
    detail.append(asset.problems);
    if (SnapshotLibrary::heads(asset).size() > 1)
        detail.append(QStringLiteral("Parallel revision heads: %1. Saving adopts the reviewed file set; it does not merge source text.")
            .arg(SnapshotLibrary::heads(asset).size()));
    for (auto it = asset.indexes.cbegin(); it != asset.indexes.cend(); ++it)
        if (!it.value().isEmpty())
        {
            detail.append(indexTitle(it.key()) + ": " + it.value().join(", "));
        }
    QStringList brief;
    if (!asset.problems.isEmpty()) brief.append(QStringLiteral("Attention: %1").arg(asset.problems.first()));
    if (SnapshotLibrary::heads(asset).size() > 1) brief.append(QStringLiteral("%1 parallel revision heads").arg(SnapshotLibrary::heads(asset).size()));
    m_description->setText(brief.join('\n'));
    m_description->setToolTip(detail.join('\n'));
    m_name->setToolTip(detail.join('\n'));
    m_description->setVisible(!brief.isEmpty());
    const QSignalBlocker blocker(m_versions->selectionModel());
    m_versionModel->removeRows(0, m_versionModel->rowCount());
    int requested = 0;
    for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
    {
        const bool editing = it->id == "current" || (asset.legacy && it->id == "working");
        if (editing) continue;
        const QString label = versionText(asset, *it);
        const QString status = QStringLiteral("Archived");
        const auto saved = it->created.isValid() ? it->created.toLocalTime().toString("yyyy-MM-dd HH:mm") : QString();
        const auto tooltip = QStringList{label, editing ? status : QStringLiteral("Archived · Read-only"),
            QStringLiteral("%1 files").arg(it->files.size()), saved, it->note, it->id}.join('\n');
        QList<QStandardItem *> row;
        for (const auto &text : {label, status})
        {
            auto *item = new QStandardItem(text);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setData(it->id, Qt::UserRole);
            item->setData(editing, Qt::UserRole + 1);
            item->setData(row.isEmpty() ? text : text + QStringLiteral(", read-only"), Qt::AccessibleTextRole);
            item->setToolTip(tooltip);
            row.append(item);
        }
        if (it->id == (requestedRevision.isEmpty() && sameAsset ? previousRevision : requestedRevision))
            requested = m_versionModel->rowCount();
        m_versionModel->appendRow(row);
    }
    m_versions->setMaximumHeight(qBound(1, m_versionModel->rowCount(), 6) * 28);
    updateVersionHeight();
    m_versions->setCurrentIndex(m_versionModel->index(requested, 0));
    m_versions->scrollTo(m_versions->currentIndex());
    m_pendingRevision.clear();
    m_update->setText(asset.discovered ? QStringLiteral("Create version")
                       : asset.legacy ? QStringLiteral("Convert legacy asset")
                                      : QStringLiteral("New revision…"));
    m_update->setFixedWidth(m_update->fontMetrics().horizontalAdvance(m_update->text()) + 34);
    m_update->setToolTip(asset.discovered ? QStringLiteral("Create a version from the checked files (Ctrl+S)")
                                         : QStringLiteral("Create a new revision; existing archived files remain read-only (Ctrl+S)"));
    m_update->setAccessibleName(m_update->text());
    m_update->setVisible(asset.referencePath.isEmpty());
    auto files = workingClean ? QStringList{} : asset.workingFiles;
    if (asset.legacy)
        for (const auto &snapshot : asset.snapshots)
            if (snapshot.id == "working") files = snapshot.files;
    auto checked = m_checkedFiles.value(workingKey());
    checked.intersect(QSet<QString>(files.cbegin(), files.cend()));
    const bool rebuilt = m_workingModel->setFiles(files, checked, asset.discovered && asset.referencePath.isEmpty());
    auto &viewState = m_workingViews[m_displayedAsset];
    if (rebuilt || !sameAsset)
    {
        m_workingFiles->collapseAll();
        for (const auto &path : viewState.expanded)
        {
            const auto folder = m_workingModel->pathIndex(path);
            if (folder.isValid()) m_workingFiles->expand(folder);
        }
        ElaTreeView::finishExpansion(m_workingFiles);
    }
    m_workingFiles->setCurrentIndex(m_workingModel->pathIndex(viewState.selectedPath));
    rememberChecks();
    const bool hasWorking = (asset.discovered || asset.legacy) && asset.referencePath.isEmpty();
    m_pages->setTabVisible(0, hasWorking);
    if (!hasWorking || (!requestedRevision.isEmpty() && requestedRevision != "current" && requestedRevision != "working"))
        m_pages->setCurrentIndex(1);
    else if (!sameAsset || requestedRevision == "current" || requestedRevision == "working")
        m_pages->setCurrentIndex(0);
    if (m_pendingPage >= 0)
        m_pages->setCurrentIndex(m_pendingPage == 0 && hasWorking ? 0 : 1);
    m_pendingPage = -1;
    selectVersion();
    const auto restoreScroll = [this, key = m_displayedAsset, generation = m_generation,
                                horizontal = viewState.horizontal, vertical = viewState.vertical]
    {
        if (key != m_displayedAsset || generation != m_generation) return;
        m_workingFiles->doItemsLayout();
        m_workingFiles->horizontalScrollBar()->setValue(horizontal);
        m_workingFiles->verticalScrollBar()->setValue(vertical);
    };
    restoreScroll();
    QTimer::singleShot(0, this, restoreScroll);
}
BrowserPanel::WorkingViewState BrowserPanel::captureWorkingView() const
{
    auto state = m_workingViews.value(m_displayedAsset);
    if (m_workingClean) return state;
    state.selectedPath = m_workingFiles->currentIndex().data(Qt::UserRole).toString();
    state.horizontal = m_workingFiles->horizontalScrollBar()->value();
    state.vertical = m_workingFiles->verticalScrollBar()->value();
    return state;
}
void BrowserPanel::rememberWorkingView()
{
    if (!m_restoringWorkingView && !m_displayedAsset.isEmpty())
        m_workingViews.insert(m_displayedAsset, captureWorkingView());
}
QString BrowserPanel::workingKey() const
{
    return m_library + '\n' + m_selected.id;
}
void BrowserPanel::rememberChecks()
{
    if (m_selected.id.isEmpty()) return;
    const auto files = m_workingModel->checkedFiles();
    m_checkedFiles.insert(workingKey(), QSet<QString>(files.cbegin(), files.cend()));
    const QSignalBlocker blocker(m_checkAll);
    m_checkAll->setCheckState(files.isEmpty() ? Qt::Unchecked
        : files.size() == m_workingModel->fileCount() ? Qt::Checked : Qt::PartiallyChecked);
    m_checkAll->setToolTip(QStringLiteral("%1 of %2 files checked").arg(files.size()).arg(m_workingModel->fileCount()));
    updateActions();
}
void BrowserPanel::selectVersion()
{
    const auto selectedFile = m_files->currentIndex().data().toString();
    const int horizontal = m_files->horizontalScrollBar()->value();
    const int vertical = m_files->verticalScrollBar()->value();
    m_snapshot = {};
    for (const auto &snapshot : m_selected.snapshots)
        if (snapshot.id == m_versions->currentIndex().data(Qt::UserRole).toString())
            m_snapshot = snapshot;
    const bool editing = m_snapshot.id == "current" || (m_selected.legacy && m_snapshot.id == "working");
    m_fileHeading->setToolTip(editing ? QStringLiteral("Working files · Editable")
                                   : versionText(m_selected, m_snapshot) + QStringLiteral(" · Read-only"));
    const auto revisionKey = workingKey() + '\n' + m_snapshot.id;
    const bool sameRevision = revisionKey == m_displayedRevision;
    m_displayedRevision = revisionKey;
    if (m_fileModel->stringList() != m_snapshot.files) m_fileModel->setStringList(m_snapshot.files);
    const int row = sameRevision ? m_snapshot.files.indexOf(selectedFile) : -1;
    const auto selected = m_fileModel->index(row >= 0 ? row : 0);
    if (m_files->currentIndex() != selected) m_files->setCurrentIndex(selected);
    m_files->setToolTip(m_snapshot.id == "current" || m_snapshot.id == "working"
        ? QStringLiteral("Double-click or press Enter to open the original file")
        : QStringLiteral("Double-click or press Enter to open a read-only copy of this revision"));
    m_files->setMaximumHeight(qMax(1, qMin(1000, int(m_snapshot.files.size()))) * 28);
    m_fileSection->setMaximumHeight(m_files->maximumHeight() + 36);
    updateActions();
    const auto restoreScroll = [this, revisionKey, generation = m_generation,
                                horizontal = sameRevision ? horizontal : 0,
                                vertical = sameRevision ? vertical : 0]
    {
        if (revisionKey != m_displayedRevision || generation != m_generation) return;
        m_files->doItemsLayout();
        m_files->horizontalScrollBar()->setValue(horizontal);
        m_files->verticalScrollBar()->setValue(vertical);
    };
    restoreScroll();
    if (sameRevision) QTimer::singleShot(0, this, restoreScroll);
}
void BrowserPanel::setBusy(bool value, const QString &message)
{
    m_busy = value;
    m_new->setEnabled(!value);
    m_newGroup->setEnabled(!value && !m_library.isEmpty());
    m_indexes->setEnabled(true);
    m_folder->setEnabled(!value);
    m_filterToggle->setEnabled(true);
    m_search->setEnabled(true);
    m_filters->setEnabled(true);
    m_list->setEnabled(true);
    m_list->setDragEnabled(!value);
    m_list->setAcceptDrops(!value);
    m_versions->setEnabled(true);
    m_take->setEnabled(!value && !m_snapshot.id.isEmpty());
    m_cancel->setVisible(value && bool(m_operation));
    m_cancel->setEnabled(value && bool(m_operation));
    if (value && m_operation) m_progressTimer->start();
    else { m_progressTimer->stop(); if (!value) m_operation.reset(); }
    if (!m_backgroundRefresh && (!message.isEmpty() || !value))
        notice(message);
    updateActivity();
    updateActions();
}
void BrowserPanel::updateActivity()
{
    const bool active = m_busy || m_loadingDetails;
    m_activity->setVisible(active);
    m_feedback->setVisible(active || !m_status->toolTip().isEmpty() || !m_problems.isEmpty() || (!m_embedded && m_lastExport.ok));
    const bool animate = active && isVisible();
    if (m_activity->getIsBusying() != animate)
        m_activity->setIsBusying(animate);
}
void BrowserPanel::notice(const QString &message, bool error)
{
    m_status->setText(m_status->fontMetrics().elidedText(message.simplified(), Qt::ElideRight, m_status->width()));
    m_status->setToolTip(message);
    m_status->setProperty("error", error);
    m_noticeError = error;
    updateActions();
    updateActivity();
    applyTheme();
}
void BrowserPanel::run(const QString &message, std::function<SnapshotResult()> work,
                       std::function<void(const SnapshotResult &)> finished, bool updateCatalog)
{
    if (m_busy)
        return;
    ++m_generation;
    m_pendingDetail.reset();
    m_loadingDetails = false;
    beginOperation();
    const auto operation = m_operation;
    setBusy(true, message);
    const auto watchPlan = std::make_shared<CatalogWatcher::Plan>();
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this,
            [this, watcher, finished, updateCatalog, watchPlan]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (updateCatalog && result.ok && !result.cancelled)
                    m_catalogWatcher->updateAsset(result.asset.id, *watchPlan);
                setBusy(false);
                if (result.cancelled)
                {
                    notice(result.error);
                    applyPendingContext();
                    return;
                }
                if (!result.ok)
                {
                    QString message = result.error;
                    if (!result.retainedPath.isEmpty())
                        message += QStringLiteral("\nRetained at: %1").arg(result.retainedPath);
                    m_problems.append(message);
                    notice(message, true);
                    applyPendingContext();
                    return;
                }
                if (finished)
                {
                    const QPointer<BrowserPanel> alive(this);
                    finished(result);
                    if (!alive) return;
                }
                else
                {
                    m_pendingId = result.asset.id;
                    if (applyPendingContext())
                        return;
                    refresh();
                }
                applyPendingContext();
            });
    watcher->setFuture(QtConcurrent::run([work = std::move(work), operation, updateCatalog, watchPlan,
        library = m_library, assets = updateCatalog ? m_assets : QList<CatalogAsset>{}]() mutable
    {
        OperationScope scope(operation.get());
        auto result = work();
        if (updateCatalog && result.ok && !result.cancelled)
        {
            *watchPlan = CatalogWatcher::assetPlan(result.asset);
            const auto previous = CatalogIndex::generation(assets);
            for (auto &asset : assets)
                if (asset.id == result.asset.id && asset.root == result.asset.root)
                {
                    asset = result.asset;
                    CatalogIndex::updateAsset(library, asset, previous, CatalogIndex::generation(assets));
                    break;
                }
        }
        return result;
    }));
}
void BrowserPanel::applyAssetResult(const SnapshotResult &result, const QString &revision)
{
    if (m_pendingContext && m_pendingContext->first != m_library)
    {
        applyPendingContext();
        return;
    }
    m_pendingId = result.asset.id;
    m_pendingRevision = revision;
    bool updated = false;
    for (auto &asset : m_assets)
        if (asset.id == result.asset.id && asset.root == result.asset.root)
        {
            asset = result.asset;
            updated = m_model->updateAsset(asset);
            break;
        }
    m_selected = {};
    if (applyPendingContext()) return;
    if (updated) filter();
    else refresh();
}
void BrowserPanel::chooseLibrary()
{
    if (m_busy)
        return;
    const QString path =
        ElaFilePicker::getExistingDirectory(this, QStringLiteral("Choose library folder"), m_library);
    if (path.isEmpty())
        return;
    QSettings settings("xIPs", "xIPs");
    settings.setValue("library/root", path);
    setContext(path, m_workspace);
}
QStringList BrowserPanel::pickSources()
{
    ElaMenu menu;
    auto *current = m_host ? menu.addAction(QStringLiteral("Current file")) : nullptr;
    const auto *files = menu.addAction(QStringLiteral("Choose files…"));
    const auto *folder = menu.addAction(QStringLiteral("Choose folder…"));
    const auto *anchor = m_collect;
    const auto *chosen = executeMenu(menu, this, anchor->mapToGlobal(QPoint(0, anchor->height())));
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
        return ElaFilePicker::getOpenFileNames(this, QStringLiteral("Choose files to collect"),
                                             m_workspace);
    if (chosen == folder)
    {
        const auto path = ElaFilePicker::getExistingDirectory(
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
void BrowserPanel::refreshIndexes()
{
    const QSignalBlocker blocker(m_indexes);
    m_indexes->clear();
    m_indexes->addItem(QStringLiteral("All indexes"), QString());
    for (const auto &kind : QStringList{"category", "tag", "interface", "purpose"})
    {
        QSet<QString> values;
        for (const auto &asset : m_assets)
            for (const auto &value : asset.indexes.value(kind))
            {
                values.insert(value);
                if (kind == "category")
                {
                    auto parent = value;
                    while (parent.contains('/'))
                    {
                        parent = parent.left(parent.lastIndexOf('/'));
                        if (!parent.isEmpty()) values.insert(parent);
                    }
                }
            }
        auto sorted = values.values();
        sorted.sort(Qt::CaseInsensitive);
        for (const auto &value : sorted)
            m_indexes->addItem(indexTitle(kind) + ": " + value, kind + ':' + value);
    }
    const int index = m_indexes->findData(m_indexTerm);
    m_indexes->setCurrentIndex(index < 0 ? 0 : index);
    m_indexTerm = m_indexes->currentData().toString();
}
void BrowserPanel::runGroup(std::function<GroupResult()> work, const QString &selectedAsset)
{
    if (m_busy) return;
    ++m_generation;
    m_pendingDetail.reset();
    m_loadingDetails = false;
    setBusy(true, QStringLiteral("Saving group…"));
    auto *watcher = new QFutureWatcher<GroupResult>(this);
    connect(watcher, &QFutureWatcher<GroupResult>::finished, this, [this, watcher, selectedAsset]
    {
        const auto result = watcher->result();
        watcher->deleteLater();
        setBusy(false);
        if (!result.ok) { notice(result.error, true); applyPendingContext(); return; }
        if (applyPendingContext()) return;
        auto groups = m_model->groups();
        groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const auto &group)
            { return group.id == result.group.id; }), groups.end());
        if (!result.removed) groups.append(result.group);
        std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b)
        { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        m_model->setAssets(m_assets, m_library, groups);
        m_selected = {};
        m_snapshot = {};
        m_activeGroup = result.removed ? QString() : result.group.id;
        m_pendingId = selectedAsset;
        if (result.removed) m_collapsedGroups.remove(result.group.id);
        else m_collapsedGroups.remove(m_activeGroup);
        filter();
        notice(result.removed ? QStringLiteral("Group removed. IPs and source files are unchanged.")
                              : QStringLiteral("Group saved: %1").arg(result.group.name));
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
void BrowserPanel::createGroup()
{
    if (m_busy || m_library.isEmpty()) return;
    Form form(this, QStringLiteral("New group"), QStringLiteral("Create"));
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("groupName");
    name->setPlaceholderText(QStringLiteral("Group name"));
    name->setMaxLength(128);
    form.body->addWidget(name);
    form.acceptButton->setEnabled(false);
    connect(name, &QLineEdit::textChanged, &form, [&] { form.acceptButton->setEnabled(!name->text().trimmed().isEmpty()); });
    if (form.exec() != QDialog::Accepted) return;
    const auto library = m_library, title = name->text();
    runGroup([library, title] { return CatalogGroups::create(library, title); });
}
void BrowserPanel::groupMenu(const QPoint &position)
{
    if (m_busy) return;
    const auto index = m_list->indexAt(position);
    if (index.isValid()) m_list->setCurrentIndex(index);
    const auto groupId = m_model->groupId(index);
    const int assetIndex = m_model->assetIndex(index);
    const auto assetId = assetIndex >= 0 ? m_assets[assetIndex].id : QString();
    ElaMenu menu;
    menu.setObjectName("groupMenu");
    auto *create = menu.addAction(QStringLiteral("New group…"));
    create->setObjectName("createGroupAction");
    create->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    create->setEnabled(!m_library.isEmpty());
    QAction *rename = nullptr, *erase = nullptr, *remove = nullptr;
    ElaMenu *add = nullptr;
    if (!assetId.isEmpty())
    {
        add = new ElaMenu(&menu);
        add->setTitle(QStringLiteral("Add to group"));
        add->setObjectName("addToGroupMenu");
        menu.addMenu(add);
        for (const auto &candidate : m_model->groups())
            if (!candidate.members.contains(assetId, Qt::CaseInsensitive))
            {
                auto *action = add->addAction(candidate.name);
                action->setObjectName("addToGroup_" + candidate.id);
                action->setData(candidate.id);
            }
        add->setEnabled(!add->actions().isEmpty());
        if (!groupId.isEmpty())
        {
            remove = menu.addAction(QStringLiteral("Remove from this group"));
            remove->setObjectName("removeFromGroupAction");
        }
    }
    else if (!groupId.isEmpty())
    {
        rename = menu.addAction(QStringLiteral("Rename group…"));
        rename->setObjectName("renameGroupAction");
        rename->setShortcut(QKeySequence(Qt::Key_F2));
        erase = menu.addAction(QStringLiteral("Delete group"));
        erase->setObjectName("deleteGroupAction");
    }
    auto *chosen = executeMenu(menu, this, m_list->viewport()->mapToGlobal(position));
    const auto library = m_library;
    if (chosen == create) createGroup();
    else if (chosen && add && add->actions().contains(chosen))
    {
        const auto target = chosen->data().toString();
        runGroup([library, target, assetId] { return CatalogGroups::setMember(library, target, assetId, true); }, assetId);
    }
    else if (chosen && chosen == remove)
        runGroup([library, groupId, assetId] { return CatalogGroups::setMember(library, groupId, assetId, false); }, assetId);
    else if (chosen && chosen == erase)
        runGroup([library, groupId] { return CatalogGroups::erase(library, groupId); });
    else if (chosen && chosen == rename) renameGroup(groupId);
}
void BrowserPanel::renameGroup(const QString &id)
{
    if (m_busy || id.isEmpty()) return;
    QString currentName;
    for (const auto &group : m_model->groups()) if (group.id == id) currentName = group.name;
    if (currentName.isEmpty()) return;
    Form form(this, QStringLiteral("Rename group"), QStringLiteral("Save"));
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("groupName");
    name->setMaxLength(128);
    name->setText(currentName);
    name->selectAll();
    form.body->addWidget(name);
    connect(name, &QLineEdit::textChanged, &form,
        [&] { form.acceptButton->setEnabled(!name->text().trimmed().isEmpty()); });
    if (form.exec() != QDialog::Accepted) return;
    const auto title = name->text(), library = m_library;
    runGroup([library, id, title] { return CatalogGroups::rename(library, id, title); });
}
void BrowserPanel::createAsset()
{
    if (m_busy) return;
    if (m_library.isEmpty()) { chooseLibrary(); return; }
    Form form(this, QStringLiteral("New Module, IP or Project"), QStringLiteral("Create"));
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("newAssetName");
    name->setPlaceholderText(QStringLiteral("Name, e.g. uart_rx"));
    form.body->addWidget(name);
    auto *type = new ElaComboBox(&form);
    type->setObjectName("newAssetType");
    type->addItem(QStringLiteral("Module"), "module");
    type->addItem(QStringLiteral("IP"), "ip");
    type->addItem(QStringLiteral("Project"), "project");
    form.body->addWidget(type);
    auto *mode = new ElaComboBox(&form);
    mode->setObjectName("newAssetMode");
    mode->addItem(QStringLiteral("Empty working folder"), "new");
    mode->addItem(QStringLiteral("Register existing folder"), "folder");
    mode->addItem(QStringLiteral("Register existing file"), "file");
    form.body->addWidget(mode);
    auto *source = new EnglishLineEdit(&form);
    source->setObjectName("newAssetSource");
    source->setPlaceholderText(QStringLiteral("Source inside the library root"));
    auto *browse = new ElaPushButton(QStringLiteral("Browse…"), &form);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(source, 1);
    pathRow->addWidget(browse);
    form.body->addLayout(pathRow);
    const auto editors = indexEditors(form);
    const auto update = [&]
    {
        const bool existing = mode->currentData().toString() != "new";
        source->setVisible(existing);
        browse->setVisible(existing);
        form.acceptButton->setEnabled(!name->text().trimmed().isEmpty() && (!existing || !source->text().trimmed().isEmpty()));
    };
    connect(mode, &QComboBox::currentIndexChanged, &form, update);
    connect(name, &QLineEdit::textChanged, &form, update);
    connect(source, &QLineEdit::textChanged, &form, update);
    connect(browse, &QPushButton::clicked, &form, [&]
    {
        const auto path = mode->currentData().toString() == "file"
            ? ElaFilePicker::getOpenFileName(&form, QStringLiteral("Choose source file"), m_library)
            : ElaFilePicker::getExistingDirectory(&form, QStringLiteral("Choose source folder"), m_library);
        if (!path.isEmpty()) source->setText(path);
    });
    update();
    if (form.exec() != QDialog::Accepted) return;
    CatalogDefinition definition;
    definition.name = name->text();
    definition.category = type->currentData().toString();
    definition.source = mode->currentData().toString() == "new" ? QString() : source->text().trimmed();
    definition.indexes = readIndexes(editors);
    const auto library = m_library;
    const auto group = m_activeGroup;
    run(QStringLiteral("Creating catalog entry…"), [library, definition, group]
    {
        auto result = SnapshotLibrary::create(library, definition);
        if (result.ok && !group.isEmpty())
        {
            const auto membership = CatalogGroups::setMember(library, group, result.asset.id, true);
            if (!membership.ok)
            {
                result.ok = false;
                result.error = QStringLiteral("IP created, but could not add it to the group: %1").arg(membership.error);
                result.retainedPath = result.asset.root;
            }
        }
        return result;
    });
}
void BrowserPanel::referenceAsset()
{
    if (m_busy || m_selected.id.isEmpty()) return;
    const auto asset = m_selected;
    Form form(this, QStringLiteral("Reference %1").arg(asset.name), QStringLiteral("Add reference"));
    auto *versions = new ElaComboBox(&form);
    versions->setObjectName("referenceVersion");
    for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
        if (!it->objects.isEmpty()) versions->addItem(versionText(asset, *it), it->id);
    const int selected = versions->findData(m_snapshot.id);
    if (selected >= 0) versions->setCurrentIndex(selected);
    form.body->addWidget(versions);
    auto *destination = new EnglishLineEdit(&form);
    destination->setObjectName("referenceDestination");
    destination->setText(m_workspace);
    destination->setPlaceholderText(QStringLiteral("Destination library or project folder"));
    auto *browse = new ElaPushButton(QStringLiteral("Browse…"), &form);
    auto *row = new QHBoxLayout;
    row->addWidget(destination, 1);
    row->addWidget(browse);
    form.body->addLayout(row);
    form.message(QStringLiteral("References the selected saved version. Source files stay in their owning library."));
    connect(browse, &QPushButton::clicked, &form, [&]
    {
        const auto path = ElaFilePicker::getExistingDirectory(&form, QStringLiteral("Choose library or project"), m_workspace);
        if (!path.isEmpty()) destination->setText(path);
    });
    const auto update = [&] { form.acceptButton->setEnabled(versions->count() > 0 && !destination->text().trimmed().isEmpty()); };
    connect(destination, &QLineEdit::textChanged, &form, update);
    update();
    if (form.exec() != QDialog::Accepted) return;
    const auto target = destination->text().trimmed(), revision = versions->currentData().toString();
    run(QStringLiteral("Adding IP reference…"), [asset, target, revision]
        { return SnapshotLibrary::addReference(asset, revision, target); },
        [this](const auto &result) { notice(QStringLiteral("Reference saved: %1").arg(result.exportedPath)); });
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
    form.message(QStringLiteral("Creates rev1. Module, IP and Project assets exclude build folders; "
                                "Artifact assets include the selected outputs."));
    connect(name, &QLineEdit::textChanged, &form,
            [&] { form.acceptButton->setEnabled(!name->text().trimmed().isEmpty()); });
    if (form.exec() != QDialog::Accepted)
        return;
    const QString library = m_library, title = name->text(),
                  type = category->currentData().toString();
    run(QStringLiteral("Collecting files…"), [library, paths, title, type]
        { return SnapshotLibrary::previewCollect(paths, type); },
        [this, library, paths, title, type](const auto &prepared)
    {
        if (m_pendingContext) { applyPendingContext(); return; }
        Form review(this, QStringLiteral("Review collection"), QStringLiteral("Collect reviewed files"));
        review.setObjectName("payloadReviewForm");
        previewContents(review, prepared.preview);
        if (review.exec() != QDialog::Accepted) return;
        const auto expected = prepared.preview;
        run(QStringLiteral("Collecting reviewed files…"), [library, paths, title, type, expected]
            { return SnapshotLibrary::collect(library, paths, title, type, {}, &expected); });
    });
}
void BrowserPanel::updateAsset()
{
    if (m_busy || m_selected.id.isEmpty() || !m_update->isEnabled())
        return;
    const auto asset = m_selected;
    if (asset.discovered)
    {
        const auto files = m_workingModel->checkedFiles();
        if (files.isEmpty()) return;
        run(QStringLiteral("Reviewing checked files…"), [asset, files] { return SnapshotLibrary::previewSelected(asset, files); },
            [this](const auto &prepared) { if (m_pendingContext) { applyPendingContext(); return; } savePrepared(prepared, {}); });
        return;
    }
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
    run(QStringLiteral("Reviewing update…"), [asset, paths] { return SnapshotLibrary::previewSave(asset, paths); },
        [this, paths](const auto &prepared) { if (m_pendingContext) { applyPendingContext(); return; } savePrepared(prepared, paths); });
}
void BrowserPanel::savePrepared(const SnapshotResult &prepared, const QStringList &paths)
{
    const auto asset = prepared.asset;
    const auto expected = prepared.preview;
    Form form(this, QStringLiteral("Create version · %1").arg(asset.name), expected.heads.size() > 1
        ? QStringLiteral("Adopt reviewed files") : QStringLiteral("Create version"));
    form.setObjectName("payloadReviewForm");
    previewContents(form, expected);
    auto *note = new EnglishLineEdit(&form);
    note->setObjectName("revisionNote");
    note->setPlaceholderText(QStringLiteral("Note (optional), e.g. verified on board"));
    form.body->addWidget(note);
    if (form.exec() != QDialog::Accepted)
        return;
    const auto text = note->text();
    run(
        QStringLiteral("Saving revision…"),
        [asset, paths, text, expected] { return asset.discovered
            ? SnapshotLibrary::saveSelected(asset, expected.files, text, &expected)
            : SnapshotLibrary::update(asset, paths, text, &expected); },
        [this](const auto &result)
        {
            applyAssetResult(result, result.snapshot.id);
            if (result.unchanged)
                notice(QStringLiteral("Content unchanged. No revision created."));
        }, true);
}
void BrowserPanel::exportAsset()
{
    if (m_busy || m_snapshot.id.isEmpty())
        return;
    const auto asset = m_selected;
    const auto snapshot = m_snapshot;
    if (asset.legacy)
    {
        SnapshotResult prepared; prepared.asset = asset; prepared.snapshot = snapshot;
        exportPrepared(prepared);
        return;
    }
    run(QStringLiteral("Reviewing export…"), [asset, snapshot] { return SnapshotLibrary::previewExport(asset, snapshot.id); },
        [this](const auto &prepared) { if (m_pendingContext) { applyPendingContext(); return; } exportPrepared(prepared); });
}
void BrowserPanel::exportPrepared(const SnapshotResult &prepared)
{
    const auto asset = prepared.asset;
    const auto snapshot = prepared.snapshot;
    const auto expected = prepared.preview;
    const QString workspace = m_workspace;
    Form form(this, QStringLiteral("Copy to project · %1 · %2").arg(asset.name, versionText(asset, snapshot)),
              QStringLiteral("Copy"));
    if (!asset.legacy) previewContents(form, expected, true);
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
    if (snapshot.id == "current")
        form.message(QStringLiteral("Copies the current source contents only when you click Copy."));
    connect(browse, &QPushButton::clicked, &form,
            [&]
            {
                const auto directory = ElaFilePicker::getExistingDirectory(
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
    if (m_embedded)
    {
        QString error;
        if (!m_host || !QMetaObject::invokeMethod(m_host, "destinationError", Qt::DirectConnection,
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
        QStringLiteral("Copying selected revision…"), [asset, snapshot, target, expected]
        { return SnapshotLibrary::exportSnapshot(asset, snapshot.id, target, asset.legacy ? nullptr : &expected); },
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
            if (m_embedded)
            {
                QString error;
                const bool recorded = m_host && QMetaObject::invokeMethod(
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
            m_lastExport = result;
            notice(QStringLiteral("Created %1 from %2").arg(result.exportedPath, versionText(result.asset, result.snapshot))
                + (m_embedded ? QString() : QStringLiteral(". Use Save receipt to record its origin.")));
        });
}
void BrowserPanel::saveOrigin()
{
    if (m_busy || !m_lastExport.ok || m_embedded) return;
    const auto exported = m_lastExport;
    Form form(this, QStringLiteral("Save export receipt"), QStringLiteral("Save receipt"));
    auto *path = new EnglishLineEdit(&form);
    path->setObjectName("receiptDestination");
    path->setText(exported.exportedPath + QStringLiteral(".xips-use.json"));
    form.body->addWidget(path);
    form.message(QStringLiteral("Records this exported asset, exact revision, digest and relative payload location. Existing files are never replaced."));
    if (form.exec() != QDialog::Accepted) return;
    const auto destination = path->text().trimmed();
    run(QStringLiteral("Saving origin receipt…"), [exported, destination] { return SnapshotLibrary::saveReceipt(exported, destination); },
        [this](const auto &result) { notice(QStringLiteral("Receipt saved: %1").arg(result.exportedPath)); });
}
void BrowserPanel::changeReference()
{
    if (m_busy || m_selected.referencePath.isEmpty()) return;
    const auto reference = m_selected;
    Form form(this, QStringLiteral("Locate reference owner"), QStringLiteral("Inspect versions"));
    auto *owner = new EnglishLineEdit(&form);
    owner->setObjectName("referenceOwner");
    owner->setText(QDir(QFileInfo(reference.referencePath).absolutePath()).absoluteFilePath(reference.referenceRecord.value("library").toString()));
    form.body->addWidget(owner);
    auto *browse = new ElaPushButton(QStringLiteral("Browse…"), &form);
    form.body->addWidget(browse);
    connect(browse, &QPushButton::clicked, &form, [&]
    {
        const auto path = ElaFilePicker::getExistingDirectory(&form, QStringLiteral("Owning library"), owner->text());
        if (!path.isEmpty()) owner->setText(path);
    });
    form.message(QStringLiteral("The selected owner must contain asset ID %1. Only this local reference will change.").arg(reference.id));
    if (form.exec() != QDialog::Accepted) return;
    const auto library = owner->text().trimmed();
    run(QStringLiteral("Inspecting owner revisions…"), [reference, library] { return SnapshotLibrary::referenceTarget(reference, library); },
        [this, reference, library](const auto &result)
    {
        if (m_pendingContext) { applyPendingContext(); return; }
        Form confirm(this, QStringLiteral("Change pinned reference"), QStringLiteral("Use selected revision"));
        confirm.message(QStringLiteral("Current revision: %1\nOwner: %2").arg(reference.pinnedRevision, library));
        auto *versions = new ElaComboBox(&confirm);
        versions->setObjectName("replacementRevision");
        for (auto it = result.asset.snapshots.crbegin(); it != result.asset.snapshots.crend(); ++it)
            if (!it->objects.isEmpty()) versions->addItem(versionText(result.asset, *it) + " · " + it->id.left(8), it->id);
        const int pinned = versions->findData(reference.pinnedRevision);
        if (pinned >= 0) versions->setCurrentIndex(pinned);
        confirm.body->addWidget(versions);
        confirm.acceptButton->setEnabled(versions->count() > 0);
        if (confirm.exec() != QDialog::Accepted) return;
        const auto revision = versions->currentData().toString();
        run(QStringLiteral("Updating local reference…"), [reference, library, revision]
            { return SnapshotLibrary::changeReference(reference, library, revision); });
    });
}
void BrowserPanel::detailsDialog()
{
    const auto asset = m_selected;
    Form form(this, QStringLiteral("Edit asset details"), QStringLiteral("Save"));
    form.setObjectName("assetDetailsForm");
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("assetDetailsName");
    name->setText(asset.name);
    auto *category = categories(&form, asset.category);
    auto *description = new EnglishPlainTextEdit(&form);
    description->setPlainText(asset.description);
    description->setMaximumHeight(100);
    description->setPlaceholderText(QStringLiteral("Description (optional)"));
    form.body->addWidget(name);
    form.body->addWidget(category);
    form.body->addWidget(description);
    const auto editors = indexEditors(form, asset.indexes);
    if (asset.discovered && asset.sourceIsDirectory)
        form.message(QStringLiteral("Changing the name also renames the working folder."));
    if (form.exec() != QDialog::Accepted)
        return;
    CatalogDefinition definition;
    definition.name = name->text();
    definition.category = category->currentData().toString();
    definition.description = description->toPlainText();
    definition.indexes = readIndexes(editors);
    run(QStringLiteral("Saving asset details…"),
        [asset, definition] { return SnapshotLibrary::setDefinition(asset, definition); });
}
void BrowserPanel::updateActions()
{
    const bool asset = !m_selected.id.isEmpty();
    const bool reference = !m_selected.referencePath.isEmpty();
    const bool group = !asset && !m_activeGroup.isEmpty();
    const bool working = asset && m_pages->currentIndex() == 0;
    const bool editable = asset && m_selected.discovered && !reference && !m_busy && !m_loadingDetails &&
        !m_selected.historyIncomplete && m_selected.sourceProblem.isEmpty();
    m_pages->setVisible(asset);
    m_addFiles->setEnabled(editable && m_selected.sourceIsDirectory);
    m_addFolder->setEnabled(m_addFiles->isEnabled());
    m_addFiles->setVisible(m_selected.discovered && m_selected.sourceIsDirectory);
    m_addFolder->setVisible(m_selected.discovered && m_selected.sourceIsDirectory);
    m_workingFiles->setEnabled(!m_busy && !m_loadingDetails);
    m_checkAll->setVisible(m_selected.discovered);
    m_checkAll->setEnabled(editable && m_workingModel->fileCount() > 0);
    m_workingEmpty->setVisible(m_workingModel->fileCount() == 0 && !m_selected.sourceProblem.isEmpty());
    m_workingEmpty->setText(m_selected.sourceProblem);
    m_openWorking->setEnabled(!m_busy && !m_loadingDetails && m_workingFiles->currentIndex().data(Qt::UserRole + 1).toBool());
    m_historyEmpty->setVisible(m_versionModel->rowCount() == 0);
    m_update->setVisible(asset && !reference);
    m_update->setEnabled(!m_busy && !m_loadingDetails && asset && !reference &&
        !m_selected.historyIncomplete && m_selected.sourceProblem.isEmpty() &&
        (!m_selected.discovered || !m_workingModel->checkedFiles().isEmpty()));
    m_take->setVisible(asset && !working && !m_snapshot.id.isEmpty());
    m_take->setEnabled(!m_busy && !m_snapshot.id.isEmpty());
    m_collect->setEnabled(!m_busy);
    m_refresh->setEnabled(!m_busy && !m_library.isEmpty());
    m_folder->setToolTip(m_library.isEmpty() ? QStringLiteral("Choose the folder for your IP catalog")
                                            : QDir::toNativeSeparators(m_library));
    m_issues->setToolTip(QStringLiteral("Issues · %1 — open details").arg(m_problems.size()));
    m_issues->setAccessibleName(m_issues->toolTip());
    m_issues->setVisible(!m_problems.isEmpty());
    m_receipt->setVisible(!m_embedded && m_lastExport.ok);
    m_receipt->setEnabled(!m_busy);
    m_remove->setText(reference ? QStringLiteral("Remove reference…") : QStringLiteral("Unregister…"));
    m_remove->setAccessibleName(m_remove->text());
    m_remove->setToolTip(reference ? QStringLiteral("Remove this catalog's reference; keep its owner and files")
                                   : QStringLiteral("Remove from the catalog; keep source files and saved history"));
    m_openFile->setEnabled(!m_busy && !m_loadingDetails && m_files->currentIndex().isValid());
    m_openFile->setText(m_snapshot.id == "current" || m_snapshot.id == "working"
        ? QStringLiteral("Edit file") : QStringLiteral("Open read-only"));
    m_openFile->setAccessibleName(m_openFile->text());
    m_openFile->setToolTip(m_snapshot.id == "current" || m_snapshot.id == "working"
        ? QStringLiteral("Edit file: open the original with its default application (double-click or Enter)")
        : QStringLiteral("Open read-only: open a read-only copy of the selected revision (double-click or Enter)"));
    m_openFolder->setEnabled(!m_busy);
    m_reference->setEnabled(!m_busy && std::any_of(m_selected.snapshots.cbegin(), m_selected.snapshots.cend(),
        [](const auto &snapshot) { return !snapshot.objects.isEmpty(); }));
    m_edit->setEnabled(!m_busy && !m_selected.historyIncomplete);
    m_remove->setEnabled(!m_busy && (reference || (!m_selected.historyRoot.isEmpty() && !m_selected.historyIncomplete)));
    m_changeReference->setEnabled(!m_busy);
    const bool editableVersion = !m_busy && !m_loadingDetails && asset && !reference && !m_selected.legacy &&
        !m_selected.historyIncomplete && !m_snapshot.id.isEmpty() && m_snapshot.id != "current" && m_snapshot.id != "working";
    m_renameRevision->setEnabled(editableVersion);
    m_deleteRevision->setEnabled(editableVersion);
    m_deleteAsset->setEnabled(!m_busy && !m_selected.historyIncomplete);
    m_renameGroup->setEnabled(!m_busy);
    m_deleteGroup->setEnabled(!m_busy);
    m_versionSection->setVisible(asset && m_versionModel->rowCount() > 0);
    m_fileSection->setVisible(asset && !m_snapshot.id.isEmpty());
    m_groupItems->setVisible(group && m_groupItems->rootIndex().isValid() &&
        m_model->rowCount(m_groupItems->rootIndex()) > 0);
    m_renameRevision->setVisible(asset && !reference && !m_selected.legacy);
    m_deleteRevision->setVisible(asset && !reference && !m_selected.legacy);
    const QList<QPair<ElaToolButton *, bool>> controls{
        {m_openFolder, asset && m_selected.discovered},
        {m_reference, asset}, {m_edit, asset && !m_selected.legacy && !reference},
        {m_changeReference, asset && reference}, {m_remove, asset && (m_selected.discovered || reference)},
        {m_deleteAsset, asset && !reference && !m_selected.discovered},
        {m_renameGroup, group}, {m_deleteGroup, group}};
    QList<QWidget *> visible;
    for (const auto &[button, shown] : controls)
    {
        button->setVisible(shown);
        if (shown) visible.append(button);
    }
    bool changed = m_actionLayout->count() != visible.size();
    for (int i = 0; !changed && i < visible.size(); ++i)
        changed = m_actionLayout->itemAt(i)->widget() != visible[i];
    if (changed)
    {
        while (auto *item = m_actionLayout->takeAt(0)) delete item;
        for (auto *widget : visible) m_actionLayout->addWidget(widget);
    }
    m_actionLayout->parentWidget()->setVisible(!visible.isEmpty());
    m_actionLayout->invalidate();
    if (m_embedded) updateVersionHeight();
}
void BrowserPanel::openFile()
{
    if (m_busy || m_loadingDetails) return;
    const bool working = m_pages->currentIndex() == 0;
    if (working ? !m_workingFiles->currentIndex().data(Qt::UserRole + 1).toBool()
                : (!m_files->currentIndex().isValid() || m_snapshot.id.isEmpty())) return;
    const auto asset = m_selected;
    const auto revision = working ? (asset.legacy ? QStringLiteral("working") : QStringLiteral("current")) : m_snapshot.id;
    const auto file = working ? m_workingFiles->currentIndex().data(Qt::UserRole).toString()
                              : m_files->currentIndex().data().toString();
    QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
#ifdef XIPS_ENABLE_TEST_HOOKS
    if (qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT"))
        cacheRoot = qEnvironmentVariable("XIPS_TEST_CACHE_ROOT");
#endif
    const auto cache = cacheRoot.isEmpty() ? QString() : QDir(cacheRoot).filePath("xIPs/file-previews");
    run(QStringLiteral("Opening %1…").arg(file),
        [asset, revision, file, cache] { return SnapshotLibrary::prepareFile(asset, revision, file, cache); },
        [this, file](const auto &result)
    {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.exportedPath)))
        {
            notice(QStringLiteral("Cannot open %1. Set a default application for this file type in Windows.")
                .arg(result.exportedPath), true);
            return;
        }
        const bool current = result.snapshot.id == "current" || result.snapshot.id == "working";
        notice(current ? QStringLiteral("Opened original: %1").arg(result.exportedPath)
                       : QStringLiteral("Opened read-only %1: %2").arg(SnapshotLibrary::revisionLabel(result.snapshot), file));
    });
}
void BrowserPanel::openSourceFolder()
{
    if (m_busy || !m_selected.discovered) return;
    const auto directory = m_selected.sourceIsDirectory ? m_selected.root : QFileInfo(m_selected.root).absolutePath();
    if (!QFileInfo(directory).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
        notice(QStringLiteral("Cannot open source folder: %1").arg(directory), true);
}
void BrowserPanel::removeMembership()
{
    if (m_busy || !m_remove->isEnabled() || m_selected.id.isEmpty()) return;
    const auto asset = m_selected;
    const bool source = asset.referencePath.isEmpty();
    Form form(this, source ? QStringLiteral("Unregister source") : QStringLiteral("Remove local reference"),
              source ? QStringLiteral("Unregister") : QStringLiteral("Remove reference"));
    form.message(source ? QStringLiteral("Hide this source from the catalog. Working files and saved history are retained. Registering the same source again restores its identity and history.")
                        : QStringLiteral("Remove only this receiving catalog's reference. Its owner and project copies remain unchanged. The local reference record is retained for recovery."));
    if (form.exec() != QDialog::Accepted) return;
    run(QStringLiteral("Updating catalog membership…"), [asset, source]
        { return source ? SnapshotLibrary::unregisterSource(asset) : SnapshotLibrary::removeReference(asset); });
}
void BrowserPanel::renameVersion()
{
    if (!m_renameRevision->isEnabled()) return;
    const auto asset = m_selected;
    const auto snapshot = m_snapshot;
    Form form(this, QStringLiteral("Edit version"), QStringLiteral("Save"));
    form.setObjectName("renameRevisionForm");
    auto *label = new EnglishLineEdit(&form);
    label->setObjectName("revisionLabel");
    label->setMaxLength(128);
    label->setText(SnapshotLibrary::revisionLabel(snapshot));
    label->setPlaceholderText(QStringLiteral("e.g. v1.0.0"));
    label->setAccessibleName(QStringLiteral("Version"));
    label->selectAll();
    form.body->addWidget(label);
    connect(label, &QLineEdit::textChanged, &form,
        [&] { form.acceptButton->setEnabled(!label->text().trimmed().isEmpty()); });
    if (form.exec() != QDialog::Accepted) return;
    const auto name = label->text();
    run(QStringLiteral("Saving version name…"),
        [asset, snapshot, name] { return SnapshotLibrary::renameSnapshot(asset, snapshot.id, name); },
        [this](const auto &result)
        {
            applyAssetResult(result, result.snapshot.id);
            notice(QStringLiteral("Version renamed to %1").arg(SnapshotLibrary::revisionLabel(result.snapshot)));
        }, true);
}
void BrowserPanel::deleteAsset(bool whole)
{
    if (m_busy || !(whole ? m_deleteAsset : m_deleteRevision)->isEnabled()) return;
    const auto asset = m_selected;
    const auto snapshot = m_snapshot;
    Form form(this, whole ? QStringLiteral("Delete asset") : QStringLiteral("Delete version"),
              whole ? QStringLiteral("Move to Recycle Bin") : QStringLiteral("Delete version"));
    form.setObjectName(whole ? "deleteAssetForm" : "deleteRevisionForm");
    form.message(whole ? QStringLiteral("Move %1 and all its revisions to the Recycle Bin? Existing project copies stay unchanged.").arg(asset.name)
                       : QStringLiteral("Delete %1, %2?\n\nThe IP, working files, groups and other versions are kept. Shared history content is retained.")
                           .arg(asset.name, versionText(asset, snapshot)));
    if (form.exec() != QDialog::Accepted) return;
    run(whole ? QStringLiteral("Moving to Recycle Bin…") : QStringLiteral("Deleting version…"),
        [asset, snapshot, whole]
        { return whole ? SnapshotLibrary::eraseAsset(asset) : SnapshotLibrary::eraseSnapshot(asset, snapshot.id); });
}
void BrowserPanel::showIssues()
{
    Form form(this, QStringLiteral("Library issues"), QStringLiteral("Done"));
    auto *issues = new EnglishPlainTextEdit(&form);
    issues->setObjectName("libraryIssues");
    issues->setReadOnly(true);
    issues->setPlainText(m_problems.isEmpty() ? QStringLiteral("No issues found") : m_problems.join("\n\n"));
    issues->setMinimumSize(360, 200);
    enableSmoothScrolling(issues);
    form.body->addWidget(issues);
    form.exec();
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
    if (m_pendingState) return *m_pendingState;
    QVariantMap checked;
    const auto prefix = m_library + '\n';
    for (auto it = m_checkedFiles.cbegin(); it != m_checkedFiles.cend(); ++it)
        if (it.key().startsWith(prefix)) checked.insert(it.key().mid(prefix.size()), QStringList(it.value().values()));
    auto views = m_workingViews;
    if (!m_displayedAsset.isEmpty()) views.insert(m_displayedAsset, captureWorkingView());
    QVariantMap workingViews;
    for (auto it = views.cbegin(); it != views.cend(); ++it)
        if (it.key().startsWith(prefix))
            workingViews.insert(it.key().mid(prefix.size()), QVariantMap{
                {"expanded", QStringList(it->expanded.values())}, {"selected", it->selectedPath},
                {"horizontal", it->horizontal}, {"vertical", it->vertical}});
    return {{"library", m_library}, {"workingChecks", checked}, {"workingViews", workingViews}, {"page", m_pages->currentIndex()},
            {"query", m_search->text()},
            {"category", m_category},
            {"index", m_indexTerm},
            {"filtersExpanded", m_filterToggle->isChecked()},
            {"assetId", m_selected.id},
            {"groupId", m_activeGroup},
            {"collapsedGroups", QStringList(m_collapsedGroups.values())},
            {"hideEmptyGroups", m_hideEmptyGroups->isChecked()},
            {"revision", m_pages->currentIndex() == 0
                ? (m_selected.legacy ? QStringLiteral("working") : QStringLiteral("current")) : m_snapshot.id},
            {"horizontalRatio", m_splitter->orientation() == Qt::Horizontal ? splitRatio() : m_horizontalRatio},
            {"verticalRatio", m_splitter->orientation() == Qt::Vertical ? splitRatio() : m_verticalRatio}};
}
void BrowserPanel::restoreState(const QVariantMap &state)
{
    if (state.isEmpty()) return;
    const auto library = state.value("library").toString();
    if (!library.isEmpty() && library != (m_pendingContext ? m_pendingContext->first : m_library))
        setContext(library, m_pendingContext ? m_pendingContext->second : m_workspace);
    if (m_busy) { m_pendingState = state; return; }
    rememberWorkingView();
    const QScopedValueRollback<bool> restoring(m_restoringWorkingView, true);
    if (state.contains("workingViews"))
    {
        const auto prefix = m_library + '\n';
        for (auto it = m_workingViews.begin(); it != m_workingViews.end();)
            if (it.key().startsWith(prefix)) it = m_workingViews.erase(it); else ++it;
        const auto views = state.value("workingViews").toMap();
        for (auto it = views.cbegin(); it != views.cend(); ++it)
        {
            const auto view = it.value().toMap();
            const auto expanded = view.value("expanded").toStringList();
            m_workingViews.insert(prefix + it.key(), {
                QSet<QString>(expanded.cbegin(), expanded.cend()), view.value("expanded").isValid() ? view.value("selected").toString() : QString(),
                qMax(0, view.value("horizontal").toInt()), qMax(0, view.value("vertical").toInt())});
        }
        // Apply supplied expansion state even when the same model stays alive.
        m_displayedAsset.clear();
    }
    if (state.contains("workingChecks"))
    {
        const auto prefix = m_library + '\n';
        for (auto it = m_checkedFiles.begin(); it != m_checkedFiles.end();)
            if (it.key().startsWith(prefix)) it = m_checkedFiles.erase(it); else ++it;
        const auto checked = state.value("workingChecks").toMap();
        for (auto it = checked.cbegin(); it != checked.cend(); ++it)
        {
            const auto files = it.value().toStringList();
            m_checkedFiles.insert(prefix + it.key(), QSet<QString>(files.cbegin(), files.cend()));
        }
    }
    m_pendingPage = state.value("page", -1).toInt();
    m_search->setText(state.value("query").toString());
    m_searchTimer->stop();
    m_horizontalRatio = qBound(0.1, state.value("horizontalRatio", m_horizontalRatio).toDouble(), 0.9);
    m_verticalRatio = qBound(0.1, state.value("verticalRatio", m_verticalRatio).toDouble(), 0.9);
    restoreSplit();
    m_category = state.value("category").toString();
    m_indexTerm = state.value("index").toString();
    if (!m_busy) refreshIndexes();
    {
        const QSignalBlocker blocker(m_types);
        const int type = m_types->findData(m_category);
        m_types->setCurrentIndex(type < 0 ? 0 : type);
        m_category = m_types->currentData().toString();
    }
    m_filterToggle->setChecked(state.value("filtersExpanded",
        !m_category.isEmpty() || !m_indexTerm.isEmpty()).toBool());
    m_pendingId = state.value("assetId").toString();
    m_activeGroup = state.value("groupId").toString();
    const auto collapsed = state.value("collapsedGroups").toStringList();
    m_collapsedGroups = QSet<QString>(collapsed.cbegin(), collapsed.cend());
    {
        const QSignalBlocker blocker(m_hideEmptyGroups);
        const bool hidden = state.value("hideEmptyGroups", false).toBool();
        m_hideEmptyGroups->setChecked(hidden);
        m_hideEmptyGroups->setIsSelected(hidden);
        m_hideEmptyGroups->setToolTip(hidden ? QStringLiteral("Show empty groups") : QStringLiteral("Hide empty groups"));
    }
    m_pendingRevision = state.value("revision").toString();
    m_selected = {};
    if (!m_busy)
        filter();
}
} // namespace xips
