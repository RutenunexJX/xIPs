#include "managed/ManagedAssetService.h"

#include "assetcore/JsonUtil.h"
#include "manifest/ManifestService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace xips {
namespace {

QString normalizedRelative(QString path)
{
    path = QDir::cleanPath(path);
    path.replace(u'\\', u'/');
    while (path.startsWith(QStringLiteral("./"))) {
        path.remove(0, 2);
    }
    return path;
}

bool safeRelative(const QString &path)
{
    const QString normalized = normalizedRelative(path);
    return !normalized.isEmpty() && normalized != QStringLiteral(".")
           && normalized != QStringLiteral("..")
           && !normalized.startsWith(QStringLiteral("../"))
           && !QDir::isAbsolutePath(normalized);
}

bool isLinkLike(const QFileInfo &info)
{
    if (info.isSymLink()) {
        return true;
    }
#ifdef Q_OS_WIN
    return info.isJunction();
#else
    return false;
#endif
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool sameAbsolutePath(const QString &left, const QString &right)
{
    return QFileInfo(left).absoluteFilePath().compare(
               QFileInfo(right).absoluteFilePath(),
               pathCaseSensitivity())
           == 0;
}

QString bytesHash(const QByteArray &contents)
{
    return QStringLiteral("sha256:")
           + QString::fromLatin1(
               QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QString fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

bool ignoredDirectory(const QString &name)
{
    const QString normalized = name.toLower();
    return normalized == QStringLiteral(".git")
           || normalized == QStringLiteral(".xips")
           || normalized == QStringLiteral(".xil")
           || normalized == QStringLiteral("build")
           || normalized == QStringLiteral("cache")
           || normalized == QStringLiteral("ip_user_files")
           || normalized.startsWith(QStringLiteral("build-"));
}

void collectFiles(const QString &directory,
                  const QString &relativeRoot,
                  QList<QPair<QString, QString>> &files)
{
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLinkLike(entry)) {
            continue;
        }
        const QString relative = relativeRoot.isEmpty()
                                     ? entry.fileName()
                                     : relativeRoot + u'/' + entry.fileName();
        if (entry.isDir()) {
            if (!ignoredDirectory(entry.fileName())) {
                collectFiles(entry.absoluteFilePath(), relative, files);
            }
        } else if (entry.isFile() && entry.fileName() != QStringLiteral(".xips.json")) {
            files.append({entry.absoluteFilePath(), normalizedRelative(relative)});
        }
    }
}

void classifyManifestFile(Manifest &manifest, const QString &relative)
{
    const QString suffix = QFileInfo(relative).suffix().toLower();
    const QString normalized = normalizedRelative(relative);
    const QString lower = normalized.toLower();
    const bool testFile = lower.startsWith(QStringLiteral("tb/"))
                          || lower.startsWith(QStringLiteral("test/"))
                          || lower.startsWith(QStringLiteral("tests/"))
                          || QFileInfo(lower).completeBaseName().endsWith(QStringLiteral("_tb"));
    if ((suffix == QStringLiteral("sv") || suffix == QStringLiteral("v")) && testFile) {
        manifest.tests.append(normalized);
    } else if (suffix == QStringLiteral("sv") || suffix == QStringLiteral("v")) {
        manifest.sources.append(normalized);
    } else if (suffix == QStringLiteral("svh") || suffix == QStringLiteral("vh")) {
        QString directory = QFileInfo(normalized).path();
        directory = directory == QStringLiteral(".") ? QString() : normalizedRelative(directory);
        if (!directory.isEmpty()) {
            manifest.includeDirs.append(directory);
        }
    } else if (suffix == QStringLiteral("xdc") || suffix == QStringLiteral("sdc")) {
        manifest.constraints.append(normalized);
    } else if (suffix == QStringLiteral("md") || suffix == QStringLiteral("pdf")
               || suffix == QStringLiteral("html")) {
        manifest.documentation.append(normalized);
    }
}

QJsonArray issuesJson(const QList<ManagedAssetIssue> &issues)
{
    QJsonArray result;
    for (const ManagedAssetIssue &issue : issues) {
        result.append(QJsonObject{
            {QStringLiteral("severity"), diagnosticSeverityToString(issue.severity)},
            {QStringLiteral("message"), issue.message},
            {QStringLiteral("path"), issue.path},
        });
    }
    return result;
}

bool writeBytes(const QString &path, const QByteArray &contents, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("Cannot create directory for %1").arg(path);
        }
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        file.cancelWriting();
        return false;
    }
    return true;
}

} // namespace

bool ManagedPlannedFile::generated() const
{
    return sourcePath.isEmpty();
}

bool ManagedAssetPlan::canExecute() const
{
    return std::none_of(issues.cbegin(), issues.cend(), [](const ManagedAssetIssue &issue) {
        return issue.severity == Diagnostic::Severity::Error;
    });
}

QJsonObject ManagedAssetPlan::toJson() const
{
    QJsonArray plannedFiles;
    for (const ManagedPlannedFile &file : files) {
        plannedFiles.append(QJsonObject{
            {QStringLiteral("source"), file.sourcePath},
            {QStringLiteral("destination"), file.destinationPath},
            {QStringLiteral("sourceHash"), file.sourceHash},
            {QStringLiteral("generated"), file.generated()},
        });
    }
    return QJsonObject{
        {QStringLiteral("libraryRoot"), libraryRoot},
        {QStringLiteral("targetRoot"), targetRoot},
        {QStringLiteral("manifestPath"), manifestPath},
        {QStringLiteral("canExecute"), canExecute()},
        {QStringLiteral("manifest"), ManifestService().toJson(manifest)},
        {QStringLiteral("files"), plannedFiles},
        {QStringLiteral("issues"), issuesJson(issues)},
    };
}

ManagedAssetPlan ManagedAssetService::plan(const ManagedAssetRequest &request) const
{
    ManagedAssetPlan plan;
    plan.libraryRoot = QFileInfo(request.libraryRoot).absoluteFilePath();
    plan.targetRoot = QDir(plan.libraryRoot).absoluteFilePath(request.id);
    plan.manifestPath =
        QDir(plan.targetRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    if (!QFileInfo(plan.libraryRoot).isDir()) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Managed library root must exist"),
            .path = plan.libraryRoot,
        });
    } else if (isLinkLike(QFileInfo(plan.libraryRoot))) {
        plan.issues.append(ManagedAssetIssue{
            .message =
                QStringLiteral("Managed library root must not be a symbolic link or junction"),
            .path = plan.libraryRoot,
        });
    }
    if (QFileInfo::exists(plan.targetRoot)
        || isLinkLike(QFileInfo(plan.targetRoot))) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Target asset directory already exists"),
            .path = plan.targetRoot,
        });
    }
    if (request.existingAssetIds.contains(request.id)) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Asset ID already exists in the managed library"),
            .path = request.id,
        });
    }

    plan.manifest.id = request.id;
    plan.manifest.type = AssetType::Module;
    plan.manifest.name = request.name;
    plan.manifest.top = request.top;
    plan.manifest.version = request.version;
    plan.manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("managed"), true},
    };

    if (request.seed == ManagedAssetSeed::EmptyModule) {
        const QString destination =
            QStringLiteral("rtl/%1.sv").arg(request.id);
        const QByteArray contents =
            QStringLiteral("module %1 (\n);\n\nendmodule\n")
                .arg(request.top)
                .toUtf8();
        plan.files.append(ManagedPlannedFile{
            .destinationPath = destination,
            .sourceHash = bytesHash(contents),
            .generatedContents = contents,
        });
        plan.manifest.sources.append(destination);
    } else if (request.seed == ManagedAssetSeed::SourceFile) {
        const QFileInfo source(request.sourceFile);
        if (!source.isFile() || isLinkLike(source)) {
            plan.issues.append(ManagedAssetIssue{
                .message = QStringLiteral(
                    "Seed source file must be a regular non-link file"),
                .path = source.absoluteFilePath(),
            });
        } else {
            const QString destination =
                QStringLiteral("rtl/%1").arg(source.fileName());
            plan.files.append(ManagedPlannedFile{
                .sourcePath = source.absoluteFilePath(),
                .destinationPath = destination,
                .sourceHash = fileHash(source.absoluteFilePath()),
            });
            classifyManifestFile(plan.manifest, destination);
        }
    } else {
        const QFileInfo directory(request.existingDirectory);
        if (!directory.isDir() || isLinkLike(directory)) {
            plan.issues.append(ManagedAssetIssue{
                .message = QStringLiteral(
                    "Seed directory must be a regular non-link directory"),
                .path = directory.absoluteFilePath(),
            });
        } else {
            QList<QPair<QString, QString>> sourceFiles;
            collectFiles(directory.absoluteFilePath(), {}, sourceFiles);
            for (const auto &[source, relative] : sourceFiles) {
                if (!safeRelative(relative)) {
                    plan.issues.append(ManagedAssetIssue{
                        .message = QStringLiteral("Unsafe seed path"),
                        .path = relative,
                    });
                    continue;
                }
                plan.files.append(ManagedPlannedFile{
                    .sourcePath = source,
                    .destinationPath = relative,
                    .sourceHash = fileHash(source),
                });
                classifyManifestFile(plan.manifest, relative);
            }
        }
    }

    plan.manifest.sources.removeDuplicates();
    plan.manifest.tests.removeDuplicates();
    plan.manifest.constraints.removeDuplicates();
    plan.manifest.includeDirs.removeDuplicates();
    plan.manifest.documentation.removeDuplicates();
    std::sort(plan.files.begin(),
              plan.files.end(),
              [](const ManagedPlannedFile &left, const ManagedPlannedFile &right) {
                  return left.destinationPath < right.destinationPath;
              });
    if (plan.files.isEmpty()) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Creation plan contains no files"),
            .path = plan.targetRoot,
        });
    }
    if (plan.manifest.sources.isEmpty()) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Managed module has no compilable source"),
            .path = plan.targetRoot,
        });
    }
    for (const Diagnostic &diagnostic :
         ManifestService().validate(plan.manifest, plan.manifestPath)) {
        plan.issues.append(ManagedAssetIssue{
            .severity = diagnostic.severity,
            .message = diagnostic.message,
            .path = diagnostic.file,
        });
    }
    if (request.top.trimmed().isEmpty()) {
        plan.issues.append(ManagedAssetIssue{
            .message = QStringLiteral("Managed module requires an explicit top unit"),
            .path = plan.manifestPath,
        });
    }
    return plan;
}

ManagedAssetExecutionResult ManagedAssetService::execute(
    const ManagedAssetPlan &plan,
    const ManagedAssetExecutionOptions &options) const
{
    ManagedAssetExecutionResult result;
    result.targetRoot = plan.targetRoot;
    if (!options.confirmed) {
        result.error = QStringLiteral("Managed asset creation requires explicit confirmation");
        return result;
    }
    if (!plan.canExecute()) {
        result.error = QStringLiteral("Managed asset creation plan contains errors");
        return result;
    }
    const QFileInfo libraryInfo(plan.libraryRoot);
    if (!libraryInfo.isDir() || isLinkLike(libraryInfo)) {
        result.error =
            QStringLiteral("Managed library root is unavailable or link-backed");
        return result;
    }
    const QList<Diagnostic> validation =
        ManifestService().validate(plan.manifest, plan.manifestPath);
    if (plan.manifest.type != AssetType::Module
        || std::any_of(validation.cbegin(),
                       validation.cend(),
                       [](const Diagnostic &diagnostic) {
                           return diagnostic.severity
                                  == Diagnostic::Severity::Error;
                       })) {
        result.error = QStringLiteral("Managed asset manifest is invalid");
        return result;
    }
    const QString expectedTarget =
        QDir(plan.libraryRoot).absoluteFilePath(plan.manifest.id);
    const QString expectedManifest =
        QDir(expectedTarget).absoluteFilePath(QStringLiteral(".xips.json"));
    if (!sameAbsolutePath(plan.targetRoot, expectedTarget)
        || !sameAbsolutePath(plan.manifestPath, expectedManifest)
        || QFileInfo(plan.targetRoot).absolutePath().compare(
               QFileInfo(plan.libraryRoot).absoluteFilePath(),
               pathCaseSensitivity())
               != 0) {
        result.error =
            QStringLiteral("Managed asset plan paths are outside the library");
        return result;
    }
    if (plan.files.isEmpty() || plan.manifest.sources.isEmpty()) {
        result.error = QStringLiteral("Managed asset plan contains no source files");
        return result;
    }

    QSet<QString> destinations;
    for (const ManagedPlannedFile &file : plan.files) {
        const QString relative = normalizedRelative(file.destinationPath);
        const QString destinationKey =
            pathCaseSensitivity() == Qt::CaseInsensitive ? relative.toLower()
                                                         : relative;
        if (!safeRelative(relative)
            || relative == QStringLiteral(".xips.json")
            || destinations.contains(destinationKey)) {
            result.error =
                QStringLiteral("Unsafe or duplicate managed destination: %1")
                    .arg(file.destinationPath);
            return result;
        }
        destinations.insert(destinationKey);
        if (!file.generated()) {
            const QFileInfo source(file.sourcePath);
            if (!source.isFile() || isLinkLike(source)) {
                result.error =
                    QStringLiteral("Managed seed source is unavailable or link-backed: %1")
                        .arg(file.sourcePath);
                return result;
            }
        }
    }
    for (const QString &source : plan.manifest.sources) {
        const QString relative = normalizedRelative(source);
        const QString sourceKey =
            pathCaseSensitivity() == Qt::CaseInsensitive ? relative.toLower()
                                                         : relative;
        if (!safeRelative(relative) || !destinations.contains(sourceKey)) {
            result.error =
                QStringLiteral("Managed manifest source is not in the file plan: %1")
                    .arg(source);
            return result;
        }
    }

    if (QFileInfo::exists(plan.targetRoot)
        || isLinkLike(QFileInfo(plan.targetRoot))) {
        result.error = QStringLiteral("Target asset directory appeared after planning");
        return result;
    }

    const QString stageName =
        QStringLiteral(".xips-create-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString stageRoot = QDir(plan.libraryRoot).absoluteFilePath(stageName);
    const QFileInfo initialStage(stageRoot);
    if (initialStage.exists() || isLinkLike(initialStage)
        || !QDir().mkpath(stageRoot)
        || !QFileInfo(stageRoot).isDir()
        || isLinkLike(QFileInfo(stageRoot))) {
        result.error = QStringLiteral("Cannot create managed-asset staging directory");
        return result;
    }
    const auto rollback = [&result, &stageRoot] {
        result.rolledBack = QDir(stageRoot).removeRecursively();
        result.filesCreated = 0;
    };

    int operations = 0;
    for (const ManagedPlannedFile &file : plan.files) {
        if (options.cancelled
            && options.cancelled->load(std::memory_order_relaxed)) {
            result.error = QStringLiteral("Managed asset creation cancelled");
            rollback();
            return result;
        }
        if (!safeRelative(file.destinationPath)) {
            result.error =
                QStringLiteral("Unsafe managed destination: %1").arg(file.destinationPath);
            rollback();
            return result;
        }
        const QString destination =
            QDir(stageRoot).absoluteFilePath(file.destinationPath);
        if (file.generated()) {
            if (bytesHash(file.generatedContents) != file.sourceHash
                || !writeBytes(destination, file.generatedContents, &result.error)) {
                rollback();
                return result;
            }
        } else {
            if (fileHash(file.sourcePath) != file.sourceHash
                || !QDir().mkpath(QFileInfo(destination).absolutePath())
                || !QFile::copy(file.sourcePath, destination)
                || fileHash(destination) != file.sourceHash) {
                result.error =
                    QStringLiteral("Seed file changed or could not be copied: %1")
                        .arg(file.sourcePath);
                rollback();
                return result;
            }
        }
        result.filesCreated += 1;
        operations += 1;
        if (options.failAfterFileOperations >= 0
            && operations >= options.failAfterFileOperations) {
            result.error = QStringLiteral("Injected managed-asset creation failure");
            rollback();
            return result;
        }
    }

    if (options.cancelled
        && options.cancelled->load(std::memory_order_relaxed)) {
        result.error = QStringLiteral("Managed asset creation cancelled");
        rollback();
        return result;
    }
    QString manifestError;
    const QString stagedManifest =
        QDir(stageRoot).absoluteFilePath(QStringLiteral(".xips.json"));
    if (!ManifestService().write(stagedManifest, plan.manifest, &manifestError)) {
        result.error = manifestError;
        rollback();
        return result;
    }
    if (options.cancelled
        && options.cancelled->load(std::memory_order_relaxed)) {
        result.error = QStringLiteral("Managed asset creation cancelled");
        rollback();
        return result;
    }
    if (QFileInfo::exists(plan.targetRoot)
        || isLinkLike(QFileInfo(plan.targetRoot))
        || !QDir(plan.libraryRoot)
                .rename(stageName, plan.manifest.id)) {
        result.error = QStringLiteral("Cannot publish managed asset directory");
        rollback();
        return result;
    }
    result.success = true;
    return result;
}

} // namespace xips
