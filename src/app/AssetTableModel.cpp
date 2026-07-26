#include "app/AssetTableModel.h"

namespace xips {

AssetTableModel::AssetTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int AssetTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_hits.size());
}

int AssetTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AssetTableModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_hits.size()) {
        return {};
    }
    const SearchHit &hit = m_hits.at(index.row());
    const AssetRecord &asset = hit.asset;
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1\nID: %2\n%3")
            .arg(asset.manifest.description,
                 asset.manifest.id,
                 asset.assetRoot);
    }
    if (role == Qt::UserRole) {
        switch (index.column()) {
        case NameColumn:
            return asset.manifest.name.toCaseFolded();
        case VersionColumn:
            return asset.manifest.version.toCaseFolded();
        case GroupsColumn:
            return asset.manifest.tags.join(u' ').toCaseFolded();
        case FilesColumn:
            return static_cast<qlonglong>(asset.fileCount);
        case ModifiedColumn:
            return asset.lastModified;
        }
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (index.column()) {
    case NameColumn:
        return asset.manifest.name;
    case VersionColumn:
        return asset.manifest.version.isEmpty() ? QStringLiteral("-")
                                                : asset.manifest.version;
    case GroupsColumn:
        return asset.manifest.tags.join(QStringLiteral(", "));
    case FilesColumn:
        return static_cast<qlonglong>(asset.fileCount);
    case ModifiedColumn:
        return asset.lastModified.isValid()
                   ? asset.lastModified.toLocalTime().toString(
                         QStringLiteral("yyyy-MM-dd HH:mm"))
                   : QStringLiteral("-");
    }
    return {};
}

QVariant AssetTableModel::headerData(const int section,
                                     const Qt::Orientation orientation,
                                     const int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{
        QStringLiteral("Asset"),
        QStringLiteral("Last saved"),
        QStringLiteral("Groups"),
        QStringLiteral("Files"),
        QStringLiteral("Modified"),
    };
    return section >= 0 && section < headers.size() ? headers.at(section)
                                                    : QVariant();
}

void AssetTableModel::setHits(QList<SearchHit> hits)
{
    beginResetModel();
    m_hits = std::move(hits);
    endResetModel();
}

const AssetRecord *AssetTableModel::recordAt(const int row) const
{
    return row >= 0 && row < m_hits.size() ? &m_hits.at(row).asset : nullptr;
}

} // namespace xips
