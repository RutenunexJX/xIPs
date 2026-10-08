void GuiSmokeTest::workingFilesOnlyShowUnarchivedChanges()
{
    QTemporaryDir tmp; const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
    };
    CatalogDefinition definition; definition.name = "editing";
    auto asset = SnapshotLibrary::create(library, definition); QVERIFY(asset.ok);
    const auto source = asset.asset.root + "/rtl/sub/top.sv";
    put(source, "original"); put(asset.asset.root + "/notes.txt", "notes");
    asset = SnapshotLibrary::saveCurrent(asset.asset); QVERIFY(asset.ok);
    BrowserPanel panel; panel.resize(800, 600); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *tree = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(tree->model());
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    QCOMPARE(model->fileCount(), 0); QCOMPARE(versions->model()->rowCount(), 1);
    QVERIFY(!panel.findChild<ElaText *>("workingEmpty")->isVisible());
    const auto cleanState = panel.saveState();
    const auto modified = QFileInfo(source).lastModified();
    put(source, "modified");
    { QFile file(source); QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime)); }
    QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 2, 10000);
    QVERIFY(!tree->isExpanded(model->pathIndex("rtl")));
    QVERIFY(!tree->isExpanded(model->pathIndex("rtl/sub")));
    tree->expand(model->pathIndex("rtl")); tree->expand(model->pathIndex("rtl/sub"));
    ElaTreeView::finishExpansion(tree);
    const auto editingState = panel.saveState();
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(tree->isExpanded(model->pathIndex("rtl")));
    QVERIFY(tree->isExpanded(model->pathIndex("rtl/sub")));
    {
        BrowserPanel reopened; reopened.show(); reopened.restoreState(editingState);
        QTRY_VERIFY(!reopened.isCatalogBusy());
        auto *view = reopened.findChild<ElaTreeView *>("workingFiles");
        auto *files = qobject_cast<WorkingFilesModel *>(view->model());
        QCOMPARE(files->fileCount(), 2);
        QVERIFY(view->isExpanded(files->pathIndex("rtl/sub")));
    }
    put(source, "original");
    QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 0, 10000);
    put(source, "changed again");
    QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 2, 10000);
    QVERIFY(tree->isExpanded(model->pathIndex("rtl/sub")));
    model->checkAll(true);
    whenVisible(&panel, "payloadReviewForm", [](QWidget *form)
    { form->findChild<ElaPushButton *>("formAccept")->click(); });
    panel.findChild<QToolButton *>("updateButton")->click(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->fileCount(), 0); QCOMPARE(versions->model()->rowCount(), 2);
    QVERIFY(QFileInfo::exists(source));
    panel.restoreState(cleanState); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->fileCount(), 0);
    const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!shots.isEmpty()) { QDir().mkpath(shots); QVERIFY(panel.grab().save(shots + "/working-clean.png")); }
    put(asset.asset.root + "/new/fresh.sv", "new");
    QTRY_COMPARE_WITH_TIMEOUT(model->fileCount(), 3, 10000);
    QVERIFY(!tree->isExpanded(model->pathIndex("new")));
    QVERIFY(!tree->isExpanded(model->pathIndex("rtl")));
    if (!shots.isEmpty()) QVERIFY(panel.grab().save(shots + "/working-collapsed.png"));
}
