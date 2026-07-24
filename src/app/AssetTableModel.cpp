#include "app/AssetTableModel.h"

#include <QFileInfo>

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
    const AssetRecord &record = hit.asset;

    if (role == Qt::ToolTipRole) {
        QString tooltip = QStringLiteral("%1\nID: %2\n%3")
                              .arg(record.manifest.description,
                                   record.manifest.id,
                                   record.manifestPath);
        if (!hit.matchedFields.isEmpty()) {
            tooltip += QStringLiteral("\nMatched: %1").arg(hit.matchedFields.join(QStringLiteral(", ")));
        }
        return tooltip;
    }
    if (role == Qt::UserRole) {
        switch (index.column()) {
        case NameColumn:
            return record.manifest.name.toCaseFolded();
        case TypeColumn:
            return assetTypeToString(record.manifest.type);
        case VersionColumn:
            return record.manifest.version;
        case TopColumn:
            return record.manifest.top;
        case LanguageColumn:
            return record.manifest.language;
        case TestColumn:
            return record.testStatus;
        case DiagnosticsColumn:
            return record.diagnosticsStatus;
        case ModifiedColumn:
            return record.modifiedStatus;
        case RepositoryColumn:
            return record.sourceRepository;
        case LastUsedColumn:
            return record.lastUsed;
        }
    }
    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case NameColumn:
        return record.manifest.name;
    case TypeColumn:
        return assetTypeToString(record.manifest.type);
    case VersionColumn:
        return record.manifest.version.isEmpty() ? QStringLiteral("working")
                                                 : record.manifest.version;
    case TopColumn:
        return record.manifest.top;
    case LanguageColumn:
        return record.manifest.language;
    case TestColumn:
        return record.testStatus;
    case DiagnosticsColumn:
        return record.diagnosticsStatus;
    case ModifiedColumn:
        return record.modifiedStatus;
    case RepositoryColumn:
        return QFileInfo(record.sourceRepository).fileName();
    case LastUsedColumn:
        return record.lastUsed.isValid()
                   ? record.lastUsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                   : QStringLiteral("—");
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
        QStringLiteral("Name"),
        QStringLiteral("Type"),
        QStringLiteral("Version"),
        QStringLiteral("Top"),
        QStringLiteral("Language"),
        QStringLiteral("Test"),
        QStringLiteral("Diagnostics"),
        QStringLiteral("Modified"),
        QStringLiteral("Repository"),
        QStringLiteral("Last used"),
    };
    return section >= 0 && section < headers.size() ? headers.at(section) : QVariant();
}

void AssetTableModel::setHits(QList<SearchHit> hits)
{
    beginResetModel();
    m_hits = std::move(hits);
    endResetModel();
}

void AssetTableModel::setLastUsed(const QString &assetId,
                                  const QDateTime &when)
{
    for (qsizetype row = 0; row < m_hits.size(); ++row) {
        if (m_hits.at(row).asset.manifest.id != assetId) {
            continue;
        }
        m_hits[row].asset.lastUsed = when;
        const QModelIndex changed =
            index(static_cast<int>(row), LastUsedColumn);
        emit dataChanged(changed,
                         changed,
                         {Qt::DisplayRole, Qt::UserRole});
    }
}

const AssetRecord *AssetTableModel::recordAt(const int row) const
{
    return row >= 0 && row < m_hits.size() ? &m_hits.at(row).asset : nullptr;
}

const SearchHit *AssetTableModel::hitAt(const int row) const
{
    return row >= 0 && row < m_hits.size() ? &m_hits.at(row) : nullptr;
}

AssetFilterProxyModel::AssetFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(Qt::UserRole);
    setDynamicSortFilter(true);
}

void AssetFilterProxyModel::clearAssetFilter()
{
    beginFilterChange();
    m_type = AssetType::Unknown;
    m_tag.clear();
    m_project.clear();
    m_status.clear();
    m_favoritesOnly = false;
    endFilterChange();
}

void AssetFilterProxyModel::setTypeFilter(const AssetType type)
{
    beginFilterChange();
    m_type = type;
    m_tag.clear();
    m_project.clear();
    m_status.clear();
    m_favoritesOnly = false;
    endFilterChange();
}

void AssetFilterProxyModel::setTagFilter(const QString &tag)
{
    beginFilterChange();
    m_type = AssetType::Unknown;
    m_tag = tag;
    m_project.clear();
    m_status.clear();
    m_favoritesOnly = false;
    endFilterChange();
}

void AssetFilterProxyModel::setProjectFilter(const QString &project)
{
    beginFilterChange();
    m_type = AssetType::Unknown;
    m_tag.clear();
    m_project = project;
    m_status.clear();
    m_favoritesOnly = false;
    endFilterChange();
}

void AssetFilterProxyModel::setStatusFilter(const QString &status)
{
    beginFilterChange();
    m_type = AssetType::Unknown;
    m_tag.clear();
    m_project.clear();
    m_status = status;
    m_favoritesOnly = false;
    endFilterChange();
}

void AssetFilterProxyModel::setFavoritesOnly(const bool enabled)
{
    beginFilterChange();
    m_type = AssetType::Unknown;
    m_tag.clear();
    m_project.clear();
    m_status.clear();
    m_favoritesOnly = enabled;
    endFilterChange();
}

bool AssetFilterProxyModel::filterAcceptsRow(const int sourceRow,
                                             const QModelIndex &sourceParent) const
{
    const auto *model = qobject_cast<const AssetTableModel *>(sourceModel());
    if (!model) {
        return true;
    }
    const AssetRecord *record = model->recordAt(sourceRow);
    if (!record) {
        return false;
    }
    if (m_type != AssetType::Unknown && record->manifest.type != m_type) {
        return false;
    }
    if (!m_tag.isEmpty() && !record->manifest.tags.contains(m_tag, Qt::CaseInsensitive)) {
        return false;
    }
    if (!m_project.isEmpty()
        && record->manifest.rawObject.value(QStringLiteral("project")).toString()
               != m_project) {
        return false;
    }
    if (m_favoritesOnly
        && !record->manifest.rawObject.value(QStringLiteral("favorite")).toBool(false)) {
        return false;
    }
    if (m_status == QStringLiteral("modified")
        && record->modifiedStatus != QStringLiteral("modified")) {
        return false;
    }
    if (m_status == QStringLiteral("diagnostics")
        && record->diagnosticsStatus != QStringLiteral("error")
        && record->diagnosticsStatus != QStringLiteral("manifest-error")) {
        return false;
    }
    if (m_status == QStringLiteral("test-failed")
        && record->testStatus != QStringLiteral("failed")) {
        return false;
    }
    return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
}

} // namespace xips
