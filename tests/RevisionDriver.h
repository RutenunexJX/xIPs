#pragma once

#include "ElaTableView.h"
#include "ElaTreeView.h"
#include "app/WorkingFilesModel.h"
inline QStringList historyPaths(QAbstractItemModel *model, const QModelIndex &parent = {})
{
    QStringList paths;
    for (int row = 0; row < model->rowCount(parent); ++row)
    {
        const auto index = model->index(row, 0, parent);
        if (index.data(Qt::UserRole + 1).toBool()) paths.append(index.data(Qt::UserRole).toString());
        else paths.append(historyPaths(model, index));
    }
    paths.sort(); return paths;
}
inline QModelIndex selectHistoryFile(ElaTreeView *view, const QString &path)
{
    const auto index = qobject_cast<xips::WorkingFilesModel *>(view->model())->fileIndex(path);
    for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) view->expand(parent);
    ElaTreeView::finishExpansion(view); view->setCurrentIndex(index); return index;
}

inline QModelIndex revisionIndex(QTableView *table, const QString &id)
{
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        const auto index = table->model()->index(row, 0);
        if (index.data(Qt::UserRole).toString() == id) return index;
    }
    return {};
}
