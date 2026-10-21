#include "app/Branding.h"
#include "app/BrowserPanel.h"
#ifdef XIPS_HAS_SUITEAPP
#include "integration/SuiteIntegration.h"
#include <QTimer>
#endif
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

namespace
{

QString defaultLibraryPath()
{
    const QString environment = qEnvironmentVariable("XIPS_LIBRARY");
    if (!environment.isEmpty())
        return QFileInfo(environment).absoluteFilePath();
    QSettings settings;
    const QString saved = settings.value(QStringLiteral("library/root")).toString().trimmed();
    if (!saved.isEmpty())
    {
        return QFileInfo(saved).absoluteFilePath();
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
    parser.setApplicationDescription(QStringLiteral("Personal FPGA reusable asset library"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption({QStringLiteral("l"), QStringLiteral("library")},
                                           QStringLiteral("Asset library directory."),
                                           QStringLiteral("directory"));
    const QCommandLineOption assetOption(QStringLiteral("open-asset"),
                                         QStringLiteral("Open an asset by stable ID."),
                                         QStringLiteral("id"));
    const QCommandLineOption searchOption(QStringLiteral("search"),
                                          QStringLiteral("Open the library with a search query."),
                                          QStringLiteral("query"));
    parser.addOption(libraryOption);
    parser.addOption(assetOption);
    parser.addOption(searchOption);
    parser.addOption({"no-runtime", "Do not register this instance with the global AppSuite broker."});
    parser.addOption({"profile", "Use isolated INI settings in this directory.", "directory"});
    parser.addOption({"session-dir", "Directory for this instance's session registration.", "directory"});
    parser.addPositionalArgument(
        QStringLiteral("uri"),
        QStringLiteral("Optional xips://asset/... or xips://search?... URI."),
        QStringLiteral("[uri]"));
    if (!parser.parse(application.arguments()))
    {
        QTextStream(stderr) << parser.errorText() << u'\n';
        return 2;
    }
    if (parser.isSet(QStringLiteral("help")))
    {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(QStringLiteral("version")))
    {
        QTextStream(stdout) << QCoreApplication::applicationName() << u' '
                            << QCoreApplication::applicationVersion() << u'\n';
        return 0;
    }

    if (parser.isSet("profile")) {
        const auto profile = QFileInfo(parser.value("profile")).absoluteFilePath();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, profile);
    }
    if (parser.isSet("session-dir")) application.setProperty("xips.sessionDirectory", QFileInfo(parser.value("session-dir")).absoluteFilePath());

    std::optional<xips::ActivationRequest> activation;
    if (parser.isSet(assetOption) && parser.isSet(searchOption))
    {
        QTextStream(stderr) << "--open-asset and --search cannot be combined\n";
        return 2;
    }
    if (parser.isSet(assetOption))
    {
        activation = xips::ActivationRequest{
            .action = xips::ActivationAction::OpenAsset,
            .value = parser.value(assetOption),
        };
    }
    else if (parser.isSet(searchOption))
    {
        activation = xips::ActivationRequest{
            .action = xips::ActivationAction::Search,
            .value = parser.value(searchOption),
        };
    }
    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1 || (!positional.isEmpty() && activation))
    {
        QTextStream(stderr) << "Specify only one activation request\n";
        return 2;
    }
    if (!positional.isEmpty())
    {
        activation = xips::IntegrationService::parseUri(QUrl(positional.first()));
        if (!activation)
        {
            QTextStream(stderr) << "Invalid xIPs URI\n";
            return 2;
        }
    }

    const QString library = parser.isSet(libraryOption)
                                ? QFileInfo(parser.value(libraryOption)).absoluteFilePath()
                                : defaultLibraryPath();
    QSettings().setValue(
        QStringLiteral("runtime/browserLibrary"),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("xips-browser.dll")));
    xips::initializeEla();
    application.setWindowIcon(xips::applicationIcon());
    xips::MainWindow window(library);
    window.show();
    if (activation)
    {
        window.applyActivation(*activation);
    }
#ifdef XIPS_HAS_SUITEAPP
    if (!parser.isSet("no-runtime")) {
        auto *suite = new xips::SuiteIntegration(window.findChild<xips::BrowserPanel *>(), &window);
        QTimer::singleShot(0, suite, [suite] { suite->start(); });
    }
#endif
    return application.exec();
}
