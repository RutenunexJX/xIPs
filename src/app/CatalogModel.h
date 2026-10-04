#pragma once
#include "library/SnapshotLibrary.h"
#include <QAbstractItemModel>
#include <QHash>

namespace xips
{
class CatalogModel final : public QAbstractItemModel
{
    Q_OBJECT
  public:
    using QAbstractItemModel::QAbstractItemModel;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    void setAssets(const QList<CatalogAsset> &assets, const QString &library = {},
                   const QList<CatalogGroup> &groups = {});
    void filter(const QString &category, const QStringList &terms);
    bool updateAsset(const CatalogAsset &asset);
    QModelIndex indexForId(const QString &id, const QString &group = {}) const;
    int assetIndex(const QModelIndex &index) const;
    QString groupId(const QModelIndex &index) const;
    const QList<CatalogGroup> &groups() const { return m_groups; }
  signals:
    void groupMembershipRequested(const QString &groupId, const QString &assetId);
  private:
    QString droppedAsset(const QMimeData *data) const;
    struct Node { int asset = -1, group = -1, parent = -1, row = 0; QList<int> children, allChildren; };
    const Node *node(const QModelIndex &index) const;
    void rebuild();
    void filterChildren(int parent, const QSet<int> &visible);
    struct Row { QString id, root, category, label, tooltip, search; };
    static Row assetRow(const CatalogAsset &asset);
    QList<Row> m_rows;
    QList<int> m_visible;
    QString m_library;
    QList<CatalogAsset> m_assets;
    QList<CatalogGroup> m_groups;
    QList<Node> m_nodes;
    QHash<QString, int> m_assetLookup;
    QList<QList<int>> m_assetNodes;
    QList<int> m_roots;
    QList<int> m_allRoots;
};
}
