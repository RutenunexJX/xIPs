#include "CoreContainer.h"
#include <QDomDocument>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <QtCore/private/qzipreader_p.h>
#include <array>
#include <stdexcept>

namespace xips::archive {
namespace {
[[noreturn]] void reject(const QString& file, const QString& reason)
{
    throw std::runtime_error(QString("Invalid core container %1: %2").arg(file, reason).toUtf8().constData());
}
bool safeName(const QString& name)
{
    if (name.isEmpty() || name.startsWith('/') || name.contains(QRegularExpression("[\\\\:\"<>|?*\\x00-\\x1f\\x7f]"))) return false;
    const auto parts = name.split('/');
    const QRegularExpression reserved("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
    for (const auto& part : parts)
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ') || reserved.match(part).hasMatch()) return false;
    return true;
}
quint32 crc32(const QByteArray& bytes)
{
    static const auto table = [] {
        std::array<quint32, 256> result{};
        for (quint32 i = 0; i < result.size(); ++i) {
            auto value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
            result[i] = value;
        }
        return result;
    }();
    quint32 value = 0xffffffffU;
    for (const auto byte : bytes) value = table[(value ^ static_cast<unsigned char>(byte)) & 255] ^ (value >> 8);
    return value ^ 0xffffffffU;
}
}
QString inspectCoreContainer(const QString& file, const std::function<void()>& check,
                             const std::function<void(const QString&, const QByteArray&)>& metadata)
{
    check();
    if (QFileInfo(file).size() > 512 * 1024 * 1024) reject(file, "compressed size exceeds 512 MiB");
    QZipReader zip(file);
    const auto entries = zip.fileInfoList();
    if (zip.status() != QZipReader::NoError || entries.isEmpty() || entries.size() > 65535)
        reject(file, "unreadable, empty or unsupported ZIP directory");
    QMap<QString, bool> names;
    QSet<QString> files;
    qint64 total = 0;
    for (const auto& entry : entries) {
        check();
        auto name = entry.filePath;
        if (entry.isDir && name.endsWith('/')) name.chop(1);
        if (!safeName(name) || entry.isSymLink || (!entry.isFile && !entry.isDir)) reject(file, "unsafe member: " + entry.filePath);
        const auto key = name.toCaseFolded();
        if (names.contains(key)) reject(file, "duplicate member: " + name);
        names.insert(key, entry.isDir);
        if (entry.isFile) files.insert(entry.filePath);
        if (entry.size < 0 || entry.size > 256 * 1024 * 1024 || (total += entry.size) > 1024LL * 1024 * 1024)
            reject(file, "expanded size exceeds the supported limit");
        if (QFileInfo(name).suffix().compare("xcix", Qt::CaseInsensitive) == 0) reject(file, "nested core containers are unsupported");
    }
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        auto parent = it.key();
        while (parent.contains('/')) {
            parent = parent.left(parent.lastIndexOf('/'));
            if (names.contains(parent) && !names.value(parent)) reject(file, "file/directory collision: " + parent);
        }
    }
    QByteArray descriptor;
    for (const auto& entry : entries) {
        check();
        if (entry.isDir) continue;
        const auto suffix = QFileInfo(entry.filePath).suffix().toLower();
        const bool isMetadata = suffix == "xml" || suffix == "xci" || suffix == "bd";
        if (isMetadata && entry.size > 64 * 1024 * 1024) reject(file, "metadata exceeds 64 MiB: " + entry.filePath);
        const auto bytes = zip.fileData(entry.filePath);
        if (zip.status() != QZipReader::NoError || bytes.size() != entry.size || crc32(bytes) != entry.crc)
            reject(file, "unreadable or corrupt member: " + entry.filePath);
        check();
        if (entry.filePath == "cc.xml") descriptor = bytes;
        if (isMetadata) metadata(entry.filePath, bytes);
    }
    QDomDocument doc;
    if (descriptor.isEmpty() || !doc.setContent(descriptor) || !doc.doctype().name().isEmpty()) reject(file, "invalid cc.xml");
    const auto root = doc.documentElement();
    const auto cores = root.elementsByTagName("CoreFile");
    if (root.tagName() != "CoreContainer" || root.attribute("MajorVersion") != "0" || root.attribute("MinorVersion") != "0" || cores.size() != 1)
        reject(file, "unsupported core-container descriptor");
    const auto core = cores.at(0).toElement().text().trimmed();
    if (!safeName(core) || !core.endsWith(".xci", Qt::CaseInsensitive) || !files.contains(core))
        reject(file, "CoreFile does not identify an existing XCI member");
    return core;
}
}
