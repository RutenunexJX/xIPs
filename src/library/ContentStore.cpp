#include "ContentStore.h"
#include "FileSystemUtil.h"
#include "OperationControl.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QtEndian>
#include <stdexcept>
#include <algorithm>

namespace xips
{
namespace
{
constexpr qsizetype blockSize = 1024 * 1024;
const QByteArray magic = "XIPSOBJ1\n";
void check(bool ok, const QString &message)
{
    if (!ok)
        throw std::runtime_error(message.toUtf8().constData());
}
void write(QIODevice &file, const QByteArray &data)
{
    check(file.write(data) == data.size(), QStringLiteral("Cannot write content object"));
}
QByteArray integer(quint32 value)
{
    QByteArray data(4, Qt::Uninitialized);
    qToBigEndian(value, data.data());
    return data;
}
}
ContentStore::ContentStore(const QString &library) : m_library(files::normalizedAbsolute(library)) {}

bool ContentStore::validHash(const QString &hash)
{
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](QChar c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
void ContentStore::safePath(const QString &path)
{
    QString current = files::normalizedAbsolute(path);
    for (;;)
    {
        const QFileInfo info(current);
        check(!files::isLinkLike(info), QStringLiteral("Path contains a link: %1").arg(current));
        if (info.absolutePath() == current)
            break;
        current = info.absolutePath();
    }
}
void ContentStore::makeDirectory(const QString &path)
{
    safePath(path);
    check(QDir().mkpath(path), QStringLiteral("Cannot create directory: %1").arg(path));
    safePath(path);
}
QString ContentStore::objectPath(const QString &hash) const
{
    check(validHash(hash), QStringLiteral("Invalid content hash"));
    return QDir(m_library).filePath(".xips/objects/" + hash.left(2) + '/' + hash.mid(2) + ".obj");
}
ContentObject ContentStore::fingerprint(const QString &source)
{
    safePath(source);
    QFile file(source);
    check(QFileInfo(source).isFile() && file.open(QIODevice::ReadOnly),
          QStringLiteral("Cannot read source: %1").arg(source));
    const auto size = file.size();
    const auto modified = file.fileTime(QFileDevice::FileModificationTime);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        OperationScope::checkpoint(QStringLiteral("Reading %1").arg(QFileInfo(source).fileName()), file.pos(), size);
        const auto data = file.read(blockSize);
        check(!data.isEmpty() && file.error() == QFileDevice::NoError, QStringLiteral("Cannot read source: %1").arg(source));
        hash.addData(data);
    }
    check(file.error() == QFileDevice::NoError && file.pos() == size &&
              file.size() == size && file.fileTime(QFileDevice::FileModificationTime) == modified,
          QStringLiteral("Source changed while reading: %1").arg(source));
    return {QString::fromLatin1(hash.result().toHex()), size};
}
ContentObject ContentStore::putFile(const QString &source) const
{
    const auto known = fingerprint(source);
    if (QFileInfo::exists(objectPath(known.hash)))
    {
        verify(known);
        return known;
    }
    safePath(source);
    QFile input(source);
    check(QFileInfo(source).isFile() && input.open(QIODevice::ReadOnly),
          QStringLiteral("Cannot read source: %1").arg(source));
    const qint64 size = input.size();
    const auto modified = input.fileTime(QFileDevice::FileModificationTime);
    const QString directory = QDir(m_library).filePath(".xips/objects");
    makeDirectory(directory);
    QTemporaryFile staging(directory + "/.pending-XXXXXX");
    check(staging.open(), QStringLiteral("Cannot stage content object"));
    write(staging, magic);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!input.atEnd())
    {
        OperationScope::checkpoint(QStringLiteral("Storing %1").arg(QFileInfo(source).fileName()), count, size);
        const auto data = input.read(blockSize);
        check(input.error() == QFileDevice::NoError && !data.isEmpty(),
              QStringLiteral("Cannot read source: %1").arg(source));
        hash.addData(data);
        count += data.size();
        const auto compressed = qCompress(data, 6);
        check(!compressed.isEmpty(), QStringLiteral("Cannot compress content"));
        write(staging, integer(static_cast<quint32>(compressed.size())));
        write(staging, compressed);
    }
    write(staging, integer(0));
    check(count == size && input.size() == size &&
              input.fileTime(QFileDevice::FileModificationTime) == modified && staging.flush(),
          QStringLiteral("Source changed while saving: %1").arg(source));
    const ContentObject object{QString::fromLatin1(hash.result().toHex()), count};
    const QString target = objectPath(object.hash);
    makeDirectory(QFileInfo(target).absolutePath());
    safePath(target);
    staging.close();
    if (!QFileInfo::exists(target))
    {
        if (staging.rename(target))
            staging.setAutoRemove(false);
        else
            check(QFileInfo::exists(target), QStringLiteral("Cannot publish content: %1").arg(target));
    }
    verify(object);
    return object;
}
void ContentStore::readObject(const ContentObject &object, QIODevice *output) const
{
    const QString path = objectPath(object.hash);
    safePath(path);
    QFile file(path);
    check(object.size >= 0 && file.open(QIODevice::ReadOnly),
          QStringLiteral("History content is unavailable; synchronization may be incomplete: %1").arg(path));
    check(file.read(magic.size()) == magic, QStringLiteral("Invalid content object: %1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 count = 0;
    for (;;)
    {
        const auto header = file.read(4);
        OperationScope::checkpoint(QStringLiteral("Verifying content"), count, object.size);
        check(header.size() == 4, QStringLiteral("Truncated content object: %1").arg(path));
        const auto length = qFromBigEndian<quint32>(header.constData());
        if (!length)
            break;
        check(length >= 4 && length <= blockSize + 1024,
              QStringLiteral("Invalid compressed block: %1").arg(path));
        const auto compressed = file.read(length);
        check(compressed.size() == length, QStringLiteral("Truncated compressed block: %1").arg(path));
        const auto expanded = qFromBigEndian<quint32>(compressed.constData());
        check(expanded > 0 && expanded <= blockSize && count <= object.size - expanded,
              QStringLiteral("Invalid expanded block: %1").arg(path));
        const auto data = qUncompress(compressed);
        check(data.size() == expanded, QStringLiteral("Corrupted content object: %1").arg(path));
        hash.addData(data);
        count += data.size();
        if (output)
            write(*output, data);
    }
    check(file.atEnd() && file.error() == QFileDevice::NoError && count == object.size &&
              QString::fromLatin1(hash.result().toHex()) == object.hash,
          QStringLiteral("Content hash mismatch: %1").arg(path));
}
void ContentStore::verify(const ContentObject &object) const { readObject(object, nullptr); }
void ContentStore::materialize(const ContentObject &object, const QString &destination) const
{
    makeDirectory(QFileInfo(destination).absolutePath());
    safePath(destination);
    QFile output(destination);
    check(output.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          QStringLiteral("Cannot create exported file: %1").arg(destination));
    readObject(object, &output);
    check(output.flush(), QStringLiteral("Cannot flush exported file: %1").arg(destination));
}
QString ContentStore::treeHash(const QMap<QString, ContentObject> &objects)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("xips-tree-v1:"));
    for (auto it = objects.cbegin(); it != objects.cend(); ++it)
    {
        const auto path = it.key().toUtf8();
        hash.addData(QByteArray::number(path.size()) + ':' + path + ':' +
                     QByteArray::number(it->size) + ':' + it->hash.toLatin1() + ';');
    }
    return "sha256:" + QString::fromLatin1(hash.result().toHex());
}
void ContentStore::publishJson(const QString &path, const QJsonObject &document)
{
    OperationScope::checkpoint();
    makeDirectory(QFileInfo(path).absolutePath());
    safePath(path);
    QTemporaryFile staging(path + ".pending-XXXXXX");
    check(staging.open(), QStringLiteral("Cannot stage version manifest"));
    write(staging, QJsonDocument(document).toJson(QJsonDocument::Compact));
    check(staging.flush(), QStringLiteral("Cannot flush version manifest"));
    staging.close();
    OperationScope::publish();
    check(!QFileInfo::exists(path) && staging.rename(path),
          QStringLiteral("Version manifest exists or cannot be published: %1").arg(path));
    staging.setAutoRemove(false);
}
} // namespace xips
