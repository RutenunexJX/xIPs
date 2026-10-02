#include "WorkingFilesModel.h"
#include "UiSupport.h"
#include <QScopedValueRollback>

namespace xips
{
WorkingFilesModel::WorkingFilesModel(QObject *parent) : QStandardItemModel(parent)
{
    connect(this, &QStandardItemModel::itemChanged, this, [this](QStandardItem *item)
    {
        if (m_updating) return;
        {
            const QScopedValueRollback<bool> guard(m_updating, true);
            if (item->hasChildren()) apply(item, item->checkState() == Qt::Unchecked ? Qt::Unchecked : Qt::Checked);
            updateParents();
        }
        emit checkedFilesChanged();
    });
}
void WorkingFilesModel::apply(QStandardItem *item, Qt::CheckState state)
{
    item->setCheckState(state);
    for (int row = 0; row < item->rowCount(); ++row) apply(item->child(row), state);
}
void WorkingFilesModel::updateParents()
{
    for (auto it = m_folders.crbegin(); it != m_folders.crend(); ++it)
    {
        auto *folder = *it;
        bool checked = false, unchecked = false;
        for (int row = 0; row < folder->rowCount(); ++row)
        {
            const auto state = folder->child(row)->checkState();
            checked |= state != Qt::Unchecked;
            unchecked |= state != Qt::Checked;
        }
        folder->setCheckState(checked ? (unchecked ? Qt::PartiallyChecked : Qt::Checked) : Qt::Unchecked);
    }
}
void WorkingFilesModel::setFiles(const QStringList &files, const QSet<QString> &checked, bool editable)
{
    const QScopedValueRollback<bool> guard(m_updating, true);
    m_files.clear(); m_folders.clear();
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Working files")});
    QHash<QString, QStandardItem *> folders;
    auto sorted = files;
    sorted.sort();
    for (const auto &relative : sorted)
    {
        auto *parent = invisibleRootItem();
        QString path;
        const auto parts = relative.split('/');
        for (int part = 0; part < parts.size(); ++part)
        {
            if (!path.isEmpty()) path += '/';
            path += parts[part];
            const bool file = part == parts.size() - 1;
            auto *item = file ? nullptr : folders.value(path);
            if (!item)
            {
                item = new QStandardItem(uiIcon(file ? UiIcon::File : UiIcon::Folder), parts[part]);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (editable ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
                item->setData(path, Qt::UserRole);
                item->setData(file, Qt::UserRole + 1);
                item->setToolTip(path);
                if (editable) item->setCheckState(file && checked.contains(path) ? Qt::Checked : Qt::Unchecked);
                parent->appendRow(item);
                if (file) m_files.insert(path, item);
                else { folders.insert(path, item); m_folders.append(item); }
            }
            parent = item;
        }
    }
    if (editable) updateParents();
}
void WorkingFilesModel::checkAll(bool checked)
{
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        for (auto *item : m_files) item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        updateParents();
    }
    emit checkedFilesChanged();
}
QStringList WorkingFilesModel::checkedFiles() const
{
    QStringList result;
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it)
        if (it.value()->checkState() == Qt::Checked) result.append(it.key());
    result.sort();
    return result;
}
QModelIndex WorkingFilesModel::fileIndex(const QString &relative) const
{
    const auto *item = m_files.value(relative);
    return item ? item->index() : QModelIndex();
}
}
