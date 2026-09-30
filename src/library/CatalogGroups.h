#pragma once
#include <QList>
#include <QStringList>

namespace xips
{
struct CatalogGroup
{
    QString id;
    QString name;
    QStringList members;
    bool operator==(const CatalogGroup &) const = default;
};
struct GroupCatalog
{
    QList<CatalogGroup> groups;
    QStringList problems;
};
struct GroupResult
{
    bool ok = false;
    bool removed = false;
    QString error;
    CatalogGroup group;
};
class CatalogGroups
{
  public:
    static GroupCatalog scan(const QString &library);
    static GroupResult create(const QString &library, const QString &name);
    static GroupResult rename(const QString &library, const QString &id, const QString &name);
    static GroupResult setMember(const QString &library, const QString &id, const QString &assetId, bool included);
    static GroupResult erase(const QString &library, const QString &id);
};
}
