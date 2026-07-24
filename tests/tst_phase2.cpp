#include "assetindex/AssetIndex.h"
#include "manifest/ManifestService.h"
#include "semantic/SlangService.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace xips;

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

AssetRecord semanticAsset(const QString &root,
                          const QString &hash = QStringLiteral("sha256:current"))
{
    Manifest manifest;
    manifest.id = QStringLiteral("semantic_asset");
    manifest.type = AssetType::Module;
    manifest.name = QStringLiteral("Semantic Asset");
    manifest.top = QStringLiteral("top");
    manifest.sources = {QStringLiteral("rtl/top.sv")};
    manifest.includeDirs = {QStringLiteral("rtl/include")};
    manifest.defines = {QStringLiteral("FROM_MANIFEST=1")};
    manifest.rawObject = QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), manifest.id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), manifest.name},
        {QStringLiteral("top"), manifest.top},
        {QStringLiteral("sources"), QJsonArray{QStringLiteral("rtl/top.sv")}},
        {QStringLiteral("includeDirs"), QJsonArray{QStringLiteral("rtl/include")}},
        {QStringLiteral("defines"), QJsonArray{QStringLiteral("FROM_MANIFEST=1")}},
        {QStringLiteral("constraints"), QJsonArray{}},
        {QStringLiteral("dependencies"), QJsonArray{}},
        {QStringLiteral("tests"), QJsonArray{}},
        {QStringLiteral("tags"), QJsonArray{}},
    };

    AssetRecord record;
    record.manifest = manifest;
    record.assetRoot = root;
    record.manifestPath = QDir(root).absoluteFilePath(QStringLiteral(".xips.json"));
    record.contentHash = hash;
    record.sourceRepository = root;
    return record;
}

const SemanticUnit *findUnit(const SemanticResult &result, const QString &name)
{
    for (const SemanticUnit &unit : result.units) {
        if (unit.name == name) {
            return &unit;
        }
    }
    return nullptr;
}

SemanticResult currentSemantic(const QString &hash, const qint64 generation, const QString &portName)
{
    SemanticResult result;
    result.available = true;
    result.success = true;
    result.engineVersion = QStringLiteral("slang test");
    result.contentHash = hash;
    result.generation = generation;
    SemanticUnit unit;
    unit.name = QStringLiteral("top");
    unit.kind = QStringLiteral("module");
    unit.ports.append(SemanticPort{
        .name = portName,
        .direction = QStringLiteral("input"),
        .type = QStringLiteral("logic"),
    });
    result.units.append(unit);
    result.topCandidates = {QStringLiteral("top")};
    return result;
}

} // namespace

class Phase2Test final : public QObject {
    Q_OBJECT

private slots:
    void parsesSlangUnitsPortsParametersAndDependencies();
    void invokesSlangProcessWithoutReadingSourceText();
    void missingSlangProducesExplicitUnavailableState();
    void semanticPublicationChecksHashAndGeneration();
};

void Phase2Test::parsesSlangUnitsPortsParametersAndDependencies()
{
    QFile astFile(QCoreApplication::applicationDirPath() + QStringLiteral("/fixture_ast.json"));
    Q_UNUSED(astFile);

    const QByteArray ast = R"JSON({
      "name":"$root","kind":"Root","members":[
        {"name":"pkg","kind":"Package","source_file":"pkg.sv","members":[]},
        {"name":"leaf","kind":"Definition","definitionKind":"Module","source_file":"leaf.sv"},
        {"name":"bus_if","kind":"Definition","definitionKind":"Interface","source_file":"bus_if.sv"},
        {"name":"top","kind":"Definition","definitionKind":"Module","source_file":"top.sv"},
        {"name":"top","kind":"Instance","body":{"name":"top","kind":"InstanceBody","members":[
          {"name":"WIDTH","kind":"Parameter","type":"int unsigned","value":"16","isLocal":false},
          {"name":"data_i","kind":"Port","direction":"In","type":{
            "kind":"PackedArrayType","elementType":{"name":"logic"},"range":{"left":15,"right":0}}},
          {"name":"pkg","kind":"WildcardImport","packageName":"pkg"},
          {"name":"u_leaf","kind":"Instance","body":{"name":"leaf","kind":"InstanceBody","members":[]}},
          {"name":"bus","kind":"Instance","body":{"name":"bus_if","kind":"InstanceBody","members":[]}}
        ]}}
      ]
    })JSON";
    const QByteArray cst = R"JSON({"kind":"CompilationUnit","members":[
      {"kind":"IncludeDirective","path":"defs.svh"},
      {"kind":"DefineDirective","name":"WIDTH_MACRO"}
    ]})JSON";
    const QByteArray diagnostics = R"JSON([{
      "severity":"warning","code":"Wfixture","message":"fixture",
      "location":{"fileName":"top.sv","line":3,"column":2}
    }])JSON";

    const SemanticResult result = SlangService::parseArtifacts(
        ast,
        cst,
        diagnostics,
        {QStringLiteral("rtl/include/defs.svh")},
        {QStringLiteral("COMMAND_DEFINE=1")},
        QStringLiteral("slang 11.0"),
        7,
        QStringLiteral("sha256:test"));
    QVERIFY(result.available);
    QVERIFY(result.success);
    QCOMPARE(result.generation, 7);
    QCOMPARE(result.contentHash, QStringLiteral("sha256:test"));
    QVERIFY(findUnit(result, QStringLiteral("pkg")));
    QCOMPARE(findUnit(result, QStringLiteral("pkg"))->kind, QStringLiteral("package"));
    QVERIFY(findUnit(result, QStringLiteral("bus_if")));
    QCOMPARE(findUnit(result, QStringLiteral("bus_if"))->kind, QStringLiteral("interface"));

    const SemanticUnit *top = findUnit(result, QStringLiteral("top"));
    QVERIFY(top);
    QCOMPARE(top->kind, QStringLiteral("module"));
    QCOMPARE(top->parameters.size(), 1);
    QCOMPARE(top->parameters.first().name, QStringLiteral("WIDTH"));
    QCOMPARE(top->ports.size(), 1);
    QCOMPARE(top->ports.first().name, QStringLiteral("data_i"));
    QCOMPARE(top->ports.first().packedDimensions, QStringLiteral("[15:0]"));
    QVERIFY(top->imports.contains(QStringLiteral("pkg::*")));
    QVERIFY(top->instances.contains(QStringLiteral("leaf")));
    QVERIFY(top->instances.contains(QStringLiteral("bus_if")));
    QCOMPARE(result.topCandidates, QStringList{QStringLiteral("top")});
    QVERIFY(result.includes.contains(QStringLiteral("defs.svh")));
    QVERIFY(result.includes.contains(QStringLiteral("rtl/include/defs.svh")));
    QVERIFY(result.defines.contains(QStringLiteral("WIDTH_MACRO")));
    QVERIFY(result.defines.contains(QStringLiteral("COMMAND_DEFINE=1")));
    QCOMPARE(result.diagnostics.size(), 1);
    QCOMPARE(result.diagnostics.first().severity, Diagnostic::Severity::Warning);
}

void Phase2Test::invokesSlangProcessWithoutReadingSourceText()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("rtl/top.sv")),
                      QByteArrayLiteral("this text is deliberately not valid SystemVerilog")));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("rtl/include/defs.svh")),
                      QByteArrayLiteral("not parsed by xIPs")));

    AssetRecord asset = semanticAsset(temporary.path());
    const QString fakeExecutable =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            QStringLiteral("fake_slang.exe"));
    QVERIFY2(QFileInfo::exists(fakeExecutable), qPrintable(fakeExecutable));

    SlangService service;
    const SemanticResult result = service.analyze(SlangService::Request{
        .asset = asset,
        .executable = fakeExecutable,
        .generation = 11,
        .timeoutMs = 10000,
    });
    QVERIFY(result.available);
    QVERIFY(result.success);
    QVERIFY(result.engineVersion.contains(QStringLiteral("11.0-test")));
    QCOMPARE(result.generation, 11);
    QCOMPARE(result.contentHash, asset.contentHash);
    QVERIFY(findUnit(result, QStringLiteral("common_pkg")));
    QVERIFY(findUnit(result, QStringLiteral("bus_if")));
    QVERIFY(result.includes.contains(QStringLiteral("rtl/include/defs.svh")));
    QVERIFY(result.defines.contains(QStringLiteral("LOCAL_WIDTH")));
    QVERIFY(result.defines.contains(QStringLiteral("FROM_MANIFEST=1")));
    QCOMPARE(result.topCandidates, QStringList{QStringLiteral("top")});
}

void Phase2Test::missingSlangProducesExplicitUnavailableState()
{
    QTemporaryDir temporary;
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("rtl/top.sv")),
                      QByteArrayLiteral("module hidden; endmodule")));
    const QByteArray oldPath = qgetenv("PATH");
    const QByteArray oldSlang = qgetenv("XIPS_SLANG");
    qputenv("PATH", temporary.path().toUtf8());
    qunsetenv("XIPS_SLANG");

    SlangService service;
    const SemanticResult result = service.analyze(SlangService::Request{
        .asset = semanticAsset(temporary.path()),
        .executable = temporary.filePath(QStringLiteral("does-not-exist.exe")),
        .generation = 1,
    });
    qputenv("PATH", oldPath);
    if (oldSlang.isNull()) {
        qunsetenv("XIPS_SLANG");
    } else {
        qputenv("XIPS_SLANG", oldSlang);
    }

    QVERIFY(!result.available);
    QVERIFY(!result.success);
    QVERIFY(result.units.isEmpty());
    QCOMPARE(result.diagnostics.size(), 1);
    QCOMPARE(result.diagnostics.first().code,
             QStringLiteral("semantic.slang.unavailable"));
}

void Phase2Test::semanticPublicationChecksHashAndGeneration()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    AssetRecord record = semanticAsset(QStringLiteral("C:/library"));
    QString error;
    QVERIFY2(index.rebuild({record}, 1, &error), qPrintable(error));

    SemanticResult first = currentSemantic(record.contentHash,
                                           1,
                                           QStringLiteral("indexed_old_port_xyz"));
    QCOMPARE(index.publishSemantic(record.manifest.id,
                                   record.contentHash,
                                   1,
                                   first,
                                   &error),
             SemanticPublishStatus::Published);
    QVERIFY(!index.search(QStringLiteral("indexed_old_port_xyz"),
                          AssetType::Unknown,
                          10,
                          &error)
                 .isEmpty());

    QVERIFY(index.rebuild({record}, 2, &error));
    QList<AssetRecord> records = index.allAssets(&error);
    QCOMPARE(records.size(), 1);
    QVERIFY(!records.first().stale);
    QCOMPARE(records.first().semantic.units.first().ports.first().name,
             QStringLiteral("indexed_old_port_xyz"));
    QCOMPARE(index.publishSemantic(record.manifest.id,
                                   record.contentHash,
                                   1,
                                   first,
                                   &error),
             SemanticPublishStatus::Stale);

    AssetRecord changed = record;
    changed.contentHash = QStringLiteral("sha256:changed");
    QVERIFY(index.rebuild({changed}, 3, &error));
    records = index.allAssets(&error);
    QVERIFY(records.first().stale);
    QVERIFY(index.search(QStringLiteral("indexed_old_port_xyz"),
                         AssetType::Unknown,
                         10,
                         &error)
                .isEmpty());
    QCOMPARE(index.publishSemantic(changed.manifest.id,
                                   record.contentHash,
                                   3,
                                   first,
                                   &error),
             SemanticPublishStatus::Stale);

    const SemanticResult current =
        currentSemantic(changed.contentHash, 3, QStringLiteral("indexed_new_port_xyz"));
    QCOMPARE(index.publishSemantic(changed.manifest.id,
                                   changed.contentHash,
                                   3,
                                   current,
                                   &error),
             SemanticPublishStatus::Published);
    QVERIFY(!index.search(QStringLiteral("indexed_new_port_xyz"),
                          AssetType::Unknown,
                          10,
                          &error)
                 .isEmpty());
}

QTEST_GUILESS_MAIN(Phase2Test)

#include "tst_phase2.moc"
