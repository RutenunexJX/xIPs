#include "library/AssetLibraryService.h"
#include "library/SnapshotLibrary.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace xips;
namespace
{
void put(const QString &path, const QByteArray &value)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size())
        qFatal("fixture write failed");
}
QByteArray get(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
} // namespace
class SnapshotTest : public QObject
{
    Q_OBJECT
  private slots:
    void linearVersionsAndPinnedCopies();
    void artifactsKeepBuildOutputs();
    void rejectsCorruptionAndConflicts();
    void deletionNeverReusesVersionNumbers();
    void legacyMigrationPreservesHistory();
};
void SnapshotTest::linearVersionsAndPinnedCopies()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source/uart.sv");
    QDir().mkpath(lib);
    put(src, "module uart; endmodule\n");
    auto first = SnapshotLibrary::collect(lib, {src}, "UART", "module");
    QVERIFY2(first.ok, qPrintable(first.error));
    QCOMPARE(first.snapshot.id, QString("1"));
    QVERIFY(!QFileInfo::exists(first.asset.root + "/uart.sv"));
    const auto same = SnapshotLibrary::update(first.asset, {src});
    QVERIFY2(same.ok, qPrintable(same.error));
    QVERIFY(same.unchanged);
    const auto metadata = SnapshotLibrary::edit(first.asset, "UART RX", "module", "receive data");
    QVERIFY2(metadata.ok, qPrintable(metadata.error));
    QCOMPARE(metadata.asset.snapshots.size(), 1);
    const auto exported =
        SnapshotLibrary::exportSnapshot(metadata.asset, "1", tmp.filePath("project.sv"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    put(src, "module uart; wire valid; endmodule\n");
    auto second = SnapshotLibrary::update(metadata.asset, {src}, "board verified");
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(second.snapshot.id, QString("2"));
    QCOMPARE(second.asset.snapshots.size(), 2);
    QCOMPARE(get(tmp.filePath("project.sv")), QByteArray("module uart; endmodule\n"));
    QVERIFY(SnapshotLibrary::exportSnapshot(second.asset, "1", tmp.filePath("old.sv")).ok);
    QCOMPARE(get(tmp.filePath("old.sv")), get(tmp.filePath("project.sv")));
    QVERIFY(!SnapshotLibrary::exportSnapshot(second.asset, "2", tmp.filePath("project.sv")).ok);
    QCOMPARE(SnapshotLibrary::scan(lib).assets.size(), 1);
}
void SnapshotTest::artifactsKeepBuildOutputs()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source");
    QDir().mkpath(lib);
    put(src + "/rtl/top.sv", "module top;endmodule");
    put(src + "/build/top.bit", "bitstream");
    put(src + "/ip_user_files/core.dcp", "checkpoint");
    put(src + "/.git/config", "excluded");
    auto module = SnapshotLibrary::collect(lib, {src}, "module", "module");
    QVERIFY2(module.ok, qPrintable(module.error));
    QCOMPARE(module.snapshot.files, QStringList{"rtl/top.sv"});
    auto artifact = SnapshotLibrary::collect(lib, {src}, "output", "artifact");
    QVERIFY2(artifact.ok, qPrintable(artifact.error));
    QCOMPARE(artifact.snapshot.files.size(), 3);
    QVERIFY(artifact.snapshot.files.contains("build/top.bit"));
    QVERIFY(artifact.snapshot.files.contains("ip_user_files/core.dcp"));
    auto exported = SnapshotLibrary::exportSnapshot(artifact.asset, "1", tmp.filePath("export"));
    QVERIFY2(exported.ok, qPrintable(exported.error));
    QCOMPARE(get(tmp.filePath("export/build/top.bit")), QByteArray("bitstream"));
}
void SnapshotTest::rejectsCorruptionAndConflicts()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "original");
    auto first = SnapshotLibrary::collect(lib, {src}, "example", "module");
    QVERIFY(first.ok);
    QVERIFY(!SnapshotLibrary::exportSnapshot(first.asset, "1", lib + "/inside.sv").ok);
    QLockFile lock(lib + "/.xips-library.lock");
    QVERIFY(lock.tryLock());
    QVERIFY(!SnapshotLibrary::update(first.asset, {src}).ok);
    lock.unlock();
    auto edit = SnapshotLibrary::edit(first.asset, "new name", "module", {});
    QVERIFY(edit.ok);
    QVERIFY(!SnapshotLibrary::edit(first.asset, "stale", "module", {}).ok);
    put(first.asset.root + "/.xips/revisions/1/source.sv", "corrupt");
    auto exportResult = SnapshotLibrary::exportSnapshot(edit.asset, "1", tmp.filePath("bad.sv"));
    QVERIFY(!exportResult.ok);
    QVERIFY(!QFileInfo::exists(tmp.filePath("bad.sv")));
    QVERIFY(!SnapshotLibrary::eraseSnapshot(edit.asset, "1", true).ok);
}
void SnapshotTest::deletionNeverReusesVersionNumbers()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "1");
    auto asset = SnapshotLibrary::collect(lib, {src}, "example", "module");
    QVERIFY(asset.ok);
    put(src, "2");
    asset = SnapshotLibrary::update(asset.asset, {src});
    QVERIFY(asset.ok);
    auto removed = SnapshotLibrary::eraseSnapshot(asset.asset, "2", true);
    QVERIFY2(removed.ok, qPrintable(removed.error));
    put(src, "3");
    auto third = SnapshotLibrary::update(removed.asset, {src});
    QVERIFY2(third.ok, qPrintable(third.error));
    QCOMPARE(third.snapshot.id, QString("3"));
    QVERIFY(SnapshotLibrary::eraseAsset(third.asset, true).ok);
    QVERIFY(QFileInfo::exists(src));
    QVERIFY(SnapshotLibrary::scan(lib).assets.isEmpty());
}
void SnapshotTest::legacyMigrationPreservesHistory()
{
    QTemporaryDir tmp;
    const QString lib = tmp.filePath("library"), src = tmp.filePath("source.sv");
    QDir().mkpath(lib);
    put(src, "old");
    AssetRecord old;
    QString error;
    AssetLibraryService service;
    QVERIFY2(
        service.importAsset({lib, src, AssetLibraryService::suggestedMetadata(src)}, &old, &error),
        qPrintable(error));
    VersionInfo saved;
    QVERIFY2(service.createVersion(old, "1.0.0", &saved, &error), qPrintable(error));
    put(old.assetRoot + "/source.sv", "current");
    const auto before = SnapshotLibrary::scan(lib);
    QCOMPARE(before.assets.size(), 1);
    QVERIFY(before.assets.first().legacy);
    auto migrated = SnapshotLibrary::migrate(before.assets.first());
    QVERIFY2(migrated.ok, qPrintable(migrated.error));
    QVERIFY(!migrated.asset.legacy);
    QCOMPARE(migrated.asset.id, old.manifest.id);
    QCOMPARE(migrated.asset.snapshots.size(), 2);
    QVERIFY(QFileInfo::exists(migrated.retainedPath + "/source.sv"));
    QVERIFY(SnapshotLibrary::exportSnapshot(migrated.asset, "1", tmp.filePath("old.sv")).ok);
    QCOMPARE(get(tmp.filePath("old.sv")), QByteArray("old"));
    QVERIFY(SnapshotLibrary::exportSnapshot(migrated.asset, "2", tmp.filePath("current.sv")).ok);
    QCOMPARE(get(tmp.filePath("current.sv")), QByteArray("current"));
}
QTEST_GUILESS_MAIN(SnapshotTest)
#include "tst_snapshots.moc"
