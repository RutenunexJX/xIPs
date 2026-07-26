#pragma once

#include "assetcore/Asset.h"

#include <QAbstractTableModel>

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

private:
    QList<SearchHit> m_hits;
};

} // namespace xips
