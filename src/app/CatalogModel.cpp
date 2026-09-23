#include "CatalogModel.h"
#include <QSize>
#include <algorithm>

namespace xips
{
int CatalogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}
QVariant CatalogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const auto &row = m_rows[m_visible[index.row()]];
    switch (role)
    {
    case Qt::DisplayRole: case Qt::ToolTipRole: return row.label;
    case Qt::UserRole: return row.root;
    case Qt::SizeHintRole: return QSize(200, 54);
    default: return {};
    }
}
void CatalogModel::setAssets(const QList<CatalogAsset> &assets)
{
    QList<Row> rows;
    rows.reserve(assets.size());
    for (const auto &asset : assets)
    {
        QString secondary = SnapshotLibrary::categoryLabel(asset.category);
        secondary += asset.legacy ? QStringLiteral(" · Legacy")
            : QStringLiteral(" · rev%1").arg(asset.snapshots.last().id);
        if (!asset.description.isEmpty())
            secondary += " · " + asset.description.simplified();
        QString search = asset.name + ' ' + asset.description + ' ' + asset.tags.join(' ');
        if (!asset.snapshots.isEmpty())
            search += ' ' + asset.snapshots.last().files.join(' ');
        rows.append({asset.id, asset.root, asset.category, asset.name + '\n' + secondary,
                     search.toCaseFolded()});
    }
    beginResetModel();
    m_rows = std::move(rows);
    m_visible.clear();
    endResetModel();
}
void CatalogModel::filter(const QString &category, const QStringList &terms)
{
    QList<int> visible;
    visible.reserve(m_rows.size());
    for (qsizetype i = 0; i < m_rows.size(); ++i)
    {
        const auto &row = m_rows[i];
        if ((!category.isEmpty() && row.category != category) ||
            !std::all_of(terms.cbegin(), terms.cend(), [&](const auto &term) { return row.search.contains(term); }))
            continue;
        visible.append(static_cast<int>(i));
    }
    if (visible == m_visible)
        return;
    beginResetModel();
    m_visible = std::move(visible);
    endResetModel();
}
int CatalogModel::rowForId(const QString &id) const
{
    for (qsizetype i = 0; i < m_visible.size(); ++i)
        if (m_rows[m_visible[i]].id == id)
            return static_cast<int>(i);
    return m_visible.isEmpty() ? -1 : 0;
}
int CatalogModel::assetIndex(int row) const
{
    return row >= 0 && row < m_visible.size() ? m_visible[row] : -1;
}
}
