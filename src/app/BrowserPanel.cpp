#include "BrowserPanel.h"
#include "CatalogModel.h"
#include "UiSupport.h"
#include "ElaFilePicker.h"
#include "ElaApplication.h"
#include "ElaComboBox.h"
#include "ElaContentDialog.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
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
#include <QDropEvent>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QMimeData>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QShowEvent>
#include <QStringListModel>
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
        if (parent != parent->window())
            connect(parent->window(), &QObject::destroyed, this, detach);
        setWindowTitle(title);
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
                control->setFont(qApp->font());
            }
        return ElaContentDialog::exec();
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
    if (snapshot.id == "current")
        return QStringLiteral("Current files");
    if (asset.legacy)
        return snapshot.id == "working" ? QStringLiteral("Legacy working copy")
                                        : QStringLiteral("Legacy version %1").arg(snapshot.id);
    QString label = SnapshotLibrary::revisionLabel(snapshot);
    if (std::count_if(asset.snapshots.cbegin(), asset.snapshots.cend(),
            [&](const auto &other) { return other.sequence == snapshot.sequence; }) > 1)
        label += " · " + snapshot.id.left(8);
    return label;
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
void initializeEla()
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(13);
    const bool siblings = QApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    eApp->init();
    qApp->setFont(font);
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
    eTheme->setThemeMode(QSettings().value("ui/dark", false).toBool() ? Dark : Light);
}
BrowserPanel::BrowserPanel(QWidget *parent, QObject *host) : QWidget(parent), m_host(host)
{
    setObjectName("xipsBrowser");
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    setMinimumSize(340, 260);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);
    auto *toolbar = new ElaToolBar(this);
    toolbar->setObjectName("catalogToolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolBarSpacing(4);
    toolbar->layout()->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_search = new EnglishLineEdit(toolbar);
    m_search->setObjectName("assetSearch");
    m_search->setPlaceholderText(QStringLiteral("Search…"));
    m_search->setToolTip(QStringLiteral("Search names, files and indexes. Example: tag:uart interface:AXI"));
    enableToolTip(m_search);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search assets"));
    m_search->setMinimumWidth(96);
    m_search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toolbar->addWidget(m_search);
    m_new = new ElaPushButton(QStringLiteral("New…"), toolbar);
    m_new->setObjectName("newAssetButton");
    m_new->setFixedSize(68, 30);
    m_new->setFont(qApp->font());
    primaryButton(m_new);
    toolbar->addWidget(m_new);
    m_filterToggle = new ElaToolButton(toolbar);
    m_filterToggle->setObjectName("filterButton");
    m_filterToggle->setText(QStringLiteral("Filter"));
    m_filterToggle->setCheckable(true);
    m_filterToggle->setFixedSize(72, 30);
    m_filterToggle->setToolTip(QStringLiteral("Filter by type and index"));
    enableToolTip(m_filterToggle);
    toolbar->addWidget(m_filterToggle);
    auto *menu = new ElaToolButton(toolbar);
    menu->setText(QStringLiteral("···"));
    menu->setToolTip(QStringLiteral("Library and asset actions"));
    menu->setObjectName("moreButton");
    menu->setFixedSize(28, 30);
    menu->setAccessibleName(QStringLiteral("More actions"));
    enableToolTip(menu);
    toolbar->addWidget(menu);
    layout->addWidget(toolbar);
    m_filters = new QWidget(this);
    m_filters->setObjectName("catalogFilters");
    auto *filters = new QHBoxLayout(m_filters);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(8);
    m_types = new ElaComboBox(m_filters);
    m_types->setObjectName("typeCombo");
    m_types->setAccessibleName(QStringLiteral("Asset type"));
    m_types->setFixedHeight(30);
    for (const auto &category : QStringList{"", "module", "ip", "artifact", "other"})
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
    clearFilters->setObjectName("clearFiltersButton");
    clearFilters->setText(QStringLiteral("Clear"));
    filters->addWidget(clearFilters);
    layout->addWidget(m_filters);
    m_filters->hide();
    m_folder = new ElaPushButton(QStringLiteral("Choose library folder…"), this);
    m_folder->setObjectName("folderButton");
    m_folder->setFixedHeight(30);
    m_folder->setFont(qApp->font());
    layout->addWidget(m_folder);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName("assetSplitter");
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_list = new ElaListView(m_splitter);
    m_list->setObjectName("assetList");
    m_list->setItemHeight(28);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setUniformItemSizes(true);
    m_list->setIsTransparent(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet({});
    enableSmoothScrolling(m_list);
    enableToolTip(m_list);
    m_model = new CatalogModel(this);
    m_list->setModel(m_model);
    m_details = new QWidget(m_splitter);
    m_details->setObjectName("assetDetails");
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(8, 0, 0, 0);
    details->setSpacing(6);
    auto *identity = new QHBoxLayout;
    m_name = new ElaText(QStringLiteral("Select an asset"), 15, m_details);
    m_name->setObjectName("assetName");
    m_name->setWordWrap(true);
    m_name->setTextFormat(Qt::PlainText);
    identity->addWidget(m_name, 1);
    m_summary = new ElaText(m_details);
    m_summary->setTextPixelSize(12);
    m_summary->setObjectName("assetSummary");
    m_summary->setThemeColorEnabled(false);
    identity->addWidget(m_summary);
    details->addLayout(identity);
    m_description = new ElaText(m_details);
    m_description->setTextPixelSize(12);
    m_description->setWordWrap(true);
    m_description->setTextFormat(Qt::PlainText);
    m_description->setMaximumHeight(36);
    m_description->setThemeColorEnabled(false);
    enableToolTip(m_description);
    details->addWidget(m_description);
    m_versions = new ElaComboBox(m_details);
    m_versions->setObjectName("versionCombo");
    m_versions->setFixedHeight(30);
    m_versions->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_versions->setMinimumContentsLength(6);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    actions->addWidget(m_versions, 1);
    m_take = new ElaPushButton(QStringLiteral("Use…"), m_details);
    m_take->setObjectName("takeButton");
    primaryButton(m_take);
    m_update = new ElaPushButton(QStringLiteral("Save…"), m_details);
    m_update->setObjectName("updateButton");
    for (auto *button : {m_take, m_update})
    {
        button->setFixedHeight(30);
        button->setFont(qApp->font());
        button->setMinimumWidth(66);
    }
    actions->addWidget(m_take);
    actions->addWidget(m_update);
    details->addLayout(actions);
    m_files = new ElaListView(m_details);
    m_files->setObjectName("fileList");
    m_files->setItemHeight(24);
    m_files->setMinimumHeight(26);
    m_files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_files->setUniformItemSizes(true);
    m_files->setIsTransparent(true);
    m_files->setFrameShape(QFrame::NoFrame);
    m_files->setStyleSheet({});
    enableSmoothScrolling(m_files);
    enableToolTip(m_files);
    m_fileModel = new QStringListModel(this);
    m_files->setModel(m_fileModel);
    details->addWidget(m_files, 1);
    details->addStretch();
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    layout->addWidget(m_splitter, 1);
    m_status =
        new ElaText(QStringLiteral("Choose a catalog folder, then create an IP or module"), this);
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
    auto *feedback = new QHBoxLayout;
    m_activity = new ElaProgressRing(this);
    m_activity->setObjectName("browserActivity");
    m_activity->setFixedSize(16, 16);
    m_activity->setBusyingWidth(2);
    m_activity->setIsDisplayValue(false);
    m_activity->setIsTransparent(true);
    m_activity->setAccessibleName(QStringLiteral("Operation in progress"));
    feedback->addWidget(m_activity);
    feedback->addWidget(m_status, 1);
    layout->addLayout(feedback);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, &BrowserPanel::filter);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &BrowserPanel::selectCurrent);
    connect(m_versions, &QComboBox::currentIndexChanged, this, &BrowserPanel::selectVersion);
    connect(m_new, &QPushButton::clicked, this, &BrowserPanel::createAsset);
    connect(m_filterToggle, &QToolButton::toggled, this, [this](bool expanded)
    {
        m_filters->setVisible(expanded);
        m_filterToggle->setIsSelected(expanded);
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
    connect(m_folder, &QPushButton::clicked, this, &BrowserPanel::chooseLibrary);
    connect(m_take, &QPushButton::clicked, this, &BrowserPanel::exportAsset);
    connect(m_update, &QPushButton::clicked, this, &BrowserPanel::updateAsset);
    connect(menu, &QToolButton::clicked, this, &BrowserPanel::more);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        (m_splitter->orientation() == Qt::Horizontal ? m_horizontalRatio : m_verticalRatio) = splitRatio();
    });
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this] { applyTheme(); });
    applyTheme();
    setBusy(false);
}
void BrowserPanel::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
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
    auto p = palette();
    p.setColor(QPalette::Window, background);
    p.setColor(QPalette::Base, background);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::PlaceholderText, secondary);
    p.setColor(QPalette::Highlight, eTheme->getThemeColor(mode, ElaThemeType::BasicSelectedAlpha));
    p.setColor(QPalette::HighlightedText, text);
    setPalette(p);
    setAutoFillBackground(true);
    setStyleSheet(QStringLiteral("QWidget#xipsBrowser, QWidget#assetDetails { background: %1; } "
                                  "QSplitter#assetSplitter::handle { background: %2; }")
                      .arg(background.name(), border.name()));
    for (auto *view : {m_list, m_files})
    {
        view->setPalette(p);
        view->viewport()->setPalette(p);
        view->viewport()->setAutoFillBackground(true);
    }
    for (auto *label : {m_status, m_summary, m_description})
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
void BrowserPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto orientation = width() < 580 ? Qt::Vertical : Qt::Horizontal;
    if (m_splitter->orientation() != orientation)
    {
        m_splitter->setOrientation(orientation);
        m_details->layout()->setContentsMargins(orientation == Qt::Horizontal ? 8 : 0,
                                                orientation == Qt::Vertical ? 6 : 0, 0, 0);
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
}
void BrowserPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_versions->finishPopupAnimation();
    static_cast<QComboBox *>(m_versions)->hidePopup();
    updateActivity();
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
    if (m_busy)
    {
        m_pendingContext = qMakePair(library, workspace);
        return;
    }
    m_workspace = workspace;
    m_take->setToolTip(workspace.isEmpty() ? QStringLiteral("Export the selected version")
                                         : QStringLiteral("Use the selected version in the project"));
    QString path = library;
    if (path.isEmpty())
    {
        QSettings settings("xIPs", "xIPs");
        path = qEnvironmentVariable("XIPS_LIBRARY");
        if (path.isEmpty())
            path = settings.value("library/root").toString();
    }
    path = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    if (path == m_library && !m_assets.isEmpty())
        return;
    m_library = path;
    m_folder->setVisible(m_library.isEmpty());
    m_assets.clear();
    m_model->setAssets({});
    m_selected = {};
    m_pendingDetail.reset();
    m_loadingDetails = false;
    ++m_generation;
    filter();
    refresh();
}
bool BrowserPanel::applyPendingContext()
{
    if (!m_pendingContext || m_busy)
        return false;
    const auto context = *m_pendingContext;
    m_pendingContext.reset();
    const auto previousLibrary = m_library;
    setContext(context.first, context.second);
    return m_busy || previousLibrary != m_library;
}
void BrowserPanel::refresh()
{
    if (m_busy)
        return;
    if (m_library.isEmpty())
    {
        notice(QStringLiteral("Choose a catalog folder, then create an IP or module."));
        return;
    }
    const QString library = m_library,
                  selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    ++m_generation;
    m_pendingDetail.reset();
    m_loadingDetails = false;
    setBusy(true, QStringLiteral("Scanning folder…"));
    auto *watcher = new QFutureWatcher<CatalogResult>(this);
    connect(watcher, &QFutureWatcher<CatalogResult>::finished, this,
            [this, watcher, selectedId]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_assets = result.assets;
                m_model->setAssets(m_assets, m_library);
                refreshIndexes();
                m_problems = result.problems;
                if (m_pendingId.isEmpty())
                    m_pendingId = selectedId;
                m_selected = {};
                setBusy(false);
                if (applyPendingContext())
                    return;
                filter();
                if (!m_problems.isEmpty())
                    notice(QStringLiteral("%1 issues found. Open More → Issues for details.")
                               .arg(m_problems.size()),
                           true);
                else
                    notice(
                        m_assets.isEmpty()
                            ? QStringLiteral(
                                  "No IPs created yet. Use New to create one or register existing sources.")
                            : QStringLiteral("%1 · %2 items").arg(QFileInfo(m_library).fileName()).arg(m_assets.size()));
            });
    watcher->setFuture(QtConcurrent::run([library] { return SnapshotLibrary::scan(library); }));
}
void BrowserPanel::filter()
{
    if (m_busy)
        return;
    m_searchTimer->stop();
    const int filterCount = int(!m_category.isEmpty()) + int(!m_indexTerm.isEmpty());
    m_filterToggle->setText(filterCount ? QStringLiteral("Filter · %1").arg(filterCount)
                                      : QStringLiteral("Filter"));
    const QString selectedId = m_pendingId.isEmpty() ? m_selected.id : m_pendingId;
    auto terms = m_search->text().trimmed().toCaseFolded().split(' ', Qt::SkipEmptyParts);
    if (!m_indexTerm.isEmpty()) terms.append(m_indexTerm);
    const QSignalBlocker selectionSignals(m_list->selectionModel());
    m_model->filter(m_category, terms);
    const int selectedRow = m_model->rowForId(selectedId);
    m_pendingId.clear();
    if (selectedRow >= 0)
    {
        m_list->setCurrentIndex(m_model->index(selectedRow, 0));
        selectCurrent();
    }
    else
    {
        ++m_generation;
        m_selected = {};
        m_snapshot = {};
        m_name->setText(m_library.isEmpty() ? QStringLiteral("Choose your library")
                         : m_assets.isEmpty() ? QStringLiteral("No IPs yet")
                                              : QStringLiteral("No results"));
        m_summary->clear();
        m_description->setText(m_library.isEmpty() ? QStringLiteral("Select the folder that holds your IPs and modules.")
            : m_assets.isEmpty() ? QStringLiteral("Use New to create an IP or register existing sources.")
                                : QStringLiteral("Try another search or clear the filters."));
        m_description->setToolTip(m_description->text());
        m_description->show();
        m_versions->clear();
        m_versions->hide();
        m_take->hide();
        m_update->hide();
        m_fileModel->setStringList({});
        m_pendingDetail.reset();
        m_loadingDetails = false;
        setBusy(false);
    }
}
void BrowserPanel::selectCurrent()
{
    const int index = m_model->assetIndex(m_list->currentIndex().row());
    if (index < 0 || index >= m_assets.size())
        return;
    if (m_selected.root == m_assets[index].root && !m_snapshot.id.isEmpty())
    {
        if (!m_pendingRevision.isEmpty())
            showDetails(m_selected);
        return;
    }
    m_selected = m_assets[index];
    m_versions->show();
    m_take->show();
    const auto asset = m_selected;
    ++m_generation;
    m_pendingDetail.reset();
    if (!asset.legacy)
    {
        m_loadingDetails = false;
        showDetails(asset);
        updateActivity();
        return;
    }
    m_name->setText(asset.name);
    m_versions->clear();
    m_fileModel->setStringList({});
    m_snapshot = {};
    m_take->setEnabled(false);
    m_update->setEnabled(false);
    m_summary->setText(QStringLiteral("Loading revisions…"));
    m_loadingDetails = true;
    updateActivity();
    readLegacyDetails(asset);
}
void BrowserPanel::readLegacyDetails(const CatalogAsset &asset)
{
    if (m_detailWatcher)
    {
        m_pendingDetail = asset;
        return;
    }
    const int generation = m_generation;
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    m_detailWatcher = watcher;
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this,
            [this, watcher, generation]
            {
                const auto result = watcher->result();
                watcher->deleteLater();
                m_detailWatcher = nullptr;
                if (generation == m_generation)
                {
                    m_loadingDetails = false;
                    if (!result.ok)
                        notice(result.error, true);
                    else
                        showDetails(result.asset);
                }
                if (m_pendingDetail)
                {
                    const auto pending = *m_pendingDetail;
                    m_pendingDetail.reset();
                    if (m_selected.root == pending.root)
                        readLegacyDetails(pending);
                }
                updateActivity();
            });
    watcher->setFuture(QtConcurrent::run([asset] { return SnapshotLibrary::describe(asset); }));
}
void BrowserPanel::showDetails(const CatalogAsset &asset)
{
    m_selected = asset;
    m_name->setText(asset.name);
    QStringList detail;
    QStringList indexValues;
    if (!asset.description.isEmpty()) detail.append(asset.description);
    for (auto it = asset.indexes.cbegin(); it != asset.indexes.cend(); ++it)
        if (!it.value().isEmpty())
        {
            detail.append(indexTitle(it.key()) + ": " + it.value().join(", "));
            indexValues.append(it.value());
        }
    indexValues.removeDuplicates();
    QStringList brief;
    if (!asset.description.isEmpty()) brief.append(asset.description);
    if (!indexValues.isEmpty()) brief.append(indexValues.join(QStringLiteral(" · ")));
    m_description->setText(brief.join('\n'));
    m_description->setToolTip(detail.join('\n'));
    m_description->setVisible(!detail.isEmpty());
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
    m_update->setText(asset.discovered ? QStringLiteral("Save…")
                       : asset.legacy ? QStringLiteral("Convert legacy asset")
                                      : QStringLiteral("Update"));
    m_update->setToolTip(asset.discovered ? QStringLiteral("Save a new revision of the current files") : m_update->text());
    m_update->setMinimumWidth(m_update->fontMetrics().horizontalAdvance(m_update->text()) + 28);
    m_update->setVisible(asset.referencePath.isEmpty());
    selectVersion();
}
void BrowserPanel::selectVersion()
{
    m_snapshot = {};
    for (const auto &snapshot : m_selected.snapshots)
        if (snapshot.id == m_versions->currentData().toString())
            m_snapshot = snapshot;
    m_fileModel->setStringList(m_snapshot.files);
    m_files->setMaximumHeight(qMax(1, qMin(1000, int(m_snapshot.files.size()))) * 24 + 2);
    m_summary->setText((m_snapshot.files.size() == 1 ? QStringLiteral("%1 · %2 file")
                                                     : QStringLiteral("%1 · %2 files"))
                           .arg(SnapshotLibrary::categoryLabel(m_selected.category))
                           .arg(m_snapshot.files.size()));
    if (!m_selected.referencePath.isEmpty())
        m_summary->setText(m_summary->text() + QStringLiteral(" · Referenced"));
    m_take->setEnabled(!m_busy && !m_snapshot.id.isEmpty());
    m_update->setEnabled(!m_busy && !m_selected.id.isEmpty() && m_selected.referencePath.isEmpty());
}
void BrowserPanel::setBusy(bool value, const QString &message)
{
    m_busy = value;
    m_new->setEnabled(!value);
    m_indexes->setEnabled(!value);
    if (value)
    {
        m_versions->finishPopupAnimation();
        static_cast<QComboBox *>(m_versions)->hidePopup();
    }
    m_folder->setEnabled(!value);
    m_folder->setVisible(m_library.isEmpty());
    m_filterToggle->setEnabled(!value);
    m_search->setEnabled(!value);
    m_filters->setEnabled(!value);
    m_list->setEnabled(!value);
    m_versions->setEnabled(!value);
    m_take->setEnabled(!value && !m_snapshot.id.isEmpty());
    m_update->setEnabled(!value && !m_selected.id.isEmpty() && m_selected.referencePath.isEmpty());
    if (!message.isEmpty())
        notice(message);
    updateActivity();
}
void BrowserPanel::updateActivity()
{
    const bool active = m_busy || m_loadingDetails;
    m_activity->setVisible(active);
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
    applyTheme();
}
void BrowserPanel::run(const QString &message, std::function<SnapshotResult()> work,
                       std::function<void(const SnapshotResult &)> finished)
{
    if (m_busy)
        return;
    ++m_generation;
    m_pendingDetail.reset();
    m_loadingDetails = false;
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
                    applyPendingContext();
                    return;
                }
                if (finished)
                    finished(result);
                else
                {
                    m_pendingId = result.asset.id;
                    if (applyPendingContext())
                        return;
                    refresh();
                }
                applyPendingContext();
            });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
void BrowserPanel::chooseLibrary()
{
    if (m_busy)
        return;
    const QString path =
        ElaFilePicker::getExistingDirectory(this, QStringLiteral("Choose library folder"), m_library);
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
    ElaMenu menu;
    auto *current = m_host ? menu.addAction(QStringLiteral("Current file")) : nullptr;
    const auto *files = menu.addAction(QStringLiteral("Choose files…"));
    const auto *folder = menu.addAction(QStringLiteral("Choose folder…"));
    const auto *anchor = findChild<ElaToolButton *>("moreButton");
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
void BrowserPanel::createAsset()
{
    if (m_busy) return;
    if (m_library.isEmpty()) { chooseLibrary(); return; }
    Form form(this, QStringLiteral("New IP or module"), QStringLiteral("Create"));
    auto *name = new EnglishLineEdit(&form);
    name->setObjectName("newAssetName");
    name->setPlaceholderText(QStringLiteral("Name, e.g. uart_rx"));
    form.body->addWidget(name);
    auto *type = new ElaComboBox(&form);
    type->setObjectName("newAssetType");
    type->addItem(QStringLiteral("Module"), "module");
    type->addItem(QStringLiteral("IP"), "ip");
    form.body->addWidget(type);
    auto *mode = new ElaComboBox(&form);
    mode->setObjectName("newAssetMode");
    mode->addItem(QStringLiteral("Create HDL source folder"), "new");
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
    run(QStringLiteral("Creating catalog entry…"), [library, definition]
        { return SnapshotLibrary::create(library, definition); });
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
    if (asset.discovered)
    {
        Form form(this, QStringLiteral("Save %1").arg(asset.name), QStringLiteral("Save revision"));
        auto *note = new EnglishLineEdit(&form);
        note->setObjectName("revisionNote");
        note->setPlaceholderText(QStringLiteral("Note (optional)"));
        form.body->addWidget(note);
        if (form.exec() != QDialog::Accepted)
            return;
        const auto text = note->text();
        run(QStringLiteral("Saving revision…"),
            [asset, text] { return SnapshotLibrary::saveCurrent(asset, text); },
            [this](const auto &result)
            {
                m_pendingId = result.asset.id;
                m_pendingRevision = result.snapshot.id;
                refresh();
            });
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
    Form form(this, QStringLiteral("Update %1").arg(asset.name), QStringLiteral("Save revision"));
    form.message(QStringLiteral("Saves a revision. Existing revisions stay unchanged. "
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
    if (snapshot.id == "current")
        form.message(QStringLiteral("Copies the current source contents only when you click Use."));
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
            notice(QStringLiteral("Created %1 from %2")
                       .arg(result.exportedPath, versionText(result.asset, result.snapshot)));
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
    const auto editors = indexEditors(form, asset.indexes);
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
void BrowserPanel::more()
{
    ElaMenu menu;
    menu.setObjectName("xipsMoreMenu");
    auto *choose = menu.addAction(QStringLiteral("Library folder…"));
    choose->setToolTip(QDir::toNativeSeparators(m_library));
    auto *refreshAction = menu.addAction(QStringLiteral("Refresh"));
    refreshAction->setObjectName("refreshAction");
    auto *collect = menu.addAction(QStringLiteral("Collect files…"));
    collect->setEnabled(!m_busy);
    menu.addSeparator();
    auto *openSource = menu.addAction(QStringLiteral("Open source folder"));
    openSource->setEnabled(!m_busy && m_selected.discovered);
    auto *reference = menu.addAction(QStringLiteral("Reference in library / project…"));
    reference->setObjectName("referenceAction");
    reference->setEnabled(!m_busy && std::any_of(m_selected.snapshots.cbegin(), m_selected.snapshots.cend(),
        [](const auto &snapshot) { return !snapshot.objects.isEmpty(); }));
    menu.addSeparator();
    auto *editAction = menu.addAction(QStringLiteral("Edit asset details…"));
    auto *deleteVersion = menu.addAction(QStringLiteral("Delete selected revision…"));
    auto *deleteAsset = menu.addAction(QStringLiteral("Delete asset…"));
    menu.addSeparator();
    auto *problems = menu.addAction(QStringLiteral("Issues (%1)").arg(m_problems.size()));
    problems->setVisible(!m_problems.isEmpty());
    auto *theme = menu.addAction(QStringLiteral("Dark theme"));
    theme->setObjectName("themeAction");
    theme->setCheckable(true);
    theme->setChecked(eTheme->getThemeMode() == ElaThemeType::Dark);
    theme->setVisible(!m_host);
    for (auto *action : {choose, refreshAction, editAction, deleteVersion, deleteAsset})
        action->setEnabled(!m_busy);
    refreshAction->setEnabled(!m_busy && !m_library.isEmpty());
    for (auto *action : {openSource, reference, editAction, deleteVersion, deleteAsset})
        action->setVisible(!m_selected.id.isEmpty());
    editAction->setEnabled(!m_busy && !m_selected.id.isEmpty() && !m_selected.legacy && m_selected.referencePath.isEmpty());
    deleteVersion->setEnabled(!m_busy && !m_selected.legacy && !m_selected.discovered && m_selected.snapshots.size() > 1 &&
                              !m_snapshot.id.isEmpty());
    deleteAsset->setEnabled(!m_busy && !m_selected.id.isEmpty() && !m_selected.discovered && m_selected.referencePath.isEmpty());
    deleteVersion->setEnabled(deleteVersion->isEnabled() && m_selected.referencePath.isEmpty());
    const auto *anchor = findChild<ElaToolButton *>("moreButton");
    const auto *action = executeMenu(menu, this, anchor->mapToGlobal(QPoint(0, anchor->height())));
    if (action == choose)
        chooseLibrary();
    else if (action == refreshAction)
        refresh();
    else if (action == collect)
        addSources();
    else if (action == openSource)
    {
        const auto directory = m_selected.sourceIsDirectory ? m_selected.root : QFileInfo(m_selected.root).absolutePath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            notice(QStringLiteral("Cannot open source folder: %1").arg(directory), true);
    }
    else if (action == editAction)
        detailsDialog();
    else if (action == reference)
        referenceAsset();
    else if (action == theme)
    {
        const bool dark = eTheme->getThemeMode() != ElaThemeType::Dark;
        eTheme->setThemeMode(dark ? ElaThemeType::Dark : ElaThemeType::Light);
        QSettings().setValue("ui/dark", dark);
    }
    else if (action == problems)
    {
        Form form(this, QStringLiteral("Library issues"), QStringLiteral("Done"));
        auto *issues = new EnglishPlainTextEdit(&form);
        issues->setObjectName("libraryIssues");
        issues->setReadOnly(true);
        issues->setPlainText(m_problems.isEmpty() ? QStringLiteral("No issues found")
                                                : m_problems.join("\n\n"));
        issues->setMinimumSize(360, 200);
        enableSmoothScrolling(issues);
        form.body->addWidget(issues);
        form.exec();
    }
    else if (action == deleteVersion || action == deleteAsset)
    {
        const auto asset = m_selected;
        const auto snapshot = m_snapshot;
        const bool whole = action == deleteAsset;
        const bool shared = !snapshot.objects.isEmpty();
        Form form(this, whole ? QStringLiteral("Delete asset") : QStringLiteral("Delete revision"),
                  shared && !whole ? QStringLiteral("Remove revision") : QStringLiteral("Move to Recycle Bin"));
        form.message(whole ? QStringLiteral("Move %1 and all its revisions to the Recycle Bin? "
                                            "Existing project copies stay unchanged.")
                                 .arg(asset.name)
                           : shared ? QStringLiteral("Remove %1, %2 from the version list? Shared history content is retained.")
                                 .arg(asset.name, SnapshotLibrary::revisionLabel(snapshot))
                           : QStringLiteral("Move %1, rev%2 to the Recycle Bin? Other revisions "
                                            "and project copies stay unchanged.")
                                 .arg(asset.name, snapshot.id));
        if (form.exec() != QDialog::Accepted)
            return;
        run(shared && !whole ? QStringLiteral("Removing revision…") : QStringLiteral("Moving to Recycle Bin…"),
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
            {"index", m_indexTerm},
            {"filtersExpanded", m_filterToggle->isChecked()},
            {"assetId", m_selected.id},
            {"revision", m_snapshot.id},
            {"horizontalRatio", m_splitter->orientation() == Qt::Horizontal ? splitRatio() : m_horizontalRatio},
            {"verticalRatio", m_splitter->orientation() == Qt::Vertical ? splitRatio() : m_verticalRatio}};
}
void BrowserPanel::restoreState(const QVariantMap &state)
{
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
    m_pendingRevision = state.value("revision").toString();
    if (!m_busy)
        filter();
}
} // namespace xips
