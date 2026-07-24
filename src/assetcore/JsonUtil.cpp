#include "assetcore/JsonUtil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>

namespace xips::json {
namespace {

QJsonValue sortedValue(const QJsonValue &value)
{
    if (value.isArray()) {
        QJsonArray result;
        const QJsonArray input = value.toArray();
        for (const QJsonValue &entry : input) {
            result.append(sortedValue(entry));
        }
        return result;
    }

    if (value.isObject()) {
        const QJsonObject input = value.toObject();
        QStringList keys = input.keys();
        std::sort(keys.begin(), keys.end());
        QJsonObject result;
        for (const QString &key : keys) {
            result.insert(key, sortedValue(input.value(key)));
        }
        return result;
    }

    return value;
}

} // namespace

QByteArray canonicalJson(const QJsonValue &value)
{
    if (value.isObject()) {
        return QJsonDocument(sortedValue(value).toObject()).toJson(QJsonDocument::Compact);
    }
    if (value.isArray()) {
        return QJsonDocument(sortedValue(value).toArray()).toJson(QJsonDocument::Compact);
    }

    QJsonArray wrapper;
    wrapper.append(value);
    QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return encoded.mid(1, encoded.size() - 2);
}

QStringList stringList(const QJsonObject &object, const QString &key, QString *error)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined() || value.isNull()) {
        return {};
    }
    if (!value.isArray()) {
        if (error) {
            *error = QStringLiteral("Field '%1' must be an array of strings").arg(key);
        }
        return {};
    }

    QStringList result;
    const QJsonArray array = value.toArray();
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isString()) {
            if (error) {
                *error = QStringLiteral("Field '%1' entry %2 must be a string").arg(key).arg(index);
            }
            return {};
        }
        result.append(array.at(index).toString());
    }
    return result;
}

QJsonArray toArray(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values) {
        result.append(value);
    }
    return result;
}

QString normalizeSearchText(const QString &value)
{
    QString result;
    result.reserve(value.size());
    bool previousWasSpace = false;
    for (const QChar character : value.normalized(QString::NormalizationForm_KC).toCaseFolded()) {
        if (character.isLetterOrNumber() || character == u'_' || character == u'-'
            || character == u'/' || character == u'.') {
            result.append(character);
            previousWasSpace = false;
        } else if (!previousWasSpace) {
            result.append(u' ');
            previousWasSpace = true;
        }
    }
    return result.trimmed();
}

QStringList trigrams(const QString &value)
{
    const QString normalized = normalizeSearchText(value);
    if (normalized.isEmpty()) {
        return {};
    }

    QSet<QString> unique;
    const QString padded = QStringLiteral("  ") + normalized + QStringLiteral("  ");
    if (padded.size() <= 3) {
        unique.insert(padded);
    } else {
        for (qsizetype index = 0; index + 3 <= padded.size(); ++index) {
            unique.insert(padded.mid(index, 3));
        }
    }

    QStringList result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace xips::json
