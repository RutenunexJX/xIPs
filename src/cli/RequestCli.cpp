#include "RequestCli.h"
#include "ArchiveCli.h"
#include "integration/SessionClient.h"
#include "library/CatalogOperations.h"
#include "library/OperationControl.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <QThread>
#include <atomic>
#include <thread>
namespace xips::cli
{
namespace
{
void output(const QJsonObject &value, FILE *stream = stdout)
{
    QTextStream text(stream);
    text << QJsonDocument(value).toJson(QJsonDocument::Compact) << '\n';
    text.flush();
}
QJsonObject read(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw OperationError("input_unavailable", "Cannot read JSON input: " + path, 2);
    if (file.size() > limit)
        throw OperationError("input_too_large", "JSON input exceeds the size limit.", 2);
    auto bytes = file.readAll();
    if (bytes.startsWith("\xef\xbb\xbf"))
        bytes.remove(0, 3);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw OperationError("invalid_json", "Expected a JSON object: " + error.errorString(), 2);
    return doc.object();
}
class Monitor
{
  public:
    OperationControl control;
    explicit Monitor(const QString &cancelFile)
    {
        if (!cancelFile.isEmpty() && QFileInfo::exists(cancelFile))
            control.cancel();
        worker = std::jthread([this, cancelFile](std::stop_token stop) {
            QString last;
            while (!stop.stop_requested())
            {
                if (!cancelFile.isEmpty() && QFileInfo::exists(cancelFile))
                    control.cancel();
                const auto status = control.status();
                if (!status.isEmpty() && status != last)
                {
                    output({{"event", "progress"}, {"detail", status}}, stderr);
                    last = status;
                }
                for (int n = 0; n < 5 && !stop.stop_requested(); ++n)
                    QThread::msleep(50);
            }
        });
    }

  private:
    std::jthread worker;
};
} // namespace
void addRequestOptions(QCommandLineParser &parser)
{
    parser.addOption({"request", "Read a UTF-8 operation request. See --action capabilities.", "json-file"});
    parser.addOption({"dry-run", "Preview a structured mutation without publishing."});
    parser.addOption({"apply", "Apply the exact preview from a saved --dry-run JSON response.", "json-file"});
    parser.addOption({"cancel-file", "Cancel before publication when this file exists.", "path"});
    parser.addOption({"session", "Exact GUI session ID; protect dialogs and verify adoption.", "id"});
    parser.addOption({"session-dir", "Optional isolated session registration directory.", "directory"});
    parser.addOption({"wait-ms", "Maximum wait for GUI adoption (0..60000).", "milliseconds", "10000"});
    parser.addOption({"focus-asset", "Explicitly select this asset after the GUI adopts the change.", "id"});
    parser.addOption({"focus-revision", "Explicit revision to select with --focus-asset.", "id"});
}
std::optional<int> runRequest(const QCommandLineParser &parser)
{
    const auto action = parser.value("action");
    if (!parser.isSet("request") && !parser.isSet("apply") && action != "capabilities" &&
        action != "sessions" && action != "session-status")
    {
        if (parser.isSet("dry-run") || parser.isSet("session") || parser.isSet("focus-asset") ||
            parser.isSet("cancel-file"))
        {
            output({{"schema", "xips.operation-result/v1"},
                    {"ok", false},
                    {"error", QJsonObject{{"code", "invalid_request"},
                                          {"message", "These options require --request or --apply; "
                                                      "list/resolve retain their existing syntax."}}}});
            return 2;
        }
        return std::nullopt;
    }
    struct Lease
    {
        QString id, token, library;
    };
    QList<Lease> reservations;
    QString lease, library;
    const auto session = parser.value("session"), sessionDirectory = parser.value("session-dir");
    QJsonObject committed;
    try
    {
        if (parser.isSet("request") || parser.isSet("apply"))
        {
            for (const auto &option : {"action", "library", "asset", "asset-version", "query", "destination"})
                if (parser.isSet(option))
                    throw OperationError("invalid_request",
                                         "Structured requests cannot be mixed with legacy --" +
                                             QString(option) + " options.",
                                         2);
        }
        if (parser.isSet("request") && parser.isSet("apply"))
            throw OperationError("invalid_request", "Choose --request or --apply.", 2);
        if (parser.isSet("apply") && parser.isSet("dry-run"))
            throw OperationError("invalid_request", "--apply cannot be combined with --dry-run.", 2);
        bool validWait = false;
        const auto waitMs = parser.value("wait-ms").toInt(&validWait);
        if (!validWait || waitMs < 0 || waitMs > 60000)
            throw OperationError("invalid_request", "--wait-ms must be from 0 through 60000.", 2);
        if (parser.isSet("focus-revision") && !parser.isSet("focus-asset"))
            throw OperationError("invalid_request", "--focus-revision requires --focus-asset.", 2);
        if (parser.isSet("focus-asset") && session.isEmpty())
            throw OperationError("invalid_request", "Explicit focus requires --session.", 2);
        QJsonObject result;
        if (action == "capabilities" && !parser.isSet("request") && !parser.isSet("apply"))
        {
            result = CatalogOperations::capabilities();
            result.insert("sessionTransport", SessionClient::available());
            result.insert("sessionActions", QJsonArray{"sessions", "session-status"});
            result.insert("archive", archiveCapabilities());
        }
        else if (action == "sessions" && !parser.isSet("request") && !parser.isSet("apply"))
        {
            if (!SessionClient::available())
                throw OperationError("sessions_unavailable", "This build has no SuiteApp transport.", 4);
            result.insert("sessions", SessionClient::list(sessionDirectory));
        }
        else if (action == "session-status" && !parser.isSet("request") && !parser.isSet("apply"))
            result = SessionClient::request(session, "xips.session.status", {}, sessionDirectory);
        else
        {
            Monitor monitor(parser.value("cancel-file"));
            OperationScope scope(&monitor.control);
            OperationScope::checkpoint();
            if (parser.isSet("request"))
            {
                const auto path = QFileInfo(parser.value("request")).absoluteFilePath();
                const auto input = read(path, 1024 * 1024);
                const bool archive = isArchiveRequest(input);
                const auto request =
                    archive ? normalizeArchive(input, QFileInfo(path).absolutePath())
                            : CatalogOperations::normalize(input, QFileInfo(path).absolutePath());
                if (CatalogOperations::isMutation(request.value("action").toString()) ||
                    request.value("action") == "archive-run")
                {
                    if (!parser.isSet("dry-run"))
                        throw OperationError("preview_required",
                                             "Write actions require --dry-run, followed by "
                                             "--apply of that response.",
                                             2);
                    result.insert("plan", archive ? previewArchiveRequest(request)
                                                  : CatalogOperations::preview(request));
                    result.insert("disk", "unchanged");
                }
                else
                {
                    if (parser.isSet("dry-run"))
                        throw OperationError("invalid_request", "Read actions do not need --dry-run.", 2);
                    result = archive ? readArchiveRequest(request, &monitor.control)
                                     : CatalogOperations::read(request);
                }
            }
            else
            {
                const auto document = read(parser.value("apply"), 64 * 1024 * 1024);
                const auto plan = (document.value("schema") == "xips.operation-plan/v1" ||
                                   document.value("schema") == "xips.archive-plan/v1")
                                      ? document
                                      : document.value("data").toObject().value("plan").toObject();
                if (!session.isEmpty() && !SessionClient::available())
                    throw OperationError("sessions_unavailable",
                                         "This build has no SuiteApp session transport.", 4);
                const bool archive = plan.value("schema") == "xips.archive-plan/v1";
                if (archive && !session.isEmpty())
                    throw OperationError("invalid_request",
                                         "Archive jobs are independent of catalog "
                                         "sessions; omit --session.",
                                         2);
                library = plan.value("request").toObject().value("library").toString();
                if (!archive && SessionClient::available())
                {
                    const auto guards = plan.value("guards").toObject();
                    auto live = SessionClient::list(sessionDirectory);
                    bool hasTarget = false;
                    for (const auto &entry : live)
                        hasTarget |= entry.toObject().value("id") == session;
                    if (!session.isEmpty() && !hasTarget)
                        live.append(
                            SessionClient::request(session, "xips.session.status", {}, sessionDirectory));
                    for (const auto &entry : live)
                    {
                        const auto status = entry.toObject();
                        const auto root = status.value("library").toString();
                        const auto id = status.value("id").toString();
                        if (status.value("reachable") == false &&
                            (root.isEmpty() || guards.contains(root) || id == session))
                            throw OperationError("session_unavailable",
                                                 "A matching GUI process is alive but cannot confirm its "
                                                 "state. No disk change was made.",
                                                 3, status);

                        if (!guards.contains(root) && id != session)
                            continue;
                        if (session.isEmpty())
                            throw OperationError("session_required",
                                                 "This library is open in a GUI. Choose its "
                                                 "exact --session ID before applying.");
                        if (id == session && root.compare(library, Qt::CaseInsensitive) != 0)
                            throw OperationError("library_mismatch",
                                                 "The selected session is displaying a different library.");
                        const auto reservation =
                            SessionClient::request(id, "xips.session.prepare",
                                                   {{"library", root},
                                                    {"viewToken", guards.value(root)},
                                                    {"pid", QCoreApplication::applicationPid()}},
                                                   sessionDirectory);
                        reservations.append({id, reservation.value("lease").toString(), root});
                        if (id == session)
                            lease = reservation.value("lease").toString();
                    }
                }
                committed =
                    archive ? applyArchivePlan(plan, &monitor.control) : CatalogOperations::apply(plan);
                result = committed;
                result.insert("gui", QJsonObject{{"state", "not_requested"}});
                if (!lease.isEmpty())
                {
                    QJsonObject focus;
                    if (parser.isSet("focus-asset"))
                    {
                        focus.insert("asset", parser.value("focus-asset"));
                        if (parser.isSet("focus-revision"))
                            focus.insert("revision", parser.value("focus-revision"));
                    }
                    QJsonObject status;
                    QJsonArray observers;
                    for (const auto &reservation : reservations)
                    {
                        const auto adopted = SessionClient::request(
                            reservation.id, "xips.session.finish",
                            {{"lease", reservation.token},
                             {"viewToken", result.value("viewTokens").toObject().value(reservation.library)},
                             {"focus", reservation.id == session ? focus : QJsonObject{}}},
                            sessionDirectory);
                        if (reservation.id == session)
                            status = adopted;
                        else
                            observers.append(adopted);
                    }
                    reservations.clear();
                    result.insert("otherSessions", observers);
                    lease.clear();
                    QElapsedTimer timer;
                    timer.start();
                    while (status.value("adoption").toObject().value("state") == "pending" &&
                           timer.elapsed() < waitMs)
                    {
                        QThread::msleep(100);
                        status = SessionClient::request(session, "xips.session.status", {}, sessionDirectory);
                    }
                    const auto adoption = status.value("adoption").toObject().value("state").toString();
                    result.insert("gui", status);
                    if (adoption != "adopted" ||
                        (!focus.isEmpty() && status.value("focus").toObject().value("state") != "focused"))
                    {
                        output({{"schema", "xips.operation-result/v1"}, {"ok", true}, {"data", result}});
                        return 6;
                    }
                }
            }
        }
        output({{"schema", "xips.operation-result/v1"}, {"ok", true}, {"data", result}});
        return 0;
    }
    catch (const std::exception &error)
    {
        for (const auto &reservation : reservations)
        {
            try
            {
                SessionClient::request(reservation.id, "xips.session.abort", {{"lease", reservation.token}},
                                       sessionDirectory);
            }
            catch (const std::exception &)
            { /* A window also releases its lease
                 when the owner exits. */
            }
        }
        const auto *failure = dynamic_cast<const OperationError *>(&error);
        const bool cancelled = dynamic_cast<const OperationCancelled *>(&error) != nullptr;
        QJsonObject response{{"schema", "xips.operation-result/v1"},
                             {"ok", false},
                             {"error", QJsonObject{{"code", failure     ? failure->code
                                                            : cancelled ? "cancelled"
                                                                        : "operation_failed"},
                                                   {"message", QString::fromUtf8(error.what())},
                                                   {"details", failure ? failure->details : QJsonObject{}}}}};
        if (!committed.isEmpty())
        {
            committed.insert("gui", QJsonObject{{"state", "unknown"}, {"session", session}});
            response.insert("data", committed);
        }
        output(response);
        return failure ? failure->exitCode : cancelled ? 5 : 3;
    }
}
} // namespace xips::cli
