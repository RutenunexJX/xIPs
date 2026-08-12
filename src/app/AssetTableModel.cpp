#include "app/AssetTableModel.h"

#include <QDir>

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
    if (role == AssetNameRole) {
        return asset.manifest.name;
    }
    if (role == MatchedFileRole) {
        return hit.matchedFile;
    }
    if (role == Qt::ToolTipRole) {
        QString details = asset.manifest.description;
        if (!hit.matchedFile.isEmpty()) {
            details += QStringLiteral("\nMatched file: %1").arg(hit.matchedFile);
        }
        return details.trimmed();
    }
    if (role == SortRole) {
        switch (index.column()) {
        case NameColumn:
            return asset.manifest.name.toCaseFolded();
        case GroupsColumn:
            return asset.manifest.tags.join(u' ').toCaseFolded();
        case VersionColumn:
            return asset.manifest.version.toCaseFolded();
        }
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (index.column()) {
    case NameColumn:
        return hit.matchedFile.isEmpty()
                   ? asset.manifest.name
                   : QStringLiteral("%1\nMatched: %2")
                         .arg(asset.manifest.name,
                              QDir::toNativeSeparators(hit.matchedFile));
    case GroupsColumn:
        return asset.manifest.tags.isEmpty()
                   ? QStringLiteral("-")
                   : asset.manifest.tags.join(QStringLiteral(", "));
    case VersionColumn:
        return asset.manifest.version.isEmpty() ? QStringLiteral("-")
                                                : asset.manifest.version;
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
        QStringLiteral("Groups"),
        QStringLiteral("Latest saved"),
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
