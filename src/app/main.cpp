#include "app/MainWindow.h"
#include "integration/IntegrationService.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTextStream>

#include <optional>

namespace {

QString defaultLibraryPath()
{
    QSettings settings;
    QString saved = settings.value(QStringLiteral("library/root")).toString();
    if (QFileInfo(saved).isDir()) {
        return QFileInfo(saved).absoluteFilePath();
    }
    const QString environment = qEnvironmentVariable("XIPS_LIBRARY");
    if (QFileInfo(environment).isDir()) {
        return QFileInfo(environment).absoluteFilePath();
    }
    return {};
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
        QStringLiteral("Personal FPGA reusable asset library"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption(
        {QStringLiteral("l"), QStringLiteral("library")},
        QStringLiteral("Asset library directory."),
        QStringLiteral("directory"));
    const QCommandLineOption assetOption(
        QStringLiteral("open-asset"),
        QStringLiteral("Open an asset by stable ID."),
        QStringLiteral("id"));
    const QCommandLineOption searchOption(
        QStringLiteral("search"),
        QStringLiteral("Open the library with a search query."),
        QStringLiteral("query"));
    parser.addOption(libraryOption);
    parser.addOption(assetOption);
    parser.addOption(searchOption);
    parser.addPositionalArgument(
        QStringLiteral("uri"),
        QStringLiteral("Optional xips://asset/... or xips://search?... URI."),
        QStringLiteral("[uri]"));
    if (!parser.parse(application.arguments())) {
        QTextStream(stderr) << parser.errorText() << u'\n';
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        QTextStream(stdout) << QCoreApplication::applicationName() << u' '
                            << QCoreApplication::applicationVersion() << u'\n';
        return 0;
    }

    std::optional<xips::ActivationRequest> activation;
    if (parser.isSet(assetOption) && parser.isSet(searchOption)) {
        QTextStream(stderr) << "--open-asset and --search cannot be combined\n";
        return 2;
    }
    if (parser.isSet(assetOption)) {
        activation = xips::ActivationRequest{
            .action = xips::ActivationAction::OpenAsset,
            .value = parser.value(assetOption),
        };
    } else if (parser.isSet(searchOption)) {
        activation = xips::ActivationRequest{
            .action = xips::ActivationAction::Search,
            .value = parser.value(searchOption),
        };
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1 || (!positional.isEmpty() && activation)) {
        QTextStream(stderr) << "Specify only one activation request\n";
        return 2;
    }
    if (!positional.isEmpty()) {
        activation = xips::IntegrationService::parseUri(QUrl(positional.first()));
        if (!activation) {
            QTextStream(stderr) << "Invalid xIPs URI\n";
            return 2;
        }
    }

    const QString library = parser.isSet(libraryOption)
                                ? QFileInfo(parser.value(libraryOption)).absoluteFilePath()
                                : defaultLibraryPath();
    if (!library.isEmpty() && !QDir().mkpath(library)) {
        QTextStream(stderr) << "Cannot create asset library: " << library << u'\n';
        return 3;
    }
    xips::MainWindow window(library);
    window.show();
    if (activation) {
        window.applyActivation(*activation);
    }
    return application.exec();
}
