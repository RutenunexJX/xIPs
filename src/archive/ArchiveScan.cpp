#include "ArchiveScan.h"
#include "VivadoWorkspace.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
#include <stdexcept>

namespace xips::archive {
ArchiveScan::ArchiveScan(QString root, QObject* parent) : QThread(parent), root_(std::move(root)) {}
ArchiveScan::~ArchiveScan() { requestInterruption(); wait(); }
void ArchiveScan::run()
{
    try {
        requirePlainPath(root_);
        if (!QFileInfo(root_).isDir() || !QFileInfo(root_).isReadable()) throw std::runtime_error("Choose a readable project folder.");
        QStringList pending{QFileInfo(root_).absoluteFilePath()};
        emit progress(0, pending.first());
        QElapsedTimer timer; timer.start();
        while (!pending.isEmpty()) {
            if (isInterruptionRequested()) { cancelled = true; return; }
            const auto directory = pending.takeLast();
            const auto entries = QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
            for (const auto& entry : entries) {
                if (isInterruptionRequested()) { cancelled = true; return; }
                const auto path = QDir::fromNativeSeparators(entry.absoluteFilePath());
                if (entry.isDir()) {
                    try {
                        requirePlainPath(path);
                        if (entry.isReadable()) pending.append(path); else ++skippedDirectories;
                    } catch (const std::exception&) { ++skippedDirectories; }
                } else if (entry.suffix().compare("xpr", Qt::CaseInsensitive) == 0) {
                    DiscoveredProject candidate{path, {}, {}};
                    try { candidate.version = inspectProject(path).version; }
                    catch (const std::exception& ex) { candidate.error = QString::fromUtf8(ex.what()); }
                    projects.append(candidate);
                }
                if (timer.elapsed() >= 100) { emit progress(projects.size(), directory); timer.restart(); }
            }
        }
        std::sort(projects.begin(), projects.end(), [](const auto& a, const auto& b) { return a.xpr.compare(b.xpr, Qt::CaseInsensitive) < 0; });
    } catch (const std::exception& ex) { error = QString::fromUtf8(ex.what()); }
}
}
