#pragma once
#include "library/SnapshotLibrary.h"
#include <QAbstractListModel>

namespace xips
{
class CatalogModel final : public QAbstractListModel
{
  public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void setAssets(const QList<CatalogAsset> &assets, const QString &library = {});
    void filter(const QString &category, const QStringList &terms);
    int rowForId(const QString &id) const;
    int assetIndex(int row) const;
  private:
    struct Row { QString id, root, category, label, tooltip, search; };
    QList<Row> m_rows;
    QList<int> m_visible;
    QString m_library, m_generation;
    QList<CatalogAsset> m_assets;
};
}
