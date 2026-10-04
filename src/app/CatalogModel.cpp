#include "CatalogModel.h"
#include "UiSupport.h"
#include "library/CatalogIndex.h"
#include <QFont>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSet>
#include <QSize>
#include <algorithm>

namespace xips
{
const CatalogModel::Node *CatalogModel::node(const QModelIndex &index) const
{
    const auto id = index.internalId();
    return index.isValid() && index.model() == this && id > 0 && id <= quintptr(m_nodes.size()) && m_nodes[qsizetype(id) - 1].row >= 0
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
        if (role == Qt::DisplayRole) return group.name;
        if (role == Qt::ToolTipRole) return QStringLiteral("%1 · %2 IPs\nDrop an IP here to add it. F2 to rename.")
            .arg(group.name).arg(value->children.size());
        if (role == Qt::DecorationRole) return uiIcon(UiIcon::Folder);
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
Qt::ItemFlags CatalogModel::flags(const QModelIndex &index) const
{
    const auto *value = node(index);
    if (!value) return Qt::NoItemFlags;
    return QAbstractItemModel::flags(index) |
        (value->asset < 0 ? Qt::ItemIsDropEnabled : Qt::ItemIsDragEnabled);
}
QStringList CatalogModel::mimeTypes() const
{
    return {QStringLiteral("application/x-xips-catalog-item")};
}
QMimeData *CatalogModel::mimeData(const QModelIndexList &indexes) const
{
    auto *data = new QMimeData;
    for (const auto &index : indexes)
        if (const int asset = assetIndex(index); asset >= 0)
        {
            data->setData(mimeTypes().first(), QJsonDocument(QJsonObject{
                {"catalog", m_library}, {"assetId", m_rows[asset].id}}).toJson(QJsonDocument::Compact));
            break;
        }
    return data;
}
QString CatalogModel::droppedAsset(const QMimeData *data) const
{
    if (!data || !data->hasFormat(mimeTypes().first())) return {};
    const auto bytes = data->data(mimeTypes().first());
    if (bytes.size() > 16384) return {};
    const auto object = QJsonDocument::fromJson(bytes).object();
    if (m_library.isEmpty() || object.value("catalog").toString() != m_library) return {};
    const auto id = object.value("assetId").toString();
    for (const auto &asset : m_rows)
        if (asset.id == id) return id;
    return {};
}
bool CatalogModel::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                  const QModelIndex &parent) const
{
    const auto *target = node(parent);
    if (action != Qt::CopyAction || row != -1 || column > 0 || !target || target->asset >= 0) return false;
    const auto id = droppedAsset(data);
    return !id.isEmpty() && !m_groups[target->group].members.contains(id, Qt::CaseInsensitive);
}
bool CatalogModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                               const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent)) return false;
    emit groupMembershipRequested(groupId(parent), droppedAsset(data));
    return true;
}
CatalogModel::Row CatalogModel::assetRow(const CatalogAsset &asset)
{
    QString secondary = SnapshotLibrary::categoryLabel(asset.category);
    secondary += !asset.referencePath.isEmpty() ? QStringLiteral(" · Referenced")
        : asset.discovered ? QStringLiteral(" · Working files")
        : asset.legacy ? QStringLiteral(" · Legacy")
        : asset.snapshots.isEmpty() ? (asset.historyIncomplete ? QStringLiteral(" · Unavailable")
                                                              : QStringLiteral(" · No saved versions"))
        : QStringLiteral(" · %1").arg(SnapshotLibrary::revisionLabel(asset.snapshots.last()));
    if (!asset.description.isEmpty())
        secondary += " · " + asset.description.simplified();
    QString search = CatalogIndex::searchText(asset);
    return {asset.id, asset.root, asset.category, asset.name, asset.name + '\n' + secondary,
            search.toCaseFolded()};
}
void CatalogModel::setAssets(const QList<CatalogAsset> &assets, const QString &library,
                             const QList<CatalogGroup> &groups)
{
    QList<Row> rows;
    rows.reserve(assets.size());
    for (const auto &asset : assets) rows.append(assetRow(asset));
    beginResetModel();
    m_rows = std::move(rows);
    m_assets = assets;
    m_assetLookup.clear();
    for (int i = 0; i < assets.size(); ++i) m_assetLookup.insert(assets[i].id, i);
    m_groups = groups;
    m_library = library.isEmpty() ? (assets.isEmpty() ? QString() : assets.first().library) : library;
    m_visible.clear();
    for (int i = 0; i < m_rows.size(); ++i) m_visible.append(i);
    rebuild();
    endResetModel();
}
bool CatalogModel::updateAsset(const CatalogAsset &asset)
{
    const int i = m_assetLookup.value(asset.id, -1);
    if (i < 0 || m_assets[i].root != asset.root || m_assets[i].name != asset.name) return false;
    m_assets[i] = asset;
    m_rows[i] = assetRow(asset);
    for (const int n : m_assetNodes[i])
        if (m_nodes[n].row >= 0)
        {
            const auto item = createIndex(m_nodes[n].row, 0, quintptr(n + 1));
            emit dataChanged(item, item);
        }
    return true;
}
void CatalogModel::filter(const QString &category, const QStringList &terms)
{
    QList<int> visible;
    visible.reserve(m_rows.size());
    for (qsizetype i = 0; i < m_rows.size(); ++i)
    {
        const auto &row = m_rows[i];
        if ((!category.isEmpty() && row.category != category) ||
            !CatalogIndex::matches(m_assets[i], terms, row.search))
            continue;
        visible.append(static_cast<int>(i));
    }
    if (visible == m_visible)
        return;
    m_visible = std::move(visible);
    const QSet<int> accepted(m_visible.cbegin(), m_visible.cend());
    for (int i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].asset < 0) filterChildren(i, accepted);
    filterChildren(-1, accepted);
}
void CatalogModel::filterChildren(int parentId, const QSet<int> &visible)
{
    auto &children = parentId < 0 ? m_roots : m_nodes[parentId].children;
    const auto &all = parentId < 0 ? m_allRoots : m_nodes[parentId].allChildren;
    const auto parentIndex = parentId < 0 ? QModelIndex()
        : createIndex(m_nodes[parentId].row, 0, quintptr(parentId + 1));
    QList<int> wanted;
    for (int id : all)
        if (m_nodes[id].asset < 0 || visible.contains(m_nodes[id].asset)) wanted.append(id);
    const QSet<int> keep(wanted.cbegin(), wanted.cend());
    const bool changed = wanted != children;
    for (int last = int(children.size()) - 1; last >= 0;)
    {
        if (keep.contains(children[last])) { --last; continue; }
        int first = last;
        while (first > 0 && !keep.contains(children[first - 1])) --first;
        beginRemoveRows(parentIndex, first, last);
        for (int i = first; i <= last; ++i) m_nodes[children[i]].row = -1;
        children.remove(first, last - first + 1);
        for (int i = first; i < children.size(); ++i) m_nodes[children[i]].row = i;
        endRemoveRows();
        last = first - 1;
    }
    for (int row = 0; row < wanted.size();)
    {
        if (row < children.size() && children[row] == wanted[row]) { ++row; continue; }
        int end = row + 1;
        while (end < wanted.size() && (row >= children.size() || wanted[end] != children[row])) ++end;
        beginInsertRows(parentIndex, row, end - 1);
        for (int i = row; i < end; ++i) children.insert(i, wanted[i]);
        for (int i = row; i < children.size(); ++i) m_nodes[children[i]].row = i;
        endInsertRows();
        row = end;
    }
    if (changed && parentIndex.isValid()) emit dataChanged(parentIndex, parentIndex, {Qt::ToolTipRole});
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
        m_nodes.append(Node{-1, group, -1, int(m_roots.size()), {}, {}});
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
            m_nodes.append(Node{asset, group, parent, row, {}, {}});
            grouped.insert(asset);
        }
    }
    for (const int asset : m_visible)
        if (!grouped.contains(asset))
        {
            m_roots.append(int(m_nodes.size()));
            m_nodes.append(Node{asset, -1, -1, int(m_roots.size()) - 1, {}, {}});
        }
    m_allRoots = m_roots;
    for (auto &node : m_nodes) node.allChildren = node.children;
    m_assetNodes = QList<QList<int>>(m_assets.size());
    for (int i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].asset >= 0) m_assetNodes[m_nodes[i].asset].append(i);
}
QModelIndex CatalogModel::indexForId(const QString &id, const QString &group) const
{
    QModelIndex firstAsset, firstMatch, groupMatch;
    for (int i = 0; i < m_nodes.size(); ++i)
    {
        const auto &value = m_nodes[i];
        if (value.row < 0) continue;
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
