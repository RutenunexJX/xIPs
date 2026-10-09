void GuiSmokeTest::issuesOfferSafeRecoveryActions()
{
    QTemporaryDir tmp; const auto library = tmp.filePath("library"), receiver = tmp.filePath("receiver");
    QVERIFY(QDir().mkpath(library) && QDir().mkpath(receiver));
    CatalogDefinition definition; definition.name = "recoverable";
    const auto saved = savedCatalogFixture(library, definition); QVERIFY(saved.ok);
    QVERIFY(SnapshotLibrary::addReference(saved.asset, saved.snapshot.id, receiver).ok);
    const auto relocated = library + "/moved-source";
    QVERIFY(QDir().rename(saved.asset.root, relocated));
    BrowserPanel panel; panel.resize(760, 530); panel.show(); panel.setContext(library, {});
    QTRY_VERIFY(!panel.isCatalogBusy()); panel.revealAsset(saved.asset.id); QTRY_VERIFY(!panel.isCatalogBusy());
    auto *issues = panel.findChild<QToolButton *>("issuesButton"); QVERIFY(issues && issues->isVisible());
    const auto choose = [&](const QString &action, bool inspect)
    {
        whenVisible(&panel, "libraryIssuesForm", [&, action, inspect](QWidget *form)
        {
            const auto closeOnFailure = qScopeGuard([form] { if (QTest::currentTestFailed()) qobject_cast<QDialog *>(form)->reject(); });
            auto *entries = form->findChild<QComboBox *>("issueEntry"); QVERIFY(entries && entries->count());
            auto *locate = form->findChild<QPushButton *>("issueLocate");
            auto *remove = form->findChild<QPushButton *>("issueRemove");
            QVERIFY(locate->isVisible() && remove->isVisible());
            if (inspect)
            {
                QCOMPARE(form->findChild<ElaText *>("issueReason")->text(), QString("Working source is missing"));
                QVERIFY(form->findChild<ElaText *>("issueImpact")->text().contains("history is retained"));
                auto *details = form->findChild<QPlainTextEdit *>("libraryIssues"); QVERIFY(!details->isVisible());
                form->findChild<QCheckBox *>("issueDetailsToggle")->click(); QVERIFY(details->isVisible());
                QVERIFY(details->toPlainText().contains(saved.asset.root));
                const auto shots = qEnvironmentVariable("XIPS_SCREENSHOT_DIR");
                if (!shots.isEmpty())
                {
                    QDir().mkpath(shots);
                    const auto previous = eTheme->getThemeMode();
                    for (const auto mode : {ElaThemeType::Dark, ElaThemeType::Light})
                    {
                        eTheme->setThemeMode(mode); QCoreApplication::processEvents(); QTest::qWait(250);
                        auto *done = form->findChild<QPushButton *>("formAccept");
                        QVERIFY(details->mapToGlobal(QPoint(0, details->height())).y() <= done->mapToGlobal(QPoint()).y());
                        QVERIFY(form->grab().save(shots + (mode == ElaThemeType::Dark ? "/actionable-issues.png" : "/actionable-issues-light.png")));
                    }
                    eTheme->setThemeMode(previous);
                }
            }
            form->findChild<QPushButton *>(action)->click();
        });
        issues->click();
    };
    whenVisible(&panel, "relocateSourceForm", [&](QWidget *form)
    {
        form->findChild<QLineEdit *>("replacementSource")->setText(relocated);
        form->findChild<QPushButton *>("formAccept")->click();
    });
    choose("issueLocate", true); QTRY_VERIFY(!panel.isCatalogBusy());
    const auto fixed = panel.catalogAsset(saved.asset.id); QVERIFY(fixed);
    QCOMPARE(fixed->root, relocated); QVERIFY(fixed->sourceProblem.isEmpty());
    QVERIFY(SnapshotLibrary::verifySnapshot(*fixed, saved.snapshot.id).ok);
    QCOMPARE(panel.findChild<ElaTreeView *>("workingFiles")->model()->rowCount(), 0);
    QCOMPARE(panel.findChild<ElaText *>("workingStatus")->text(), QString("No unarchived changes"));
    QVERIFY(!issues->isVisible());
    const auto second = library + "/kept-source"; QVERIFY(QDir().rename(relocated, second));
    panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    whenVisible(&panel, "removeMembershipForm", [](QWidget *form) { qobject_cast<QDialog *>(form)->reject(); });
    choose("issueRemove", false); QVERIFY(panel.catalogAsset(saved.asset.id)); // cancellation keeps registration
    whenVisible(&panel, "removeMembershipForm", [](QWidget *form) { form->findChild<QPushButton *>("formAccept")->click(); });
    choose("issueRemove", false); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(!panel.catalogAsset(saved.asset.id)); QVERIFY(QFileInfo::exists(second));
    QVERIFY(QFileInfo::exists(saved.asset.historyRoot + "/.xips.json"));
    const auto movedLibrary = tmp.filePath("moved-library"); QVERIFY(QDir().rename(library, movedLibrary));
    panel.setContext(receiver, {}); QTRY_VERIFY(!panel.isCatalogBusy());
    whenVisible(&panel, "referenceOwnerForm", [&](QWidget *form)
    {
        form->findChild<QLineEdit *>("referenceOwner")->setText(movedLibrary);
        form->findChild<QPushButton *>("formAccept")->click();
    });
    whenVisible(&panel, "replacementRevision", [&](QWidget *combo)
    {
        auto *form = combo->window(); form->findChild<QPushButton *>("formAccept")->click();
    });
    choose("issueLocate", false); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(!issues->isVisible());
    const auto reference = panel.catalogAsset(saved.asset.id); QVERIFY(reference);
    QVERIFY(SnapshotLibrary::verifySnapshot(*reference, saved.snapshot.id).ok);
    // An orphan metadata directory has no trusted asset target and must never offer deletion.
    const auto orphan = receiver + "/.xips/assets/12345678-1234-5678-1234-123456789abc";
    QVERIFY(QDir().mkpath(orphan)); panel.refresh(); QTRY_VERIFY(!panel.isCatalogBusy());
    QVERIFY(issues->isVisible());
    whenVisible(&panel, "libraryIssuesForm", [&](QWidget *form)
    {
        const auto closeOnFailure = qScopeGuard([form] { if (QTest::currentTestFailed()) qobject_cast<QDialog *>(form)->reject(); });
        QVERIFY(!form->findChild<QPushButton *>("issueLocate")->isVisible());
        QVERIFY(!form->findChild<QPushButton *>("issueRemove")->isVisible());
        QVERIFY(form->findChild<QPlainTextEdit *>("libraryIssues")->toPlainText().contains(orphan));
        form->findChild<QPushButton *>("issueRetry")->click();
    });
    issues->click(); QTRY_VERIFY(!panel.isCatalogBusy()); QVERIFY(issues->isVisible()); QVERIFY(QFileInfo::exists(orphan));
}
