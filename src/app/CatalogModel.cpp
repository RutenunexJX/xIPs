#include "CatalogModel.h"
#include "library/CatalogIndex.h"
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
    case Qt::DisplayRole: return row.label;
    case Qt::ToolTipRole: return row.tooltip;
    case Qt::UserRole: return row.root;
    case Qt::SizeHintRole: return QSize(160, 28);
    default: return {};
    }
}
void CatalogModel::setAssets(const QList<CatalogAsset> &assets, const QString &library)
{
    QList<Row> rows;
    rows.reserve(assets.size());
    for (const auto &asset : assets)
    {
        QString secondary = SnapshotLibrary::categoryLabel(asset.category);
        secondary += !asset.referencePath.isEmpty() ? QStringLiteral(" · Referenced")
            : asset.discovered ? QStringLiteral(" · Working files")
            : asset.legacy ? QStringLiteral(" · Legacy")
            : QStringLiteral(" · %1").arg(SnapshotLibrary::revisionLabel(asset.snapshots.last()));
        if (!asset.description.isEmpty())
            secondary += " · " + asset.description.simplified();
        QString search = CatalogIndex::searchText(asset);
        rows.append({asset.id, asset.root, asset.category, asset.name, asset.name + '\n' + secondary,
                     search.toCaseFolded()});
    }
    beginResetModel();
    m_rows = std::move(rows);
    m_assets = assets;
    m_library = library.isEmpty() ? (assets.isEmpty() ? QString() : assets.first().library) : library;
    m_generation = CatalogIndex::generation(assets);
    m_visible.clear();
    endResetModel();
}
void CatalogModel::filter(const QString &category, const QStringList &terms)
{
    QList<int> visible;
    const auto indexed = m_library.isEmpty() ? std::nullopt
        : CatalogIndex::matchingRoots(m_library, m_generation, category, terms);
    visible.reserve(m_rows.size());
    for (qsizetype i = 0; i < m_rows.size(); ++i)
    {
        const auto &row = m_rows[i];
        if (indexed ? !indexed->contains(row.root) : ((!category.isEmpty() && row.category != category) ||
            !CatalogIndex::matches(m_assets[i], terms))
            )
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
