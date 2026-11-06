#include "RevisionCompareDialog.h"
#include "BrowserPanel.h"
#include "UiSupport.h"
#include "library/OperationControl.h"
#include "ElaComboBox.h"
#include "ElaText.h"
#include "ElaTableView.h"
#include "ElaPushButton.h"
#include "ElaPlainTextEdit.h"
#include "ElaTheme.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringConverter>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace xips
{
namespace
{
QString cacheDirectory()
{
    auto root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
#ifdef XIPS_ENABLE_TEST_HOOKS
    if (qEnvironmentVariableIsSet("XIPS_TEST_CACHE_ROOT")) root = qEnvironmentVariable("XIPS_TEST_CACHE_ROOT");
#endif
    return root.isEmpty() ? QString() : QDir(root).filePath("xIPs/file-previews");
}
QString readText(const QString &path)
{
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read the verified preview.");
    constexpr qint64 limit = 2 * 1024 * 1024;
    if (file.size() > limit) throw std::runtime_error("Text preview limit: 2 MiB per file. Use Open base / Open target.");
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit)
        throw std::runtime_error("Cannot read the complete text preview.");
    auto encoding = QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8);
    QStringDecoder decoder(encoding);
    const QString text = decoder(bytes);
    if (decoder.hasError() || text.contains(QChar(u'\0')))
        throw std::runtime_error("Binary or unsupported text encoding. Open either side read-only instead.");
    return text;
}
QString difference(const QString &before, const QString &after)
{
    const auto a = before.isEmpty() ? QStringList{} : before.split('\n');
    const auto b = after.isEmpty() ? QStringList{} : after.split('\n');
    if (a.size() > 2000 || b.size() > 2000)
        throw std::runtime_error("Text preview limit: 2000 lines per file. File-level comparison remains available.");
    const int rows = int(a.size()), columns = int(b.size()), stride = columns + 1;
    std::vector<int> lengths((rows + 1) * stride);
    for (int i = rows - 1; i >= 0; --i)
    {
        OperationScope::checkpoint();
        for (int j = columns - 1; j >= 0; --j)
            lengths[i * stride + j] = a[i] == b[j] ? 1 + lengths[(i + 1) * stride + j + 1]
                : std::max(lengths[(i + 1) * stride + j], lengths[i * stride + j + 1]);
    }
    QStringList lines{QStringLiteral("--- Base"), QStringLiteral("+++ Target"),
        QStringLiteral("CR characters are shown as ␍; the final empty line records a trailing LF."), QString()};
    const auto visible = [](QString line) { return line.replace('\r', QChar(0x240d)); };
    int i = 0, j = 0;
    while (i < rows || j < columns)
    {
        OperationScope::checkpoint();
        if (i < rows && j < columns && a[i] == b[j]) { lines.append("  " + visible(a[i++])); ++j; }
        else if (j < columns && (i == rows || lengths[i * stride + j + 1] > lengths[(i + 1) * stride + j]))
            lines.append("+ " + visible(b[j++]));
        else lines.append("- " + visible(a[i++]));
    }
    if (before == after) lines.prepend(QStringLiteral("Text is identical. Encoding/BOM may still differ if file hashes differ."));
    return lines.join('\n');
}
struct TextResult { QString text, error; };
}
RevisionCompareDialog::RevisionCompareDialog(BrowserPanel *panel, const CatalogAsset &asset,
                                             const QString &target, const QString &base)
    : ElaContentDialog(panel->window()), m_asset(asset)
{
    setObjectName("revisionCompareDialog");
    setWindowTitle(QStringLiteral("Compare versions · %1").arg(asset.name));
    setStandardButtonsVisible(false);
    setFont(panel->font());
    connect(panel, &BrowserPanel::contextChanging, this, &QDialog::reject);
    connect(panel, &QObject::destroyed, this, [this] { reject(); setParent(nullptr); });
    if (panel != panel->window()) connect(panel->window(), &QObject::destroyed, this, [this] { reject(); setParent(nullptr); });
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    auto *heading = new ElaText(windowTitle(), 16, content);
    heading->setTextFormat(Qt::PlainText); heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *choices = new QHBoxLayout;
    const auto choice = [content, choices](const QString &label, const char *name)
    {
        auto *column = new QVBoxLayout;
        column->addWidget(new ElaText(label, 12, content));
        auto *combo = new ElaComboBox(content); combo->setObjectName(name);
        combo->setMinimumWidth(0); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8); combo->setFixedHeight(30);
        column->addWidget(combo); choices->addLayout(column, 1);
        return combo;
    };
    m_base = choice(QStringLiteral("Base"), "compareBase");
    m_target = choice(QStringLiteral("Target"), "compareTarget");
    m_base->addItem(QStringLiteral("Choose a base version"), QString());
    QString selectedBase = base;
    if (selectedBase.isEmpty())
        for (const auto &snapshot : asset.snapshots)
            if (snapshot.id == target && snapshot.parents.size() == 1) selectedBase = snapshot.parents.first();
    for (auto it = asset.snapshots.crbegin(); it != asset.snapshots.crend(); ++it)
    {
        if (it->id == "current" || it->id == "working") continue;
        const auto label = SnapshotLibrary::revisionLabel(*it) + " · " + it->id.left(8);
        m_base->addItem(label, it->id); m_target->addItem(label, it->id);
    }
    m_base->setCurrentIndex(qMax(0, m_base->findData(selectedBase)));
    m_target->setCurrentIndex(m_target->findData(target));
    layout->addLayout(choices);
    auto *controls = new QHBoxLayout;
    auto *swap = new ElaPushButton(QStringLiteral("Swap base / target"), content);
    swap->setObjectName("compareSwap"); controls->addWidget(swap);
    m_filter = new ElaComboBox(content); m_filter->setObjectName("compareFilter");
    for (const auto &status : QStringList{"All files", "Added", "Modified", "Only in base", "Unchanged"}) m_filter->addItem(status);
    controls->addWidget(m_filter, 1); layout->addLayout(controls);
    m_status = new ElaText({}, 12, content); m_status->setObjectName("comparisonStatus");
    m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_files = new ElaTableView(content); m_files->setObjectName("comparisonFiles");
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_files->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_files->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_files->setNativeItemContent(true); m_files->setMinimumHeight(110);
    m_files->verticalHeader()->hide(); m_files->verticalHeader()->setDefaultSectionSize(28);
    m_model = new QStandardItemModel(0, 4, this);
    m_model->setHorizontalHeaderLabels({"Change", "Path", "Base bytes", "Target bytes"});
    m_files->setModel(m_model);
    m_files->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const int column : {0, 2, 3}) m_files->setColumnWidth(column, column == 0 ? 100 : 80);
    enableSmoothScrolling(m_files); layout->addWidget(m_files, 1);
    auto *actions = new QHBoxLayout;
    m_openBase = new ElaPushButton(QStringLiteral("Open base"), content); m_openBase->setObjectName("compareOpenBase");
    m_openTarget = new ElaPushButton(QStringLiteral("Open target"), content); m_openTarget->setObjectName("compareOpenTarget");
    m_textButton = new ElaPushButton(QStringLiteral("Text difference"), content); m_textButton->setObjectName("compareText");
    for (auto *button : {m_openBase, m_openTarget, m_textButton}) actions->addWidget(button);
    layout->addLayout(actions);
    m_text = new ElaPlainTextEdit(content); m_text->setObjectName("comparisonText");
    m_text->setReadOnly(true); m_text->setMinimumHeight(100); m_text->hide();
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap); layout->addWidget(m_text, 1);
    auto *close = new ElaPushButton(QStringLiteral("Close"), content); close->setObjectName("compareClose");
    layout->addWidget(close, 0, Qt::AlignRight); connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_base, &QComboBox::currentIndexChanged, this, [this] { load(); });
    connect(m_target, &QComboBox::currentIndexChanged, this, [this] { load(); });
    connect(swap, &QPushButton::clicked, this, [this]
    {
        const auto a = m_base->currentData(), b = m_target->currentData();
        if (a.toString().isEmpty()) return;
        { const QSignalBlocker x(m_base), y(m_target); m_base->setCurrentIndex(m_base->findData(b)); m_target->setCurrentIndex(m_target->findData(a)); }
        load();
    });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { populate(); });
    connect(m_files->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateActions(); m_text->hide(); });
    connect(m_openBase, &QPushButton::clicked, this, [this] { openSide(true); });
    connect(m_openTarget, &QPushButton::clicked, this, [this] { openSide(false); });
    connect(m_textButton, &QPushButton::clicked, this, [this] { showTextDifference(); });
    setCentralWidget(content);
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this] { applyDialogTheme(this); });
    applyDialogTheme(this); resize(760, 540); load();
}
RevisionCompareDialog::~RevisionCompareDialog() { cancelRead(); }
void RevisionCompareDialog::cancelRead() { ++m_generation; if (m_operation) m_operation->cancel(); }
void RevisionCompareDialog::done(int result) { cancelRead(); ElaContentDialog::done(result); }
void RevisionCompareDialog::showEvent(QShowEvent *event) { ElaContentDialog::showEvent(event); applyDialogTheme(this); keepDialogOnScreen(this); }
void RevisionCompareDialog::load()
{
    cancelRead(); m_result = {}; m_text->hide(); m_model->removeRows(0, m_model->rowCount());
    const auto base = m_base->currentData().toString(), target = m_target->currentData().toString();
    m_busy = !base.isEmpty() && !target.isEmpty(); updateActions();
    if (!m_busy) { m_status->setText(QStringLiteral("Choose both saved versions. Parallel histories require an explicit base.")); return; }
    m_status->setText(QStringLiteral("Comparing recorded file hashes…"));
    m_operation = std::make_shared<OperationControl>();
    const int generation = m_generation;
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this, [this, watcher, generation]
    {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != m_generation) return;
        m_busy = false; m_result = result;
        if (!result.ok) { m_status->setText(result.error); updateActions(); return; }
        const auto &p = result.preview;
        m_status->setText(QStringLiteral("%1 added · %2 modified · %3 only in base · %4 unchanged\nRecorded hashes compared. Opening a file verifies its contents.")
            .arg(p.added.size()).arg(p.modified.size()).arg(p.removed.size()).arg(p.unchanged));
        populate();
    });
    watcher->setFuture(QtConcurrent::run([asset = m_asset, base, target, control = m_operation]
    { OperationScope scope(control.get()); return SnapshotLibrary::compareSnapshots(asset, base, target); }));
}
void RevisionCompareDialog::populate()
{
    m_model->removeRows(0, m_model->rowCount());
    if (!m_result.ok) return;
    auto paths = m_result.comparisonBase.objects.keys();
    paths.append(m_result.snapshot.objects.keys()); paths.removeDuplicates(); paths.sort();
    for (const auto &path : paths)
    {
        const auto &p = m_result.preview;
        const auto status = p.added.contains(path) ? QStringLiteral("Added") : p.modified.contains(path) ? QStringLiteral("Modified")
            : p.removed.contains(path) ? QStringLiteral("Only in base") : QStringLiteral("Unchanged");
        if (m_filter->currentIndex() > 0 && status != m_filter->currentText()) continue;
        QList<QStandardItem *> row;
        const auto size = [&path](const Snapshot &snapshot)
        { return snapshot.objects.contains(path) ? QString::number(snapshot.objects.value(path).size) : QStringLiteral("—"); };
        for (const auto &text : {status, path, size(m_result.comparisonBase), size(m_result.snapshot)})
        { auto *item = new QStandardItem(text); item->setData(path, Qt::UserRole); item->setToolTip(path); row.append(item); }
        m_model->appendRow(row);
    }
    if (m_model->rowCount()) m_files->selectRow(0);
    updateActions();
}
void RevisionCompareDialog::updateActions()
{
    const auto path = m_files->currentIndex().data(Qt::UserRole).toString();
    const bool ready = !m_busy && m_result.ok && !path.isEmpty();
    m_openBase->setEnabled(ready && m_result.comparisonBase.files.contains(path));
    m_openTarget->setEnabled(ready && m_result.snapshot.files.contains(path));
    m_textButton->setEnabled(ready);
}
void RevisionCompareDialog::openSide(bool base)
{
    if (m_busy || !m_result.ok) return;
    const auto path = m_files->currentIndex().data(Qt::UserRole).toString();
    const auto revision = base ? m_result.comparisonBase.id : m_result.snapshot.id;
    cancelRead(); m_operation = std::make_shared<OperationControl>(); m_busy = true; updateActions();
    const int generation = m_generation;
    auto *watcher = new QFutureWatcher<SnapshotResult>(this);
    connect(watcher, &QFutureWatcher<SnapshotResult>::finished, this, [this, watcher, generation]
    {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != m_generation) return;
        m_busy = false; updateActions();
        if (!result.ok) m_status->setText(result.error);
        else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.exportedPath))) m_status->setText(QStringLiteral("Could not open the file. Configure its default application."));
    });
    const auto cache = cacheDirectory();
    watcher->setFuture(QtConcurrent::run([asset = m_result.asset, revision, path, cache, control = m_operation]
    { OperationScope scope(control.get()); return SnapshotLibrary::prepareFile(asset, revision, path, cache); }));
}
void RevisionCompareDialog::showTextDifference()
{
    if (m_busy || !m_result.ok) return;
    cancelRead(); m_operation = std::make_shared<OperationControl>(); m_busy = true; updateActions();
    const auto path = m_files->currentIndex().data(Qt::UserRole).toString();
    const auto cache = cacheDirectory(); const int generation = m_generation;
    auto *watcher = new QFutureWatcher<TextResult>(this);
    connect(watcher, &QFutureWatcher<TextResult>::finished, this, [this, watcher, generation, path]
    {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != m_generation) return;
        m_busy = false; updateActions();
        if (path != m_files->currentIndex().data(Qt::UserRole).toString()) return;
        m_text->setPlainText(result.error.isEmpty() ? result.text : result.error); m_text->show();
    });
    watcher->setFuture(QtConcurrent::run([result = m_result, path, cache, control = m_operation]
    {
        OperationScope scope(control.get()); TextResult output;
        try
        {
            const auto text = [&](const Snapshot &snapshot)
            {
                if (!snapshot.files.contains(path)) return QString();
                const auto prepared = SnapshotLibrary::prepareFile(result.asset, snapshot.id, path, cache);
                if (!prepared.ok) throw std::runtime_error(prepared.error.toStdString());
                return readText(prepared.exportedPath);
            };
            output.text = difference(text(result.comparisonBase), text(result.snapshot));
        }
        catch (const OperationCancelled &) { output.error = QStringLiteral("Comparison cancelled."); }
        catch (const std::exception &error) { output.error = QString::fromUtf8(error.what()); }
        return output;
    }));
}
}
