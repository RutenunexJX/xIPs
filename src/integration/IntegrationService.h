#pragma once

#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <optional>

namespace xips {

enum class ActivationAction {
    ShowLibrary,
    OpenAsset,
    Search
};

struct ActivationRequest {
    ActivationAction action = ActivationAction::ShowLibrary;
    QString value;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<ActivationRequest> fromJson(
        const QJsonObject &object);
};

class IntegrationService {
public:
    [[nodiscard]] static QUrl assetUri(const QString &assetId);
    [[nodiscard]] static QUrl searchUri(const QString &query);
    [[nodiscard]] static std::optional<ActivationRequest> parseUri(
        const QUrl &uri);
};

} // namespace xips
