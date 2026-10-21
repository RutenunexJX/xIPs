#include "CatalogSession.h"
#include "SessionClient.h"
#include "app/BrowserPanel.h"
#include "library/ContentStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <suiteapp/transport.h>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#endif
namespace xips
{
namespace
{
bool processAlive(qint64 pid)
{
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}
} // namespace
CatalogSession::CatalogSession(BrowserPanel *p) : QObject(p), panel(p)
{
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto root =
        SessionClient::directory(QCoreApplication::instance()->property("xips.sessionDirectory").toString());
    const auto endpoint = "xips.session." + id;
    server = new SuiteApp::JsonServer(
        endpoint, [this](const QJsonObject &request) { return handle(request); }, this);
    QString reason;
    if (!server->start(&reason))
        return;
    recordPath = root + '/' + id + ".json";
    processStart = SessionClient::processStamp(QCoreApplication::applicationPid());
    registeredLibrary = panel->libraryRoot();
    try
    {
        ContentStore::publishJson(recordPath, {{"schema", "xips.session/v1"},
                                               {"id", id},
                                               {"endpoint", endpoint},
                                               {"pid", QCoreApplication::applicationPid()},
                                               {"processStart", processStart},
                                               {"library", registeredLibrary}});
    }
    catch (const std::exception &)
    {
        server->stop();
        recordPath.clear();
        return;
    }
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        syncRegistration();
        if (!lease.isEmpty() && !processAlive(ownerPid))
        {
            panel->finishExternalWrite(lease, {});
            lease.clear();
            outcome = "owner_disconnected";
        }
    });
    timer->start();
}
CatalogSession::~CatalogSession()
{
    if (!recordPath.isEmpty())
        QFile::remove(recordPath);
}
void CatalogSession::syncRegistration()
{
    const auto library = panel->libraryRoot();
    if (recordPath.isEmpty() || library == registeredLibrary)
        return;
    try
    {
        ContentStore::safePath(recordPath);
        QSaveFile file(recordPath);
        const auto data = QJsonDocument(QJsonObject{{"schema", "xips.session/v1"},
                                                    {"id", id},
                                                    {"endpoint", "xips.session." + id},
                                                    {"pid", QCoreApplication::applicationPid()},
                                                    {"processStart", processStart},
                                                    {"library", library}})
                              .toJson(QJsonDocument::Compact);
        if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit())
            registeredLibrary = library;
    }
    catch (const std::exception &)
    { /* Registration failure must not interrupt the GUI. */
    }
}
QJsonObject CatalogSession::state()
{
    syncRegistration();
    auto status = panel->externalState();
    status.insert("reachable", true);
    status.insert("id", id);
    status.insert("pid", QCoreApplication::applicationPid());
    if (outcome == "pending" && !status.value("busy").toBool())
        outcome = status.value("viewToken").toString() == expected ? "adopted" : "conflict";
    if (!focus.isEmpty())
    {
        const auto selection = status.value("selection").toObject();
        const bool selected =
            selection.value("asset") == focus.value("asset") &&
            (!focus.contains("revision") || selection.value("revision") == focus.value("revision"));
        status.insert("focus", QJsonObject{{"requested", focus},
                                           {"state", status.value("busy").toBool() ? "pending"
                                                     : selected                    ? "focused"
                                                                                   : "unavailable"}});
    }
    status.insert("adoption", QJsonObject{{"state", outcome}, {"expectedViewToken", expected}});
    return status;
}
QJsonObject CatalogSession::handle(const QJsonObject &request)
{
    QString reason;
    if (!SuiteApp::validateRequest(request, &reason))
        return SuiteApp::errorResponse(request, "invalid_request", reason);
    const auto params = request.value("params").toObject();
    if (params.value("session") != id)
        return SuiteApp::errorResponse(request, "session_mismatch",
                                       "The requested window does not match this session.");
    const auto method = request.value("method").toString();
    if (method == "xips.session.status")
        return SuiteApp::successResponse(request, state());
    if (method == "xips.session.prepare")
    {
        if (!lease.isEmpty())
            return SuiteApp::errorResponse(request, "session_busy",
                                           "This window already has an active external operation.");
        if (params.value("library").toString().compare(state().value("library").toString(),
                                                       Qt::CaseInsensitive) != 0)
            return SuiteApp::errorResponse(request, "library_mismatch",
                                           "Session is displaying a different library.");
        const auto pid = params.value("pid").toInteger();
        if (pid <= 0 || !processAlive(pid))
            return SuiteApp::errorResponse(request, "invalid_owner",
                                           "A live requesting process is required.");
        const auto next = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto failure = panel->beginExternalWrite(next, params.value("viewToken").toString());
        if (!failure.isEmpty())
            return SuiteApp::errorResponse(request, "session_conflict", failure, state());
        ownerPid = pid;
        lease = next;
        expected.clear();
        focus = {};
        outcome = "reserved";
        auto result = state();
        result.insert("lease", lease);
        return SuiteApp::successResponse(request, result);
    }
    if (method == "xips.session.finish" || method == "xips.session.abort")
    {
        if (lease.isEmpty() || params.value("lease") != lease)
            return SuiteApp::errorResponse(request, "lease_mismatch",
                                           "The external operation lease does not match.");
        expected = params.value("viewToken").toString();
        focus = params.value("focus").toObject();
        panel->finishExternalWrite(lease, focus);
        lease.clear();
        outcome = method.endsWith("abort") ? "aborted" : "pending";
        return SuiteApp::successResponse(request, state());
    }
    return SuiteApp::errorResponse(request, "unsupported_method", "Unknown session method.");
}
} // namespace xips
