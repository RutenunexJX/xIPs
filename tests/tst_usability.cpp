#include "app/BrowserPanel.h"
#include "app/MainWindow.h"
#include "app/RevisionCompareDialog.h"
#include "app/WorkingFilesModel.h"
#include "library/CatalogOperations.h"
#include "library/CatalogGroups.h"
#include "library/ContentStore.h"
#include "DialogDriver.h"
#include "RevisionDriver.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTabWidget.h"
#include <QDirIterator>
#include <QFile>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <algorithm>
using namespace xips;
namespace
{
void put(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
}
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
CatalogAsset emptyAsset(const QString &root, const QString &name, const QString &type = "module")
{
    QDir().mkpath(root); CatalogDefinition definition; definition.name = name; definition.category = type;
    const auto result = SnapshotLibrary::create(root, definition); if (!result.ok) qFatal("%s", qPrintable(result.error)); return result.asset;
}
QMap<QString, QByteArray> inventory(const QString &root)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) { const auto path = files.next(); result.insert(QDir(root).relativeFilePath(path), read(path)); }
    return result;
}
}
class UsabilityTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir profile;
  private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.path());
        QStandardPaths::setTestModeEnabled(true);
        qputenv("XIPS_TEST_CACHE_ROOT", profile.filePath("cache").toUtf8());
        QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));
        initializeEla();
    }
    void namedSavesAreAtomic_data()
    {
        QTest::addColumn<bool>("working");
        QTest::newRow("working") << true; QTest::newRow("collected") << false;
    }
    void namedSavesAreAtomic()
    {
        QFETCH(bool, working); QTemporaryDir tmp; const auto library = tmp.filePath("library");
        auto asset = working ? emptyAsset(library, "named") : CatalogAsset{};
        QDir().mkpath(library);
        const auto source = working ? asset.root + "/top.sv" : tmp.filePath("input/top.sv"); put(source, "first\n");
        if (!working) { const auto initial = SnapshotLibrary::collect(library, {source}, "named", "module"); QVERIFY(initial.ok); asset = initial.asset; }
        const auto save = [&](const QString &name)
        { return working ? SnapshotLibrary::saveCurrent(asset, "note", nullptr, name)
                         : SnapshotLibrary::update(asset, {source}, "note", nullptr, name); };
        auto first = save("  v1.0  "); QVERIFY2(first.ok, qPrintable(first.error));
        QCOMPARE(first.snapshot.label, QString("v1.0")); asset = first.asset;
        const auto metadata = (working ? asset.historyRoot : asset.root) + "/.xips.json";
        const auto manifest = (working ? asset.historyRoot : asset.root) + "/.xips/revisions/" + first.snapshot.id + ".json";
        const auto original = read(manifest);
        QVERIFY(!QJsonDocument::fromJson(original).object().contains("label"));
        auto unchanged = save(""); QVERIFY(unchanged.ok && unchanged.unchanged); QCOMPARE(unchanged.snapshot.id, first.snapshot.id);
        auto named = save("v2.0"); QVERIFY2(named.ok, qPrintable(named.error)); QVERIFY(!named.unchanged);
        QVERIFY(named.snapshot.id != first.snapshot.id); QCOMPARE(named.snapshot.hash, first.snapshot.hash); asset = named.asset;
        put(source, "other\n"); const auto definition = read(metadata);
        for (const auto &name : {QString("v1.0"), QString("V2.0"), QString("bad\nname")})
        { const auto failed = save(name); QVERIFY(!failed.ok); QCOMPARE(read(metadata), definition); QCOMPARE(read(manifest), original); }
        auto next = save("v3.0"); QVERIFY2(next.ok, qPrintable(next.error)); QCOMPARE(next.snapshot.label, QString("v3.0"));
        QCOMPARE(read(manifest), original); QVERIFY(SnapshotLibrary::verifySnapshot(next.asset, first.snapshot.id).ok);
    }
    void namedPublicationFailureRestoresDefinition()
    {
        QTemporaryDir tmp; auto asset = emptyAsset(tmp.filePath("library"), "blocked"); put(asset.root + "/file.txt", "source");
        const auto metadata = asset.historyRoot + "/.xips.json"; const auto before = read(metadata);
        put(asset.historyRoot + "/.xips/revisions", "block directory");
        const auto result = SnapshotLibrary::saveCurrent(asset, {}, nullptr, "v1");
        QVERIFY(!result.ok); QCOMPARE(read(metadata), before); QCOMPARE(read(asset.root + "/file.txt"), QByteArray("source"));
        QVERIFY(QFile::remove(asset.historyRoot + "/.xips/revisions"));
        const auto scan = SnapshotLibrary::scan(asset.library); QCOMPARE(scan.assets.size(), 1);
        QVERIFY(SnapshotLibrary::heads(scan.assets.first()).isEmpty());
    }
    void comparisonsKeepHistoryAndHandleUnavailableContent()
    {
        QTemporaryDir tmp; auto asset = emptyAsset(tmp.filePath("library"), "compare");
        put(asset.root + "/rtl/top.sv", "old"); put(asset.root + "/same.txt", "same"); put(asset.root + "/old.txt", "removed");
        const auto first = SnapshotLibrary::saveCurrent(asset); QVERIFY(first.ok);
        put(asset.root + "/rtl/top.sv", "new"); QVERIFY(QFile::remove(asset.root + "/old.txt")); put(asset.root + "/new file.txt", "added");
        const auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
        const auto before = inventory(asset.library);
        const auto compared = SnapshotLibrary::compareSnapshots(second.asset, first.snapshot.id, second.snapshot.id);
        QVERIFY2(compared.ok, qPrintable(compared.error));
        QCOMPARE(compared.preview.modified, QStringList{"rtl/top.sv"}); QCOMPARE(compared.preview.added, QStringList{"new file.txt"});
        QCOMPARE(compared.preview.removed, QStringList{"old.txt"}); QCOMPARE(compared.preview.unchanged, 1);
        const auto swapped = SnapshotLibrary::compareSnapshots(second.asset, second.snapshot.id, first.snapshot.id);
        QCOMPARE(swapped.preview.added, compared.preview.removed); QCOMPARE(swapped.preview.removed, compared.preview.added);
        const auto equal = SnapshotLibrary::compareSnapshots(second.asset, first.snapshot.id, first.snapshot.id);
        QVERIFY(equal.ok); QCOMPARE(equal.preview.unchanged, 3); QVERIFY(equal.preview.modified.isEmpty());
        QVERIFY(!SnapshotLibrary::compareSnapshots(second.asset, "current", first.snapshot.id).ok);
        QVERIFY(!SnapshotLibrary::compareSnapshots(second.asset, "absent", first.snapshot.id).ok);
        QCOMPARE(inventory(asset.library), before);
        const auto object = ContentStore(asset.library).objectPath(first.snapshot.objects.value("rtl/top.sv").hash);
        const auto objectBytes = read(object);
        QVERIFY(QFile::remove(object));
        QVERIFY(SnapshotLibrary::compareSnapshots(second.asset, first.snapshot.id, second.snapshot.id).ok);
        QVERIFY(!SnapshotLibrary::prepareFile(second.asset, first.snapshot.id, "rtl/top.sv", tmp.filePath("cache")).ok);
        put(object, objectBytes);
        const auto deleted = SnapshotLibrary::eraseSnapshot(second.asset, first.snapshot.id); QVERIFY2(deleted.ok, qPrintable(deleted.error));
        QVERIFY(!SnapshotLibrary::compareSnapshots(second.asset, first.snapshot.id, second.snapshot.id).ok);
    }
    void importPreflightDoesNotWrite()
    {
        QTemporaryDir tmp; const auto asset = emptyAsset(tmp.filePath("library"), "imported");
        const auto source = tmp.filePath("incoming"); put(source + "/sub/data.txt", "incoming");
        ImportRequest request; request.sources = {source}; request.selection = SnapshotLibrary::previewImport(request.sources).preview;
        request.version = "v1"; request.expectedRoot = asset.root; request.move = true;
        auto before = inventory(tmp.path());
        const auto valid = SnapshotLibrary::previewImportTarget(asset.library, {}, asset, request); QVERIFY2(valid.ok, qPrintable(valid.error));
        QCOMPARE(inventory(tmp.path()), before);
        put(asset.root + "/incoming/sub/data.txt", "conflict"); before = inventory(tmp.path());
        QVERIFY(!SnapshotLibrary::previewImportTarget(asset.library, {}, asset, request).ok); QCOMPARE(inventory(tmp.path()), before);
        CatalogDefinition definition; definition.name = "new";
        request.expectedRoot = SnapshotLibrary::suggestedWorkingDirectory(asset.library, definition.name);
        QVERIFY(QDir().mkpath(request.expectedRoot)); before = inventory(tmp.path());
        QVERIFY(!SnapshotLibrary::previewImportTarget(asset.library, definition, {}, request).ok); QCOMPARE(inventory(tmp.path()), before);
    }
    void standaloneRestoresPerLibraryAndExplicitActivation()
    {
        QTemporaryDir tmp; const auto a = emptyAsset(tmp.filePath("a"), "alpha", "project");
        const auto other = emptyAsset(a.library, "beta"); const auto b = emptyAsset(tmp.filePath("b"), "gamma");
        {
            MainWindow window(a.library); window.show(); auto *panel = window.findChild<BrowserPanel *>();
            QTRY_VERIFY(!panel->isCatalogBusy());
            panel->restoreState({{"library", a.library}, {"assetId", a.id}, {"query", "alpha"}, {"category", "project"},
                {"hideEmptyGroups", true}, {"horizontalRatio", 0.35}, {"page", 1}});
            QTRY_VERIFY(!panel->isCatalogBusy()); QCOMPARE(panel->saveState()["assetId"].toString(), a.id);
            panel->setContext(b.library, {}); QTRY_VERIFY(!panel->isCatalogBusy());
            QCOMPARE(panel->saveState()["query"].toString(), QString()); panel->restoreState({{"assetId", b.id}, {"query", "gamma"}});
            panel->setContext(a.library, {}); QTRY_VERIFY(!panel->isCatalogBusy());
            QCOMPARE(panel->saveState()["query"].toString(), QString("alpha")); QVERIFY(panel->saveState()["hideEmptyGroups"].toBool());
            window.close();
        }
        {
            MainWindow window(a.library); window.show(); auto *panel = window.findChild<BrowserPanel *>();
            QTRY_VERIFY(!panel->isCatalogBusy()); QCOMPARE(panel->saveState()["query"].toString(), QString("alpha"));
            QCOMPARE(panel->saveState()["assetId"].toString(), a.id); QCOMPARE(panel->saveState()["page"].toInt(), 1);
            window.applyActivation({ActivationAction::OpenAsset, other.id}); QTRY_VERIFY(!panel->isCatalogBusy());
            QCOMPARE(panel->saveState()["assetId"].toString(), other.id); QCOMPARE(panel->saveState()["query"].toString(), QString());
            window.close();
        }
    }
    void visibleFiltersCountAssetsOnce()
    {
        QTemporaryDir tmp; const auto a = emptyAsset(tmp.filePath("library"), "alpha", "project"); emptyAsset(a.library, "beta");
        for (const auto &name : {"one", "two"})
        { const auto g = CatalogGroups::create(a.library, name); QVERIFY(g.ok); QVERIFY(CatalogGroups::setMember(a.library, g.group.id, a.id, true).ok); }
        BrowserPanel panel; panel.resize(720, 480); panel.show(); panel.setContext(a.library, {}); QTRY_VERIFY(!panel.isCatalogBusy());
        panel.findChild<QToolButton *>("hideEmptyGroupsButton")->setChecked(true);
        auto *type = panel.findChild<ElaComboBox *>("typeCombo"); type->setCurrentIndex(type->findData("project"));
        QCOMPARE(panel.findChild<ElaText *>("filterResultCount")->text(), QString("1 of 2 assets"));
        QVERIFY(panel.findChild<QWidget *>("filterSummary")->isVisible());
        panel.findChild<ElaLineEdit *>("assetSearch")->setText("absent");
        QTRY_COMPARE(panel.findChild<ElaText *>("filterResultCount")->text(), QString("0 of 2 assets"));
        panel.findChild<QToolButton *>("resetSearchFilters")->click();
        QVERIFY(panel.saveState()["query"].toString().isEmpty()); QVERIFY(panel.saveState()["category"].toString().isEmpty());
        QVERIFY(panel.saveState()["hideEmptyGroups"].toBool());
        panel.revealAsset(a.id); QTRY_VERIFY(!panel.isCatalogBusy());
        QVERIFY(panel.findChild<ElaText *>("assetSummary")->text().contains("Project"));
        panel.findChild<QToolButton *>("assetInfoToggle")->click();
        QVERIFY(panel.findChild<ElaText *>("assetInfo")->text().contains("one, two"));
        QVERIFY(panel.findChild<QToolButton *>("moreAssetActions")->isVisible());
    }
    void invalidNamedSaveCanBeCorrectedInPlace()
    {
        QTemporaryDir tmp; auto asset = emptyAsset(tmp.filePath("library"), "save"); put(asset.root + "/top.sv", "first");
        const auto first = SnapshotLibrary::saveCurrent(asset, {}, nullptr, "first"); QVERIFY(first.ok); put(asset.root + "/top.sv", "second");
        BrowserPanel panel; panel.resize(760, 520); panel.show(); panel.setContext(asset.library, {}); QTRY_VERIFY(!panel.isCatalogBusy());
        auto *working = panel.findChild<ElaTreeView *>("workingFiles"); qobject_cast<WorkingFilesModel *>(working->model())->checkAll(true);
        bool corrected = false;
        whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
        {
            auto *version = form->findChild<ElaLineEdit *>("revisionVersion"); version->setText("first");
            whenVisible(&panel, "versionSaveError", [&](QWidget *error)
            {
                QVERIFY(qobject_cast<ElaText *>(error)->text().contains("already"));
                auto *dialog = error->window(); auto *name = dialog->findChild<ElaLineEdit *>("revisionVersion");
                QCOMPARE(name->text(), QString("first")); name->setText("second"); corrected = true;
                dialog->findChild<ElaPushButton *>("formAccept")->click();
            });
            form->findChild<ElaPushButton *>("formAccept")->click();
        });
        panel.findChild<QToolButton *>("updateButton")->click();
        QTRY_VERIFY(corrected); QTRY_VERIFY(!panel.isCatalogBusy());
        const auto saved = SnapshotLibrary::scan(asset.library).assets.first();
        QCOMPARE(SnapshotLibrary::heads(saved).size(), 1);
        QVERIFY(std::any_of(saved.snapshots.cbegin(), saved.snapshots.cend(), [](const auto &s) { return s.label == "second"; }));
    }
    void importRetryRetainsSelectionAndMoveWaitsForArchive()
    {
        QTemporaryDir tmp; const auto asset = emptyAsset(tmp.filePath("library"), "destination");
        put(asset.root + "/first.txt", "existing");
        const auto first = SnapshotLibrary::saveCurrent(asset, {}, nullptr, "taken"); QVERIFY(first.ok);
        const auto source = tmp.filePath("incoming"); put(source + "/keep.txt", "import"); put(source + "/leave.txt", "unchecked");
        BrowserPanel panel; panel.resize(800, 600); panel.show(); panel.setContext(asset.library, {}); QTRY_VERIFY(!panel.isCatalogBusy());
        panel.setDarkTheme(true);
        bool retried = false;
        whenVisible(&panel, "folderImportForm", [&](QWidget *form)
        {
            auto *tree = form->findChild<ElaTreeView *>("importFiles");
            QVERIFY(tree->viewport()->palette().color(QPalette::Base).lightness() < 100);
            for (auto *label : form->findChildren<QLabel *>())
                if (label->text() == "Version" || label->text() == "Note")
                    QVERIFY(label->palette().color(QPalette::WindowText).lightness() > 150);
            auto *destination = form->findChild<ElaComboBox *>("importDestination"); destination->setCurrentIndex(destination->findData(asset.id));
            auto *files = qobject_cast<WorkingFilesModel *>(form->findChild<ElaTreeView *>("importFiles")->model());
            files->checkAll(false); files->setData(files->fileIndex("incoming/keep.txt"), Qt::Checked, Qt::CheckStateRole);
            form->findChild<ElaComboBox *>("importTransfer")->setCurrentIndex(1);
            form->findChild<ElaLineEdit *>("importVersion")->setText("taken");
            const auto before = inventory(tmp.path());
            whenVisible(&panel, "importError", [&, before](QWidget *error)
            {
                QCOMPARE(inventory(tmp.path()), before);
                auto *dialog = error->window();
                QCOMPARE(dialog->findChild<ElaComboBox *>("importDestination")->currentData().toString(), asset.id);
                auto *selected = qobject_cast<WorkingFilesModel *>(dialog->findChild<ElaTreeView *>("importFiles")->model());
                QCOMPARE(selected->checkedFiles(), QStringList{"incoming/keep.txt"});
                QVERIFY(dialog->findChild<ElaComboBox *>("importTransfer")->currentData().toBool());
                dialog->findChild<ElaLineEdit *>("importVersion")->setText("imported"); retried = true;
                dialog->findChild<ElaPushButton *>("formAccept")->click();
            });
            form->findChild<ElaPushButton *>("formAccept")->click();
        });
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(source)});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&panel, &enter);
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&panel, &drop);
        QVERIFY(drop.isAccepted()); QTRY_VERIFY(retried); QTRY_VERIFY(!panel.isCatalogBusy());
        QVERIFY(!QFileInfo::exists(source + "/keep.txt")); QCOMPARE(read(source + "/leave.txt"), QByteArray("unchecked"));
        const auto saved = SnapshotLibrary::scan(asset.library).assets.first();
        const auto verified = SnapshotLibrary::verifySnapshot(saved, SnapshotLibrary::heads(saved).first());
        QVERIFY(verified.ok); QCOMPARE(verified.snapshot.label, QString("imported"));
        QCOMPARE(verified.snapshot.files, QStringList{"incoming/keep.txt"});
        panel.setDarkTheme(false);
    }
    void referenceComparisonCanBeCancelledWithoutChangingPin()
    {
        QTemporaryDir tmp; const auto asset = emptyAsset(tmp.filePath("owner"), "owner"); put(asset.root + "/top.sv", "before");
        const auto first = SnapshotLibrary::saveCurrent(asset); QVERIFY(first.ok);
        put(asset.root + "/top.sv", "after"); const auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
        const auto receiver = tmp.filePath("receiver"); QVERIFY(QDir().mkpath(receiver));
        const auto reference = SnapshotLibrary::addReference(second.asset, first.snapshot.id, receiver); QVERIFY(reference.ok);
        BrowserPanel panel; panel.resize(800, 600); panel.show(); panel.setContext(receiver, {}); QTRY_VERIFY(!panel.isCatalogBusy());
        const auto before = inventory(tmp.path()); bool compared = false;
        whenVisible(&panel, "referenceOwnerForm", [](QWidget *form) { form->findChild<ElaPushButton *>("formAccept")->click(); });
        whenVisible(&panel, "referenceVersionForm", [&](QWidget *form)
        {
            auto *choices = form->findChild<ElaComboBox *>("replacementRevision"); choices->setCurrentIndex(choices->findData(second.snapshot.id));
            whenVisible(&panel, "revisionCompareDialog", [&](QWidget *window)
            {
                auto *dialog = qobject_cast<QDialog *>(window);
                QTRY_VERIFY(dialog->findChild<ElaPushButton *>("compareText")->isEnabled());
                QCOMPARE(dialog->findChild<ElaComboBox *>("compareBase")->currentData().toString(), first.snapshot.id);
                QCOMPARE(dialog->findChild<ElaComboBox *>("compareTarget")->currentData().toString(), second.snapshot.id);
                compared = true; dialog->reject();
            });
            form->findChild<ElaPushButton *>("compareReferenceButton")->click();
            qobject_cast<QDialog *>(form)->reject();
        });
        panel.findChild<QToolButton *>("changeReferenceButton")->click(); QTRY_VERIFY(compared); QTRY_VERIFY(!panel.isCatalogBusy());
        QCOMPARE(inventory(tmp.path()), before);
        QCOMPARE(SnapshotLibrary::scan(receiver).assets.first().pinnedRevision, first.snapshot.id);
    }
    void comparisonDialogShowsTextAndKeepsWorkingBlank()
    {
        QTemporaryDir tmp; auto asset = emptyAsset(tmp.filePath("library"), "versions");
        put(asset.root + "/rtl/top.sv", "same\nold\n"); put(asset.root + "/binary.bin", QByteArray("a\0b", 3));
        const auto first = SnapshotLibrary::saveCurrent(asset); QVERIFY(first.ok);
        put(asset.root + "/rtl/top.sv", "same\nnew\n"); const auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
        BrowserPanel panel; panel.resize(800, 560); panel.show(); panel.setContext(asset.library, {}); QTRY_VERIFY(!panel.isCatalogBusy());
        QCOMPARE(qobject_cast<WorkingFilesModel *>(panel.findChild<ElaTreeView *>("workingFiles")->model())->fileCount(), 0);
        const auto before = inventory(asset.library);
        RevisionCompareDialog dialog(&panel, second.asset, second.snapshot.id); dialog.show();
        auto *files = dialog.findChild<ElaTableView *>("comparisonFiles"); QTRY_COMPARE(files->model()->rowCount(), 2);
        for (int row = 0; row < files->model()->rowCount(); ++row)
            if (files->model()->index(row, 0).data(Qt::UserRole).toString() == "rtl/top.sv") files->selectRow(row);
        dialog.findChild<ElaPushButton *>("compareText")->click(); auto *text = dialog.findChild<QPlainTextEdit *>("comparisonText");
        QTRY_VERIFY(text->isVisible()); QVERIFY(text->toPlainText().contains("- old")); QVERIFY(text->toPlainText().contains("+ new"));
        dialog.findChild<ElaPushButton *>("compareSwap")->click(); QTRY_VERIFY(dialog.findChild<ElaPushButton *>("compareText")->isEnabled());
        QCOMPARE(dialog.findChild<ElaComboBox *>("compareBase")->currentData().toString(), second.snapshot.id);
        for (int row = 0; row < files->model()->rowCount(); ++row)
            if (files->model()->index(row, 0).data(Qt::UserRole).toString() == "binary.bin") files->selectRow(row);
        dialog.findChild<ElaPushButton *>("compareText")->click(); QTRY_VERIFY(text->isVisible());
        QVERIFY(text->toPlainText().contains("Binary")); dialog.reject(); QCOMPARE(inventory(asset.library), before);
        QCOMPARE(qobject_cast<WorkingFilesModel *>(panel.findChild<ElaTreeView *>("workingFiles")->model())->fileCount(), 0);
    }
};
QTEST_MAIN(UsabilityTest)
#include "tst_usability.moc"
