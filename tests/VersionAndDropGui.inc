// Native/offscreen Qt event coverage; all content lives in isolated fixtures.
void GuiSmokeTest::archivedVersionsCanBeDeleted()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "delete_versions";
    auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
    const auto source = created.asset.root + "/top.sv";
    const auto put = [&](const QByteArray &bytes)
    { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); };
    put("first"); auto first = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(first.ok);
    put("second"); auto second = SnapshotLibrary::saveCurrent(first.asset); QVERIFY(second.ok);
    auto group = CatalogGroups::create(library, "AXI"); QVERIFY(group.ok);
    QVERIFY(CatalogGroups::setMember(library, group.group.id, first.asset.id, true).ok);
    const auto groups = CatalogGroups::scan(library).groups;
    BrowserPanel panel; panel.resize(720, 440); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(first.asset.id);
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *button = panel.findChild<QToolButton *>("deleteRevisionButton");
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(working->model());
    QTRY_COMPARE(versions->model()->rowCount(), 2);
    QTRY_VERIFY(!panel.isCatalogBusy());
    model->checkAll(true); pages->setCurrentIndex(1);
    versions->setCurrentIndex(revisionIndex(versions, second.snapshot.id));
    QVERIFY(button->isVisible() && button->isEnabled());
    QCOMPARE(button->text(), QString("Delete version"));
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    const auto capture = [&](const QString &name)
    {
        const auto directory = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (directory.isEmpty()) return;
        QDir().mkpath(directory); QTest::qWait(500);
        panel.grab().save(directory + '/' + name + ".png");
    };
    qInfo() << "Version deletion event test platform" << QGuiApplication::platformName() << "DPR" << panel.devicePixelRatioF();
    const auto previousTheme = eTheme->getThemeMode();
    eTheme->setThemeMode(ElaThemeType::Light); capture("delete-version-light");
    eTheme->setThemeMode(ElaThemeType::Dark); capture("delete-version-dark");
    eTheme->setThemeMode(previousTheme);
    bool confirmedTarget = false, cancelled = false;
    whenVisible(&panel, "deleteRevisionForm", [&](QWidget *form)
    {
        QString message;
        for (auto *label : form->findChildren<ElaText *>()) message += label->text();
        confirmedTarget = message.contains("delete_versions") && message.contains("rev2");
        for (auto *cancel : form->findChildren<ElaPushButton *>())
            if (cancel->text() == "Cancel") { QTest::mouseClick(cancel, Qt::LeftButton); cancelled = true; break; }
        if (!cancelled) qobject_cast<QDialog *>(form)->reject();
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(confirmedTarget && cancelled); QVERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 2);
    QVERIFY(!QFileInfo::exists(first.asset.historyRoot + "/.xips/deleted-revisions/" + second.snapshot.id + ".json"));
    const auto erase = [&]
    {
        whenVisible(&panel, "deleteRevisionForm", [&](QWidget *form)
        { QTest::mouseClick(form->findChild<ElaPushButton *>("formAccept"), Qt::LeftButton); });
        QTest::mouseClick(button, Qt::LeftButton);
    };
    erase(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 1);
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), first.snapshot.id);
    QVERIFY(button->isVisible() && button->isEnabled());
    FileOpenCapture opened;
    auto *files = panel.findChild<ElaListView *>("fileList");
    files->setCurrentIndex(files->model()->index(0, 0));
    panel.findChild<QToolButton *>("openFileButton")->click(); QTRY_COMPARE(opened.urls.size(), 1);
    QFile original(opened.urls.first().toLocalFile()); QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("first")); original.close();
    erase(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 0); QCOMPARE(pages->currentIndex(), 1);
    QVERIFY(panel.findChild<ElaText *>("historyEmpty")->isVisible());
    QVERIFY(!panel.findChild<QWidget *>("fileSection")->isVisible());
    QVERIFY(!button->isEnabled());
    QVERIFY(model->checkedFiles().isEmpty());
    QCOMPARE(panel.saveState().value("assetId").toString(), first.asset.id);
    QCOMPARE(CatalogGroups::scan(library).groups, groups);
    capture("delete-last-version");
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 0);
    {
        BrowserPanel reopened; reopened.resize(720, 440); reopened.show(); reopened.setContext(library, {});
        QTRY_VERIFY(!reopened.isCatalogBusy()); reopened.revealAsset(first.asset.id);
        auto *tabs = reopened.findChild<ElaTabWidget *>("assetPages"); tabs->setCurrentIndex(1);
        QTRY_VERIFY(reopened.findChild<ElaText *>("historyEmpty")->isVisible());
        auto *tree = reopened.findChild<ElaTreeView *>("workingFiles");
        QCOMPARE(qobject_cast<WorkingFilesModel *>(tree->model())->fileCount(), 1);
        tabs->setCurrentIndex(0);
        QTest::mouseClick(reopened.findChild<ElaCheckBox *>("checkAllFiles"), Qt::LeftButton);
        whenVisible(&reopened, "payloadReviewForm", [&](QWidget *form)
        { form->findChild<ElaPushButton *>("formAccept")->click(); });
        reopened.findChild<QToolButton *>("updateButton")->click(); QTRY_VERIFY(!reopened.isCatalogBusy());
        auto *saved = reopened.findChild<ElaTableView *>("revisionTable");
        QCOMPARE(saved->model()->rowCount(), 1); QCOMPARE(saved->model()->index(0, 0).data().toString(), QString("rev3"));
    }
    const auto latest = SnapshotLibrary::describe(first.asset); QVERIFY(latest.ok);
    const auto heads = SnapshotLibrary::heads(latest.asset); QCOMPARE(heads.size(), 1);
    const auto receiver = tmp.filePath("receiver"); QVERIFY(QDir().mkpath(receiver));
    QVERIFY(SnapshotLibrary::addReference(latest.asset, heads.first(), receiver).ok);
    panel.setContext(receiver, {}); QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(first.asset.id);
    QTRY_COMPARE(versions->model()->rowCount(), 1);
    QVERIFY(!button->isVisible() && !button->isEnabled());
    QCOMPARE(CatalogGroups::scan(library).groups, groups);
}

void GuiSmokeTest::externalDropsReachEveryWorkingArea()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), external = tmp.filePath("external");
    QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &bytes)
    { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) qFatal("Fixture write failed"); };
    const auto get = [](const QString &path)
    { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; };
    const QStringList initial{"one space.sv", "two.sv", "three.sv", "rtl/sub/helper.sv", "rtl/top.sv", "include/defs.svh", "notes.txt", "page.sv", "frame.sv"};
    for (const auto &file : initial) put(external + '/' + file, file.toUtf8());
    CatalogDefinition definition; definition.name = "drop_files";
    auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
    BrowserPanel panel; panel.resize(720, 440); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(created.asset.id);
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages");
    auto *tree = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(tree->model());
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    QTRY_VERIFY(panel.findChild<QToolButton *>("addFilesButton")->isEnabled());
    qInfo() << "External drop event test platform" << QGuiApplication::platformName() << "DPR" << panel.devicePixelRatioF();
    const auto chain = [&](QWidget *target, const QPoint &position, const QMimeData &mime,
                           Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction)
    {
        QDragEnterEvent enter(position, actions, &mime, Qt::LeftButton, Qt::NoModifier); enter.ignore();
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(position, actions, &mime, Qt::LeftButton, Qt::NoModifier); move.ignore();
        QApplication::sendEvent(target, &move);
        QDropEvent drop(position, actions, &mime, Qt::LeftButton, Qt::NoModifier); drop.ignore();
        QTimer guard; guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &panel, [&]
        { for (auto *dialog : panel.findChildren<QDialog *>()) if (dialog->isVisible()) dialog->reject(); });
        guard.start(1000); QApplication::sendEvent(target, &drop); guard.stop();
        return enter.isAccepted() && move.isAccepted() && drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
    };
    const auto dropPaths = [&](QWidget *target, const QPoint &position, const QStringList &paths)
    {
        QMimeData mime; QByteArray uriList;
        for (const auto &path : paths) uriList += QUrl::fromLocalFile(path).toEncoded() + "\r\n";
        mime.setData("text/uri-list", uriList);
        if (std::any_of(paths.cbegin(), paths.cend(), [](const auto &path) { return QFileInfo(path).isDir(); }))
            whenVisible(&panel, "folderImportForm", [](QWidget *form)
            { form->findChild<ElaPushButton *>("formAccept")->click(); });
        return chain(target, position, mime);
    };
    QCOMPARE(model->fileCount(), 0);
    QMimeData empty; QVERIFY(!chain(tree->viewport(), QPoint(20, 20), empty));
    QMimeData noUrls; noUrls.setText("not a file URL"); QVERIFY(!chain(tree->viewport(), QPoint(20, 20), noUrls));
    QMimeData mixedUrls; mixedUrls.setUrls({QUrl::fromLocalFile(external + "/two.sv"), QUrl("https://example.invalid/file.sv")});
    QVERIFY(!chain(tree->viewport(), QPoint(20, 20), mixedUrls));
    QMimeData moveOnly; moveOnly.setUrls({QUrl::fromLocalFile(external + "/two.sv")});
    QVERIFY(!chain(tree->viewport(), QPoint(20, 20), moveOnly, Qt::MoveAction));
    QCOMPARE(model->fileCount(), 0); QVERIFY(!panel.isCatalogBusy());
    QVERIFY(dropPaths(tree->viewport(), QPoint(20, 20), {external + "/one space.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 1); QCOMPARE(versions->model()->rowCount(), 0);
    const auto blank = [&] { return QPoint(30, tree->viewport()->height() - 8); };
    QVERIFY(!tree->indexAt(blank()).isValid());
    QVERIFY(dropPaths(tree->viewport(), blank(), {external + "/two.sv", external + "/three.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 3);
    QVERIFY(dropPaths(tree->viewport(), tree->visualRect(model->fileIndex("two.sv")).center(), {external + "/rtl"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 5);
    auto folder = model->fileIndex("rtl/top.sv").parent(); QVERIFY(folder.isValid());
    QVERIFY(dropPaths(tree->viewport(), tree->visualRect(folder).center(), {external + "/include", external + "/notes.txt"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 5);
    QVERIFY(!model->fileIndex("rtl/top.sv").isValid());
    auto *page = panel.findChild<QWidget *>("workingPage");
    QVERIFY(dropPaths(page, QPoint(2, 2), {external + "/page.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 6);
    QVERIFY(dropPaths(tree, QPoint(2, 2), {external + "/frame.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 7);
    put(external + "/fallback.sv", "fallback");
    QVERIFY(dropPaths(&panel, tree->viewport()->mapTo(&panel, blank()), {external + "/fallback.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 8);
    QCOMPARE(versions->model()->rowCount(), 2);
    for (const auto &file : initial) QCOMPARE(get(created.asset.root + '/' + file), get(external + '/' + file));
    QTest::mouseClick(panel.findChild<ElaCheckBox *>("checkAllFiles"), Qt::LeftButton);
    QCOMPARE(model->checkedFiles().size(), 8);
    const auto directory = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!directory.isEmpty())
    { QDir().mkpath(directory); QTest::qWait(500); panel.grab().save(directory + "/working-files-dropped.png"); }
    whenVisible(&panel, "payloadReviewForm", [&](QWidget *form)
    { form->findChild<ElaPushButton *>("formAccept")->click(); });
    panel.findChild<QToolButton *>("updateButton")->click(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->rowCount(), 3);
    const auto revision = versions->currentIndex().data(Qt::UserRole).toString();
    const auto archived = SnapshotLibrary::verifySnapshot(created.asset, revision); QVERIFY(archived.ok);
    QCOMPARE(archived.snapshot.files.size(), 8);
    pages->setCurrentIndex(0);
    QVERIFY(dropPaths(tree->viewport(), blank(), {external + "/rtl", external + "/two.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 0);
    QVERIFY(!panel.findChild<ElaText *>("browserNotice")->property("error").toBool());
    put(external + "/two.sv", "conflicting original");
    QVERIFY(dropPaths(tree->viewport(), tree->visualRect(model->fileIndex("two.sv")).center(), {external + "/two.sv"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QVERIFY(panel.findChild<ElaText *>("browserNotice")->property("error").toBool());
    QCOMPARE(get(created.asset.root + "/two.sv"), QByteArray("two.sv"));
    QCOMPARE(get(external + "/two.sv"), QByteArray("conflicting original"));
    QCOMPARE(versions->model()->rowCount(), 4);
    QCOMPARE(SnapshotLibrary::verifySnapshot(created.asset, revision).snapshot.hash, archived.snapshot.hash);
    definition.name = "empty_hint"; const auto another = SnapshotLibrary::create(library, definition); QVERIFY(another.ok);
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(another.asset.id);
    QTRY_COMPARE(model->fileCount(), 0);
    auto *hint = panel.findChild<ElaText *>("workingEmpty"); QVERIFY(!hint->isVisible());
    QVERIFY(dropPaths(tree->viewport(), QPoint(10, 8), {external + "/notes.txt"}));
    QTRY_VERIFY(!panel.isCatalogBusy()); QCOMPARE(model->fileCount(), 1);
    QCOMPARE(get(another.asset.root + "/notes.txt"), QByteArray("notes.txt"));
    QCOMPARE(versions->model()->rowCount(), 0);
}
