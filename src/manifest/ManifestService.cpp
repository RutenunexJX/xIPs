#include "manifest/ManifestService.h"

#include "assetcore/JsonUtil.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace xips {
namespace {

QString readString(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

QString sourceError(const QString &sourceName, const QString &message)
{
    return sourceName.isEmpty() ? message
                                : QStringLiteral("%1: %2").arg(sourceName, message);
}

} // namespace

ManifestLoadResult ManifestService::load(const QString &manifestPath) const
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {
            .manifest = std::nullopt,
            .errors = {QStringLiteral("Cannot open manifest: %1")
                           .arg(file.errorString())},
        };
    }
    return parse(file.readAll(), QFileInfo(manifestPath).absoluteFilePath());
}

ManifestLoadResult ManifestService::parse(const QByteArray &contents,
                                          const QString &sourceName) const
{
    ManifestLoadResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.errors.append(sourceError(
            sourceName,
            QStringLiteral("Invalid JSON object at byte %1: %2")
                .arg(parseError.offset)
                .arg(parseError.errorString())));
        return result;
    }

    const QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schemaVersion"));
    if (!schema.isDouble() || schema.toDouble() != CurrentSchemaVersion) {
        result.errors.append(sourceError(
            sourceName,
            QStringLiteral("schemaVersion must be %1").arg(CurrentSchemaVersion)));
        return result;
    }

    Manifest manifest;
    manifest.rawObject = object;
    manifest.id = readString(object, QStringLiteral("id"));
    manifest.name = readString(object, QStringLiteral("name"));
    manifest.description = readString(object, QStringLiteral("description"));
    manifest.version = readString(object, QStringLiteral("version"));
    QString tagsError;
    manifest.tags = json::stringList(object, QStringLiteral("tags"), &tagsError);
    if (!tagsError.isEmpty()) {
        result.errors.append(sourceError(sourceName, tagsError));
    }
    for (const QString &error : validate(manifest)) {
        result.errors.append(sourceError(sourceName, error));
    }
    if (result.errors.isEmpty()) {
        result.manifest = manifest;
    }
    return result;
}

QJsonObject ManifestService::toJson(const Manifest &manifest) const
{
    QJsonObject object = manifest.rawObject;
    static const QStringList removedFields{
        QStringLiteral("type"),
        QStringLiteral("kind"),
        QStringLiteral("displayName"),
        QStringLiteral("files"),
        QStringLiteral("top"),
        QStringLiteral("language"),
        QStringLiteral("sources"),
        QStringLiteral("constraints"),
        QStringLiteral("documentation"),
        QStringLiteral("tools"),
    };
    for (const QString &field : removedFields) {
        object.remove(field);
    }

    object.insert(QStringLiteral("schemaVersion"), CurrentSchemaVersion);
    object.insert(QStringLiteral("id"), manifest.id.trimmed());
    object.insert(QStringLiteral("name"), manifest.name.trimmed());
    const auto setOptional = [&object](const QString &key, const QString &value) {
        if (value.trimmed().isEmpty()) {
            object.remove(key);
        } else {
            object.insert(key, value.trimmed());
        }
    };
    setOptional(QStringLiteral("description"), manifest.description);
    setOptional(QStringLiteral("version"), manifest.version);
    if (manifest.tags.isEmpty()) {
        object.remove(QStringLiteral("tags"));
    } else {
        object.insert(QStringLiteral("tags"), json::toArray(manifest.tags));
    }
    return object;
}

bool ManifestService::write(const QString &manifestPath,
                            const Manifest &manifest,
                            QString *error) const
{
    const QStringList errors = validate(manifest);
    if (!errors.isEmpty()) {
        if (error) {
            *error = errors.first();
        }
        return false;
    }

    QSaveFile file(manifestPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot open manifest for atomic write: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    const QByteArray data = QJsonDocument(toJson(manifest)).toJson(
        QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error) {
            *error = QStringLiteral("Cannot write manifest: %1")
                         .arg(file.errorString());
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("Cannot atomically replace manifest: %1")
                         .arg(file.errorString());
        }
        return false;
    }
    return true;
}

QStringList ManifestService::validate(const Manifest &manifest) const
{
    QStringList result;
    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    if (!idPattern.match(manifest.id).hasMatch()) {
        result.append(QStringLiteral("Asset id must start with an alphanumeric character and use only letters, digits, '_', '-' or '.'"));
    }
    if (manifest.name.trimmed().isEmpty()) {
        result.append(QStringLiteral("Asset name is required"));
    }
    return result;
}

} // namespace xips
