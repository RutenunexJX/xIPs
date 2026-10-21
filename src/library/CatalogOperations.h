#pragma once
#include "SnapshotLibrary.h"
#include <QJsonObject>
#include <stdexcept>
namespace xips
{
class OperationError final : public std::runtime_error
{
  public:
    OperationError(QString code, QString message, int exitCode = 3, QJsonObject details = {});
    QString code;
    int exitCode;
    QJsonObject details;
};
class CatalogOperations
{
  public:
    static QJsonObject capabilities();
    static QJsonObject normalize(QJsonObject request, const QString &baseDirectory);
    static bool isMutation(const QString &action);
    static QJsonObject read(const QJsonObject &request);
    static QJsonObject preview(const QJsonObject &request);
    static QJsonObject apply(const QJsonObject &plan);
    static QString viewToken(const QList<CatalogAsset> &assets, const QList<CatalogGroup> &groups);
    static QJsonObject assetJson(const CatalogAsset &asset);
    static QJsonObject snapshotJson(const Snapshot &snapshot);
};
} // namespace xips
