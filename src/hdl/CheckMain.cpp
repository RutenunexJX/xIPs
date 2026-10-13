#include <slang/ast/Compilation.h>
#include <slang/ast/symbols/ClassSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/symbols/SubroutineSymbols.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/syntax/SyntaxVisitor.h>
#include <slang/text/SourceManager.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <cstdio>
#include <stdexcept>

using namespace slang;
using namespace slang::ast;
namespace {
QString text(std::string_view value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
void require(bool value, const QString &reason) { if (!value) throw std::runtime_error(reason.toStdString()); }
struct DependencyOrder : syntax::SyntaxVisitor<DependencyOrder> {
    int position = 0;
    QString owner;
    QMap<QString,int> packages;
    struct Reference { QString package, owner; int position; };
    QList<Reference> references;
    void handle(const syntax::ModuleDeclarationSyntax &node)
    {
        const auto previous = owner;
        if (node.kind == syntax::SyntaxKind::PackageDeclaration)
        {
            owner = text(node.header->name.valueText());
            packages.insert(owner, ++position);
        }
        visitDefault(node); owner = previous;
    }
    void handle(const syntax::PackageImportItemSyntax &node)
    { references.append({text(node.package.valueText()), owner, ++position}); }
    void handle(const syntax::ScopedNameSyntax &node)
    {
        if (node.separator.valueText() == "::" && node.left->kind == syntax::SyntaxKind::IdentifierName)
            references.append({text(node.left->as<syntax::IdentifierNameSyntax>().identifier.valueText()), owner, ++position});
        visitDefault(node);
    }
    void validate() const
    {
        for (const auto &reference : references)
            if (reference.package != reference.owner && packages.contains(reference.package))
                require(packages.value(reference.package) < reference.position,
                    "Dependency cycle or compile order: package " + reference.package + " must precede its consumer " + reference.owner + '.');
    }
};
QString sourcePath(const QString &root, const QString &relative)
{
    require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) && !relative.contains('\\') &&
        !relative.split('/').contains("..") && !relative.contains(':'), "Invalid relative source path: " + relative);
    const auto info = QFileInfo(QDir(root).filePath(relative));
    const auto canonical = info.canonicalFilePath();
    require(info.isFile() && !canonical.isEmpty() && canonical.startsWith(root + '/', Qt::CaseInsensitive),
            "Source is missing or outside the asset: " + relative);
    return canonical;
}
QJsonObject analyze(const QJsonObject &request)
{
    const auto root = QFileInfo(request.value("root").toString()).canonicalFilePath();
    require(!root.isEmpty(), "Source root is unavailable.");
    const auto files = request.value("files").toArray();
    require(!files.isEmpty() && files.size() <= 256, "Choose 1 to 256 dependency source files.");
    SourceManager sources;
    sources.addUserDirectories(root.toStdString());
    Compilation compilation;
    DependencyOrder dependencyOrder;
    QSet<QString> supplied;
    QSet<QString> payload;
    for (const auto &path : request.value("payload").toArray())
        payload.insert(sourcePath(root,path.toString()).toCaseFolded());
    for (const auto &value : files)
    {
        const auto relative = value.toString();
        const auto path = sourcePath(root, relative);
        require(!supplied.contains(path.toCaseFolded()), "Duplicate dependency input: " + relative);
        supplied.insert(path.toCaseFolded());
        require(QFileInfo(path).size() <= 16 * 1024 * 1024, "Source exceeds the 16 MiB analysis limit.");
        const auto tree = syntax::SyntaxTree::fromFile(path.toStdString(), sources);
        require(bool(tree), "Cannot parse source: " + relative);
        (*tree)->root().visit(dependencyOrder);
        compilation.addSyntaxTree(*tree);
    }
    dependencyOrder.validate();
    QJsonArray entries;
    for (const auto &value : request.value("commands").toArray())
    {
        auto entry = value.toObject();
        QJsonArray errors;
        const auto qualified = entry.value("symbol").toString().split("::");
        const auto packageName = entry.value("package").toString();
        const auto package = compilation.getPackage(packageName.toStdString());
        const Symbol *symbol = nullptr;
        QJsonArray typeParameters;
        if (package && (qualified.size() == 2 || qualified.size() == 3) && qualified.first() == packageName)
        {
            symbol = package->find(qualified[1].toStdString());
            if (qualified.size() == 3 && symbol)
            {
                const ClassType *classType = nullptr;
                if (symbol->kind == SymbolKind::GenericClassDef)
                {
                    const auto type = symbol->as<GenericClassDefSymbol>().getDefaultSpecialization(*package);
                    if (type && type->kind == SymbolKind::ClassType) classType = &type->as<ClassType>();
                }
                else if (symbol->kind == SymbolKind::ClassType) classType = &symbol->as<ClassType>();
                if (classType)
                {
                    for (const auto &member : classType->members())
                        if (member.kind == SymbolKind::TypeParameter) typeParameters.append(text(member.name));
                    symbol = classType->find(qualified[2].toStdString());
                }
                else symbol = nullptr;
            }
        }
        if (!symbol || symbol->kind != SymbolKind::Subroutine)
            errors.append("The source does not define the requested package task/function: " + entry.value("symbol").toString());
        else
        {
            const auto &subroutine = symbol->as<SubroutineSymbol>();
            if (qualified.size() == 3 && !subroutine.flags.has(MethodFlags::Static))
                errors.append("Bind a static class method or a package-level task/function.");
            if (subroutine.visibility != Visibility::Public || subroutine.flags.has(MethodFlags::Pure | MethodFlags::DPIImport))
                errors.append("The command needs a public implementation in this source bundle.");
            const auto actualFile = QString::fromStdString(sources.getFullPath(symbol->location.buffer()).generic_string());
            const auto expectedFile = sourcePath(root, entry.value("file").toString());
            if (QFileInfo(actualFile).canonicalFilePath() != expectedFile)
                errors.append("The symbol is declared in a different source file.");
            entry.insert("kind", subroutine.subroutineKind == SubroutineKind::Task ? "task" : "function");
            entry.insert("returnType", QString::fromStdString(subroutine.getReturnType().toString()));
            QJsonArray parameters;
            for (const auto *argument : subroutine.getArguments())
            {
                QString direction = "input";
                if (argument->direction == ArgumentDirection::Out) direction = "output";
                else if (argument->direction == ArgumentDirection::InOut) direction = "inout";
                else if (argument->direction == ArgumentDirection::Ref) direction = "ref";
                parameters.append(QJsonObject{{"name",text(argument->name)}, {"type",QString::fromStdString(argument->getType().toString())},
                    {"direction",direction}, {"hasDefault",argument->getDefaultValue() != nullptr}});
            }
            entry.insert("parameters", parameters);
            entry.insert("typeParameters", typeParameters);
            subroutine.getBody();
        }
        entry.insert("diagnostics", errors);
        entries.append(entry);
    }
    const auto &diagnostics = compilation.getAllDiagnostics();
    bool failed = false;
    for (const auto &diagnostic : diagnostics) failed |= diagnostic.isError();
    QJsonArray errors;
    if (failed) errors.append(QString::fromStdString(DiagnosticEngine::reportAll(sources, diagnostics)));
    // Included source must be self-contained and part of the archived payload.
    for (const auto buffer : sources.getAllBuffers())
    {
        const auto path = QString::fromStdString(sources.getFullPath(buffer).generic_string());
        if (path.isEmpty()) continue;
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.startsWith(root + '/', Qt::CaseInsensitive))
            errors.append("Included source escapes the asset: " + path);
        else if (!payload.contains(canonical.toCaseFolded()))
            errors.append("Included source is not in the versioned payload: " + path);
    }
    return {{"schema","xips.hdl-analysis/v1"},{"engine","slang-10.0"},{"entries",entries},{"diagnostics",errors}};
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QJsonObject result;
    try
    {
        QFile input; require(input.open(stdin, QIODevice::ReadOnly), "Cannot read analysis input.");
        const auto bytes = input.read(1024 * 1024 + 1);
        require(bytes.size() <= 1024 * 1024, "Analysis request exceeds 1 MiB.");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid analysis request JSON.");
        result = analyze(document.object());
    }
    catch (const std::exception &error)
    {
        result = {{"schema","xips.hdl-analysis/v1"},{"entries",QJsonArray{}},{"diagnostics",QJsonArray{QString::fromUtf8(error.what())}}};
    }
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    return 0;
}
