#include "CatalogModel.h"
#include "library/CatalogIndex.h"
#include <QFont>
#include <QHash>
#include <QSet>
#include <QSize>
#include <algorithm>

namespace xips
{
const CatalogModel::Node *CatalogModel::node(const QModelIndex &index) const
{
    const auto id = index.internalId();
    return index.isValid() && index.model() == this && id > 0 && id <= quintptr(m_nodes.size())
        ? &m_nodes[qsizetype(id) - 1] : nullptr;
}
QModelIndex CatalogModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column != 0 || (parent.isValid() && parent.column() != 0)) return {};
    const auto *container = node(parent);
    if (parent.isValid() && !container) return {};
    const auto &children = container ? container->children : m_roots;
    return row < children.size() ? createIndex(row, column, quintptr(children[row] + 1)) : QModelIndex();
}
QModelIndex CatalogModel::parent(const QModelIndex &index) const
{
    const auto *value = node(index);
    if (!value || value->parent < 0) return {};
    return createIndex(m_nodes[value->parent].row, 0, quintptr(value->parent + 1));
}
int CatalogModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) return int(m_roots.size());
    const auto *value = node(parent);
    return value && parent.column() == 0 ? int(value->children.size()) : 0;
}
QVariant CatalogModel::data(const QModelIndex &index, int role) const
{
    const auto *value = node(index);
    if (!value) return {};
    if (role == Qt::SizeHintRole) return QSize(160, 28);
    if (value->asset < 0)
    {
        const auto &group = m_groups[value->group];
        if (role == Qt::DisplayRole) return QStringLiteral("%1 · %2").arg(group.name).arg(value->children.size());
        if (role == Qt::ToolTipRole) return QStringLiteral("Group: %1\nRight-click to manage this group.").arg(group.name);
        if (role == Qt::FontRole) { QFont font; font.setBold(true); return font; }
        return {};
    }
    const auto &row = m_rows[value->asset];
    switch (role)
    {
    case Qt::DisplayRole: return row.label;
    case Qt::ToolTipRole: return row.tooltip;
    case Qt::UserRole: return row.root;
    default: return {};
    }
}
void CatalogModel::setAssets(const QList<CatalogAsset> &assets, const QString &library,
                             const QList<CatalogGroup> &groups)
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
    m_groups = groups;
    m_library = library.isEmpty() ? (assets.isEmpty() ? QString() : assets.first().library) : library;
    m_generation = CatalogIndex::generation(assets);
    m_visible.clear();
    m_nodes.clear();
    m_roots.clear();
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
    if (visible == m_visible && (!m_nodes.isEmpty() || m_groups.isEmpty()))
        return;
    beginResetModel();
    m_visible = std::move(visible);
    rebuild();
    endResetModel();
}
void CatalogModel::rebuild()
{
    m_nodes.clear();
    m_roots.clear();
    QHash<QString, int> visible;
    for (const int asset : m_visible) visible.insert(m_rows[asset].id.toCaseFolded(), asset);
    QSet<int> grouped;
    for (int group = 0; group < m_groups.size(); ++group)
    {
        const int parent = int(m_nodes.size());
        m_nodes.append(Node{-1, group, -1, int(m_roots.size()), {}});
        m_roots.append(parent);
        QList<int> members;
        for (const auto &member : m_groups[group].members)
            if (visible.contains(member.toCaseFolded())) members.append(visible.value(member.toCaseFolded()));
        std::sort(members.begin(), members.end());
        for (const int asset : members)
        {
            const int child = int(m_nodes.size());
            const int row = int(m_nodes[parent].children.size());
            m_nodes[parent].children.append(child);
            m_nodes.append(Node{asset, group, parent, row, {}});
            grouped.insert(asset);
        }
    }
    for (const int asset : m_visible)
        if (!grouped.contains(asset))
        {
            m_roots.append(int(m_nodes.size()));
            m_nodes.append(Node{asset, -1, -1, int(m_roots.size()) - 1, {}});
        }
}
QModelIndex CatalogModel::indexForId(const QString &id, const QString &group) const
{
    QModelIndex firstAsset, firstMatch, groupMatch;
    for (int i = 0; i < m_nodes.size(); ++i)
    {
        const auto &value = m_nodes[i];
        const auto index = createIndex(value.row, 0, quintptr(i + 1));
        const auto groupId = value.group < 0 ? QString() : m_groups[value.group].id;
        if (value.asset < 0)
        {
            if (groupId == group) groupMatch = index;
            continue;
        }
        if (!firstAsset.isValid()) firstAsset = index;
        if (m_rows[value.asset].id.compare(id, Qt::CaseInsensitive) == 0)
        {
            if (groupId == group) return index;
            if (!firstMatch.isValid()) firstMatch = index;
        }
    }
    if (firstMatch.isValid()) return firstMatch;
    if (id.isEmpty() && groupMatch.isValid()) return groupMatch;
    if (firstAsset.isValid()) return firstAsset;
    return groupMatch.isValid() ? groupMatch : index(0, 0);
}
int CatalogModel::assetIndex(const QModelIndex &index) const
{
    const auto *value = node(index);
    return value ? value->asset : -1;
}
QString CatalogModel::groupId(const QModelIndex &index) const
{
    const auto *value = node(index);
    return value && value->group >= 0 ? m_groups[value->group].id : QString();
}
}
