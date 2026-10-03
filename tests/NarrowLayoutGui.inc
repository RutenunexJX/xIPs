// Embedded layout changes must not trigger catalog I/O or replace model indexes.
void EmbeddedTest::narrowLayoutStateAndModelIdentity()
{
    QTemporaryDir tmp;
    const auto root = tmp.filePath("library"); QVERIFY(QDir().mkpath(root));
    CatalogDefinition definition; definition.name = "stable";
    const auto created = SnapshotLibrary::create(root, definition); QVERIFY(created.ok);
    put(created.asset.root + "/top.sv", "module top; endmodule");
    put(created.asset.root + "/other.sv", "module other; endmodule");
    const auto archived = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(archived.ok);
    definition.name = "empty";
    const auto empty = SnapshotLibrary::create(root, definition); QVERIFY(empty.ok);
    const auto group = CatalogGroups::create(root, "AXI"); QVERIFY(group.ok);
    QVERIFY(CatalogGroups::setMember(root, group.group.id, created.asset.id, true).ok);
    QWidget owner; HostBridge host;
    auto *panel = panelIn(&owner, &host); QVERIFY(panel);
    QVERIFY(context(panel, root)); QTRY_VERIFY(!busy(panel));
    const QVariantMap wanted{{"library", root}, {"assetId", created.asset.id}, {"revision", "current"}, {"page", 0},
        {"workingChecks", QVariantMap{{created.asset.id, QStringList{"top.sv"}}}}};
    QVERIFY(restore(panel, wanted));
    auto *catalog = panel->findChild<QAbstractItemView *>("assetList");
    auto *working = panel->findChild<QAbstractItemView *>("workingFiles");
    auto *versions = panel->findChild<QAbstractItemView *>("revisionTable");
    const auto workingSelection = fileIndex(working->model(), "top.sv");
    working->setCurrentIndex(workingSelection);
    const QPersistentModelIndex retainedFile(workingSelection), retainedAsset(catalog->currentIndex());
    QSignalSpy catalogResets(catalog->model(), &QAbstractItemModel::modelReset);
    QSignalSpy fileResets(working->model(), &QAbstractItemModel::modelReset);
    QSignalSpy fileInserts(working->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy versionInserts(versions->model(), &QAbstractItemModel::rowsInserted);
    const auto checkState = state(panel)["workingChecks"];
    {
        // Any accidentally started asynchronous scan would stay busy behind this gate.
        PoolGate gate;
        for (const auto size : {QSize(280, 760), QSize(520, 760), QSize(960, 760), QSize(280, 380),
                                QSize(580, 760), QSize(960, 620), QSize(280, 760)})
        {
            owner.setFixedSize(size); QTest::qWait(40);
            QCOMPARE(panel->size(), size);
            QVERIFY(!busy(panel));
            QCOMPARE(state(panel)["assetId"].toString(), created.asset.id);
            QCOMPARE(state(panel)["workingChecks"], checkState);
            QCOMPARE(state(panel)["page"].toInt(), 0);
            QVERIFY(retainedFile.isValid()); QVERIFY(retainedAsset.isValid());
            QCOMPARE(working->currentIndex(), QModelIndex(retainedFile));
            QCOMPARE(catalog->currentIndex(), QModelIndex(retainedAsset));
            QCOMPARE(catalogResets.count(), 0); QCOMPARE(fileResets.count(), 0);
            QCOMPARE(fileInserts.count(), 0); QCOMPARE(versionInserts.count(), 0);
            hitEvidence.append(QJsonObject{{"case", "resize-state"}, {"width", size.width()}, {"height", size.height()},
                {"modelResets", 0}, {"rowsInserted", 0}, {"busyWithWorkerGate", false}, {"retainedIndexes", true}});
        }
        QWidget floating;
        auto *layout = new QVBoxLayout(&floating); layout->setContentsMargins(0, 0, 0, 0);
        floating.setFixedSize(520, 620);
        panel->setParent(&floating); layout->addWidget(panel); floating.show(); panel->show(); QTest::qWait(40);
        QVERIFY(!busy(panel)); QCOMPARE(state(panel)["workingChecks"], checkState);
        QCOMPARE(state(panel)["assetId"].toString(), created.asset.id);
        panel->setParent(&owner); owner.layout()->addWidget(panel); panel->show(); QTest::qWait(40);
        QVERIFY(!busy(panel)); QVERIFY(retainedFile.isValid()); QVERIFY(retainedAsset.isValid());
        QCOMPARE(catalogResets.count(), 0); QCOMPARE(fileResets.count(), 0); QCOMPARE(fileInserts.count(), 0);
    }
    QVERIFY(restore(panel, {{"library", root}, {"assetId", empty.asset.id}, {"page", 1}}));
    const auto emptyState = state(panel);
    for (const int width : {960, 280, 520, 280})
    {
        owner.setFixedSize(width, 620); QTest::qWait(40);
        QCOMPARE(state(panel)["assetId"].toString(), empty.asset.id);
        QCOMPARE(state(panel)["page"].toInt(), 1);
        QCOMPARE(versions->model()->rowCount(), 0); QVERIFY(!busy(panel));
    }
    delete panel;
    hostSize = QSize(280, 620);
    panel = panelIn(&owner, &host); QVERIFY(panel);
    QVERIFY(context(panel, root)); QVERIFY(restore(panel, emptyState)); QTRY_VERIFY(!busy(panel));
    QCOMPARE(state(panel)["assetId"].toString(), empty.asset.id);
    QCOMPARE(state(panel)["page"].toInt(), 1);
    QCOMPARE(panel->findChild<QAbstractItemView *>("revisionTable")->model()->rowCount(), 0);
    hostSize = QSize(920, 620);
}
