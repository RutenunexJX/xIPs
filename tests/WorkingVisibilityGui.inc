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

void GuiSmokeTest::archivedFolderRemainsBlankAfterVersionChanges()
{
    QTemporaryDir tmp; const auto library = tmp.filePath("library");
    QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "archived_project"; definition.category = "project";
    auto asset = SnapshotLibrary::create(library, definition); QVERIFY(asset.ok);
    const auto source = asset.asset.root + "/fi_232_v1_1/artifacts/top.sv";
    QVERIFY(QDir().mkpath(QFileInfo(source).absolutePath()));
    { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("archived"); }
    const auto lockPath = asset.asset.root + "/fi_232_v1_1/ip/.Xil/session.lock";
    QVERIFY(QDir().mkpath(QFileInfo(lockPath).absolutePath()));
    { QFile file(lockPath); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("machine-local lock"); }
    for (int i = 0; i < 3; ++i)
    {
        asset = SnapshotLibrary::saveCurrent(asset.asset); QVERIFY(asset.ok);
        const auto revision = asset.snapshot.id;
        asset = SnapshotLibrary::renameSnapshot(asset.asset, revision, "1.1");
        QVERIFY(asset.ok);
        if (i < 2) { asset = SnapshotLibrary::eraseSnapshot(asset.asset, revision); QVERIFY(asset.ok); }
    }
    // Match older folder imports, including retained manifests of deleted versions.
    auto document = asset.asset.document; document.remove("inactiveWorkingFiles");
    { QFile file(asset.asset.historyRoot + "/.xips.json"); QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QJsonDocument(document).toJson()); }
    BrowserPanel panel; panel.resize(800, 600); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(working->model());
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    QCOMPARE(versions->model()->rowCount(), 1);
    QCOMPARE(versions->model()->index(0, 0).data().toString(), QString("1.1"));
    QCOMPARE(model->fileCount(), 0);
    pages->setCurrentIndex(1); pages->setCurrentIndex(0);
    QCOMPARE(model->fileCount(), 0);
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->fileCount(), 0);
    const auto state = panel.saveState();
    BrowserPanel reopened; reopened.show(); reopened.restoreState(state);
    QTRY_VERIFY(!reopened.isCatalogBusy());
    auto *tree = reopened.findChild<ElaTreeView *>("workingFiles");
    QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 0);
    QTemporaryDir relocated(QCoreApplication::applicationDirPath() + "/relocated-library-XXXXXX");
    QVERIFY(relocated.isValid());
    QDirIterator iterator(library, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const auto from = iterator.next();
        if (from.endsWith(".lock")) continue; // Sync may omit machine-local lock files.
        const auto to = QDir(relocated.path()).filePath(QDir(library).relativeFilePath(from));
        QVERIFY(QDir().mkpath(QFileInfo(to).absolutePath())); QVERIFY(QFile::copy(from, to));
    }
    qInfo() << "Archived library relocation:" << QDir(library).rootPath() << library << "to" << relocated.path();
    reopened.setContext(relocated.path(), {}); QTRY_VERIFY(!reopened.isCatalogBusy());
    reopened.revealAsset(asset.asset.id); QTRY_VERIFY(!reopened.isCatalogBusy());
    QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 0);
    QCOMPARE(reopened.findChild<ElaTableView *>("revisionTable")->model()->rowCount(), 1);
    QCOMPARE(reopened.saveState().value("assetId").toString(), asset.asset.id);
    const auto movedSource = QDir(relocated.path()).filePath(QDir(library).relativeFilePath(source));
    { QFile file(movedSource); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("modified"); }
    reopened.refresh(); QTRY_VERIFY(!reopened.isCatalogBusy());
    QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 1);
    { QFile file(movedSource); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("archived"); }
    reopened.refresh(); QTRY_VERIFY(!reopened.isCatalogBusy());
    QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 0);
    { QFile file(QFileInfo(movedSource).absolutePath() + "/new.sv"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("new"); }
    reopened.refresh(); QTRY_VERIFY(!reopened.isCatalogBusy());
    QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 2);
}

void GuiSmokeTest::unreadableWorkingFilesStayHiddenUntilChecked()
{
#ifdef Q_OS_WIN
    QTemporaryDir tmp; const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "unavailable";
    auto asset = SnapshotLibrary::create(library, definition); QVERIFY(asset.ok);
    const auto source = asset.asset.root + "/top.sv";
    { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("archived"); }
    asset = SnapshotLibrary::saveCurrent(asset.asset); QVERIFY(asset.ok);
    const auto native = QDir::toNativeSeparators(source);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    const auto unlock = qScopeGuard([&] { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); });
    BrowserPanel panel; panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *tree = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(tree->model());
    QCOMPARE(model->fileCount(), 0);
    QVERIFY(panel.findChild<ElaText *>("browserNotice")->property("error").toBool());
    QVERIFY(!panel.findChild<QToolButton *>("updateButton")->isEnabled());
    CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 0);
    { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("modified"); }
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 1);
#else
    QSKIP("Uses Windows exclusive sharing to model unavailable synchronized files.");
#endif
}
