#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

namespace xips::archive {
// Validate a container without extracting it. Return the exact virtual XCI path.
QString inspectCoreContainer(const QString& file, const std::function<void()>& check,
                             const std::function<void(const QString&, const QByteArray&)>& metadata);
}
