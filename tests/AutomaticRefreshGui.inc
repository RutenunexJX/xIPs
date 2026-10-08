void GuiSmokeTest::externalChangesRefreshAfterSettling()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
    };
    const auto replace = [](const QString &path, const QByteArray &bytes)
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            qFatal("Fixture replacement failed: %s: %s", qPrintable(path), qPrintable(file.errorString()));
    };
    CatalogDefinition definition; definition.name = "watched";
    auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
    put(created.asset.root + "/rtl/top.sv", "initial");
    QVERIFY(QDir().mkpath(created.asset.root + "/build"));
    QVERIFY(QDir().mkpath(created.asset.root + "/.settings"));
    created = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(created.ok);
    BrowserPanel panel; panel.resize(800, 440); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *tree = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(tree->model());
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *monitor = panel.findChild<CatalogWatcher *>(); QVERIFY(monitor);
    QTest::qWait(1100); QTRY_VERIFY(!panel.isCatalogBusy());
    QSignalSpy changes(monitor, &CatalogWatcher::refreshRequested);
    QSignalSpy resets(panel.findChild<ElaTreeView *>("assetList")->model(), &QAbstractItemModel::modelReset);
    QCOMPARE(model->fileCount(), 0);
    put(created.asset.root + "/rtl/top.sv", "editing");
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex("rtl/top.sv").isValid(), 10000);
    tree->expand(model->pathIndex("rtl"));
    tree->setCurrentIndex(model->fileIndex("rtl/top.sv")); model->checkAll(true);
    for (int i = 0; i < 4; ++i) put(created.asset.root + QString("/rtl/nested/new%1.sv").arg(i), "new");
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex("rtl/nested/new3.sv").isValid(), 10000);
    QCOMPARE(resets.count(), 0);
    QVERIFY(!changes.isEmpty());
    for (const auto &event : changes) QVERIFY(!event[0].toBool());
    QCOMPARE(tree->currentIndex().data(Qt::UserRole).toString(), QString("rtl/top.sv"));
    QCOMPARE(model->checkedFiles(), QStringList{"rtl/top.sv"});
    // Managed working areas include hidden/build folders, including files added
    // externally to directories that were empty when monitoring started.
    put(created.asset.root + "/build/generated.sv", "generated");
    put(created.asset.root + "/.settings/ip.tcl", "settings");
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex("build/generated.sv").isValid(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex(".settings/ip.tcl").isValid(), 10000);
    QCOMPARE(resets.count(), 0);
    // In-place writes and atomic editor replacement both remain subscribed.
    changes.clear(); put(created.asset.root + "/rtl/top.sv", "edited in place");
    QTRY_VERIFY_WITH_TIMEOUT(!changes.isEmpty(), 10000); QTRY_VERIFY(!panel.isCatalogBusy());
    changes.clear(); replace(created.asset.root + "/rtl/top.sv", "atomic replacement");
    QTRY_VERIFY_WITH_TIMEOUT(!changes.isEmpty(), 10000); QTRY_VERIFY(!panel.isCatalogBusy());
    changes.clear(); put(created.asset.root + "/rtl/top.sv", "after replacement");
    QTRY_VERIFY_WITH_TIMEOUT(!changes.isEmpty(), 10000); QTRY_VERIFY(!panel.isCatalogBusy());
    const auto next = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(next.ok);
    QTRY_COMPARE_WITH_TIMEOUT(versions->model()->rowCount(), 2, 10000);
    QTRY_COMPARE(model->fileCount(), 0);
    QCOMPARE(resets.count(), 0);
    // Keep the previous catalog while a sync writer has an incomplete definition.
    const auto metadata = created.asset.historyRoot + "/.xips.json";
    QFile original(metadata); QVERIFY(original.open(QIODevice::ReadOnly));
    auto document = QJsonDocument::fromJson(original.readAll()).object(); original.close();
    put(metadata, "{partial"); QTest::qWait(1100);
    QVERIFY(panel.catalogAsset(created.asset.id).has_value());
    document.insert("description", "synchronized description");
    replace(metadata, QJsonDocument(document).toJson());
    QTRY_VERIFY_WITH_TIMEOUT(panel.catalogAsset(created.asset.id).has_value() &&
        panel.catalogAsset(created.asset.id)->description == "synchronized description", 10000);
    // A visible modal editor holds background updates until it closes.
    {
        QDialog editing(&panel); editing.setModal(true); editing.show();
        put(created.asset.root + "/rtl/after-dialog.sv", "queued");
        QTest::qWait(1100);
        QVERIFY(!model->fileIndex("rtl/after-dialog.sv").isValid());
        editing.accept();
    }
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex("rtl/after-dialog.sv").isValid(), 10000);
    QVERIFY(QFile::remove(created.asset.root + "/rtl/top.sv"));
    QTRY_VERIFY_WITH_TIMEOUT(!model->fileIndex("rtl/top.sv").isValid(), 10000);
    QVERIFY(model->checkedFiles().isEmpty());
    // New definitions/groups are discovered without an explicit Rescan.
    definition.name = "newly_synced";
    const auto synced = SnapshotLibrary::create(library, definition); QVERIFY(synced.ok);
    const auto group = CatalogGroups::create(library, "New group"); QVERIFY(group.ok);
    QVERIFY(CatalogGroups::setMember(library, group.group.id, synced.asset.id, true).ok);
    QTRY_VERIFY_WITH_TIMEOUT(panel.catalogAsset(synced.asset.id).has_value(), 10000);
    auto *catalog = qobject_cast<CatalogModel *>(panel.findChild<ElaTreeView *>("assetList")->model());
    QTRY_VERIFY_WITH_TIMEOUT(!catalog->groups().isEmpty(), 10000);
    panel.revealAsset(created.asset.id);
    panel.hide(); put(created.asset.root + "/rtl/while-hidden.sv", "hidden"); QTest::qWait(900);
    panel.show();
    QTRY_VERIFY_WITH_TIMEOUT(model->fileIndex("rtl/while-hidden.sv").isValid(), 10000);
    // A persistently invalid definition remains monitored and can recover in place.
    put(metadata, "{invalid");
    QTRY_VERIFY_WITH_TIMEOUT(!panel.catalogAsset(created.asset.id).has_value(), 10000);
    replace(metadata, QJsonDocument(document).toJson());
    QTRY_VERIFY_WITH_TIMEOUT(panel.catalogAsset(created.asset.id).has_value(), 10000);
    // Switching roots discards old subscriptions and delayed retry callbacks.
    const auto otherLibrary = tmp.filePath("other-library"); QVERIFY(QDir().mkpath(otherLibrary));
    definition.name = "other";
    const auto other = SnapshotLibrary::create(otherLibrary, definition); QVERIFY(other.ok);
    panel.setContext(otherLibrary, {}); QTRY_VERIFY(!panel.isCatalogBusy());
    put(created.asset.root + "/rtl/old-library.sv", "ignored"); QTest::qWait(1000);
    QCOMPARE(panel.saveState().value("library").toString(), otherLibrary);
    QVERIFY(panel.catalogAsset(other.asset.id).has_value());
    QVERIFY(!panel.catalogAsset(created.asset.id).has_value());
}
