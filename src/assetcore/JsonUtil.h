#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace xips::json {

QByteArray canonicalJson(const QJsonValue &value);
QStringList stringList(const QJsonObject &object, const QString &key, QString *error = nullptr);
QJsonArray toArray(const QStringList &values);
QString normalizeSearchText(const QString &value);
QStringList trigrams(const QString &value);

} // namespace xips::json
