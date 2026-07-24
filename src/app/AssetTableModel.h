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
        VersionColumn,
        TagsColumn,
        FilesColumn,
        ModifiedColumn,
        ColumnCount
    };

    explicit AssetTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setHits(QList<SearchHit> hits);
    [[nodiscard]] const AssetRecord *recordAt(int row) const;
    [[nodiscard]] const SearchHit *hitAt(int row) const;

private:
    QList<SearchHit> m_hits;
};

class AssetFilterProxyModel final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit AssetFilterProxyModel(QObject *parent = nullptr);
    void setTagFilter(const QString &tag);

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override;

private:
    QString m_tag;
};

} // namespace xips
