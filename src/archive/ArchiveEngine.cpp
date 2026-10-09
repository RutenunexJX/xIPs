#include "ArchiveEngine.h"
#include "ArchiveRuntime.h"
#include "CoreContainer.h"
#include "VivadoWorkspace.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <functional>
#include <algorithm>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xips::archive {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
QString clean(QString path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)); }
bool within(const QString& path, const QString& root)
{
    const auto p = clean(path), r = clean(root);
    return p.compare(r, Qt::CaseInsensitive) == 0 || p.startsWith(r + '/', Qt::CaseInsensitive);
}
void noLinks(QString path)
{
    path = QFileInfo(path).absoluteFilePath();
    while (!path.isEmpty()) {
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) fail("Links and junctions are not supported: " + path);
#ifdef Q_OS_WIN
        const auto attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            fail("Reparse points are not supported: " + path);
#endif
        const auto parent = info.dir().absolutePath();
        if (parent == path) break;
        path = parent;
    }
}
QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) fail("Cannot read " + path + ": " + f.errorString());
    const auto bytes = f.readAll();
    if (f.error() != QFile::NoError) fail("Cannot finish reading " + path);
    return bytes;
}
QDomDocument xml(const QString& file)
{
    QDomDocument doc;
    const auto result = doc.setContent(readFile(file));
    if (!result) fail("Invalid XPR XML: " + result.errorMessage);
    if (!doc.doctype().name().isEmpty()) fail("XPR documents with a DTD are not supported.");
    const auto root = doc.documentElement();
    // Older Vivado XPRs omit Product. The header still supplies the exact release.
    if (root.tagName() != "Project" || (root.hasAttribute("Product") && root.attribute("Product") != "Vivado"))
        fail("This file is not a Vivado project.");
    return doc;
}
QString release(const QString& text)
{
    const auto m = QRegularExpression("(?:Vivado\\s+v?)([0-9]{4}\\.[0-9]+(?:\\.[0-9]+)?)",
                                     QRegularExpression::CaseInsensitiveOption).match(text);
    return m.hasMatch() ? m.captured(1) : QString{};
}
void writeFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        fail("Cannot write " + path + ": " + f.errorString());
}
struct Cancelled {};
using Check = std::function<void()>;
struct Entry { QString relative; qint64 size; bool directory; };
QList<Entry> inventory(const QString& root, const Check& check)
{
    QList<Entry> entries;
    std::function<void(const QString&)> visit = [&](const QString& relative) {
        check();
        const QString current = root + (relative.isEmpty() ? "" : "/" + relative);
        noLinks(current);
        if (!QFileInfo(current).isReadable()) fail("Cannot list directory: " + current);
        const auto children = QDir(current).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto& child : children) {
            check();
            noLinks(child.absoluteFilePath());
            const QString rel = relative.isEmpty() ? child.fileName() : relative + '/' + child.fileName();
            if (!child.isFile() && !child.isDir()) fail("Unsupported filesystem entry: " + child.absoluteFilePath());
            entries.append({rel, child.isDir() ? 0 : child.size(), child.isDir()});
            if (child.isDir()) visit(rel);
        }
    };
    visit({});
    return entries;
}
void copyFile(const QString& source, const QString& target, const Check& check,
              const std::function<void(qint64)>& advance = {})
{
    noLinks(source);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) fail("Cannot create destination directory.");
    QFile input(source), output(target);
    if (!input.open(QIODevice::ReadOnly)) fail("Cannot read " + source + ": " + input.errorString());
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("Cannot create " + target + ": " + output.errorString());
    const qint64 size = input.size();
    qint64 done = 0;
    while (!input.atEnd()) {
        check();
        const auto block = input.read(1024 * 1024);
        if (block.isEmpty() && input.error() != QFile::NoError) fail("Read failed: " + source);
        if (output.write(block) != block.size()) fail("Write failed (check free space): " + target);
        done += block.size();
        if (advance) advance(block.size());
    }
    if (done != size || input.size() != size) fail("A source file changed while copying. Close Vivado and retry: " + source);
    if (!output.flush()) fail("Cannot flush " + target);
}
const QMap<QString, QString> suffixes{{"$PPRDIR", ""}, {"$PSRCDIR", ".srcs"}, {"$PGENDIR", ".gen"},
    {"$PRUNDIR", ".runs"}, {"$PCACHEDIR", ".cache"}, {"$PSIMDIR", ".sim"}, {"$PIPUSERFILESDIR", ".ip_user_files"}};
QString expandedPath(QString value, const ProjectInfo& project)
{
    value = QDir::fromNativeSeparators(value);
    for (auto it = suffixes.cbegin(); it != suffixes.cend(); ++it) {
        if (value == it.key() || value.startsWith(it.key() + '/')) {
            value.replace(0, it.key().size(), project.root + (it.value().isEmpty() ? "" : '/' + project.name + it.value()));
            break;
        }
    }
    if (value.contains('$') || value.contains(';') || value.contains('{') || value.contains('}'))
        fail("Unsupported variable or path list in XPR: " + value);
    return clean(QDir::isAbsolutePath(value) ? value : project.root + '/' + value);
}
QString resolve(const QString& value, const ProjectInfo& project)
{
    const auto result = expandedPath(value, project);
    if (!within(result, project.root)) fail("External reference cannot be isolated safely: " + value + ". Import it into the project before archiving.");
    noLinks(result);
    return result;
}
bool pathValue(const QString& key, const QString& value)
{
    return !value.isEmpty() && (value.startsWith('$') || value.startsWith('.') || QDir::isAbsolutePath(value)
        || key.contains("Dir", Qt::CaseInsensitive) || key.contains("Path", Qt::CaseInsensitive)
        || key == "File" || key.contains("Tcl", Qt::CaseInsensitive));
}
QList<Entry> projectArtifacts(const ProjectInfo& info, const QList<Entry>& files)
{
    const auto doc = xml(info.xpr);
    QMap<QString, QDomElement> sets, runs;
    const auto root = doc.documentElement();
    for (auto set = root.firstChildElement("FileSets").firstChildElement("FileSet"); !set.isNull(); set = set.nextSiblingElement("FileSet"))
        sets.insert(set.attribute("Name"), set);
    for (auto run = root.firstChildElement("Runs").firstChildElement("Run"); !run.isNull(); run = run.nextSiblingElement("Run"))
        runs.insert(run.attribute("Id"), run);
    QSet<QString> implementationDirectories;
    QString activeSources, activeImplementationSources;
    for (const auto& run : runs) {
        if (run.attribute("Type") == "Ft3:Synth" && run.attribute("State") == "current") activeSources = run.attribute("SrcSet");
        if (run.attribute("Type") != "Ft2:EntireDesign") continue;
        const auto sourceSet = run.attribute("SrcSet", runs.value(run.attribute("SynthRun")).attribute("SrcSet"));
        if (sets.value(sourceSet).attribute("Type") != "DesignSrcs") continue;
        if (run.attribute("State") == "current") activeImplementationSources = sourceSet;
        if (run.attribute("IncludeInArchive").compare("false", Qt::CaseInsensitive) == 0 || run.attribute("Dir").isEmpty()) continue;
        const auto directory = resolve(run.attribute("Dir"), info);
        const auto runsRoot = info.root + '/' + info.name + ".runs";
        if (QFileInfo(directory).absolutePath().compare(runsRoot, Qt::CaseInsensitive) == 0)
            implementationDirectories.insert(directory.toCaseFolded());
    }
    if (!activeImplementationSources.isEmpty()) activeSources = activeImplementationSources;
    if (activeSources.isEmpty()) {
        QStringList designSets;
        for (const auto& set : sets) if (set.attribute("Type") == "DesignSrcs") designSets.append(set.attribute("Name"));
        if (designSets.size() == 1) activeSources = designSets.first();
    }
    QSet<QString> handoffs;
    const auto active = sets.value(activeSources);
    if (active.attribute("Type") == "DesignSrcs") {
        for (auto file = active.firstChildElement("File"); !file.isNull(); file = file.nextSiblingElement("File")) {
            if (QFileInfo(file.attribute("Path")).suffix().compare("bd", Qt::CaseInsensitive) != 0) continue;
            bool disabled = false;
            QStringList usedIn;
            for (auto attr = file.firstChildElement("FileInfo").firstChildElement("Attr"); !attr.isNull(); attr = attr.nextSiblingElement("Attr")) {
                const auto name = attr.attribute("Name"), value = attr.attribute("Val");
                if ((name == "AutoDisabled" || name == "UserDisabled" || name == "Disabled") && (value == "1" || value.compare("true", Qt::CaseInsensitive) == 0)) disabled = true;
                if (name == "UsedIn") usedIn.append(value);
            }
            if (disabled || (!usedIn.isEmpty() && !usedIn.contains("synthesis"))) continue;
            const auto bd = resolve(file.attribute("Path"), info);
            auto generated = QFileInfo(bd).absolutePath();
            if (!active.attribute("RelGenDir").isEmpty()) {
                const auto sources = resolve(active.attribute("RelSrcDir"), info);
                if (!within(bd, sources)) fail("Cannot identify the generated HWH directory for " + bd);
                generated = clean(resolve(active.attribute("RelGenDir"), info) + '/' + QDir(sources).relativeFilePath(generated));
            }
            handoffs.insert((generated + "/hw_handoff/" + QFileInfo(bd).completeBaseName() + ".hwh").toCaseFolded());
        }
    }
    const QSet<QString> runTypes{"bit", "bin", "mcs", "ltx"};
    QList<Entry> selected;
    for (const auto& entry : files) {
        if (entry.directory) continue;
        const auto path = info.root + '/' + entry.relative;
        const QFileInfo file(path);
        if ((runTypes.contains(file.suffix().toLower()) && implementationDirectories.contains(file.absolutePath().toCaseFolded()))
            || handoffs.contains(path.toCaseFolded())) selected.append(entry);
    }
    return selected;
}
// Validate BEFORE launching Vivado. Only the independent XML copy is rewritten.
QDomDocument relocate(const ProjectInfo& info, const QString& destination, QSet<QString>& imports,
                      QSet<QString>& generatedSources, const QMap<QString, QString>& containerSources,
                      bool archiveCleanup = false, QStringList* missingRepositories = nullptr)
{
    auto doc = xml(info.xpr);
    QList<QDomElement> elements;
    std::function<void(QDomElement)> walk = [&](QDomElement e) {
        elements.append(e);
        for (auto child = e.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) walk(child);
    };
    walk(doc.documentElement());
    QSet<QString> declaredContainers, requiredSources;
    const auto autoDisabled = [](const QDomElement& element) {
        if (element.parentNode().toElement().attribute("Type") != "SimulationSrcs") return false;
        const QSet<QString> hdlTypes{"v", "sv", "vh", "svh", "vhd", "vhdl"};
        if (!hdlTypes.contains(QFileInfo(element.attribute("Path")).suffix().toLower())) return false;
        const auto attributes = element.firstChildElement("FileInfo").elementsByTagName("Attr");
        for (int n = 0; n < attributes.size(); ++n) {
            const auto attribute = attributes.at(n).toElement();
            if (attribute.attribute("Name") == "AutoDisabled" && attribute.attribute("Val") == "1") return true;
        }
        return false;
    };
    for (const auto& element : elements) {
        if (element.tagName() != "File") continue;
        const auto path = resolve(element.attribute("Path"), info).toCaseFolded();
        if (path.endsWith(".xcix")) declaredContainers.insert(path);
        if (!autoDisabled(element)) requiredSources.insert(path);
    }
    for (int i = 0; i < elements.size(); ++i) {
        auto element = elements.at(i);
        if (element.tagName() == "Project") { element.setAttribute("Path", destination + '/' + info.name + ".xpr"); continue; }
        if (archiveCleanup && element.tagName() == "Option" && element.attribute("Name") == "IPRepoPath"
            && element.parentNode() == doc.documentElement().firstChildElement("Configuration")
            && !element.attribute("Val").isEmpty()) {
            const auto value = element.attribute("Val");
            const auto path = expandedPath(value, info);
            noLinks(path);
            if (!QFileInfo::exists(path)) {
                // A missing catalog search directory supplies no files. Check the copied
                // project's IP definitions with the matching Vivado before allowing reset.
                if (missingRepositories) missingRepositories->append(value);
                element.parentNode().removeChild(element);
                continue;
            }
        }
        if (archiveCleanup && element.tagName() == "File" && autoDisabled(element)) {
            const auto path = resolve(element.attribute("Path"), info);
            if (!QFileInfo::exists(path) && !requiredSources.contains(path.toCaseFolded())
                && !containerSources.contains(path.toCaseFolded())) {
                element.parentNode().removeChild(element);
                continue;
            }
        }
        // A stale automatic checkpoint is a run product, not a required design source.
        if (element.tagName() == "File" && element.parentNode().toElement().attribute("Type") == "Utils"
            && QFileInfo(element.attribute("Path")).suffix().compare("dcp", Qt::CaseInsensitive) == 0) {
            bool automatic = false;
            const auto attributes = element.firstChildElement("FileInfo").elementsByTagName("Attr");
            for (int n = 0; n < attributes.size(); ++n) {
                const auto attribute = attributes.at(n).toElement();
                if (attribute.attribute("Name") == "AutoDcp" && attribute.attribute("Val") == "1") automatic = true;
            }
            if (automatic && !QFileInfo::exists(resolve(element.attribute("Path"), info))) {
                element.parentNode().removeChild(element);
                continue;
            }
        }
        const auto semanticKey = element.attribute("Name", element.attribute("Id"));
        if (semanticKey.contains("Tcl", Qt::CaseInsensitive) || semanticKey.contains("Hook", Qt::CaseInsensitive)) {
            std::function<bool(QDomElement)> hasValue = [&](QDomElement node) {
                const auto attributes = node.attributes();
                for (int n = 0; n < attributes.size(); ++n) {
                    const auto attr = attributes.item(n).toAttr();
                    if (attr.name() != "Name" && attr.name() != "Id" && !attr.value().isEmpty()) return true;
                }
                if (!node.text().trimmed().isEmpty()) return true;
                for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                    if (hasValue(child)) return true;
                return false;
            };
            if (hasValue(element)) fail("Custom Tcl hooks are not supported by safe archiving: " + semanticKey);
        }
        const auto attrs = element.attributes();
        for (int a = 0; a < attrs.size(); ++a) {
            const auto attr = attrs.item(a).toAttr();
            const auto key = attr.name() == "Val" ? element.attribute("Name", element.attribute("Id")) : attr.name();
            QString value = attr.value();
            // Provenance is not an active dependency; do not leave original locations in the copy.
            if (key == "ImportPath" || key == "ImportedFrom") { element.setAttribute(attr.name(), ""); continue; }
            if (!pathValue(key, value)) continue;
            if (key.contains("Tcl", Qt::CaseInsensitive) && !value.isEmpty())
                fail("Custom Tcl hooks are not supported by safe archiving: " + key);
            // Values such as numeric TransportPathDelay and non-path boolean options are not paths.
            if (!value.contains('/') && !value.contains('\\') && !value.startsWith('$') && attr.name() == "Val") continue;
            const auto resolved = resolve(value, info);
            const QString rel = QDir(info.root).relativeFilePath(resolved);
            const bool fileReference = element.tagName() == "File" && attr.name() == "Path";
            const bool directoryReference = key.contains("IncludeDir", Qt::CaseInsensitive)
                || key.contains("IPRepo", Qt::CaseInsensitive) || key.contains("BoardRepo", Qt::CaseInsensitive);
            if (fileReference || directoryReference) {
                const auto container = containerSources.value(resolved.toCaseFolded());
                const bool virtualSource = !container.isEmpty() && declaredContainers.contains(container.toCaseFolded());
                if (fileReference && !QFileInfo(resolved).isFile() && !virtualSource) fail("Missing project source: " + resolved);
                if (directoryReference && !QFileInfo(resolved).isDir()) fail("Missing dependency directory: " + resolved);
                if (resolved.compare(info.root, Qt::CaseInsensitive) == 0) fail("A dependency directory refers to the entire project root. Use a dedicated source/include directory.");
                if (!within(resolved, info.root + '/' + info.name + ".srcs")) {
                    const auto top = rel.section('/', 0, 0);
                    if (top.startsWith(info.name + '.', Qt::CaseInsensitive)) {
                        const QSet<QString> hdlTypes{"v", "sv", "vh", "svh", "vhd", "vhdl"};
                        const bool generatedRoot = top.compare(info.name + ".gen", Qt::CaseInsensitive) == 0
                            || top.compare(info.name + ".ip_user_files", Qt::CaseInsensitive) == 0;
                        const bool hdlFile = fileReference && hdlTypes.contains(QFileInfo(resolved).suffix().toLower());
                        const bool includeDirectory = directoryReference && key.contains("IncludeDir", Qt::CaseInsensitive);
                        if (!generatedRoot || (!hdlFile && !includeDirectory))
                            fail("Unsupported dependency in generated project data: " + value);
                        const auto directory = fileReference ? QFileInfo(resolved).absolutePath() : resolved;
                        const auto relativeDirectory = QDir(info.root).relativeFilePath(directory);
                        if (QFileInfo::exists(info.root + '/' + info.name + ".srcs/_archive_generated/" + relativeDirectory))
                            fail("Generated source import would overwrite existing sources: " + relativeDirectory);
                        generatedSources.insert(relativeDirectory);
                        // Rewrite inputs only. RelGenDir and other output paths must remain disposable.
                        element.setAttribute(attr.name(), "$PSRCDIR/_archive_generated/" + rel);
                        continue;
                    }
                    imports.insert(top);
                }
            }
            element.setAttribute(attr.name(), rel == "." ? "$PPRDIR" : "$PPRDIR/" + rel);
        }
    }
    // Keep ordinary local source directories, including sibling headers, under .srcs.
    for (int i = 0; i < elements.size(); ++i) {
        auto element = elements.at(i);
        const auto attrs = element.attributes();
        for (int a = 0; a < attrs.size(); ++a) {
            const auto attr = attrs.item(a).toAttr();
            const auto value = attr.value();
            for (const auto& top : imports) {
                const auto old = "$PPRDIR/" + top;
                if (value == old || value.startsWith(old + '/'))
                    element.setAttribute(attr.name(), "$PSRCDIR/_archive_imports/" + value.mid(8));
            }
        }
    }
    return doc;
}
void validateMetadataText(const QString& root, const QString& relativePath, const QByteArray& bytes)
{
    // IP/BD metadata can carry paths independently of the XPR. Fail closed for external paths.
    // A drive letter cannot begin inside a word, such as the final 'p' of http://.
    const QRegularExpression absolute("(?<![A-Za-z0-9_])(?:[A-Za-z]:[/\\\\][^\"<>\\r\\n]*|\\\\\\\\[^\"<>\\r\\n]+)");
    const auto text = QString::fromUtf8(bytes);
    const QRegularExpression relative("(?:\\.\\./)+[^\\\"<>\\r\\n ]+");
    auto relatives = relative.globalMatch(text);
    while (relatives.hasNext()) {
        const auto value = relatives.next().captured();
        const auto target = clean(QFileInfo(root + '/' + relativePath).dir().absoluteFilePath(value));
        if (!within(target, root)) fail("External relative path in IP/BD metadata: " + relativePath + " -> " + value);
    }
    auto matches = absolute.globalMatch(text);
    while (matches.hasNext()) {
        const auto value = matches.next().captured();
        // Absolute paths in IP metadata cannot be generically rewritten without changing IP semantics.
        fail("Absolute path in IP/BD metadata requires a self-contained source: " + relativePath + " -> " + value);
    }
}
QMap<QString, QString> validateMetadata(const QString& root, const QList<Entry>& entries, const Check& check)
{
    QMap<QString, QString> containers;
    for (const auto& entry : entries) {
        check();
        if (entry.directory) continue;
        const auto ext = QFileInfo(entry.relative).suffix().toLower();
        const auto path = root + '/' + entry.relative;
        if (ext == "xcix") {
            const auto core = inspectCoreContainer(path, check, [&](const QString& member, const QByteArray& bytes) {
                validateMetadataText(root, QFileInfo(entry.relative).path() + '/' + member, bytes);
            });
            const auto virtualPath = clean(QFileInfo(path).dir().absoluteFilePath(core)).toCaseFolded();
            if (containers.contains(virtualPath)) fail("Ambiguous core-container source: " + virtualPath);
            containers.insert(virtualPath, path);
        } else if (ext == "xci" || ext == "bd" || QFileInfo(entry.relative).fileName() == "component.xml") {
            if (entry.size > 64 * 1024 * 1024) fail("Metadata file is too large to validate: " + entry.relative);
            validateMetadataText(root, entry.relative, readFile(path));
        }
    }
    return containers;
}
const char* resetScript = R"TCL(
proc ftb_inside {path root} {
    set p [string tolower [file normalize $path]]
    set r [string tolower [file normalize $root]]
    return [expr {$p eq $r || [string first "$r/" $p] == 0}]
}
proc ftb_require_ipdef {vlnv owner} {
    if {[string match "xilinx.com:module_ref:*" $vlnv]} {return}
    if {$vlnv eq "" || [lsearch -exact [get_ipdefs -all -quiet $vlnv] $vlnv] < 0} {
        error "Missing IP definition $vlnv ($owner). Restore the required IP repository before archiving."
    }
}
if {[catch {
    if {[version -short] ne $::env(FTB_VERSION)} {error "Vivado version does not match the selected project release"}
    set root [file normalize $::env(FTB_PROJECT_ROOT)]
    set xpr [file normalize $::env(FTB_XPR)]
    if {![ftb_inside $xpr $root]} {error "Project path escaped the archive workspace"}
    open_project $xpr
    if {![ftb_inside [get_property DIRECTORY [current_project]] $root]} {error "Project directory escaped the archive workspace"}
    foreach f [get_files -all -quiet] {
        if {![ftb_inside $f $root]} {error "External file detected before reset: $f"}
    }
    foreach run [get_runs -quiet] {
        if {![ftb_inside [get_property DIRECTORY $run] $root]} {error "External run directory detected before reset"}
    }
    if {$::env(FTB_VERIFY_IP_CATALOG) eq "1"} {
        foreach ip [get_ips -all -quiet] {ftb_require_ipdef [get_property IPDEF $ip] $ip}
        foreach bd [get_files -quiet -filter {FILE_TYPE == "Block Designs"}] {
            open_bd_design $bd
            foreach cell [get_bd_cells -hierarchical -quiet] {
                if {[get_property TYPE $cell] eq "ip"} {ftb_require_ipdef [get_property VLNV $cell] $cell}
            }
            close_bd_design [current_bd_design]
        }
        puts "FTB_IP_CATALOG_VERIFIED: Missing repository paths were not required by the copied IP definitions."
    }
    puts "FTB_STAGE_RESET"
    reset_project
    close_project
    set marker [open $::env(FTB_SUCCESS_FILE) w]
    puts $marker "reset_project completed"
    close $marker
} reason]} {
    puts stderr "FTB_ERROR: $reason"
    exit 1
}
exit 0
)TCL";

void run7Zip(const QString& executable, const QStringList& arguments, const QString& workspace,
             const QString& control, const Check& check, const std::function<void(int)>& advance,
             const std::function<void(const QString&)>& log)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setWorkingDirectory(workspace);
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    QFile output(control + "/7zip.log");
    if (!output.open(QIODevice::WriteOnly | QIODevice::Append)) fail("Cannot create the compression log.");
    check();
    process.start();
    if (!process.waitForStarted(10000)) fail("Cannot start the bundled 7-Zip runtime: " + process.errorString());
    QElapsedTimer elapsed; elapsed.start();
    QString pending;
    const QRegularExpression percentage("(?:^|[\\s\\x08])(\\d{1,3})%");
    int lastPercent = -1;
    try {
        do {
            check();
            process.waitForFinished(100);
            const auto bytes = process.readAll();
            if (!bytes.isEmpty()) {
                if (output.write(bytes) != bytes.size()) fail("Cannot write the compression log.");
                auto text = QString::fromUtf8(bytes);
                pending += text;
                auto matches = percentage.globalMatch(pending);
                while (matches.hasNext()) {
                    const int value = qBound(0, matches.next().captured(1).toInt(), 100);
                    if (value > lastPercent) { lastPercent = value; advance(value); }
                }
                pending = pending.right(16);
                text.remove(QChar(8)); text.replace('\r', '\n');
                log(text.trimmed());
            }
            if (elapsed.elapsed() > 60 * 60 * 1000) fail("7-Zip timed out after one hour.");
        } while (process.state() != QProcess::NotRunning);
    } catch (...) {
        // The trusted standalone compressor has no child processes.
        process.kill(); process.waitForFinished(5000);
        throw;
    }
    check();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        fail(QString("7-Zip failed (exit %1). See the compression log.").arg(process.exitCode()));
}

}

void runBatch(const Installation& vivado, const QString& control, const QByteArray& script,
              const QMap<QString, QString>& variables, const Check& check, const Log& log)
{
    const auto launcher = clean(QFileInfo(vivado.launcher).absoluteFilePath());
    if (!QFileInfo(launcher).isFile()) fail("Vivado launcher is missing: " + launcher);
    const auto install = QFileInfo(launcher).dir().absoluteFilePath("..");
    for (const auto& name : {"Vivado_init.tcl", "init.tcl"})
        if (QFileInfo::exists(install + "/scripts/" + name))
            fail("Installation startup script must be reviewed/removed before safe batch use: " + install + "/scripts/" + name);
    noLinks(control);
    if (!QDir().mkpath(control)) fail("Cannot create the batch control directory.");
    if (QFileInfo::exists(control + "/reset.ok")) fail("Batch control directory has already been used.");
    writeFile(control + "/reset.tcl", script);
    auto env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    // Vivado's launcher adds its own runtime paths. Inherited tool paths (or
    // line breaks in PATH) can prevent its batch loader from finding DLLs.
    const auto windows = QDir::fromNativeSeparators(env.value("SystemRoot"));
    env.insert("PATH", QDir::toNativeSeparators(windows + "/System32;" + windows + ';' + windows + "/System32/Wbem"));
#endif
    for (const auto* name : {"APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TMP", "TEMP", "XILINX_LOCAL_USER_DATA"}) {
        const auto dir = control + "/profile";
        QDir().mkpath(dir);
        env.insert(name, dir);
    }
    env.remove("MYVIVADO");
    env.remove("RDI_VERBOSE");
    for (auto it = variables.cbegin(); it != variables.cend(); ++it) env.insert(it.key(), it.value());
    env.insert("FTB_VERSION", vivado.version);
    env.insert("FTB_SUCCESS_FILE", control + "/reset.ok");
    QProcess process;
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(control);
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    // A suspended process enters a job before any launcher child can escape it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) fail("Cannot create the Vivado process job.");
    struct JobHandle { HANDLE h; ~JobHandle() { CloseHandle(h); } } jobHandle{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        fail("Cannot configure the Vivado process job.");
    PROCESS_INFORMATION* pi = nullptr;
    process.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW;
        pi = reinterpret_cast<PROCESS_INFORMATION*>(args->processInformation);
    });
    if (launcher.endsWith(".bat", Qt::CaseInsensitive) || launcher.endsWith(".cmd", Qt::CaseInsensitive)) {
        if (launcher.contains(QRegularExpression("[\"%!^&|<>\\r\\n]"))) fail("Vivado launcher path contains unsupported command-shell characters.");
        process.setProgram(QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot")) + "/System32/cmd.exe");
        process.setNativeArguments("/D /S /C \"\"" + QDir::toNativeSeparators(launcher) + "\" -mode batch -nojournal -nolog -notrace -source reset.tcl\"");
    } else {
        process.setProgram(launcher);
        process.setArguments({"-mode", "batch", "-nojournal", "-nolog", "-notrace", "-source", "reset.tcl"});
    }
#else
    process.setProgram(launcher);
    process.setArguments({"-mode", "batch", "-nojournal", "-nolog", "-notrace", "-source", "reset.tcl"});
#endif
    process.start();
    if (!process.waitForStarted(10000)) fail("Cannot start Vivado: " + process.errorString());
#ifdef Q_OS_WIN
    if (!pi || !AssignProcessToJobObject(job, pi->hProcess)) {
        process.kill(); process.waitForFinished(5000);
        fail("Cannot contain the Vivado process tree. No project was opened.");
    }
    if (ResumeThread(pi->hThread) == DWORD(-1)) {
        TerminateJobObject(job, 1); process.waitForFinished(5000);
        fail("Cannot resume the contained Vivado process.");
    }
#endif
    QFile logfile(control + "/vivado.log");
    if (!logfile.open(QIODevice::WriteOnly)) { process.kill(); process.waitForFinished(); fail("Cannot create the Vivado log."); }
    QElapsedTimer elapsed;
    elapsed.start();
    try {
        do {
            check();
            process.waitForFinished(100);
            const auto bytes = process.readAll();
            if (!bytes.isEmpty()) {
                if (logfile.write(bytes) != bytes.size()) fail("Cannot write the Vivado log.");
                log(QString::fromLocal8Bit(bytes));
            }
            if (elapsed.elapsed() > 30 * 60 * 1000) fail("Vivado timed out after 30 minutes.");
        } while (process.state() != QProcess::NotRunning);
    } catch (...) {
#ifdef Q_OS_WIN
        TerminateJobObject(job, 1);
#endif
        process.kill(); process.waitForFinished(5000);
        throw;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !QFileInfo::exists(control + "/reset.ok"))
        fail(QString("Vivado batch did not complete (exit %1, %2). See the batch log.").arg(process.exitCode()).arg(process.exitStatus() == QProcess::NormalExit ? "normal exit" : "crash"));
}
namespace {
void runVivado(const Request& request, const QString& control, const QString& project,
               const QString& xpr, bool verifyIpCatalog, const Check& check, const Log& log)
{
    runBatch(request.vivado, control, resetScript, {{"FTB_PROJECT_ROOT", project}, {"FTB_XPR", xpr},
             {"FTB_VERIFY_IP_CATALOG", verifyIpCatalog ? "1" : "0"}}, check, log);
}
}

void requirePlainPath(const QString& path) { noLinks(path); }
bool isWithin(const QString& path, const QString& root) { return within(path, root); }
ProjectInfo isolateProject(const QString& xpr, const QString& destination, const Check& check, const Log& log)
{
    const auto info = inspectProject(xpr);
    const auto target = clean(QFileInfo(destination).absoluteFilePath());
    noLinks(target);
    if (within(target, info.root) || within(info.root, target)) fail("The copy and source project must not contain one another.");
    if (QFileInfo::exists(target)) fail("The independent copy destination already exists.");
    QSet<QString> imports, generated;
    const auto entries = inventory(info.root, check);
    const auto containers = validateMetadata(info.root, entries, check);
    relocate(info, target, imports, generated, containers);
    if (!QDir().mkpath(target)) fail("Cannot create the independent project copy.");
    for (const auto& entry : entries) {
        check();
        const auto path = target + '/' + entry.relative;
        if (entry.directory) { if (!QDir().mkpath(path)) fail("Cannot create " + path); }
        else copyFile(info.root + '/' + entry.relative, path, check);
    }
    const auto srcs = target + '/' + info.name + ".srcs";
    QDir().mkpath(srcs);
    for (const auto& entry : entries) {
        bool keep = false;
        for (const auto& root : generated) if (within(entry.relative, root)) keep = true;
        if (!keep) continue;
        const auto path = srcs + "/_archive_generated/" + entry.relative;
        if (entry.directory) QDir().mkpath(path);
        else copyFile(target + '/' + entry.relative, path, check);
    }
    for (const auto& root : imports) {
        const auto path = srcs + "/_archive_imports/" + root;
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (QFileInfo::exists(path) || !QDir().rename(target + '/' + root, path)) fail("Cannot isolate local source: " + root);
    }
    const auto copiedXpr = target + '/' + QFileInfo(info.xpr).fileName();
    auto copiedInfo = info; copiedInfo.xpr = copiedXpr;
    QSet<QString> copiedImports, copiedGenerated;
    const auto document = relocate(copiedInfo, target, copiedImports, copiedGenerated, containers);
    if (imports != copiedImports || generated != copiedGenerated) fail("Project references changed while copying.");
    writeFile(copiedXpr, document.toByteArray(2));
    copiedInfo.root = target;
    const auto copiedContainers = validateMetadata(target, inventory(target, check), check);
    QSet<QString> verifiedImports, verifiedGenerated;
    relocate(copiedInfo, target, verifiedImports, verifiedGenerated, copiedContainers);
    log("Independent project copy: " + target);
    return {copiedXpr, target, info.name, info.version};
}

QString archiveNameError(const QString& value)
{
    const auto name = value.trimmed();
    if (name.isEmpty()) return {};
    if (name.size() > 252) return "Use an archive name of at most 252 characters.";
    if (name == "." || name == ".." || name.endsWith('.')) return "The archive name must not end with a dot.";
    if (name.contains(QRegularExpression("[\\\\/:\"<>|?*\\x00-\\x1f\\x7f]")))
        return "Use a folder name, without paths or these characters: \\ / : \" < > | ? *";
    const QRegularExpression reserved("^(?:CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|(?:COM|LPT)[1-9\\x{00b9}\\x{00b2}\\x{00b3}])(?:\\.|$)",
                                      QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch()) return "This archive name is reserved by Windows. Choose another name.";
    return {};
}

ProjectInfo inspectProject(const QString& path)
{
    noLinks(path);
    const QFileInfo f(path);
    if (!f.isFile() || f.suffix().compare("xpr", Qt::CaseInsensitive) != 0) fail("Select one existing .xpr file.");
    ProjectInfo info{clean(f.canonicalFilePath()), clean(f.canonicalPath()), f.completeBaseName(), {}};
    if (QDir(info.root).isRoot()) fail("The project cannot be located at a drive root.");
    if (info.name.contains(QRegularExpression("[^A-Za-z0-9_.-]"))) fail("The project name must contain only English letters, digits, dots, hyphens or underscores.");
    const auto doc = xml(info.xpr);
    QSet<QString> versions;
    for (auto n = doc.firstChild(); !n.isNull(); n = n.nextSibling())
        if (n.isComment()) { const auto v = release(n.nodeValue()); if (!v.isEmpty()) versions.insert(v); }
    if (versions.size() != 1) fail("The XPR does not identify exactly one Vivado product version.");
    info.version = *versions.begin();
    return info;
}
QString installationVersion(const QString& launcher)
{
    auto dir = QFileInfo(launcher).dir();
    for (int i = 0; i < 4; ++i) {
        const auto name = dir.dirName();
        if (QRegularExpression("^[0-9]{4}\\.[0-9]+(?:\\.[0-9]+)?$").match(name).hasMatch()) return name;
        if (!dir.cdUp()) break;
    }
    return {};
}
QList<Installation> discoverInstallations(const QStringList& requestedRoots)
{
    QStringList roots = requestedRoots;
    if (roots.isEmpty()) {
        if (!qEnvironmentVariable("XILINX_VIVADO").isEmpty()) roots << qEnvironmentVariable("XILINX_VIVADO");
        for (const auto& drive : QDir::drives()) {
            for (const auto& dir : QDir(drive.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (dir.fileName().contains(QRegularExpression("vivado|xilinx|^vvd|^amd$", QRegularExpression::CaseInsensitiveOption))) roots << dir.absoluteFilePath();
        }
    }
    QList<Installation> result;
    QSet<QString> visited;
    std::function<void(QString, int)> visit = [&](QString path, int depth) {
        if (QThread::currentThread()->isInterruptionRequested()) return;
        QFileInfo info(path);
        if (!info.isDir() || info.isSymLink() || info.isJunction()) return;
        path = clean(info.canonicalFilePath());
        if (visited.contains(path.toLower()) || visited.size() > 1000) return;
        visited.insert(path.toLower());
#ifdef Q_OS_WIN
        const auto launcher = path + "/bin/vivado.bat";
#else
        const auto launcher = path + "/bin/vivado";
#endif
        if (QFileInfo(launcher).isFile()) {
            const auto version = installationVersion(launcher);
            if (!version.isEmpty()) result.append({version, launcher});
            return;
        }
        if (depth == 0) return;
        for (const auto& child : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            visit(child.absoluteFilePath(), depth - 1);
    };
    for (const auto& root : roots) visit(root, 3);
    std::sort(result.begin(), result.end(), [](const Installation& a, const Installation& b) { return a.version > b.version; });
    return result;
}
ArchiveJob::ArchiveJob(Request request, QObject* parent) : QThread(parent), request_(std::move(request)) {}
ArchiveJob::~ArchiveJob() { requestInterruption(); wait(); }
void ArchiveJob::run()
{
    QString workspace, destination;
    bool published = false;
    auto check = [this] { if (isInterruptionRequested()) throw Cancelled{}; };
    auto guarded = [&](const QString& path) {
        check();
        if (workspace.isEmpty() || !within(path, workspace) || clean(path) == clean(workspace)) fail("Refusing a write outside the temporary workspace.");
        noLinks(path);
    };
    try {
        emit progress(0, 0, "Inspecting project and Vivado version...");
        check();
        const auto nameError = archiveNameError(request_.archiveName);
        if (!nameError.isEmpty()) fail(nameError);
        const auto info = inspectProject(request_.xpr);
        if (request_.vivado.version != info.version) fail("This project requires Vivado " + info.version + ". Select that exact release.");
        if (!QFileInfo(request_.vivado.launcher).isFile()) fail("Vivado " + info.version + " is not configured or its launcher is missing.");
        const auto compressor = request_.compressor.isEmpty()
            ? bundledCompressor() : request_.compressor;
        if (!QFileInfo(compressor).isFile()) fail("The bundled 7-Zip runtime is missing. Repair the application package.");
        const auto output = clean(QFileInfo(request_.outputRoot).absoluteFilePath());
        noLinks(output);
        if (within(output, info.root) || within(info.root, output)) fail("The output folder and source project must not contain one another.");
        const auto name = request_.archiveName.trimmed().isEmpty()
            ? info.name + '-' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + '-' + QUuid::createUuid().toString(QUuid::Id128).left(8)
            : request_.archiveName.trimmed();
        destination = output + '/' + name;
        const auto compressedDestination = destination + ".7z";
        const auto requireAvailableDestination = [&] {
            noLinks(destination); noLinks(compressedDestination);
            if (QFileInfo::exists(destination) || QFileInfo::exists(compressedDestination))
                fail("The archive folder or 7z file already exists. Choose another archive name.");
        };
        requireAvailableDestination();
        QSet<QString> imports, generatedSources;
        const auto files = inventory(info.root, check);
        const auto containers = validateMetadata(info.root, files, check);
        QStringList missingRepositories;
        relocate(info, "VALIDATION_ONLY", imports, generatedSources, containers, true, &missingRepositories);
        for (const auto& path : missingRepositories)
            emit logMessage("Omitting missing IP catalog path from the archive copy: " + path + ". Vivado will verify the copied IP definitions before reset.");
        const auto outputs = projectArtifacts(info, files);
        const auto preserveGenerated = [&](const QString& relative) {
            for (const auto& directory : generatedSources) if (within(relative, directory)) return true;
            return false;
        };
        qint64 totalBytes = 0, generatedBytes = 0;
        for (const auto& entry : files) {
            totalBytes += entry.size;
            if (preserveGenerated(entry.relative)) generatedBytes += entry.size;
        }
        if (!QDir().mkpath(output)) fail("Cannot create the archive output folder.");
        const QStorageInfo storage(output);
        if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < totalBytes * 3 + generatedBytes * 2 + 32 * 1024 * 1024)
            fail("Insufficient free space for the project copy, artifacts and 7z package.");
        QTemporaryDir temporary(output + "/.xips-archive-XXXXXX");
        if (!temporary.isValid()) fail("Cannot create an independent temporary workspace.");
        temporary.setAutoRemove(false);
        workspace = temporary.path();
        const auto project = workspace + "/project";
        const auto control = workspace + "/control";
        const auto artifacts = control + "/artifacts";
        QDir().mkpath(project); QDir().mkpath(artifacts);
        emit progress(0, 100, "Project verified. Required Vivado: " + info.version);
        qint64 copied = 0;
        int lastPercent = -1;
        emit progress(1, 0, "Copying the complete project...");
        for (const auto& entry : files) {
            const auto target = project + '/' + entry.relative;
            guarded(target);
            if (entry.directory) { if (!QDir().mkpath(target)) fail("Cannot create " + target); }
            else copyFile(info.root + '/' + entry.relative, target, check, [&](qint64 bytes) {
                copied += bytes;
                const int pct = totalBytes ? int(copied * 100 / totalBytes) : 100;
                if (pct != lastPercent) { lastPercent = pct; emit progress(1, pct, "Copying " + entry.relative); }
            });
        }
        emit progress(1, 100, "Project copy complete.");
        emit progress(2, 0, "Preserving project implementation outputs and active BD handoffs...");
        emit logMessage("Artifact policy: registered design implementation runs; enabled synthesis BDs in the current design source set.");
        QSet<QString> reservedNames, savedNames;
        for (const auto& entry : outputs) reservedNames.insert(QFileInfo(entry.relative).fileName().toCaseFolded());
        for (int i = 0; i < outputs.size(); ++i) {
            const QFileInfo file(outputs[i].relative);
            auto name = file.fileName();
            if (savedNames.contains(name.toCaseFolded())) {
                int suffix = 2;
                do { name = file.completeBaseName() + "__" + QString::number(suffix++) + '.' + file.suffix(); }
                while (reservedNames.contains(name.toCaseFolded()) || savedNames.contains(name.toCaseFolded()));
            }
            savedNames.insert(name.toCaseFolded());
            guarded(artifacts + '/' + name);
            copyFile(project + '/' + outputs[i].relative, artifacts + '/' + name, check);
            emit logMessage("Artifact: " + outputs[i].relative + " -> artifacts/" + name);
            emit progress(2, (i + 1) * 100 / outputs.size(), "Saved " + name);
        }
        emit progress(2, 100, QString("Preserved %1 artifacts.").arg(outputs.size()));
        const auto srcs = project + '/' + info.name + ".srcs";
        guarded(srcs);
        QDir().mkpath(srcs);
        // Preserve referenced HDL directories and sibling includes from the independent copy.
        // Leave the original generated directories in place for Vivado's normal reset behavior.
        for (const auto& entry : files) {
            if (!preserveGenerated(entry.relative)) continue;
            const auto target = srcs + "/_archive_generated/" + entry.relative;
            guarded(target);
            if (entry.directory) { if (!QDir().mkpath(target)) fail("Cannot preserve generated source directory."); }
            else copyFile(project + '/' + entry.relative, target, check);
        }
        if (!generatedSources.isEmpty()) emit logMessage("Preserved generated HDL sources under .srcs/_archive_generated.");
        for (const auto& top : imports) {
            const auto target = srcs + "/_archive_imports/" + top;
            guarded(target);
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (QFileInfo::exists(target) || !QDir().rename(project + '/' + top, target)) fail("Cannot isolate local source: " + top);
        }
        const auto xpr = project + '/' + info.name + ".xpr";
        guarded(xpr);
        const auto copiedName = project + '/' + QFileInfo(info.xpr).fileName();
        if (copiedName != xpr) {
            const auto intermediate = control + "/normalized.xpr";
            if (!QFile::rename(copiedName, intermediate) || !QFile::rename(intermediate, xpr))
                fail("Cannot normalize the copied XPR filename.");
        }
        // Re-read the already copied XPR, using original roots only as path metadata.
        auto copiedInfo = info; copiedInfo.xpr = xpr;
        const auto copiedOutputs = projectArtifacts(copiedInfo, files);
        if (outputs.size() != copiedOutputs.size()) fail("Project artifact settings changed while copying. Close Vivado and retry.");
        for (int index = 0; index < outputs.size(); ++index)
            if (outputs[index].relative != copiedOutputs[index].relative) fail("Project artifact settings changed while copying. Close Vivado and retry.");
        QSet<QString> copiedImports, copiedGenerated;
        QStringList copiedMissingRepositories;
        auto relocated = relocate(copiedInfo, project, copiedImports, copiedGenerated, containers, true, &copiedMissingRepositories);
        if (imports != copiedImports || generatedSources != copiedGenerated || missingRepositories != copiedMissingRepositories)
            fail("Project references changed while copying. Close Vivado and retry.");
        writeFile(xpr, relocated.toByteArray(2));
        copiedInfo.root = project;
        const auto copiedContainers = validateMetadata(project, inventory(project, check), check);
        QSet<QString> verifiedImports, verifiedGenerated;
        relocate(copiedInfo, project, verifiedImports, verifiedGenerated, copiedContainers);
        emit progress(3, -1, "Running Vivado " + info.version + " in batch mode...");
        runVivado(request_, control, project, xpr, !missingRepositories.isEmpty(), check, [this](const QString& line) { emit logMessage(line); });
        emit progress(3, 100, "reset_project completed.");
        check();
        emit progress(4, 0, "Keeping only .xpr, .srcs and artifacts...");
        const auto after = inventory(project, check); // Reject links created by the child before pruning.
        Q_UNUSED(after)
        if (!QFileInfo(xpr).isFile() || !QFileInfo(srcs).isDir()) fail("Vivado did not leave a valid project and source directory.");
        const auto children = QDir(project).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto& child : children) {
            if (child.fileName() == info.name + ".xpr" || child.fileName() == info.name + ".srcs") continue;
            guarded(child.absoluteFilePath());
            const bool removed = child.isDir() ? QDir(child.absoluteFilePath()).removeRecursively() : QFile::remove(child.absoluteFilePath());
            if (!removed) fail("Cannot remove temporary generated data: " + child.fileName());
        }
        guarded(project + "/artifacts");
        if (!QDir().rename(artifacts, project + "/artifacts")) fail("Cannot restore the preserved artifacts.");
        // The root XPR path is metadata; keep it consistent with the final published location.
        auto finalDoc = xml(xpr);
        finalDoc.documentElement().setAttribute("Path", destination + '/' + info.name + ".xpr");
        writeFile(xpr, finalDoc.toByteArray(2));
        check();
        // A separate parent allows names such as "project" and "control" without workspace collisions.
        const auto publication = workspace + "/publish";
        guarded(publication);
        if (!QDir().mkpath(publication)) fail("Cannot prepare the archive publication directory.");
        const auto ready = publication + '/' + name;
        guarded(ready);
        if (!QDir().rename(project, ready)) fail("Cannot prepare the completed archive folder.");
        emit progress(4, 100, "Archive folder prepared.");
        const auto compressed = control + '/' + name + ".7z";
        guarded(compressed);
        emit progress(5, 0, "Creating 7z package...");
        const auto compressionLog = [this](const QString& line) { if (!line.isEmpty()) emit logMessage(line); };
        run7Zip(compressor, {"a", "-t7z", "-mx=5", "-mmt=2", "-bsp1", "-sccUTF-8", "-y", "--", compressed, "./" + name},
            publication, control, check, [this](int percent) { emit progress(5, percent * 90 / 100, "Creating 7z package..."); }, compressionLog);
        QFile signature(compressed);
        if (!signature.open(QIODevice::ReadOnly) || signature.read(6) != QByteArray::fromHex("377abcaf271c"))
            fail("7-Zip did not create a valid 7z package.");
        signature.close();
        emit progress(5, 90, "Checking 7z package...");
        run7Zip(compressor, {"t", "-bsp1", "-sccUTF-8", "--", compressed}, workspace, control, check,
            [this](int percent) { emit progress(5, 90 + percent * 9 / 100, "Checking 7z package..."); }, compressionLog);
        check();
        noLinks(output);
        requireAvailableDestination();
        if (!QDir().rename(ready, destination)) fail("Cannot publish the completed archive folder.");
        if (!QFile::rename(compressed, compressedDestination)) {
            noLinks(destination);
            if (!QDir().rename(destination, ready)) fail("7z publication failed. The archive folder remains at " + destination);
            fail("Cannot publish the 7z package. The completed folder is retained in diagnostics.");
        }
        published = true;
        emit progress(5, 100, "Archive and 7z package ready.");
        // Only this job's independently allocated workspace is eligible for cleanup.
        noLinks(workspace);
        if (!QDir(workspace).removeRecursively()) emit logMessage("Temporary diagnostics retained at " + workspace);
        emit completed(true, false, destination, {}, QString("Archive ready. %1 artifacts saved. Folder and 7z created.").arg(outputs.size()));
    } catch (const Cancelled&) {
        emit completed(false, true, {}, workspace, "Cancelled. The source project was not modified.");
    } catch (const std::exception& ex) {
        emit completed(published, false, published ? destination : QString{}, workspace, QString::fromUtf8(ex.what()));
    }
}
}
