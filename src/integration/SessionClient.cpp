#include "SessionClient.h"
#include "library/CatalogOperations.h"
#include "library/ContentStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#ifdef XIPS_HAS_SESSION_TRANSPORT
#include <suiteapp/transport.h>
#endif
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
QString SessionClient::processStamp(qint64 pid)
{
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return {};
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
                       GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    return alive ? QString::number((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime)
                 : QString();
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM ? QString::number(pid) : QString();
#endif
}
bool SessionClient::available()
{
#ifdef XIPS_HAS_SESSION_TRANSPORT
    return true;
#else
    return false;
#endif
}
QString SessionClient::directory(const QString &overrideDirectory)
{
    return overrideDirectory.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/xIPs/sessions"
               : QDir(overrideDirectory).absolutePath();
}
QJsonObject SessionClient::request(const QString &id, const QString &method, const QJsonObject &params,
                                   const QString &root, int timeoutMs)
{
#ifdef XIPS_HAS_SESSION_TRANSPORT
    if (!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
             .match(id)
             .hasMatch())
        throw OperationError("invalid_session", "Use an exact session ID from --action sessions.", 2);
    const auto path = directory(root) + '/' + id + ".json";
    ContentStore::safePath(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        throw OperationError("session_unavailable", "Session registration is unavailable.", 4);
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    const auto endpoint = "xips.session." + id;
    if (record.value("id") != id || record.value("endpoint") != endpoint)
        throw OperationError("invalid_session", "Session identity is invalid.");
    auto arguments = params;
    arguments.insert("session", id);
    const auto sent = SuiteApp::makeRequest(method, arguments);
    const auto response = SuiteApp::sendRequest(endpoint, sent, timeoutMs);
    if (!response.hasResponse())
        throw OperationError("session_unavailable", response.errorMessage, 4);
    QString reason;
    if (!SuiteApp::validateResponse(response.response, sent.value("requestId").toString(), &reason))
        throw OperationError("invalid_session_response", reason);
    if (!response.response.value("ok").toBool())
    {
        const auto error = response.response.value("error").toObject();
        throw OperationError(error.value("code").toString(), error.value("message").toString(), 3,
                             error.value("details").toObject());
    }
    return response.response.value("result").toObject();
#else
    Q_UNUSED(id);
    Q_UNUSED(method);
    Q_UNUSED(params);
    Q_UNUSED(root);
    Q_UNUSED(timeoutMs);
    throw OperationError("sessions_unavailable",
                         "This build has no SuiteApp public transport. Offline "
                         "catalog operations remain available.",
                         4);
#endif
}
QJsonArray SessionClient::list(const QString &root)
{
    QJsonArray sessions;
    for (const auto &file : QDir(directory(root)).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
    {
        try
        {
            sessions.append(request(file.completeBaseName(), "xips.session.status", {}, root, 300));
        }
        catch (const OperationError &error)
        {
            QFile registration(file.absoluteFilePath());
            if (!registration.open(QIODevice::ReadOnly) || registration.size() > 65536)
                continue;
            const auto record = QJsonDocument::fromJson(registration.readAll()).object();
            const auto stamp = processStamp(record.value("pid").toInteger());
            if (stamp.isEmpty() ||
                (record.contains("processStart") && record.value("processStart").toString() != stamp))
                continue;
            sessions.append(QJsonObject{{"id", file.completeBaseName()},
                                        {"pid", record.value("pid")},
                                        {"library", record.value("library")},
                                        {"reachable", false},
                                        {"error", QString::fromUtf8(error.what())}});
        }
    }
    return sessions;
}
} // namespace xips
