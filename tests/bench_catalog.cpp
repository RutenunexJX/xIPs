#include "app/BrowserPanel.h"
#include "library/CatalogIndex.h"
#include "library/CatalogOperations.h"
#include "library/SnapshotLibrary.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QThread>
#include <cstdio>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#endif

using namespace xips;
namespace {
void check(bool ok, const QString &reason) { if (!ok) throw std::runtime_error(reason.toStdString()); }
void put(const QString &path, const QByteArray &data)
{
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create fixture directory");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "Cannot write fixture");
}
double elapsed(const QElapsedTimer &timer) { return timer.nsecsElapsed() / 1e6; }
QString digest(const QJsonValue &value)
{
    const auto bytes = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                      : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QJsonObject seed(const QString &library, int assets, int revisions, int files)
{
    check(!QFileInfo::exists(library), "Seed library must not exist");
    check(QDir().mkpath(library), "Cannot create library");
    for (int i = 0; i < assets; ++i)
    {
        CatalogDefinition definition;
        definition.name = QStringLiteral("unit_%1").arg(i, 4, 10, QChar('0'));
        definition.category = i % 2 ? "ip" : "module";
        definition.indexes = {{"interface", {"AXI4 Lite"}}, {"tag", {"measured"}}};
        auto result = SnapshotLibrary::create(library, definition);
        check(result.ok, result.error);
        for (int f = 0; f < files; ++f)
            put(result.asset.root + QStringLiteral("/rtl/unit_%1.sv").arg(f),
                "module unit_" + QByteArray::number(f) + "; endmodule\n" + QByteArray(256, ' '));
        for (int r = 0; r < revisions; ++r)
        {
            put(result.asset.root + "/rtl/unit_0.sv", "module unit_0; localparam N=" + QByteArray::number(r) + "; endmodule\n");
            result = SnapshotLibrary::saveCurrent(result.asset, QStringLiteral("revision %1").arg(r + 1));
            check(result.ok && !result.unchanged, result.error);
        }
    }
    return {{"assets", assets}, {"revisionsPerAsset", revisions}, {"filesPerRevision", files}};
}
QJsonObject measure(const QString &library, bool gui)
{
    QElapsedTimer timer;
    timer.start();
    const auto catalog = SnapshotLibrary::scan(library);
    const double scan = elapsed(timer);
    check(!catalog.cancelled && catalog.problems.isEmpty() && !catalog.assets.isEmpty(), catalog.problems.join('\n'));
    QJsonArray assets, descriptions;
    for (const auto &a : catalog.assets) assets.append(CatalogOperations::assetJson(a));
    timer.restart();
    QList<CatalogAsset> detailed;
    for (const auto &asset : catalog.assets)
    {
        const auto described = SnapshotLibrary::describe(asset);
        check(described.ok && described.asset.problems.isEmpty(), described.error);
        detailed.append(described.asset);
    }
    const double describe = elapsed(timer);
    for (const auto &a : detailed) descriptions.append(CatalogOperations::assetJson(a));
    timer.restart();
    check(CatalogIndex::rebuild(library, catalog.assets), "Index rebuild failed");
    const double index = elapsed(timer);
    timer.restart();
    int matches = 0;
    for (int repeat = 0; repeat < 20; ++repeat)
        for (const auto &asset : catalog.assets)
            matches += CatalogIndex::matches(asset, CatalogIndex::queryTerms("interface:\"AXI4 Lite\" tag:measured"));
    const double filtering = elapsed(timer);
    QJsonObject result{{"scanMs", scan}, {"describeAllMs", describe}, {"indexRebuildMs", index},
                       {"filter20Ms", filtering}, {"filterMatches", matches}, {"assets", catalog.assets.size()},
                       {"catalogDigest", digest(assets)}, {"describeDigest", digest(descriptions)}};
    if (gui)
    {
        timer.restart();
        initializeEla();
        BrowserPanel panel;
        panel.resize(1000, 700);
        panel.show();
        panel.setContext(library, {});
        while (panel.isCatalogBusy())
        {
            QCoreApplication::processEvents();
            QThread::msleep(1);
            check(timer.elapsed() < 60000, "GUI catalog load timeout");
        }
        result.insert("guiCatalogReadyMs", elapsed(timer));
        result.insert("guiState", panel.externalState());
        QJsonArray filters;
        for (const auto &query : {"unit_00", "absent", "interface:\"AXI4 Lite\"", ""})
        {
            timer.restart();
            panel.restoreState({{"query", query}});
            filters.append(QJsonObject{{"query", query}, {"dispatchMs", elapsed(timer)}});
            QCoreApplication::processEvents();
        }
        result.insert("guiFilters", filters);
        panel.close();
    }
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        result.insert("peakWorkingSetBytes", qint64(memory.PeakWorkingSetSize));
#endif
    return result;
}
}
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName("xIPsPerformance");
    app.setApplicationName("Round01");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"library", "Isolated fixture library", "path"});
    parser.addOption({"seed", "Create fixture through product services"});
    parser.addOption({"gui", "Also measure actual BrowserPanel load"});
    parser.addOption({"assets", "Fixture asset count", "count", "24"});
    parser.addOption({"revisions", "Saved revisions per asset", "count", "12"});
    parser.addOption({"files", "Files per version", "count", "32"});
    parser.addOption({"output", "JSON result path", "path"});
    parser.process(app);
    try
    {
        const auto library = QFileInfo(parser.value("library")).absoluteFilePath();
        const auto isolation = QFileInfo(library).absolutePath();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolation + "/profile");
        app.setProperty("xips.sessionDirectory", isolation + "/sessions");
        const auto report = parser.isSet("seed")
            ? seed(library, parser.value("assets").toInt(), parser.value("revisions").toInt(), parser.value("files").toInt())
            : measure(library, parser.isSet("gui"));
        const auto bytes = QJsonDocument(report).toJson();
        if (parser.isSet("output")) put(parser.value("output"), bytes);
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        return 0;
    }
    catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
