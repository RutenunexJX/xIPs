#pragma once
#include <QStandardItemModel>
#include <QSet>
#include <QHash>

namespace xips
{
class WorkingFilesModel final : public QStandardItemModel
{
    Q_OBJECT
  public:
    explicit WorkingFilesModel(QObject *parent = nullptr);
    bool setFiles(const QStringList &files, const QSet<QString> &checked, bool editable);
    void checkAll(bool checked);
    QStringList checkedFiles() const;
    QModelIndex fileIndex(const QString &relative) const;
    QModelIndex pathIndex(const QString &relative) const;
    int fileCount() const { return m_files.size(); }
  signals:
    void checkedFilesChanged();
  private:
    void apply(QStandardItem *item, Qt::CheckState state);
    void updateParents();
    QHash<QString, QStandardItem *> m_files;
    QHash<QString, QStandardItem *> m_folderItems;
    QList<QStandardItem *> m_folders;
    QStringList m_paths;
    bool m_editable = false;
    bool m_updating = false;
};
}
