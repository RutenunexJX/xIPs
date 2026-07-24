#pragma once

#include "assetcore/Asset.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace xips {

struct ManifestLoadResult {
    std::optional<Manifest> manifest;
    QList<Diagnostic> diagnostics;
    bool migrated = false;

    [[nodiscard]] bool ok() const
    {
        return manifest.has_value();
    }
};

class ManifestService {
public:
    static constexpr int CurrentSchemaVersion = 1;

    ManifestLoadResult load(const QString &manifestPath) const;
    ManifestLoadResult parse(const QByteArray &contents,
                             const QString &sourceName = QStringLiteral("<memory>")) const;

    QJsonObject toJson(const Manifest &manifest) const;
    bool write(const QString &manifestPath, const Manifest &manifest, QString *error = nullptr) const;
    QList<Diagnostic> validate(const Manifest &manifest,
                               const QString &sourceName = QStringLiteral("<memory>")) const;

private:
    bool migrate(QJsonObject &object,
                 int fromVersion,
                 QList<Diagnostic> &diagnostics,
                 const QString &sourceName) const;
};

} // namespace xips
