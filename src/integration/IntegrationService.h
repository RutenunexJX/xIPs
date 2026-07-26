#pragma once

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
};

class IntegrationService {
public:
    [[nodiscard]] static QUrl assetUri(const QString &assetId);
    [[nodiscard]] static std::optional<ActivationRequest> parseUri(
        const QUrl &uri);
};

} // namespace xips
