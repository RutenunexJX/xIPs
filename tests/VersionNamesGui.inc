void GuiSmokeTest::archivedVersionNamesCanBeEdited()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"); QVERIFY(QDir().mkpath(library));
    CatalogDefinition definition; definition.name = "editable_versions";
    const auto created = savedCatalogFixture(library, definition); QVERIFY(created.ok);
    BrowserPanel panel; panel.resize(780, 500); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(created.asset.id);
    auto *pages = panel.findChild<ElaTabWidget *>("assetPages"); pages->setCurrentIndex(1);
    auto *versions = panel.findChild<ElaTableView *>("revisionTable");
    auto *button = panel.findChild<QToolButton *>("renameRevisionButton");
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    auto *workingModel = qobject_cast<WorkingFilesModel *>(working->model());
    workingModel->checkAll(true);
    const auto checked = panel.saveState().value("workingChecks");
    QTRY_VERIFY(button->isVisible() && button->isEnabled());
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), created.snapshot.id);
    bool cancelled = false;
    whenVisible(&panel, "renameRevisionForm", [&](QWidget *form)
    {
        auto *field = form->findChild<QLineEdit *>("revisionLabel"); QVERIFY(field);
        QCOMPARE(field->text(), QString("rev1"));
        field->clear(); QVERIFY(!form->findChild<ElaPushButton *>("formAccept")->isEnabled());
        field->setText("discarded");
        qobject_cast<QDialog *>(form)->reject(); cancelled = true;
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(cancelled); QCOMPARE(versions->model()->index(0, 0).data().toString(), QString("rev1"));
    const auto edit = [&](const QString &name)
    {
        whenVisible(&panel, "renameRevisionForm", [name](QWidget *form)
        {
            auto *field = form->findChild<QLineEdit *>("revisionLabel");
            field->setText(name);
            QTest::mouseClick(form->findChild<ElaPushButton *>("formAccept"), Qt::LeftButton);
        });
    };
    edit("v1.0.0");
    const auto point = versions->visualRect(versions->model()->index(0, 0)).center();
    QTest::mouseClick(versions->viewport(), Qt::LeftButton, {}, point);
    QTest::mouseDClick(versions->viewport(), Qt::LeftButton, {}, point);
    QTRY_COMPARE(versions->model()->index(0, 0).data().toString(), QString("v1.0.0"));
    QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->currentIndex().data(Qt::UserRole).toString(), created.snapshot.id);
    QCOMPARE(working->model(), workingModel);
    QCOMPARE(panel.saveState().value("workingChecks"), checked);
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(versions->model()->index(0, 0).data().toString(), QString("v1.0.0"));
    edit("v1.1");
    panel.activateWindow(); versions->setFocus(); QTest::qWait(30);
    QTest::keyClick(versions, Qt::Key_F2);
    QTRY_COMPARE(versions->model()->index(0, 0).data().toString(), QString("v1.1"));
    QTRY_VERIFY(!panel.isCatalogBusy());
    const auto savedState = panel.saveState();
    {
        BrowserPanel reopened; reopened.resize(780, 500); reopened.show(); reopened.restoreState(savedState);
        QTRY_VERIFY(!reopened.isCatalogBusy());
        auto *list = reopened.findChild<ElaTableView *>("revisionTable");
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QCOMPARE(list->model()->index(0, 0).data().toString(), QString("v1.1"));
        QCOMPARE(reopened.saveState().value("workingChecks"), checked);
    }
    const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!shots.isEmpty())
    {
        QDir().mkpath(shots); QTest::qWait(200);
        QVERIFY(panel.grab().save(shots + "/editable-version.png"));
        whenVisible(&panel, "renameRevisionForm", [&](QWidget *form)
        {
            QVERIFY(form->grab().save(shots + "/edit-version-form.png"));
            qobject_cast<QDialog *>(form)->reject();
        });
        button->click();
    }
    auto current = SnapshotLibrary::describe(created.asset); QVERIFY(current.ok);
    const auto receiver = tmp.filePath("receiver"); QVERIFY(QDir().mkpath(receiver));
    QVERIFY(SnapshotLibrary::addReference(current.asset, created.snapshot.id, receiver).ok);
    panel.setContext(receiver, {}); QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(created.asset.id);
    QTRY_COMPARE(versions->model()->index(0, 0).data().toString(), QString("v1.1"));
    QVERIFY(!button->isVisible() && !button->isEnabled());
}
