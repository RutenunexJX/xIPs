#pragma once

#include "ElaTableView.h"

inline QModelIndex revisionIndex(QTableView *table, const QString &id)
{
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        const auto index = table->model()->index(row, 0);
        if (index.data(Qt::UserRole).toString() == id) return index;
    }
    return {};
}
