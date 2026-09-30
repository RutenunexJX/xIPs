#include "CatalogGroups.h"
#include "ContentStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace xips
{
namespace
{
void require(bool value, const QString &message)
{
    if (!value) throw std::runtime_error(message.toUtf8().constData());
}
QString directory(const QString &library)
{
    require(!library.isEmpty() && QFileInfo(library).isDir(), QStringLiteral("Choose an available library folder first."));
    const auto root = QDir(library).absoluteFilePath(".xips/groups");
    ContentStore::safePath(root);
    require(!QFileInfo::exists(root) || (QFileInfo(root).isDir() && QFileInfo(root).isReadable()),
            QStringLiteral("Cannot read groups directory: %1").arg(root));
    return root;
}
QString filePath(const QString &root, const QString &id)
{
    require(!QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id,
            QStringLiteral("Invalid group ID."));
    return QDir(root).filePath(id + ".json");
}
QByteArray read(const QString &path)
{
    ContentStore::safePath(path);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024,
            QStringLiteral("Cannot read group: %1").arg(path));
    const auto bytes = file.readAll();
    require(file.error() == QFileDevice::NoError, QStringLiteral("Cannot read group: %1").arg(path));
    return bytes;
}
CatalogGroup decode(const QByteArray &bytes, const QString &id)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    const auto object = document.object();
    require(error.error == QJsonParseError::NoError && object["schema"] == "xips.group/v1" &&
                object["id"].toString() == id && object["name"].isString() && object["members"].isArray(),
            QStringLiteral("Invalid group metadata: %1").arg(id));
    CatalogGroup group{id, object["name"].toString().trimmed(), {}};
    require(!group.name.isEmpty() && group.name.size() <= 128, QStringLiteral("Invalid group name."));
    for (const auto &value : object["members"].toArray())
    {
        require(value.isString() && !value.toString().isEmpty() && value.toString().size() <= 512,
                QStringLiteral("Invalid group member."));
        if (!group.members.contains(value.toString(), Qt::CaseInsensitive)) group.members.append(value.toString());
    }
    return group;
}
GroupResult change(const QString &library, const QString &id,
                   const std::function<void(CatalogGroup &)> &edit, bool erase = false)
{
    GroupResult result;
    try
    {
        const auto root = directory(library);
        if (!id.isEmpty()) filePath(root, id);
        ContentStore::makeDirectory(root);
        const auto lockPath = QDir(root).filePath(".groups.lock");
        ContentStore::safePath(lockPath);
        QLockFile lock(lockPath);
        require(lock.tryLock(0), QStringLiteral("Groups are busy. Try again."));
        const bool creating = id.isEmpty();
        CatalogGroup group;
        group.id = creating ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
        const auto path = filePath(root, group.id);
        ContentStore::safePath(path);
        const QByteArray original = creating ? QByteArray() : read(path);
        if (!creating) group = decode(original, id);
        edit(group);
        group.name = group.name.trimmed();
        require(!group.name.isEmpty() && group.name.size() <= 128 && !group.name.contains('\n'),
                QStringLiteral("Use a group name of 1–128 characters."));
        if (!erase)
            for (const auto &other : CatalogGroups::scan(library).groups)
                require(other.id == group.id || other.name.compare(group.name, Qt::CaseInsensitive) != 0,
                        QStringLiteral("A group with this name already exists."));
        require(creating ? !QFileInfo::exists(path) : read(path) == original,
                QStringLiteral("Group changed elsewhere. Refresh and try again."));
        if (erase)
            require(QFile::remove(path), QStringLiteral("Cannot remove group."));
        else
        {
            const auto bytes = QJsonDocument(QJsonObject{{"schema", "xips.group/v1"}, {"id", group.id},
                {"name", group.name}, {"members", QJsonArray::fromStringList(group.members)}}).toJson();
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                    QStringLiteral("Cannot save group."));
            require(creating ? !QFileInfo::exists(path) : read(path) == original,
                    QStringLiteral("Group changed elsewhere. Refresh and try again."));
            require(file.commit(), QStringLiteral("Cannot save group."));
        }
        result.ok = true;
        result.removed = erase;
        result.group = group;
    }
    catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
    return result;
}
}
GroupCatalog CatalogGroups::scan(const QString &library)
{
    GroupCatalog result;
    try
    {
        const auto root = directory(library);
        for (const auto &entry : QDir(root).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        {
            try
            {
                const auto id = entry.completeBaseName();
                const auto path = filePath(root, id);
                result.groups.append(decode(read(path), id));
            }
            catch (const std::exception &error) { result.problems.append(QString::fromUtf8(error.what())); }
        }
        std::sort(result.groups.begin(), result.groups.end(), [](const auto &a, const auto &b)
        {
            const int order = a.name.compare(b.name, Qt::CaseInsensitive);
            return order ? order < 0 : a.id < b.id;
        });
    }
    catch (const std::exception &error) { result.problems.append(QString::fromUtf8(error.what())); }
    return result;
}
GroupResult CatalogGroups::create(const QString &library, const QString &name)
{
    return change(library, {}, [name](CatalogGroup &group) { group.name = name; });
}
GroupResult CatalogGroups::rename(const QString &library, const QString &id, const QString &name)
{
    return change(library, id, [name](CatalogGroup &group) { group.name = name; });
}
GroupResult CatalogGroups::setMember(const QString &library, const QString &id, const QString &assetId, bool included)
{
    return change(library, id, [assetId, included](CatalogGroup &group)
    {
        require(!assetId.isEmpty() && assetId.size() <= 512, QStringLiteral("Invalid IP ID."));
        group.members.erase(std::remove_if(group.members.begin(), group.members.end(), [&](const QString &value)
            { return value.compare(assetId, Qt::CaseInsensitive) == 0; }), group.members.end());
        if (included) group.members.append(assetId);
    });
}
GroupResult CatalogGroups::erase(const QString &library, const QString &id)
{
    return change(library, id, [](CatalogGroup &) {}, true);
}
}
