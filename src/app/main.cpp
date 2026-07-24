#include "app/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString defaultLibraryPath()
{
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("library/primary")).toString();
    if (QFileInfo(saved).isDir()) {
        return QFileInfo(saved).absoluteFilePath();
    }

    const QString environment = qEnvironmentVariable("XIPS_LIBRARY");
    if (QFileInfo(environment).isDir()) {
        return QFileInfo(environment).absoluteFilePath();
    }

    const QString currentExample = QDir::current().absoluteFilePath(QStringLiteral("examples/library"));
    if (QFileInfo(currentExample).isDir()) {
        return currentExample;
    }

    QDir applicationDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        applicationDir.absoluteFilePath(QStringLiteral("../../examples/library")),
        applicationDir.absoluteFilePath(QStringLiteral("../../../examples/library")),
        applicationDir.absoluteFilePath(QStringLiteral("../share/xips/examples/library")),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isDir()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QDir::currentPath();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("xIPs"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local.xips"));
    QCoreApplication::setApplicationName(QStringLiteral("xIPs"));
    QCoreApplication::setApplicationVersion(QStringLiteral(XIPS_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("FPGA reusable asset library manager"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption(
        {QStringLiteral("l"), QStringLiteral("library")},
        QStringLiteral("Managed asset library root."),
        QStringLiteral("directory"));
    const QCommandLineOption externalOption(
        QStringLiteral("external"),
        QStringLiteral("Register an external library root without copying it. May be repeated."),
        QStringLiteral("directory"));
    parser.addOption(libraryOption);
    parser.addOption(externalOption);
    if (!parser.parse(application.arguments())) {
        QTextStream error(stderr);
        error << parser.errorText() << u'\n';
        error.flush();
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream output(stdout);
        output << parser.helpText();
        output.flush();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QTextStream output(stdout);
        output << QCoreApplication::applicationName() << u' '
               << QCoreApplication::applicationVersion() << u'\n';
        output.flush();
        return 0;
    }

    QSettings settings;
    const QString primary = parser.isSet(libraryOption)
                                ? QFileInfo(parser.value(libraryOption)).absoluteFilePath()
                                : defaultLibraryPath();
    QStringList external = parser.values(externalOption);
    if (external.isEmpty()) {
        external = settings.value(QStringLiteral("library/external")).toStringList();
    }
    for (QString &path : external) {
        path = QFileInfo(path).absoluteFilePath();
    }

    const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString indexPath = QDir(dataPath).absoluteFilePath(QStringLiteral("index.sqlite"));

    xips::MainWindow window(primary, external, indexPath);
    window.show();
    return application.exec();
}
