#include "ArchiveCli.h"
#include "archive/ArchiveEngine.h"
#include "archive/ArchiveScan.h"
#include "library/CatalogOperations.h"
#include "library/OperationControl.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
namespace xips::cli
{
namespace
{
QString hash(const QJsonObject &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
archive::Request engineRequest(const QJsonObject &r)
{
    archive::Request result;
    result.xpr = r.value("project").toString();
    result.outputRoot = r.value("output").toString();
    result.archiveName = r.value("name").toString();
    const auto launcher = r.value("vivado").toString();
    result.vivado = {archive::installationVersion(launcher), launcher};
    for (const auto &root : r.value("sharedRoots").toArray())
        result.sharedRoots.append(root.toString());
    return result;
}
void event(const QString &type, const QJsonObject &details)
{
    auto result = details;
    result.insert("event", type);
    QTextStream stream(stderr);
    stream << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    stream.flush();
}
} // namespace
bool isArchiveRequest(const QJsonObject &r)
{
    return r.value("action").toString().startsWith("archive-");
}
QJsonObject archiveCapabilities()
{
    return {
        {"actions", QJsonArray{"archive-inspect", "archive-scan", "archive-installations", "archive-run"}},
        {"archive-run", QJsonArray{"project", "output", "name", "vivado", "sharedRoots"}},
        {"archive-inspect", QJsonArray{"project"}},
        {"archive-scan", QJsonArray{"root"}},
        {"archive-installations", QJsonArray{"roots"}},
        {"policy", "archive-run requires dry-run/apply, an explicit archive name and the "
                   "exact Vivado launcher; bundled 7-Zip; source remains untouched"}};
}
QJsonObject normalizeArchive(QJsonObject r, const QString &base)
{
    const auto action = r.value("action").toString();
    QMap<QString, QStringList> actions{
        {"archive-inspect", {"project"}},
        {"archive-scan", {"root"}},
        {"archive-installations", {"roots"}},
        {"archive-run", {"project", "output", "name", "vivado", "sharedRoots"}}};
    if (!actions.contains(action))
        throw OperationError("invalid_request", "Unknown archive action.", 2);
    for (auto it = r.begin(); it != r.end(); ++it)
        if (it.key() != "action" && !actions.value(action).contains(it.key()))
            throw OperationError("invalid_request", "Unknown archive field: " + it.key(), 2);
    for (const auto &key : actions.value(action))
    {
        const auto value = r.value(key);
        if (key == "roots" || key == "sharedRoots")
        {
            if (value.isUndefined())
                continue;
            if (!value.isArray())
                throw OperationError("invalid_request", key + " must be an array.", 2);
            QJsonArray roots;
            for (const auto &entry : value.toArray())
            {
                if (!entry.isString() || entry.toString().trimmed().isEmpty())
                    throw OperationError("invalid_request", "Invalid root.", 2);
                roots.append(QDir::cleanPath(QDir(base).absoluteFilePath(entry.toString())));
            }
            r.insert(key, roots);
        }
        else
        {
            if (!value.isString() || value.toString().trimmed().isEmpty())
                throw OperationError("invalid_request", key + " is required.", 2);
            if (key != "name")
                r.insert(key, QDir::cleanPath(QDir(base).absoluteFilePath(value.toString())));
        }
    }
    return r;
}
QJsonObject previewArchiveRequest(const QJsonObject &request)
{
    QJsonObject plan{
        {"schema", "xips.archive-plan/v1"},
        {"request", request},
        {"preview", archive::previewArchive(engineRequest(request), [] { OperationScope::checkpoint(); })}};
    plan.insert("token", hash(plan));
    return plan;
}
QJsonObject readArchiveRequest(const QJsonObject &r, OperationControl *control)
{
    const auto action = r.value("action").toString();
    if (action == "archive-inspect")
    {
        const auto p = archive::inspectProject(r.value("project").toString());
        return {{"project", p.xpr}, {"root", p.root}, {"name", p.name}, {"version", p.version}};
    }
    if (action == "archive-installations")
    {
        QStringList roots;
        for (const auto &entry : r.value("roots").toArray())
            roots.append(entry.toString());
        QJsonArray found;
        for (const auto &v : archive::discoverInstallations(roots))
            found.append(QJsonObject{{"version", v.version}, {"launcher", v.launcher}});
        return {{"installations", found}};
    }
    archive::ArchiveScan job(r.value("root").toString());
    QEventLoop loop;
    QTimer timer;
    QObject::connect(&job, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (control->state.load() == OperationControl::CancelRequested)
            job.requestInterruption();
    });
    QObject::connect(&job, &archive::ArchiveScan::progress, &loop, [](int found, const QString &directory) {
        event("progress", {{"found", found}, {"directory", directory}});
    });
    timer.start(100);
    job.start();
    loop.exec();
    job.wait();
    if (job.cancelled)
        throw OperationError("cancelled", "Project scan cancelled.", 5);
    if (!job.error.isEmpty())
        throw OperationError("scan_failed", job.error);
    QJsonArray projects;
    for (const auto &p : job.projects)
        projects.append(QJsonObject{{"project", p.xpr}, {"version", p.version}, {"error", p.error}});
    return {{"projects", projects}, {"skippedDirectories", job.skippedDirectories}};
}
QJsonObject applyArchivePlan(const QJsonObject &plan, OperationControl *control)
{
    auto unsignedPlan = plan;
    const auto token = unsignedPlan.take("token").toString();
    if (plan.value("schema") != "xips.archive-plan/v1" || token.isEmpty() || hash(unsignedPlan) != token)
        throw OperationError("invalid_preview", "Invalid or altered archive preview.", 2);
    const auto request = normalizeArchive(plan.value("request").toObject(), QDir::currentPath());
    if (request != plan.value("request").toObject() || previewArchiveRequest(request) != plan)
        throw OperationError("preview_conflict", "Archive inputs changed after preview.");
    auto input = engineRequest(request);
    input.expectedInputHash = plan.value("preview").toObject().value("inputHash").toString();
    archive::ArchiveJob job(input);
    QEventLoop loop;
    QTimer timer;
    bool success = false, cancelled = false;
    QString directory, diagnostics, message;
    QObject::connect(
        &job, &archive::ArchiveJob::completed, &loop,
        [&](bool ok, bool stopped, const QString &path, const QString &retained, const QString &detail) {
            success = ok;
            cancelled = stopped;
            directory = path;
            diagnostics = retained;
            message = detail;
        });
    QObject::connect(&job, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&job, &archive::ArchiveJob::progress, &loop,
                     [](int stage, int percent, const QString &detail) {
                         event("progress", {{"stage", stage}, {"percent", percent}, {"detail", detail}});
                     });
    QObject::connect(&job, &archive::ArchiveJob::logMessage, &loop,
                     [](const QString &line) { event("log", {{"detail", line}}); });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (control->state.load() == OperationControl::CancelRequested)
            job.requestInterruption();
    });
    timer.start(100);
    job.start();
    loop.exec();
    job.wait();
    if (!success)
        throw OperationError(cancelled ? "cancelled" : "archive_failed", message, cancelled ? 5 : 3,
                             {{"directory", directory},
                              {"diagnostics", diagnostics},
                              {"disk", "not_published_or_retained_diagnostics"}});
    return {{"disk", "committed"},        {"directory", directory}, {"archive", directory + ".7z"},
            {"diagnostics", diagnostics}, {"message", message},     {"sourceModified", false}};
}
} // namespace xips::cli
