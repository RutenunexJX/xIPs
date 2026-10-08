// End-to-end Qt event test, shared by offscreen and native Windows runs.
void GuiSmokeTest::emptyIpImportCheckAndCreateVersions()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), external = tmp.filePath("external");
    QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &value)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(value) != value.size()) qFatal("Fixture write failed");
    };
    const auto get = [](const QString &path)
    { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    const auto group = CatalogGroups::create(library, "AXI");
    QVERIFY(group.ok);
    QString id;
    {
        BrowserPanel panel;
        panel.resize(720, 420); panel.show(); panel.setContext(library, external);
        QTRY_VERIFY(!panel.isCatalogBusy());
        auto *tree = panel.findChild<ElaTreeView *>("assetList");
        auto *catalog = qobject_cast<CatalogModel *>(tree->model());
        tree->setCurrentIndex(catalog->indexForId({}, group.group.id));
        whenVisible(&panel, "newAssetName", [&](QWidget *field)
        {
            qobject_cast<ElaLineEdit *>(field)->setText("empty_ip");
            panel.findChild<ElaComboBox *>("newAssetType")->setCurrentIndex(1);
            panel.findChild<ElaPushButton *>("formAccept")->click();
        });
        panel.findChild<ElaPushButton *>("newAssetButton")->click();
        QTRY_VERIFY(!panel.isCatalogBusy());
        id = panel.saveState().value("assetId").toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(catalog->rowCount(catalog->indexForId({}, group.group.id)), 1);
        QCOMPARE(panel.findChild<ElaTableView *>("revisionTable")->model()->rowCount(), 0);
        QVERIFY(!panel.findChild<QToolButton *>("updateButton")->isEnabled());
        panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
        QCOMPARE(panel.saveState().value("assetId").toString(), id);
        QVERIFY(SnapshotLibrary::scan(library).assets.first().workingFiles.isEmpty());
    }
    BrowserPanel panel;
    panel.resize(720, 420); panel.show(); panel.setContext(library, external);
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(id);
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(working->model());
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *create = panel.findChild<QToolButton *>("updateButton");
    QAbstractItemModelTester check(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QCOMPARE(versions->model()->rowCount(), 0); QCOMPARE(model->fileCount(), 0);
    QVERIFY(!create->isEnabled());
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    const auto capture = [&](const QString &name)
    {
        if (screenshots.isEmpty()) return;
        QDir().mkpath(screenshots); QCoreApplication::processEvents();
        QTest::qWait(500);
        panel.grab().save(screenshots + '/' + name + ".png");
    };
    capture("empty-ip");
    for (const auto *button : {"addFilesButton", "addFolderButton"})
    {
        bool cancelled = false;
        whenVisible(&panel, "xipsFilePicker", [&](QWidget *picker)
        { qobject_cast<QDialog *>(picker)->reject(); cancelled = true; });
        panel.findChild<QToolButton *>(button)->click();
        QVERIFY(cancelled); QCOMPARE(model->fileCount(), 0);
    }
    put(external + "/rtl/top.sv", "original top");
    put(external + "/rtl/sub/helper.sv", "helper");
    put(external + "/rtl/unused.sv", "unused");
    put(external + "/notes.txt", "original notes");
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(external + "/rtl"), QUrl::fromLocalFile(external + "/notes.txt")});
    QDragEnterEvent enter(QPoint(30, 30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(working->viewport(), &enter); QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(30, 30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    whenVisible(&panel, "folderImportForm", [&](QWidget *form)
    { form->findChild<ElaPushButton *>("formAccept")->click(); });
    QApplication::sendEvent(working->viewport(), &drop); QVERIFY(drop.isAccepted());
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->fileCount(), 0); QCOMPARE(versions->model()->rowCount(), 1); QVERIFY(!create->isEnabled());
    put(external + "/extra.v", "extra"); put(external + "/docs/nested/readme.txt", "readme");
    for (const auto &item : QList<QPair<QString, QString>>{{"addFilesButton", external + "/extra.v"},
                                                        {"addFolderButton", external + "/docs"}})
    {
        whenVisible(&panel, "xipsFilePicker", [&](QWidget *picker)
        {
            auto *path = picker->findChild<ElaLineEdit *>("pickerPath");
            path->setFocus(); path->selectAll(); QTest::keyClicks(path, item.second);
            picker->findChild<ElaPushButton *>("pickerAccept")->click();
        });
        panel.findChild<QToolButton *>(item.first)->click(); QTRY_VERIFY(!panel.isCatalogBusy());
    }
    QCOMPARE(model->fileCount(), 6); QCOMPARE(versions->model()->rowCount(), 1);
    auto *all = panel.findChild<ElaCheckBox *>("checkAllFiles");
    all->click(); QCOMPARE(model->checkedFiles().size(), 6);
    all->click(); QVERIFY(model->checkedFiles().isEmpty()); QVERIFY(!create->isEnabled());
    working->setCurrentIndex(model->fileIndex("rtl/sub/helper.sv").parent());
    QTest::keyClick(working, Qt::Key_Space);
    QCOMPARE(model->checkedFiles(), QStringList{"rtl/sub/helper.sv"});
    working->setCurrentIndex(model->fileIndex("notes.txt")); QTest::keyClick(working, Qt::Key_Space);
    const QStringList selected{"notes.txt", "rtl/sub/helper.sv"};
    QCOMPARE(model->checkedFiles(), selected);
    working->setCurrentIndex(model->fileIndex("rtl/unused.sv")); QCOMPARE(model->checkedFiles(), selected);
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->checkedFiles(), selected);
    pages->setCurrentIndex(1); pages->setCurrentIndex(0); QCOMPARE(model->checkedFiles(), selected);
    bool rejected = false;
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    { qobject_cast<QDialog *>(form)->reject(); rejected = true; });
    create->click(); QTRY_VERIFY(rejected && !panel.isCatalogBusy()); QCOMPARE(versions->model()->rowCount(), 1);
    const auto saveVersion = [&]
    {
        whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
        {
            QTimer::singleShot(0, form->findChild<ElaPushButton *>("formAccept"), &QPushButton::click);
            QVERIFY(form->findChild<QPlainTextEdit *>("payloadPreview")->toPlainText()
                .contains("Included files\n" + selected.join('\n')));
            form->findChild<ElaPushButton *>("formAccept")->click();
        });
        create->click();
    };
    capture("working-selection-light");
    const auto previousTheme = eTheme->getThemeMode();
    eTheme->setThemeMode(ElaThemeType::Dark); capture("working-selection-dark"); eTheme->setThemeMode(previousTheme);
    saveVersion(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 2);
    const auto first = versions->currentIndex().data(Qt::UserRole).toString();
    QCOMPARE(versions->model()->index(0, 0).data().toString(), QString("rev2"));
    QCOMPARE(versions->model()->index(0, 1).data().toString(), QString("Archived"));
    QCOMPARE(model->checkedFiles(), selected);
    const auto asset = SnapshotLibrary::scan(library).assets.first();
    const auto manifest = asset.historyRoot + "/.xips/revisions/" + first + ".json", copy = tmp.filePath("project-copy");
    const auto firstBytes = get(manifest);
    QCOMPARE(SnapshotLibrary::verifySnapshot(asset, first).snapshot.files, selected);
    whenVisible(&panel, "exportDestination", [&](QWidget *field)
    { qobject_cast<ElaLineEdit *>(field)->setText(copy); panel.findChild<ElaPushButton *>("formAccept")->click(); });
    panel.findChild<ElaPushButton *>("takeButton")->click(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(get(copy + "/notes.txt"), QByteArray("original notes"));
    QCOMPARE(get(copy + "/rtl/sub/helper.sv"), QByteArray("helper"));
    QVERIFY(!QFileInfo::exists(copy + "/rtl/top.sv")); QVERIFY(!QFileInfo::exists(copy + "/extra.v"));
    put(asset.root + "/notes.txt", "second notes");
    saveVersion(); QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(versions->model()->rowCount(), 3);
    QCOMPARE(versions->model()->index(0, 0).data().toString(), QString("rev3")); QCOMPARE(get(manifest), firstBytes);
    versions->setCurrentIndex(revisionIndex(versions, first));
    FileOpenCapture opened;
    auto *files = panel.findChild<ElaListView *>("fileList"); files->setCurrentIndex(files->model()->index(0, 0));
    panel.findChild<QToolButton *>("openFileButton")->click(); QTRY_COMPARE(opened.urls.size(), 1);
    QCOMPARE(get(opened.urls.first().toLocalFile()), QByteArray("original notes"));
    QVERIFY(!QFileInfo(opened.urls.first().toLocalFile()).permissions().testFlag(QFile::WriteOwner));
    capture("saved-versions");
    put(external + "/notes.txt", "conflicting external");
    panel.importWorkingFiles({external + "/notes.txt"}); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(panel.findChild<ElaText *>("browserNotice")->property("error").toBool());
    QCOMPARE(get(asset.root + "/notes.txt"), QByteArray("second notes")); QCOMPARE(versions->model()->rowCount(), 3);
    QVERIFY(QFile::remove(asset.root + "/notes.txt")); panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(model->checkedFiles(), QStringList{"rtl/sub/helper.sv"});
    QCOMPARE(get(external + "/rtl/top.sv"), QByteArray("original top"));
}
