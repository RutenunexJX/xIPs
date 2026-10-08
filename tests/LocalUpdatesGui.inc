void GuiSmokeTest::localUpdatesPreserveViewsAndReviewChanges()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
    };
    CatalogDefinition definition; definition.name = "alpha";
    auto first = SnapshotLibrary::create(library, definition); QVERIFY(first.ok);
    for (int i = 0; i < 80; ++i)
        put(first.asset.root + QString("/rtl/file-%1.sv").arg(i, 2, 10, QLatin1Char('0')), "original");
    put(first.asset.root + "/docs/nested/readme.txt", "documentation");
    first = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(first.ok);
    put(first.asset.root + "/rtl/file-00.sv", "pending change");
    definition.name = "beta";
    const auto second = SnapshotLibrary::create(library, definition); QVERIFY(second.ok);
    put(second.asset.root + "/other.sv", "other");
    BrowserPanel panel; panel.resize(800, 440); panel.show(); panel.setContext(library, {});
    // This case isolates explicit save/import updates; automatic filesystem
    // refresh has its own end-to-end coverage.
    const QSignalBlocker automaticRefresh(panel.findChild<QObject *>("catalogWatcher"));
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(first.asset.id);
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(working->model());
    auto *catalog = qobject_cast<CatalogModel *>(panel.findChild<ElaTreeView *>("assetList")->model());
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    QAbstractItemModelTester tester(catalog, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QTest::qWait(50);
    working->expand(model->pathIndex("rtl"));
    working->collapse(model->pathIndex("docs"));
    ElaTreeView::finishExpansion(working);
    const QString selected = "rtl/file-60.sv";
    working->setCurrentIndex(model->fileIndex(selected));
    working->scrollTo(model->fileIndex(selected), QAbstractItemView::PositionAtTop);
    QTest::qWait(50);
    const auto scroll = working->verticalScrollBar()->value(); QVERIFY(scroll > 0);
    const QPersistentModelIndex retained(catalog->indexForId(second.asset.id));
    QSignalSpy catalogResets(catalog, &QAbstractItemModel::modelReset);
    // New external definitions should only appear after explicit Rescan.
    definition.name = "external";
    const auto external = SnapshotLibrary::create(library, definition); QVERIFY(external.ok);
    const auto added = tmp.filePath("extra.sv"); put(added, "extra");
    panel.importWorkingFiles({added}); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(catalogResets.size(), 0); QVERIFY(retained.isValid());
    QVERIFY(!panel.catalogAsset(external.asset.id).has_value());
    QVERIFY(model->fileIndex("extra.sv").isValid());
    QVERIFY(!working->isExpanded(model->pathIndex("docs")));
    QCOMPARE(working->currentIndex().data(Qt::UserRole).toString(), selected);
    QTRY_COMPARE(working->verticalScrollBar()->value(), scroll);
    // Switching IPs and refreshing preserve the path-based selection and folding.
    panel.revealAsset(second.asset.id); panel.revealAsset(first.asset.id);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(!working->isExpanded(model->pathIndex("docs")));
    QCOMPARE(working->currentIndex().data(Qt::UserRole).toString(), selected);
    QTRY_COMPARE(working->verticalScrollBar()->value(), scroll);
    model->checkAll(false);
    model->setData(model->fileIndex(selected), Qt::Checked, Qt::CheckStateRole);
    model->setData(model->fileIndex("extra.sv"), Qt::Checked, Qt::CheckStateRole);
    put(first.asset.root + '/' + selected, "modified");
    bool reviewed = false;
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    {
        const auto closeOnFailure = qScopeGuard([&] { if (form->isVisible()) qobject_cast<QDialog *>(form)->reject(); });
        const auto preview = form->findChild<QPlainTextEdit *>("payloadPreview")->toPlainText();
        QVERIFY(preview.contains("Added\nextra.sv"));
        QVERIFY(preview.contains("Modified\n" + selected));
        QVERIFY(preview.contains("Not included in this version (working files are kept)"));
        QVERIFY(preview.contains("docs/nested/readme.txt"));
        const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) { QDir().mkpath(screenshots); form->grab().save(screenshots + "/version-changes.png"); }
        reviewed = true;
        form->findChild<ElaPushButton *>("formAccept")->click();
    });
    panel.findChild<QToolButton *>("updateButton")->click();
    QTRY_VERIFY(reviewed); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 2);
    QCOMPARE(catalogResets.size(), 0); QVERIFY(retained.isValid());
    QVERIFY(QFileInfo::exists(first.asset.root + "/docs/nested/readme.txt"));
    QVERIFY(!panel.catalogAsset(external.asset.id).has_value());
    const auto updated = *panel.catalogAsset(first.asset.id);
    const auto other = *panel.catalogAsset(second.asset.id);
    const auto matches = CatalogIndex::matchingRoots(library, CatalogIndex::generation({updated, other}), {}, {"extra.sv"});
    QVERIFY(matches.has_value()); QCOMPARE(*matches, QSet<QString>{updated.root});
    pages->setCurrentIndex(0);
    const auto state = panel.saveState();
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(panel.catalogAsset(external.asset.id).has_value());
    QVERIFY(!working->isExpanded(model->pathIndex("docs")));
    QCOMPARE(working->currentIndex().data(Qt::UserRole).toString(), selected);
    QTRY_COMPARE(working->verticalScrollBar()->value(), scroll);
    {
        BrowserPanel reopened; reopened.resize(800, 440); reopened.show(); reopened.restoreState(state);
        QTRY_VERIFY(!reopened.isCatalogBusy());
        auto *tree = reopened.findChild<ElaTreeView *>("workingFiles");
        auto *files = qobject_cast<WorkingFilesModel *>(tree->model());
        QVERIFY(!tree->isExpanded(files->pathIndex("docs")));
        QCOMPARE(tree->currentIndex().data(Qt::UserRole).toString(), selected);
        QTRY_COMPARE(tree->verticalScrollBar()->value(), scroll);
    }
    QVERIFY(QFile::remove(first.asset.root + '/' + selected));
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(!working->currentIndex().isValid());
    QVERIFY(!working->isExpanded(model->pathIndex("docs")));
}
