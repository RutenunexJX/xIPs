#pragma once
#include <QJsonObject>
#include <QString>
namespace xips
{
class OperationControl;
}
namespace xips::cli
{
bool isArchiveRequest(const QJsonObject &request);
QJsonObject archiveCapabilities();
QJsonObject normalizeArchive(QJsonObject request, const QString &baseDirectory);
QJsonObject previewArchiveRequest(const QJsonObject &request);
QJsonObject readArchiveRequest(const QJsonObject &request, OperationControl *control);
QJsonObject applyArchivePlan(const QJsonObject &plan, OperationControl *control);
} // namespace xips::cli
