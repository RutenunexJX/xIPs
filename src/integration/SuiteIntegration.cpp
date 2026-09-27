#include "SuiteIntegration.h"
#include "IntegrationService.h"
#include "app/BrowserPanel.h"
#include "xips/BrowserApi.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <suiteapp/protocol.h>

namespace xips {
namespace {
QJsonObject surface() {
  return {
      {"id", "xips.library"},
      {"mode", "native"},
      {"fallback", "external"},
      {"native",
       QJsonObject{{"library", "xips-browser"},
                   {"path", QDir(QCoreApplication::applicationDirPath())
                                .filePath("xips-browser.dll")},
                   {"abi", 1},
                   {"abiTag", QString::fromLatin1(xipsExpectedBrowserAbi())},
                   {"elaBaseline", "454cac2d-p27"},
                   {"capabilityFactory", "xips_browser_capabilities_v1"},
                   {"factory", "xips_create_browser_v1"}}}};
}
} // namespace
SuiteIntegration::SuiteIntegration(BrowserPanel *panel, QWidget *window)
    : QObject(window), panel(panel), window(window) {}
bool SuiteIntegration::start(QString *error,
                             const SuiteApp::RuntimeStartOptions &options) {
  if (provider && provider->isListening())
    return true;
  const auto runtime = SuiteApp::ensureRuntime(options);
  if (!runtime.available) {
    if (error)
      *error = runtime.errorMessage;
    return false;
  }
  provider = std::make_unique<SuiteApp::Provider>(
      descriptor(QCoreApplication::applicationVersion()),
      [this](const QJsonObject &request) { return processRequest(request); },
      this);
  return provider->start(options.endpoint, error);
}
QJsonObject SuiteIntegration::descriptor(const QString &version) {
  return {
      {"appId", "xips"},
      {"displayName", "xIPs"},
      {"version", version.isEmpty() ? "0.0.0" : version},
      {"processId", static_cast<double>(QCoreApplication::applicationPid())},
      {"endpoint", SuiteApp::endpointForApp("xips")},
      {"protocols", QJsonArray{"suite-app/v1"}},
      {"resourceSchemes", QJsonArray{"xips"}},
      {"icon", QDir(QCoreApplication::applicationDirPath())
                   .filePath("assets/icons/xips-256.png")},
      {"identityAccent", "#F58B72"},
      {"actions",
       QJsonArray{QJsonObject{{"id", "xips.library.open"},
                              {"sideEffect", "ui"},
                              {"resourceSchemes", QJsonArray{"xips"}}},
                  QJsonObject{{"id", "xips.asset.open"},
                              {"sideEffect", "ui"},
                              {"resourceSchemes", QJsonArray{"xips"}}}}},
      {"surfaces", QJsonArray{surface()}},
      {"launch",
       QJsonObject{{"executable", QCoreApplication::applicationFilePath()},
                   {"arguments", QJsonArray{}}}}};
}
QJsonObject SuiteIntegration::processRequest(const QJsonObject &request) {
  const auto fail = [&](const QString &code, const QString &message) {
    return SuiteApp::errorResponse(request, code, message);
  };
  if (!panel || !window)
    return fail("provider_unavailable", "The xIPs window is unavailable.");
  const auto method = request.value("method").toString();
  const auto params = request.value("params").toObject();
  const auto uri = QUrl(params.value("uri").toString(
      params.value("resourceUri").toString("xips://show")));
  const auto activation = IntegrationService::parseUri(uri);
  if (!activation || !uri.userInfo().isEmpty() || uri.port() != -1 ||
      uri.hasFragment())
    return fail("invalid_resource",
                "Use an xips://show or xips://asset/<id> resource.");
  if (activation->action == ActivationAction::Search)
    return fail("invalid_resource",
                "Suite resources must identify a library or asset.");
  if (method == "action.invoke") {
    const auto action = params.value("actionId").toString();
    if (action != "xips.library.open" && action != "xips.asset.open")
      return fail("action_not_supported", "Unknown xIPs action.");
    if (action == "xips.asset.open" &&
        activation->action != ActivationAction::OpenAsset)
      return fail("invalid_resource", "The action requires an asset URI.");
  } else if (method == "surface.describe" || method == "surface.open") {
    if (params.value("surfaceId").toString() != "xips.library")
      return fail("surface_not_supported", "Unknown xIPs surface.");
  } else if (method != "resource.resolve") {
    return fail("method_not_supported", "Unknown xIPs provider method.");
  }
  QJsonObject resource{
      {"uri", uri.toString()}, {"title", "xIPs Library"}, {"kind", "library"}};
  const auto revision = QUrlQuery(uri).queryItemValue("revision");
  if (activation->action == ActivationAction::OpenAsset) {
    if (panel->isCatalogBusy())
      return fail("provider_busy", "The catalog is loading. Retry shortly.");
    const auto asset = panel->catalogAsset(activation->value);
    if (!asset)
      return fail("resource_not_found",
                  "The asset is not in the current library.");
    resource.insert("kind", "asset");
    resource.insert("assetId", asset->id);
    resource.insert("title", asset->name);
    resource.insert("category", asset->category);
    resource.insert("description", asset->description);
    resource.insert("legacy", asset->legacy);
    resource.insert("discovered", asset->discovered);
    resource.insert("revisionCount", asset->snapshots.size());
    if (!asset->snapshots.isEmpty()) {
      auto selected = asset->snapshots.last();
      if (!revision.isEmpty()) {
        bool found = false;
        for (const auto &candidate : asset->snapshots)
          if (candidate.id == revision) {
            selected = candidate;
            found = true;
            break;
          }
        if (!found)
          return fail("revision_not_found",
                      "The selected revision is unavailable.");
      }
      resource.insert("revision", selected.id);
      resource.insert("contentHash", selected.hash);
      resource.insert("fileCount", selected.files.size());
      resource.insert("contentVerified", false);
    } else if (!revision.isEmpty()) {
      return fail(
          "legacy_revision_unavailable",
          "Convert the legacy asset before resolving a numbered revision.");
    }
  }
  if (method == "resource.resolve")
    return SuiteApp::successResponse(request, resource);
  if (method == "surface.describe") {
    auto description = surface();
    description.insert("surfaceId", "xips.library");
    description.insert("resource", resource);
    return SuiteApp::successResponse(request, description);
  }
  if (activation->action == ActivationAction::OpenAsset) {
    panel->restoreState(
        {{"assetId", activation->value}, {"revision", revision}});
  }
  window->showNormal();
  window->raise();
  window->activateWindow();
  return SuiteApp::successResponse(request,
                                   {{"opened", true}, {"resource", resource}});
}
} // namespace xips
