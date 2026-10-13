#include "BrowserPanel.h"
#include "BrowserForms.h"
#include "library/HdlCommands.h"
#include "library/OperationControl.h"
#include "ElaListView.h"
#include <QDesktopServices>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

namespace xips {
QVariantMap BrowserPanel::hdlCommandCatalog() const
{
    auto result = m_hdlCatalog;
    result.insert("schema", "xips.hdl-catalog/v1");
    result.insert("generation", QString::number(m_hdlGeneration));
    result.insert("library", m_library);
    result.insert("state", (m_loadingHdl || m_busy) ? "loading" : m_hdlCatalog.isEmpty() ? "unavailable" : "ready");
    return result;
}
void BrowserPanel::refreshHdlCatalog()
{
    ++m_hdlGeneration;
    m_loadingHdl = true;
    if (m_hdlWatcher) { if (m_hdlOperation) m_hdlOperation->cancel(); return; }
    const int generation = m_hdlGeneration;
    const auto operation = std::make_shared<OperationControl>();
    m_hdlOperation = operation;
    auto *watcher = new QFutureWatcher<HdlCatalogResult>(this); m_hdlWatcher = watcher;
    connect(watcher, &QFutureWatcher<HdlCatalogResult>::finished, this, [this, watcher, generation]
    {
        const auto result = watcher->result(); m_hdlWatcher = nullptr; watcher->deleteLater(); m_hdlOperation.reset();
        if (generation != m_hdlGeneration) { refreshHdlCatalog(); return; }
        m_loadingHdl = false;
        m_hdlCatalog = {{"entries",result.entries},{"errors",result.errors}};
        updateActivity(); updateActions();
        emit hdlCommandCatalogChanged(QString::number(m_hdlGeneration));
    });
    watcher->setFuture(QtConcurrent::run([assets = m_assets, previous = m_hdlCatalog.value("entries").toList(), operation]
    {
        OperationScope scope(operation.get());
        try { return HdlCommands::catalog(assets, previous); }
        catch (const std::exception &error)
        { return HdlCatalogResult{{}, {QVariantMap{{"code","analysis_unavailable"},{"message",QString::fromUtf8(error.what())}}}}; }
    }));
    updateActivity();
}
QVariantMap BrowserPanel::useHdlCommand(const QVariantMap &request)
{
    auto result = QVariantMap{{"schema","xips.hdl-use-result/v1"},{"requestId",request.value("requestId")}};
    const auto reject = [&](const QString &status, const QString &message)
    { result.insert("status",status); result.insert("message",message); return result; };
    for (const auto *field : {"requestId","generation","commandId","revision"})
        if (request.value(field).toString().isEmpty()) return reject("invalid_request", "Missing request field: " + QString::fromLatin1(field));
    if (isCatalogBusy()) return reject("busy", "xIPs is completing another operation.");
    if (request.value("generation").toString() != QString::number(m_hdlGeneration))
        return reject("stale", "The command catalog changed. Refresh the selection.");
    QVariantMap entry;
    for (const auto &item : m_hdlCatalog.value("entries").toList())
        if (item.toMap().value("commandId") == request.value("commandId") && item.toMap().value("revision") == request.value("revision"))
        { entry = item.toMap(); break; }
    if (entry.isEmpty()) return reject("not_found", "The selected command is no longer in this catalog.");
    if (entry.value("availability") != "available")
        return reject("unavailable", "Save a valid command source version before using it. " + entry.value("diagnostics").toStringList().join('\n'));
    if (m_library.isEmpty()) return reject("unavailable", "Choose an xIPs library before using builtin commands.");
    m_hdlUse = request; m_hdlUseLibrary = m_library; m_hdlUseWorkspace = m_workspace;
    QTimer::singleShot(0, this, [this, entry] { if (!m_hdlUse.isEmpty()) startHdlUse(entry); });
    result.insert("status","accepted"); return result;
}
void BrowserPanel::finishHdlUse(const QString &status, const QString &code, const QString &message, const QVariantMap &receipt)
{
    if (m_hdlUse.isEmpty()) return;
    QVariantMap result{{"schema","xips.hdl-use-result/v1"},{"requestId",m_hdlUse.value("requestId")},{"status",status}};
    if (!code.isEmpty()) result.insert("code",code);
    if (!message.isEmpty()) result.insert("message",message);
    if (!receipt.isEmpty()) result.insert("receipt",receipt);
    m_hdlUse.clear(); m_hdlUseLibrary.clear(); m_hdlUseWorkspace.clear();
    if (std::exchange(m_hdlImported, false))
        QTimer::singleShot(0, this, [this] { if (!m_busy) refresh(); });
    updateActions(); emit hdlCommandUseFinished(result);
}
void BrowserPanel::startHdlUse(QVariantMap entry)
{
    if (m_pendingContext || m_library != m_hdlUseLibrary || m_workspace != m_hdlUseWorkspace)
    { finishHdlUse("cancelled", "context_changed"); return; }
    if (entry.value("assetId") == "builtin:tb-behaviors")
    {
        Form form(this, "Add built-in HDL packages", "Add to library"); form.setObjectName("hdlBuiltinForm");
        form.message("Create an ordinary versioned asset in the selected library, containing the reset, sequence, random and signal-adapter packages. Existing files are retained.");
        if (form.exec() != QDialog::Accepted || m_pendingContext)
        { finishHdlUse("cancelled"); return; }
        const auto library = m_library;
        auto actual = std::make_shared<QVariantMap>();
        run("Adding HDL packages…", [library, entry, actual]
        {
            auto result = HdlCommands::installBuiltins(library);
            if (!result.ok) return result;
            const auto catalog = HdlCommands::catalog({result.asset});
            for (const auto &item : catalog.entries)
                if (item.toMap().value("assetId") == result.asset.id && item.toMap().value("mappingId") == entry.value("mappingId"))
                { *actual = item.toMap(); actual->insert("commandId",entry.value("commandId")); break; }
            if (actual->isEmpty()) { result.ok = false; result.error = "Installed command validation failed; the new asset is retained for inspection."; }
            return result;
        }, [this, actual](const SnapshotResult &result)
        {
            m_hdlImported = true;
            if (m_pendingContext || m_library != m_hdlUseLibrary || m_workspace != m_hdlUseWorkspace)
            { finishHdlUse("cancelled", "context_changed"); return; }
            m_pendingId = result.asset.id;
            m_pendingRevision = result.snapshot.id;
            prepareHdlUse(result.asset,*actual);
        }, true, [this](const SnapshotResult &failure)
        { m_hdlImported = true; finishHdlUse(failure.cancelled ? "cancelled" : "error", "builtin_import_failed", failure.error); });
        return;
    }
    for (const auto &asset : m_assets)
        if (asset.id == entry.value("assetId").toString()) { prepareHdlUse(asset,entry); return; }
    finishHdlUse("error", "not_found", "The owning asset is unavailable.");
}
void BrowserPanel::prepareHdlUse(const CatalogAsset &asset, const QVariantMap &entry)
{
    run("Reviewing HDL command source…", [asset, entry] { return HdlCommands::prepareUse(asset,entry); },
        [this, entry](const SnapshotResult &prepared)
        {
            if (m_pendingContext || m_library != m_hdlUseLibrary || m_workspace != m_hdlUseWorkspace)
            { finishHdlUse("cancelled", "context_changed"); return; }
            exportPrepared(prepared,entry);
        }, false, [this](const SnapshotResult &failure)
        { finishHdlUse(failure.cancelled ? "cancelled" : "error", "source_unavailable", failure.error); });
}
void BrowserPanel::openHdlCommands()
{
    if (isCatalogBusy()) { notice("Wait for the current source check to finish."); return; }
    const auto asset = m_selected;
    Form form(this, "HDL commands", "Done"); form.setObjectName("hdlCommandsForm"); form.setMinimumWidth(660);
    form.message(asset.id.isEmpty() ? "Select an asset to create or bind user commands. Builtins can be added to your selected library."
        : "Commands belong to " + asset.name + ". Edit their implementation in ordinary source files; save a version before Use.");
    QStringList catalogProblems;
    for (const auto &value : m_hdlCatalog.value("errors").toList())
    {
        const auto error = value.toMap();
        if (asset.id.isEmpty() || !error.contains("assetId") || error.value("assetId") == asset.id)
            catalogProblems.append(error.value("message").toString());
    }
    if (!catalogProblems.isEmpty())
    {
        auto *problems = new EnglishPlainTextEdit(&form); problems->setObjectName("hdlCatalogProblems");
        problems->setReadOnly(true); problems->setMaximumHeight(90); problems->setPlainText(catalogProblems.join('\n'));
        form.body->addWidget(problems);
    }
    auto *view = new ElaListView(&form); view->setObjectName("hdlCommandList");
    auto *model = new QStandardItemModel(view); view->setModel(model); view->setMinimumHeight(180);
    for (const auto &item : m_hdlCatalog.value("entries").toList())
    {
        const auto entry = item.toMap();
        if (!asset.id.isEmpty() && entry.value("assetId") != asset.id && entry.value("assetId") != "builtin:tb-behaviors") continue;
        auto *row = new QStandardItem(entry.value("name").toString() + " · " + entry.value("symbol").toString() + " · " + entry.value("availability").toString());
        row->setEditable(false); row->setData(entry, Qt::UserRole);
        row->setToolTip(entry.value("description").toString() + '\n' + entry.value("assetName").toString() + " · " + entry.value("revision").toString() + '\n' + entry.value("diagnostics").toStringList().join('\n'));
        model->appendRow(row);
    }
    form.body->addWidget(view); if (model->rowCount()) view->setCurrentIndex(model->index(0,0));
    auto *details = new ElaText(&form); details->setObjectName("hdlCommandDetails");
    details->setWordWrap(true); details->setTextFormat(Qt::PlainText);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse); form.body->addWidget(details);
    enum Action { Done, Create, Edit, Open, Save, Builtins, Use }; Action action = Done;
    const auto editable = asset.discovered && asset.sourceIsDirectory && asset.referencePath.isEmpty() && asset.sourceProblem.isEmpty();
    auto *row1 = new QHBoxLayout; auto *row2 = new QHBoxLayout;
    const auto button = [&](const QString &text, const char *name, Action choice, QHBoxLayout *row)
    {
        auto *control = new ElaPushButton(text,&form); control->setObjectName(name); row->addWidget(control);
        connect(control,&QPushButton::clicked,&form,[&,choice] { action = choice; form.accept(); }); return control;
    };
    auto *create = button("New command…","newHdlCommandButton",Create,row1); create->setEnabled(editable);
    auto *edit = button("Edit mapping…","editHdlCommandButton",Edit,row1);
    auto *open = button("Open source","openHdlSourceButton",Open,row1);
    auto *save = button("Save version","saveHdlVersionButton",Save,row2); save->setEnabled(editable);
    auto *builtins = button("Add built-ins","addHdlBuiltinsButton",Builtins,row2); builtins->setEnabled(!m_library.isEmpty());
    auto *use = button("Use selected","useHdlCommandButton",Use,row2);
    form.body->addLayout(row1); form.body->addLayout(row2);
    const auto selected = [&] { return view->currentIndex().data(Qt::UserRole).toMap(); };
    const auto update = [&]
    {
        const auto entry = selected(); const bool own = editable && entry.value("assetId") == asset.id;
        details->setText(entry.value("assetName").toString() + " · " + entry.value("revision").toString() + '\n' +
            entry.value("description").toString() + '\n' + entry.value("diagnostics").toStringList().join('\n'));
        edit->setEnabled(own); open->setEnabled(own);
        use->setEnabled(entry.value("availability") == "available" && !m_library.isEmpty());
    };
    connect(view->selectionModel(),&QItemSelectionModel::currentChanged,&form,[&] { update(); }); update();
    if (form.exec() != QDialog::Accepted || m_pendingContext) return;
    const auto entry = selected();
    if (action == Create) editHdlCommand(asset);
    else if (action == Edit)
    {
        try
        {
            const auto doc = QJsonDocument::fromJson(HdlCommands::workingManifest(asset)).object();
            for (const auto &item : doc.value("commands").toArray())
                if (item.toObject().value("id").toString() == entry.value("mappingId").toString())
                { editHdlCommand(asset,item.toObject()); return; }
            notice("This saved command has no current mapping. Restore or create its source mapping first.",true);
        }
        catch (const std::exception &error) { notice(QString::fromUtf8(error.what()),true); }
    }
    else if (action == Open)
    {
        run("Opening command source…", [asset, entry]
        { return SnapshotLibrary::prepareFile(asset,"current",entry.value("sourceFile").toString(),QStandardPaths::writableLocation(QStandardPaths::CacheLocation)); },
        [this](const SnapshotResult &result) { if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.exportedPath))) notice("Cannot open the command source editor.",true); });
    }
    else if (action == Save)
        run("Checking and saving HDL commands…",[asset] { return HdlCommands::saveVersion(asset); },
            [this](const SnapshotResult &result) { applyAssetResult(result,result.snapshot.id); },true);
    else if (action == Builtins)
    {
        const auto library = m_library;
        run("Adding HDL packages…",[library] { return HdlCommands::installBuiltins(library); },
            [this](const SnapshotResult &result) { applyAssetResult(result,result.snapshot.id); },true);
    }
    else if (action == Use)
    {
        const auto response = useHdlCommand({{"requestId",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"generation",QString::number(m_hdlGeneration)}, {"commandId",entry.value("commandId")},{"revision",entry.value("revision")}});
        if (response.value("status") != "accepted") notice(response.value("message").toString(),true);
    }
}
void BrowserPanel::editHdlCommand(CatalogAsset asset, const QJsonObject &command)
{
    QByteArray before;
    try { before = HdlCommands::workingManifest(asset); }
    catch (const std::exception &error) { notice(QString::fromUtf8(error.what()),true); return; }
    Form form(this,command.isEmpty() ? "New HDL command" : "Edit HDL command mapping","Save draft");
    form.setObjectName("hdlCommandEditor"); form.setMinimumWidth(570);
    auto *name = new EnglishLineEdit(&form); name->setObjectName("hdlCommandName"); name->setPlaceholderText("Search name"); name->setText(command.value("name").toString());
    auto *aliases = new EnglishLineEdit(&form); aliases->setObjectName("hdlCommandAliases"); aliases->setPlaceholderText("Aliases, separated by commas");
    QStringList aliasValues; for (const auto &value : command.value("aliases").toArray()) aliasValues.append(value.toString()); aliases->setText(aliasValues.join(", "));
    auto *description = new EnglishLineEdit(&form); description->setObjectName("hdlCommandDescription"); description->setPlaceholderText("Description"); description->setText(command.value("description").toString());
    auto *mode = new ElaComboBox(&form); mode->setObjectName("hdlCommandMode");
    mode->addItem("Create task in a new package file","task"); mode->addItem("Create function in a new package file","function"); mode->addItem("Bind an existing source symbol",QString());
    if (!command.isEmpty()) { mode->setCurrentIndex(2); mode->setEnabled(false); }
    auto *file = new ElaComboBox(&form); file->setObjectName("hdlCommandFile"); file->setEditable(true);
    for (const auto &path : asset.workingFiles) if (path.endsWith("_package.sv")) file->addItem(path);
    file->setCurrentText(command.value("file").toString("user_package.sv"));
    auto *package = new EnglishLineEdit(&form); package->setObjectName("hdlCommandPackage"); package->setPlaceholderText("Actual package identifier"); package->setText(command.value("package").toString("user_package"));
    auto *symbol = new EnglishLineEdit(&form); symbol->setObjectName("hdlCommandSymbol"); symbol->setPlaceholderText("package::task_or_function"); symbol->setText(command.value("symbol").toString("user_package::user_action"));
    auto *dependencies = new EnglishPlainTextEdit(&form); dependencies->setObjectName("hdlCommandFiles"); dependencies->setMaximumHeight(90);
    QStringList files; for (const auto &path : command.value("files").toArray()) files.append(path.toString());
    dependencies->setPlainText(files.isEmpty() ? "user_package.sv" : files.join('\n'));
    for (auto *control : QList<QWidget *>{name,aliases,description,mode,file,package,symbol}) form.body->addWidget(control);
    form.message("Dependency .sv files, in compile order, one asset-relative path per line:"); form.body->addWidget(dependencies);
    form.message("The mapping does not define the HDL signature. Edit the ordinary source file, then Save version to validate and archive it. Invalid drafts remain available for correction.");
    connect(name,&QLineEdit::textChanged,&form,[&] { form.acceptButton->setEnabled(!name->text().trimmed().isEmpty()); });
    form.acceptButton->setEnabled(!name->text().trimmed().isEmpty());
    if (form.exec() != QDialog::Accepted || m_pendingContext) return;
    auto updated = command;
    updated.insert("name",name->text().trimmed()); updated.insert("description",description->text());
    QStringList aliasList; for (const auto &value : aliases->text().split(',')) if (!value.trimmed().isEmpty()) aliasList.append(value.trimmed());
    updated.insert("aliases",QJsonArray::fromStringList(aliasList)); updated.insert("file",file->currentText().trimmed());
    updated.insert("package",package->text().trimmed()); updated.insert("symbol",symbol->text().trimmed());
    QStringList inputFiles; for (const auto &value : dependencies->toPlainText().split('\n')) if (!value.trimmed().isEmpty()) inputFiles.append(value.trimmed());
    updated.insert("files",QJsonArray::fromStringList(inputFiles));
    const auto kind = mode->currentData().toString(), expected = HdlCommands::digest(before);
    run("Saving HDL command draft…",[asset,updated,kind,expected] { return HdlCommands::edit(asset,updated,kind,expected); },
        [this](const SnapshotResult &result) { applyAssetResult(result,"current"); notice("Command draft saved. Open source to edit the implementation, then Save version."); },true);
}
}
