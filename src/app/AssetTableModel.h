#pragma once

#include "assetcore/Asset.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

namespace xips {

class AssetTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        TypeColumn,
        VersionColumn,
        TopColumn,
        LanguageColumn,
        TestColumn,
        DiagnosticsColumn,
        ModifiedColumn,
        RepositoryColumn,
        LastUsedColumn,
        ColumnCount
    };

    explicit AssetTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setHits(QList<SearchHit> hits);
    void setLastUsed(const QString &assetId, const QDateTime &when);
    [[nodiscard]] const AssetRecord *recordAt(int row) const;
    [[nodiscard]] const SearchHit *hitAt(int row) const;

private:
    QList<SearchHit> m_hits;
};

class AssetFilterProxyModel final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit AssetFilterProxyModel(QObject *parent = nullptr);

    void clearAssetFilter();
    void setTypeFilter(AssetType type);
    void setTagFilter(const QString &tag);
    void setProjectFilter(const QString &project);
    void setStatusFilter(const QString &status);
    void setFavoritesOnly(bool enabled);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    AssetType m_type = AssetType::Unknown;
    QString m_tag;
    QString m_project;
    QString m_status;
    bool m_favoritesOnly = false;
};

} // namespace xips
