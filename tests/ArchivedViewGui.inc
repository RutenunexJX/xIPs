void GuiSmokeTest::refreshPreservesArchivedFileBrowsing()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            qFatal("Cannot write archived browsing fixture: %s", qPrintable(path));
    };
    CatalogDefinition definition; definition.name = "alpha";
    auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
    for (int i = 0; i < 80; ++i)
        put(created.asset.root + QString("/rtl/file-%1.sv").arg(i, 2, 10, QLatin1Char('0')), "original");
    const auto first = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(first.ok);
    QVERIFY(QFile::remove(created.asset.root + "/rtl/file-00.sv"));
    put(created.asset.root + "/rtl/second-only.sv", "second version");
    const auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
    definition.name = "beta";
    const auto other = SnapshotLibrary::create(library, definition); QVERIFY(other.ok);
    put(other.asset.root + "/other.sv", "other IP");

    BrowserPanel panel; panel.resize(800, 440); panel.show(); panel.setContext(library, {});
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(first.asset.id);
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *files = panel.findChild<ElaListView *>("fileList");
    QVERIFY(pages && versions && files);
    pages->setCurrentIndex(1);
    versions->setCurrentIndex(revisionIndex(versions, first.snapshot.id));
    QTRY_COMPARE(files->model()->rowCount(), first.snapshot.files.size());
    QTRY_VERIFY(files->verticalScrollBar()->maximum() > 0);
    const auto selected = first.snapshot.files.at(60);
    const auto selectedIndex = files->model()->index(60, 0);
    QCOMPARE(selectedIndex.data().toString(), selected);
    files->setCurrentIndex(selectedIndex);
    files->scrollTo(selectedIndex, QAbstractItemView::PositionAtTop);
    QTRY_VERIFY(files->verticalScrollBar()->value() > 0);
    const auto scroll = files->verticalScrollBar()->value();
    QSignalSpy resets(files->model(), &QAbstractItemModel::modelReset);

    // An unrelated IP changing must not interrupt the archived file being read.
    put(other.asset.root + "/synchronized.sv", "external update");
    QTRY_VERIFY_WITH_TIMEOUT(panel.catalogAsset(other.asset.id).has_value() &&
        panel.catalogAsset(other.asset.id)->workingFiles.contains("synchronized.sv"), 10000);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), first.snapshot.id);
    QCOMPARE(files->currentIndex().data().toString(), selected);
    QTRY_COMPARE(files->verticalScrollBar()->value(), scroll);
    QCOMPARE(resets.count(), 0);

    // A newly synchronized version appears without switching away from the old one.
    put(first.asset.root + "/rtl/third-only.sv", "third version");
    const auto third = SnapshotLibrary::saveCurrent(second.asset); QVERIFY(third.ok);
    QTRY_VERIFY_WITH_TIMEOUT(revisionIndex(versions, third.snapshot.id).isValid(), 10000);
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), first.snapshot.id);
    QCOMPARE(files->currentIndex().data().toString(), selected);
    QTRY_COMPARE(files->verticalScrollBar()->value(), scroll);
    QCOMPARE(resets.count(), 0);

    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(pages->currentIndex(), 1);
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), first.snapshot.id);
    QCOMPARE(files->currentIndex().data().toString(), selected);
    QTRY_COMPARE(files->verticalScrollBar()->value(), scroll);
    QCOMPARE(resets.count(), 0);

    // Retaining the old view must not suppress a real version change.
    versions->setCurrentIndex(revisionIndex(versions, third.snapshot.id));
    QStringList shown;
    for (int row = 0; row < files->model()->rowCount(); ++row)
        shown.append(files->model()->index(row, 0).data().toString());
    QCOMPARE(shown, third.snapshot.files);
    QVERIFY(!shown.contains("rtl/file-00.sv"));
    QVERIFY(shown.contains("rtl/third-only.sv"));
}
