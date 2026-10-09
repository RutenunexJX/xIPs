void GuiSmokeTest::folderImportSelectsProjectFilesAndTransfer()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), source = tmp.filePath("incoming");
    QVERIFY(QDir().mkpath(library));
    const auto put = [](const QString &path, const QByteArray &value)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size()) qFatal("Fixture write failed");
    };
    put(source + "/rtl/top.sv", "top");
    put(source + "/rtl/unused.sv", "unused");
    BrowserPanel panel;
    panel.resize(800, 640); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    const auto dropFolder = [&](QWidget *target, const QString &path)
    {
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
        const QPoint position(20, 20);
        QDragEnterEvent enter(position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDropEvent drop(position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        QVERIFY(enter.isAccepted() && drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
    };
    bool rejected = false;
    whenVisible(&panel, "folderImportForm", [&](QWidget *form)
    {
        form->findChild<ElaComboBox *>("importTransfer")->setCurrentIndex(1);
        form->findChild<QLineEdit *>("importVersion")->setText("cancelled");
        qobject_cast<QDialog *>(form)->reject(); rejected = true;
    });
    dropFolder(&panel, source);
    QTRY_VERIFY(rejected && !panel.isCatalogBusy());
    QVERIFY(SnapshotLibrary::scan(library).assets.isEmpty());
    QVERIFY(QFileInfo::exists(source + "/rtl/top.sv"));

    bool reviewed = false;
    whenVisible(&panel, "folderImportForm", [&](QWidget *form)
    {
        QCOMPARE(form->findChild<ElaComboBox *>("importDestination")->count(), 1);
        form->findChild<ElaLineEdit *>("importName")->setText("board_project");
        auto *type = form->findChild<ElaComboBox *>("importType");
        QVERIFY(type->findData("project") >= 0);
        type->setCurrentIndex(type->findData("project"));
        auto *model = qobject_cast<WorkingFilesModel *>(form->findChild<ElaTreeView *>("importFiles")->model());
        model->checkAll(false);
        QVERIFY(!form->findChild<ElaPushButton *>("formAccept")->isEnabled());
        model->setData(model->fileIndex("incoming/rtl/top.sv"), Qt::Checked, Qt::CheckStateRole);
        QCOMPARE(model->checkedFiles(), QStringList{"incoming/rtl/top.sv"});
        QCOMPARE(form->findChild<ElaComboBox *>("importTransfer")->currentData().toBool(), false);
        auto *version = form->findChild<QLineEdit *>("importVersion");
        QVERIFY(version && version->isVisible());
        QVERIFY(version->text().isEmpty());
        QVERIFY(version->placeholderText().contains("rev1"));
        version->setText("v1.0.0");
        form->findChild<ElaLineEdit *>("importNote")->setText("selected project files");
        const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
        {
            QDir().mkpath(screenshots);
            QTest::qWait(100);
            QVERIFY(form->grab().save(screenshots + "/folder-import-project.png"));
            static_cast<QComboBox *>(type)->showPopup();
            type->finishPopupAnimation();
            QCoreApplication::processEvents();
            QVERIFY(type->view()->window()->grab().save(screenshots + "/folder-import-types.png"));
            static_cast<QComboBox *>(type)->hidePopup();
        }
        reviewed = true;
        form->findChild<ElaPushButton *>("formAccept")->click();
    });
    dropFolder(&panel, source);
    QTRY_VERIFY(reviewed && !panel.isCatalogBusy());
    auto catalog = SnapshotLibrary::scan(library);
    QCOMPARE(catalog.assets.size(), 1);
    auto project = catalog.assets.first();
    QCOMPARE(project.category, QString("project"));
    QCOMPARE(project.name, QString("board_project"));
    QCOMPARE(project.workingFiles, QStringList{"incoming/rtl/top.sv"});
    QCOMPARE(SnapshotLibrary::heads(project).size(), 1);
    const auto first = SnapshotLibrary::verifySnapshot(project, SnapshotLibrary::heads(project).first());
    QVERIFY(first.ok);
    QCOMPARE(first.snapshot.files, project.workingFiles);
    QCOMPARE(first.snapshot.note, QString("selected project files"));
    QCOMPARE(SnapshotLibrary::revisionLabel(first.snapshot), QString("v1.0.0"));
    QVERIFY(QFileInfo::exists(source + "/rtl/top.sv"));
    QVERIFY(QFileInfo::exists(source + "/rtl/unused.sv"));

    auto *types = panel.findChild<ElaComboBox *>("typeCombo");
    QVERIFY(types->findData("project") >= 0);
    types->setCurrentIndex(types->findData("project"));
    QCOMPARE(panel.saveState().value("category").toString(), QString("project"));
    QCOMPARE(panel.saveState().value("assetId").toString(), project.id);
    const auto savedState = panel.saveState();
    panel.restoreState(savedState);
    QCOMPARE(panel.saveState().value("category").toString(), QString("project"));

    const auto next = tmp.filePath("next");
    put(next + "/readme.txt", "move selected");
    put(next + "/keep.txt", "retain unchecked");
    reviewed = false;
    whenVisible(&panel, "folderImportForm", [&](QWidget *form)
    {
        auto *destination = form->findChild<ElaComboBox *>("importDestination");
        QCOMPARE(destination->currentData().toString(), project.id);
        auto *version = form->findChild<QLineEdit *>("importVersion");
        QVERIFY(version && version->isVisible());
        QVERIFY(version->placeholderText().contains("rev2"));
        version->setText("v2.0.0");
        destination->setCurrentIndex(0);
        QVERIFY(version->placeholderText().contains("rev1"));
        QVERIFY(form->findChild<ElaLineEdit *>("importName")->isVisible());
        destination->setCurrentIndex(destination->findData(project.id));
        QVERIFY(!form->findChild<ElaLineEdit *>("importName")->isVisible());
        QVERIFY(version->isVisible());
        QCOMPARE(version->text(), QString("v2.0.0"));
        QVERIFY(version->placeholderText().contains("rev2"));
        auto *model = qobject_cast<WorkingFilesModel *>(form->findChild<ElaTreeView *>("importFiles")->model());
        model->checkAll(false);
        model->setData(model->fileIndex("next/readme.txt"), Qt::Checked, Qt::CheckStateRole);
        form->findChild<ElaComboBox *>("importTransfer")->setCurrentIndex(1);
        reviewed = true;
        form->findChild<ElaPushButton *>("formAccept")->click();
    });
    auto *working = panel.findChild<ElaTreeView *>("workingFiles");
    dropFolder(working->viewport(), next);
    QTRY_VERIFY(reviewed && !panel.isCatalogBusy());
    project = SnapshotLibrary::scan(library).assets.first();
    QVERIFY(!QFileInfo::exists(next + "/readme.txt"));
    QVERIFY(QFileInfo::exists(next + "/keep.txt"));
    QVERIFY(QFileInfo::exists(project.root + "/next/readme.txt"));
    QVERIFY(!QFileInfo::exists(project.root + "/next/keep.txt"));
    QCOMPARE(project.workingFiles, QStringList{"next/readme.txt"});
    auto *workingModel = qobject_cast<WorkingFilesModel *>(working->model());
    QVERIFY(!workingModel->fileIndex("incoming/rtl/top.sv").isValid());
    QVERIFY(!workingModel->fileIndex("next/readme.txt").isValid());
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QCOMPARE(workingModel->fileCount(), 0);
    const auto latest = SnapshotLibrary::verifySnapshot(project, SnapshotLibrary::heads(project).first());
    QVERIFY(latest.ok);
    QCOMPARE(latest.snapshot.sequence, 2);
    QCOMPARE(SnapshotLibrary::revisionLabel(latest.snapshot), QString("v2.0.0"));
    QCOMPARE(latest.snapshot.files, QStringList{"next/readme.txt"});
    QVERIFY(SnapshotLibrary::verifySnapshot(project, first.snapshot.id).ok);
    QCOMPARE(panel.findChild<ElaTableView *>("revisionTable")->model()->rowCount(), 2);
    const auto state = panel.saveState();
    BrowserPanel reopened; reopened.resize(800, 640); reopened.show(); reopened.restoreState(state);
    QTRY_VERIFY(!reopened.isCatalogBusy());
    auto *reopenedModel = qobject_cast<WorkingFilesModel *>(reopened.findChild<ElaTreeView *>("workingFiles")->model());
    QCOMPARE(reopenedModel->fileCount(), 0);
    QVERIFY(!reopenedModel->fileIndex("next/readme.txt").isValid());
    QCOMPARE(reopened.findChild<ElaTableView *>("revisionTable")->model()->rowCount(), 2);
    whenVisible(&reopened, "assetDetailsForm", [](QWidget *form)
    {
        form->findChild<QLineEdit *>("assetDetailsName")->setText("renamed_project");
        form->findChild<ElaPushButton *>("formAccept")->click();
    });
    reopened.findChild<QToolButton *>("editAssetButton")->click();
    QTRY_VERIFY(!reopened.isCatalogBusy());
    const auto renamed = SnapshotLibrary::scan(library).assets.first();
    QCOMPARE(renamed.name, QString("renamed_project"));
    QCOMPARE(renamed.root, library + "/renamed_project");
    QCOMPARE(renamed.id, project.id);
    QVERIFY(!QFileInfo::exists(project.root));
    QCOMPARE(reopenedModel->fileCount(), 0);
    QCOMPARE(reopened.findChild<ElaTableView *>("revisionTable")->model()->rowCount(), 2);
    const auto screenshots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) QVERIFY(reopened.grab().save(screenshots + "/current-working-files.png"));
}


void GuiSmokeTest::registeredFolderImportTracksSelectedHiddenFiles()
{
    QTemporaryDir tmp;
    const auto library = tmp.filePath("library"), incoming = tmp.filePath("pcie_uart_v33");
    const auto put = [](const QString &path, const QByteArray &value)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(value) != value.size()) qFatal("Fixture write failed");
    };
    CatalogDefinition definition; definition.name = "pcie_uart"; definition.source = library + "/pcie_uart";
    put(definition.source + "/baseline.sv", "baseline");
    auto created = SnapshotLibrary::create(library, definition); QVERIFY(created.ok);
    created = SnapshotLibrary::saveCurrent(created.asset); QVERIFY(created.ok);
    auto document = created.asset.document; document.remove("workingArea");
    put(created.asset.historyRoot + "/.xips.json", QJsonDocument(document).toJson());
    QVERIFY(QFile::remove(created.asset.root + "/baseline.sv"));
    const QString hidden = "pcie_uart_v33/pcie_uart.srcs/sources_1/new/.zeroslack/pinloom-links.json";
    put(incoming + "/pcie_uart.srcs/sources_1/new/.zeroslack/pinloom-links.json", "links");
    put(incoming + "/rtl/top.sv", "rtl");
    put(incoming + "/build/uart.bit", "bitstream");
    BrowserPanel panel; panel.resize(800, 600); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy());
    auto *tree = panel.findChild<ElaTreeView *>("workingFiles");
    auto *model = qobject_cast<WorkingFilesModel *>(tree->model());
    QCOMPARE(model->fileCount(), 0);
    bool reviewed = false;
    whenVisible(&panel, "folderImportForm", [&](QWidget *form)
    {
        QCOMPARE(form->findChild<ElaComboBox *>("importDestination")->currentData().toString(), created.asset.id);
        auto *files = qobject_cast<WorkingFilesModel *>(form->findChild<ElaTreeView *>("importFiles")->model());
        QVERIFY(files->fileIndex(hidden).isValid());
        files->checkAll(true);
        form->findChild<QLineEdit *>("importVersion")->setText("v33");
        reviewed = true;
        form->findChild<ElaPushButton *>("formAccept")->click();
    });
    QMimeData mime; mime.setUrls({QUrl::fromLocalFile(incoming)});
    const QPoint position(20, 20);
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &enter);
    QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &drop);
    QVERIFY(enter.isAccepted() && drop.isAccepted());
    QTRY_VERIFY(reviewed && !panel.isCatalogBusy());
    const auto asset = SnapshotLibrary::scan(library).assets.first();
    QVERIFY(asset.workingFiles.contains(hidden));
    const auto saved = SnapshotLibrary::verifySnapshot(asset, SnapshotLibrary::heads(asset).first());
    QVERIFY(saved.ok); QCOMPARE(SnapshotLibrary::revisionLabel(saved.snapshot), QString("v33"));
    QVERIFY(saved.snapshot.files.contains(hidden));
    QCOMPARE(model->fileCount(), 0);
    const auto state = panel.saveState();
    BrowserPanel reopened; reopened.resize(800, 600); reopened.show(); reopened.restoreState(state);
    QTRY_VERIFY(!reopened.isCatalogBusy());
    auto *reopenedModel = qobject_cast<WorkingFilesModel *>(reopened.findChild<ElaTreeView *>("workingFiles")->model());
    QCOMPARE(reopenedModel->fileCount(), 0);
    put(asset.root + '/' + hidden, "modified links");
    QTRY_VERIFY_WITH_TIMEOUT(reopenedModel->fileIndex(hidden).isValid(), 10000);
    put(asset.root + '/' + hidden, "links");
    QTRY_COMPARE_WITH_TIMEOUT(reopenedModel->fileCount(), 0, 10000);
    QVERIFY(QFile::remove(asset.root + '/' + hidden));
    QTest::qWait(1000); QTRY_VERIFY(!reopened.isCatalogBusy());
    put(asset.root + '/' + hidden, "recreated links");
    QTRY_VERIFY_WITH_TIMEOUT(reopenedModel->fileIndex(hidden).isValid(), 10000);
    put(asset.root + "/pcie_uart_v33/pcie_uart.srcs/sources_1/new/.zeroslack/unselected.json", "unselected");
    reopened.refresh(); QTRY_VERIFY(!reopened.isCatalogBusy());
    QCOMPARE(reopenedModel->fileCount(), 3);
}
