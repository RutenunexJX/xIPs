#include "integration/IntegrationService.h"

#include <QUrlQuery>

namespace xips {
namespace {

QString actionName(const ActivationAction action)
{
    switch (action) {
    case ActivationAction::ShowLibrary:
        return QStringLiteral("show");
    case ActivationAction::OpenAsset:
        return QStringLiteral("asset");
    case ActivationAction::Search:
        return QStringLiteral("search");
    }
    return QStringLiteral("show");
}

std::optional<ActivationAction> parseAction(const QString &value)
{
    if (value == QStringLiteral("show")) {
        return ActivationAction::ShowLibrary;
    }
    if (value == QStringLiteral("asset")) {
        return ActivationAction::OpenAsset;
    }
    if (value == QStringLiteral("search")) {
        return ActivationAction::Search;
    }
    return std::nullopt;
}

} // namespace

bool ActivationRequest::isValid() const
{
    return action == ActivationAction::ShowLibrary || !value.trimmed().isEmpty();
}

QJsonObject ActivationRequest::toJson() const
{
    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("action"), actionName(action)},
        {QStringLiteral("value"), value},
    };
}

std::optional<ActivationRequest> ActivationRequest::fromJson(
    const QJsonObject &object)
{
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return std::nullopt;
    }
    const auto action = parseAction(object.value(QStringLiteral("action")).toString());
    if (!action) {
        return std::nullopt;
    }
    ActivationRequest request{
        .action = *action,
        .value = object.value(QStringLiteral("value")).toString(),
    };
    return request.isValid() ? std::optional<ActivationRequest>(request)
                             : std::nullopt;
}

QUrl IntegrationService::assetUri(const QString &assetId)
{
    QUrl uri;
    uri.setScheme(QStringLiteral("xips"));
    uri.setHost(QStringLiteral("asset"));
    uri.setPath(u'/' + assetId.trimmed());
    return uri;
}

QUrl IntegrationService::searchUri(const QString &query)
{
    QUrl uri;
    uri.setScheme(QStringLiteral("xips"));
    uri.setHost(QStringLiteral("search"));
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("q"), query);
    uri.setQuery(parameters);
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
