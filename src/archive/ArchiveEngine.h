#pragma once
#include <QList>
#include <QStringList>
#include <QThread>

namespace xips::archive {
struct Installation { QString version; QString launcher; };
struct ProjectInfo { QString xpr; QString root; QString name; QString version; };
struct Request {
    QString xpr;
    QString outputRoot;
    Installation vivado;
    QString compressor; // Optional trusted helper override for embedding; empty uses the bundled runtime.
    QString archiveName; // Optional folder and 7z basename; blank keeps automatic naming.
};
// Surrounding whitespace is ignored. Empty means automatic naming; otherwise one Windows-safe component.
QString archiveNameError(const QString& name);
// Read-only discovery. Invalid or ambiguous inputs throw std::runtime_error.
ProjectInfo inspectProject(const QString& xpr);
QList<Installation> discoverInstallations(const QStringList& roots = {});
QString installationVersion(const QString& launcher);

// All filesystem and process work stays on this thread. Cancellation is cooperative.
class ArchiveJob final : public QThread {
    Q_OBJECT
public:
    explicit ArchiveJob(Request request, QObject* parent = nullptr);
    ~ArchiveJob() override;
signals:
    void progress(int stage, int percent, const QString& detail);
    void logMessage(const QString& message);
    void completed(bool success, bool cancelled, const QString& directory,
                   const QString& diagnostics, const QString& message);
protected:
    void run() override;
private:
    Request request_;
};
}
