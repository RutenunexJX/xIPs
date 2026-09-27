#include "ElaFilePicker.h"
#include "UiSupport.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaListView.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToolBar.h"
#include "ElaToolButton.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace xips
{
namespace
{
class PickerModel final : public QFileSystemModel
{
  public:
    using QFileSystemModel::QFileSystemModel;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        const auto value = QFileSystemModel::data(index, role);
        return role == Qt::DisplayRole && index.column() == 0 && isDir(index)
            ? value.toString() + '/' : value;
    }
};
}
ElaFilePicker::ElaFilePicker(QWidget *parent, const QString &title, const QString &initial, Mode mode)
    : ElaContentDialog(parent ? parent->window() : nullptr), m_mode(mode)
{
    setObjectName("xipsFilePicker");
    setWindowTitle(title);
    setStandardButtonsVisible(false);
    if (parent)
        connect(parent, &QObject::destroyed, this, [this] { reject(); setParent(nullptr); });
    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    layout->addWidget(new ElaText(title, 15, body));
    auto *navigation = new ElaToolBar(body);
    navigation->setMovable(false);
    navigation->setFloatable(false);
    navigation->setToolBarSpacing(4);
    navigation->layout()->setContentsMargins(0, 0, 0, 0);
    m_up = new ElaToolButton(navigation);
    m_up->setObjectName("pickerUp");
    m_up->setText(QStringLiteral("Up"));
    m_up->setFixedSize(38, 30);
    navigation->addWidget(m_up);
    m_drives = new ElaComboBox(navigation);
    m_drives->setObjectName("pickerDrives");
    m_drives->setFixedSize(72, 30);
    m_drives->setAccessibleName(QStringLiteral("Drive"));
    for (const auto &drive : QDir::drives())
        m_drives->addItem(QDir::toNativeSeparators(drive.absoluteFilePath()), drive.absoluteFilePath());
    navigation->addWidget(m_drives);
    m_path = new ElaLineEdit(navigation);
    m_path->setObjectName("pickerPath");
    m_path->setTextMargins(8, 0, 4, 0);
    m_path->setFixedHeight(30);
    m_path->setMinimumWidth(140);
    m_path->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_path->setAccessibleName(QStringLiteral("Folder or file path"));
    navigation->addWidget(m_path);
    layout->addWidget(navigation);
    m_model = new PickerModel(this);
    m_model->setReadOnly(true);
    m_model->setOption(QFileSystemModel::DontUseCustomDirectoryIcons, true);
    m_model->setFilter(QDir::NoDotAndDotDot | QDir::AllDirs |
        (mode == Mode::Directory ? QDir::Dirs : QDir::Files));
    m_entries = new ElaListView(body);
    m_entries->setObjectName("pickerEntries");
    m_entries->setModel(m_model);
    m_entries->setItemHeight(26);
    m_entries->setUniformItemSizes(true);
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entries->setSelectionMode(mode == Mode::Files ? QAbstractItemView::ExtendedSelection
                                                   : QAbstractItemView::SingleSelection);
    m_entries->setIsTransparent(true);
    m_entries->setFrameShape(QFrame::NoFrame);
    m_entries->setStyleSheet({});
    enableSmoothScrolling(m_entries);
    enableToolTip(m_entries);
    m_model->sort(0, Qt::AscendingOrder);
    layout->addWidget(m_entries, 1);
    m_message = new ElaText(body);
    m_message->setObjectName("pickerMessage");
    m_message->setTextPixelSize(12);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setThemeColorEnabled(false);
    m_message->hide();
    layout->addWidget(m_message);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new ElaPushButton(QStringLiteral("Cancel"), body);
    cancel->setObjectName("pickerCancel");
    m_accept = new ElaPushButton(mode == Mode::Directory ? QStringLiteral("Select folder")
                                                        : QStringLiteral("Select"), body);
    m_accept->setObjectName("pickerAccept");
    m_accept->setDefault(true);
    primaryButton(m_accept);
    for (auto *button : {cancel, m_accept})
    {
        button->setFixedHeight(30);
        button->setFont(qApp->font());
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    setCentralWidget(body);
    resize(560, 360);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_accept, &QPushButton::clicked, this, &ElaFilePicker::submit);
    connect(m_path, &QLineEdit::returnPressed, this, [this]
    {
        const auto path = absoluteInput();
        if (QFileInfo(path).isDir()) navigate(path);
        else submit();
    });
    connect(m_path, &QLineEdit::textEdited, this, [this] { m_accept->setEnabled(!m_path->text().trimmed().isEmpty()); });
    connect(m_up, &QToolButton::clicked, this, [this]
    {
        QDir directory(m_directory);
        if (directory.cdUp()) navigate(directory.absolutePath());
    });
    connect(m_drives, &QComboBox::currentIndexChanged, this, [this]
    {
        if (m_drives->currentIndex() >= 0) navigate(m_drives->currentData().toString());
    });
    connect(m_entries->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { selectionChanged(); });
    connect(m_entries, &QAbstractItemView::activated, this, [this](const QModelIndex &index)
    {
        if (m_model->isDir(index)) navigate(m_model->filePath(index));
        else if (m_mode != Mode::Directory) submit();
    });
    connect(eTheme, &ElaTheme::themeModeChanged, this, [this] { applyTheme(); });
    const QFileInfo start(initial);
    navigate(initial.isEmpty() || !start.exists() ? QDir::homePath()
              : start.isDir() ? start.absoluteFilePath() : start.absolutePath());
    applyTheme();
}
QString ElaFilePicker::absoluteInput() const
{
    return QDir::cleanPath(QDir(m_directory).absoluteFilePath(QDir::fromNativeSeparators(m_path->text().trimmed())));
}
void ElaFilePicker::navigate(const QString &path)
{
    const QFileInfo target(path);
    if (!target.isDir())
    {
        m_message->setText(QStringLiteral("Folder not found."));
        m_message->show();
        return;
    }
    m_directory = target.absoluteFilePath();
    m_entries->setRootIndex(m_model->setRootPath(m_directory));
    m_entries->clearSelection();
    m_path->setText(QDir::toNativeSeparators(m_directory));
    m_path->setToolTip(m_path->text());
    m_up->setEnabled(!QDir(m_directory).isRoot());
    const QSignalBlocker block(m_drives);
    int driveIndex = -1;
    for (int index = 0; index < m_drives->count(); ++index)
        if (m_directory.startsWith(m_drives->itemData(index).toString(), Qt::CaseInsensitive))
            driveIndex = index;
    m_drives->setCurrentIndex(driveIndex);
    selectionChanged();
}
void ElaFilePicker::selectionChanged()
{
    bool valid = m_mode == Mode::Directory;
    for (const auto &index : m_entries->selectionModel()->selectedIndexes())
        valid |= index.column() == 0 && !m_model->isDir(index);
    m_accept->setEnabled(valid);
    m_message->hide();
}
void ElaFilePicker::submit()
{
    QStringList paths;
    const auto input = absoluteInput();
    if (input != QDir::cleanPath(m_directory))
        paths.append(input);
    else
    {
        for (const auto &index : m_entries->selectionModel()->selectedIndexes())
            if (index.column() == 0) paths.append(m_model->filePath(index));
        if (paths.isEmpty() && m_mode == Mode::Directory) paths.append(m_directory);
    }
    QStringList valid;
    for (const auto &path : paths)
    {
        const QFileInfo file(path);
        if (m_mode == Mode::Directory ? file.isDir() : file.isFile())
            valid.append(file.absoluteFilePath());
    }
    if (valid.isEmpty())
    {
        if (m_mode != Mode::Directory && QFileInfo(input).isDir() && input != QDir::cleanPath(m_directory))
            navigate(input);
        m_message->setText(m_mode == Mode::Directory ? QStringLiteral("Select an existing folder.")
                                                   : QStringLiteral("Select an existing file."));
        m_message->show();
        return;
    }
    m_selected = m_mode == Mode::Files ? valid : QStringList{valid.first()};
    accept();
}
void ElaFilePicker::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
    auto colors = palette();
    colors.setColor(QPalette::Window, eTheme->getThemeColor(mode, ElaThemeType::DialogBase));
    colors.setColor(QPalette::Base, colors.color(QPalette::Window));
    colors.setColor(QPalette::Text, eTheme->getThemeColor(mode, ElaThemeType::BasicText));
    setPalette(colors);
    m_entries->setPalette(colors);
    m_entries->viewport()->setPalette(colors);
    m_entries->viewport()->setAutoFillBackground(true);
    auto ink = m_message->palette();
    ink.setColor(QPalette::WindowText, QColor(mode == ElaThemeType::Dark ? "#ffaaa0" : "#a12828"));
    m_message->setPalette(ink);
}
QString ElaFilePicker::getExistingDirectory(QWidget *parent, const QString &title, const QString &initial)
{
    ElaFilePicker picker(parent, title, initial, Mode::Directory);
    return picker.exec() == QDialog::Accepted ? picker.selectedPaths().value(0) : QString();
}
QString ElaFilePicker::getOpenFileName(QWidget *parent, const QString &title, const QString &initial)
{
    ElaFilePicker picker(parent, title, initial, Mode::File);
    return picker.exec() == QDialog::Accepted ? picker.selectedPaths().value(0) : QString();
}
QStringList ElaFilePicker::getOpenFileNames(QWidget *parent, const QString &title, const QString &initial)
{
    ElaFilePicker picker(parent, title, initial, Mode::Files);
    return picker.exec() == QDialog::Accepted ? picker.selectedPaths() : QStringList();
}
}
