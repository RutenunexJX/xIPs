#pragma once

#include <QMap>
#include <QString>
#include <QIODevice>
#include <QJsonObject>

namespace xips
{
struct ContentObject
{
    QString hash;
    qint64 size = 0;
    bool operator==(const ContentObject &) const = default;
};

// Library-wide immutable objects. Files are streamed in bounded compressed blocks.
class ContentStore
{
  public:
    explicit ContentStore(const QString &library);
    ContentObject putFile(const QString &source) const;
    void verify(const ContentObject &object) const;
    void materialize(const ContentObject &object, const QString &destination) const;
    QString objectPath(const QString &hash) const;
    static ContentObject fingerprint(const QString &source);
    static QString treeHash(const QMap<QString, ContentObject> &objects);
    static bool validHash(const QString &hash);
    static void safePath(const QString &path);
    static void makeDirectory(const QString &path);
    static void publishJson(const QString &path, const QJsonObject &document);

  private:
    QString m_library;
    void readObject(const ContentObject &object, QIODevice *output) const;
};
} // namespace xips
