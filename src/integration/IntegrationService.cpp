#include "integration/IntegrationService.h"

#include <QUrlQuery>

namespace xips {

bool ActivationRequest::isValid() const
{
    return action == ActivationAction::ShowLibrary || !value.trimmed().isEmpty();
}

QUrl IntegrationService::assetUri(const QString &assetId)
{
    QUrl uri;
    uri.setScheme(QStringLiteral("xips"));
    uri.setHost(QStringLiteral("asset"));
    uri.setPath(u'/' + assetId.trimmed());
    return uri;
}

std::optional<ActivationRequest> IntegrationService::parseUri(const QUrl &uri)
{
    if (!uri.isValid() || uri.scheme().compare(QStringLiteral("xips"),
                                               Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    if (uri.host().compare(QStringLiteral("asset"), Qt::CaseInsensitive) == 0) {
        ActivationRequest request{
            .action = ActivationAction::OpenAsset,
            .value = uri.path().mid(1),
        };
        return request.isValid() ? std::optional<ActivationRequest>(request)
                                 : std::nullopt;
    }
    if (uri.host().compare(QStringLiteral("search"), Qt::CaseInsensitive) == 0) {
        ActivationRequest request{
            .action = ActivationAction::Search,
            .value = QUrlQuery(uri).queryItemValue(QStringLiteral("q")),
        };
        return request.isValid() ? std::optional<ActivationRequest>(request)
                                 : std::nullopt;
    }
    if (uri.host().compare(QStringLiteral("show"), Qt::CaseInsensitive) == 0) {
        return ActivationRequest{};
    }
    return std::nullopt;
}

} // namespace xips
