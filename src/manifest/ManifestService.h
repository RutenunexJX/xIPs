#pragma once

#include "assetcore/Asset.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace xips {

struct ManifestLoadResult {
    std::optional<Manifest> manifest;
    QStringList errors;

    [[nodiscard]] bool ok() const
    {
        return manifest.has_value();
    }
};

class ManifestService {
public:
    static constexpr int CurrentSchemaVersion = 1;

    [[nodiscard]] ManifestLoadResult load(const QString &manifestPath) const;
    [[nodiscard]] ManifestLoadResult parse(
        const QByteArray &contents,
        const QString &sourceName = QStringLiteral("<memory>")) const;

    [[nodiscard]] QJsonObject toJson(const Manifest &manifest) const;
    bool write(const QString &manifestPath,
               const Manifest &manifest,
               QString *error = nullptr) const;
    [[nodiscard]] QStringList validate(const Manifest &manifest) const;
};

} // namespace xips
