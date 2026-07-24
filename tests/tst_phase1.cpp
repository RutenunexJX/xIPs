#include "assetindex/AssetIndex.h"
#include "assetindex/AssetScanner.h"
#include "manifest/ManifestService.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

QByteArray basicManifest(const QString &id,
                         const QString &name,
                         const QString &source = QStringLiteral("rtl/top.sv"))
{
    QJsonObject object{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("module")},
        {QStringLiteral("name"), name},
        {QStringLiteral("top"), QStringLiteral("top")},
        {QStringLiteral("sources"), QJsonArray{source}},
        {QStringLiteral("includeDirs"), QJsonArray{}},
        {QStringLiteral("defines"), QJsonArray{}},
        {QStringLiteral("constraints"), QJsonArray{}},
        {QStringLiteral("dependencies"), QJsonArray{}},
        {QStringLiteral("tests"), QJsonArray{}},
        {QStringLiteral("tags"), QJsonArray{QStringLiteral("reset")}},
    };
    return QJsonDocument(object).toJson();
}

AssetRecord makeRecord(const QString &id, const QString &name)
{
    ManifestService service;
    const ManifestLoadResult parsed = service.parse(basicManifest(id, name));
    Q_ASSERT(parsed.ok());
    AssetRecord record;
    record.manifest = *parsed.manifest;
    record.assetRoot = QStringLiteral("C:/library/") + id;
    record.manifestPath = record.assetRoot + QStringLiteral("/.xips.json");
    record.contentHash = QStringLiteral("sha256:") + id;
    record.sourceRepository = QStringLiteral("C:/library");
    return record;
}

} // namespace

class Phase1Test final : public QObject {
    Q_OBJECT

private slots:
    void manifestParsingAndMigration();
    void unknownFieldsSurviveSafeWrite();
    void relativePathsSurviveDirectoryMove();
    void duplicateIdAndMissingFileAreReported();
    void sqliteCanBeDeletedAndRebuilt();
    void lastUsedSurvivesIncrementalReindex();
    void indexedFuzzySearchReportsField();
    void oldGenerationCannotOverwriteNewIndex();
    void incrementalUpdateTouchesOnlyAffectedAsset();
    void largeIndexSearchUsesCache();
};

void Phase1Test::manifestParsingAndMigration()
{
    const QByteArray versionZero = R"JSON({
        "schemaVersion": 0,
        "id": "legacy_counter",
        "kind": "module",
        "displayName": "Legacy Counter",
        "files": ["rtl/counter.sv"],
        "dependencies": [],
        "tests": []
    })JSON";
    ManifestService service;
    const ManifestLoadResult result = service.parse(versionZero);
    QVERIFY(result.ok());
    QVERIFY(result.migrated);
    QCOMPARE(result.manifest->schemaVersion, 1);
    QCOMPARE(result.manifest->type, AssetType::Module);
    QCOMPARE(result.manifest->name, QStringLiteral("Legacy Counter"));
    QCOMPARE(result.manifest->sources, QStringList{QStringLiteral("rtl/counter.sv")});

    const ManifestLoadResult noSchema =
        service.parse(R"JSON({"id":"broken","type":"module","name":"Broken"})JSON");
    QVERIFY(!noSchema.ok());
}

void Phase1Test::unknownFieldsSurviveSafeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray source = R"JSON({
        "schemaVersion": 1,
        "id": "opaque_ip",
        "type": "ip",
        "name": "Opaque IP",
        "sources": [],
        "dependencies": [],
        "tests": [],
        "vendorExtension": {"opaque": true, "token": 42}
    })JSON";
    ManifestService service;
    const ManifestLoadResult parsed = service.parse(source);
    QVERIFY(parsed.ok());
    const QString path = temporary.filePath(QStringLiteral(".xips.json"));
    QString error;
    QVERIFY2(service.write(path, *parsed.manifest, &error), qPrintable(error));

    const ManifestLoadResult reloaded = service.load(path);
    QVERIFY(reloaded.ok());
    QCOMPARE(reloaded.manifest->rawObject.value(QStringLiteral("vendorExtension"))
                 .toObject()
                 .value(QStringLiteral("token"))
                 .toInt(),
             42);
    QVERIFY(!QFileInfo::exists(path + QStringLiteral(".tmp")));
}

void Phase1Test::relativePathsSurviveDirectoryMove()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString before = temporary.filePath(QStringLiteral("before/asset"));
    QVERIFY(writeFile(before + QStringLiteral("/.xips.json"),
                      basicManifest(QStringLiteral("movable"), QStringLiteral("Movable"))));
    QVERIFY(writeFile(before + QStringLiteral("/rtl/top.sv"),
                      QByteArrayLiteral("module top; endmodule\n")));

    AssetScanner scanner;
    ScanResult first =
        scanner.scan({LibraryRoot{.path = temporary.filePath(QStringLiteral("before")),
                                  .origin = AssetOrigin::External}});
    QCOMPARE(first.assets.size(), 1);
    const QString firstHash = first.assets.first().contentHash;

    QVERIFY(QDir(temporary.path()).rename(QStringLiteral("before"), QStringLiteral("after")));
    ScanResult moved =
        scanner.scan({LibraryRoot{.path = temporary.filePath(QStringLiteral("after")),
                                  .origin = AssetOrigin::External}});
    QCOMPARE(moved.assets.size(), 1);
    QCOMPARE(moved.assets.first().contentHash, firstHash);
    QVERIFY(moved.assets.first().assetRoot.contains(QStringLiteral("after")));
}

void Phase1Test::duplicateIdAndMissingFileAreReported()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("a/.xips.json")),
                      basicManifest(QStringLiteral("same_id"), QStringLiteral("First"))));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("b/.xips.json")),
                      basicManifest(QStringLiteral("same_id"), QStringLiteral("Second"))));

    AssetScanner scanner;
    const ScanResult result =
        scanner.scan({LibraryRoot{.path = temporary.path(), .origin = AssetOrigin::Managed}});
    QCOMPARE(result.assets.size(), 1);
    int duplicateIssues = 0;
    int missingIssues = 0;
    for (const ScanIssue &issue : result.issues) {
        duplicateIssues += issue.message.contains(QStringLiteral("Duplicate asset id")) ? 1 : 0;
        missingIssues += issue.message.contains(QStringLiteral("missing")) ? 1 : 0;
    }
    QCOMPARE(duplicateIssues, 1);
    QCOMPARE(missingIssues, 1);
}

void Phase1Test::sqliteCanBeDeletedAndRebuilt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("cache/index.sqlite"));
    const QList<AssetRecord> records{makeRecord(QStringLiteral("one"), QStringLiteral("One"))};

    {
        AssetIndex index(path);
        QString error;
        const qint64 generation = index.reserveGeneration(&error);
        QVERIFY2(generation > 0, qPrintable(error));
        QVERIFY2(index.rebuild(records, generation, &error), qPrintable(error));
        QCOMPARE(index.allAssets(&error).size(), 1);
    }

    QVERIFY(QFile::remove(path));
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));

    AssetIndex rebuilt(path);
    QString error;
    const qint64 generation = rebuilt.reserveGeneration(&error);
    QVERIFY2(generation > 0, qPrintable(error));
    QVERIFY2(rebuilt.rebuild(records, generation, &error), qPrintable(error));
    QCOMPARE(rebuilt.allAssets(&error).size(), 1);
}

void Phase1Test::lastUsedSurvivesIncrementalReindex()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    const AssetRecord record =
        makeRecord(QStringLiteral("used"), QStringLiteral("Used Asset"));
    QString error;
    const qint64 initialGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.rebuild({record}, initialGeneration, &error),
             qPrintable(error));
    const QDateTime usedAt =
        QDateTime::fromString(QStringLiteral("2026-07-24T01:02:03.456Z"),
                              Qt::ISODateWithMs);
    QVERIFY2(index.markUsed(record.manifest.id, usedAt, &error),
             qPrintable(error));

    QList<AssetRecord> indexed = index.allAssets(&error);
    QCOMPARE(indexed.size(), 1);
    QCOMPARE(indexed.first().lastUsed, usedAt);

    const qint64 nextGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.updateAssets({record}, nextGeneration, &error),
             qPrintable(error));
    indexed = index.allAssets(&error);
    QCOMPARE(indexed.size(), 1);
    QCOMPARE(indexed.first().lastUsed, usedAt);
}

void Phase1Test::indexedFuzzySearchReportsField()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    QList<AssetRecord> records{
        makeRecord(QStringLiteral("reset_gen"), QStringLiteral("Reset Generator")),
        makeRecord(QStringLiteral("packet_fifo"), QStringLiteral("Packet FIFO")),
    };
    records[0].manifest.tags = {QStringLiteral("cdc"), QStringLiteral("synchronizer")};
    records[1].manifest.tags = {QStringLiteral("buffer")};
    QString error;
    const qint64 generation = index.reserveGeneration(&error);
    QVERIFY2(index.rebuild(records, generation, &error), qPrintable(error));

    const QList<SearchHit> exact = index.search(QStringLiteral("reset generator"),
                                                AssetType::Unknown,
                                                10,
                                                &error);
    QVERIFY2(!exact.isEmpty(), qPrintable(error));
    QCOMPARE(exact.first().asset.manifest.id, QStringLiteral("reset_gen"));
    QVERIFY(exact.first().matchedFields.contains(QStringLiteral("name")));

    const QList<SearchHit> fuzzy =
        index.search(QStringLiteral("syncronizer"), AssetType::Unknown, 10, &error);
    QVERIFY(!fuzzy.isEmpty());
    QCOMPARE(fuzzy.first().asset.manifest.id, QStringLiteral("reset_gen"));
    QVERIFY(fuzzy.first().matchedFields.contains(QStringLiteral("tag")));

    const QList<SearchHit> selective =
        index.search(QStringLiteral("reset_gen"), AssetType::Unknown, 10, &error);
    QVERIFY(!selective.isEmpty());
    QCOMPARE(selective.first().asset.manifest.id, QStringLiteral("reset_gen"));
    QVERIFY(std::none_of(
        selective.cbegin(),
        selective.cend(),
        [](const SearchHit &hit) {
            return hit.asset.manifest.id == QStringLiteral("packet_fifo");
        }));
}

void Phase1Test::oldGenerationCannotOverwriteNewIndex()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    QString error;
    QVERIFY(index.initialize(&error));
    QVERIFY(index.rebuild({makeRecord(QStringLiteral("asset"), QStringLiteral("New"))}, 2, &error));
    QVERIFY(index.rebuild({makeRecord(QStringLiteral("asset"), QStringLiteral("Old"))}, 1, &error));
    const QList<AssetRecord> records = index.allAssets(&error);
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().manifest.name, QStringLiteral("New"));
    QCOMPARE(records.first().generation, 2);
}

void Phase1Test::incrementalUpdateTouchesOnlyAffectedAsset()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    AssetRecord first = makeRecord(QStringLiteral("first"), QStringLiteral("First"));
    AssetRecord second = makeRecord(QStringLiteral("second"), QStringLiteral("Second"));
    QString error;
    const qint64 initialGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.rebuild({first, second}, initialGeneration, &error),
             qPrintable(error));

    first.manifest.name = QStringLiteral("First Updated");
    first.contentHash = QStringLiteral("sha256:first-updated");
    const qint64 updateGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.updateAssets({first}, updateGeneration, &error),
             qPrintable(error));
    const QList<AssetRecord> updated = index.allAssets(&error);
    QCOMPARE(updated.size(), 2);
    const auto firstIterator = std::find_if(
        updated.cbegin(), updated.cend(), [](const AssetRecord &record) {
            return record.manifest.id == QStringLiteral("first");
        });
    const auto secondIterator = std::find_if(
        updated.cbegin(), updated.cend(), [](const AssetRecord &record) {
            return record.manifest.id == QStringLiteral("second");
        });
    QVERIFY(firstIterator != updated.cend());
    QVERIFY(secondIterator != updated.cend());
    QCOMPARE(firstIterator->manifest.name, QStringLiteral("First Updated"));
    QCOMPARE(firstIterator->generation, updateGeneration);
    QCOMPARE(secondIterator->manifest.name, QStringLiteral("Second"));
    QCOMPARE(secondIterator->generation, initialGeneration);

    second.manifest.name = QStringLiteral("Stale Update");
    QVERIFY2(index.updateAssets({second}, initialGeneration, &error),
             qPrintable(error));
    const QList<AssetRecord> afterStale = index.allAssets(&error);
    const auto unchanged = std::find_if(
        afterStale.cbegin(), afterStale.cend(), [](const AssetRecord &record) {
            return record.manifest.id == QStringLiteral("second");
        });
    QVERIFY(unchanged != afterStale.cend());
    QCOMPARE(unchanged->manifest.name, QStringLiteral("Second"));

    const qint64 removalGeneration = index.reserveGeneration(&error);
    QVERIFY2(index.removeAssets({QStringLiteral("first")}, removalGeneration, &error),
             qPrintable(error));
    const QList<AssetRecord> remaining = index.allAssets(&error);
    QCOMPARE(remaining.size(), 1);
    QCOMPARE(remaining.first().manifest.id, QStringLiteral("second"));
}

void Phase1Test::largeIndexSearchUsesCache()
{
    QTemporaryDir temporary;
    AssetIndex index(temporary.filePath(QStringLiteral("index.sqlite")));
    QList<AssetRecord> records;
    records.reserve(2000);
    for (int item = 0; item < 2000; ++item) {
        AssetRecord record = makeRecord(QStringLiteral("asset_%1").arg(item),
                                        QStringLiteral("Reusable Block %1").arg(item));
        record.manifest.tags = {QStringLiteral("group_%1").arg(item % 20)};
        records.append(record);
    }
    QString error;
    QVERIFY(index.rebuild(records, 1, &error));

    QElapsedTimer timer;
    timer.start();
    const QList<SearchHit> hits =
        index.search(QStringLiteral("Reusable Block 1734"), AssetType::Unknown, 20, &error);
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(!hits.isEmpty(), qPrintable(error));
    QCOMPARE(hits.first().asset.manifest.id, QStringLiteral("asset_1734"));
    QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("Search took %1 ms").arg(elapsed)));
}

QTEST_GUILESS_MAIN(Phase1Test)

#include "tst_phase1.moc"
