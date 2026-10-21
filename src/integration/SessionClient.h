#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
namespace xips
{
class SessionClient
{
  public:
    static bool available();
    static QString processStamp(qint64 pid);
    static QString directory(const QString &overrideDirectory = {});
    static QJsonArray list(const QString &directory = {});
    static QJsonObject request(const QString &id, const QString &method, const QJsonObject &params = {},
                               const QString &directory = {}, int timeoutMs = 2000);
};
} // namespace xips
