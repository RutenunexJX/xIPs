#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace {

QString optionValue(const QStringList &arguments, const QString &option)
{
    const qsizetype index = arguments.indexOf(option);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return !path.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--version"))) {
        QTextStream(stdout) << "slang version 11.0-test\n";
        return 0;
    }

    const QByteArray ast = R"JSON({
      "name": "$root",
      "kind": "Root",
      "members": [
        {
          "name": "common_pkg",
          "kind": "Package",
          "source_file": "common_pkg.sv",
          "members": [
            {
              "name": "WORD_WIDTH",
              "kind": "Parameter",
              "type": "int unsigned",
              "value": "32",
              "isLocal": true
            }
          ]
        },
        {
          "name": "child",
          "kind": "Definition",
          "source_file": "child.sv",
          "definitionKind": "Module"
        },
        {
          "name": "bus_if",
          "kind": "Definition",
          "source_file": "bus_if.sv",
          "definitionKind": "Interface"
        },
        {
          "name": "top",
          "kind": "Definition",
          "source_file": "top.sv",
          "definitionKind": "Module"
        },
        {
          "name": "top",
          "kind": "Instance",
          "source_file": "top.sv",
          "body": {
            "name": "top",
            "kind": "InstanceBody",
            "source_file": "top.sv",
            "members": [
              {
                "name": "WIDTH",
                "kind": "Parameter",
                "type": {"kind": "IntegerType", "name": "int unsigned"},
                "value": "8",
                "isLocal": false
              },
              {
                "name": "clk_i",
                "kind": "Port",
                "type": {"kind": "ScalarType", "name": "logic"},
                "direction": "In"
              },
              {
                "name": "data_o",
                "kind": "Port",
                "type": {
                  "kind": "PackedArrayType",
                  "elementType": {"kind": "ScalarType", "name": "logic"},
                  "range": {"left": 7, "right": 0}
                },
                "direction": "Out"
              },
              {
                "name": "common_pkg",
                "kind": "ExplicitImport",
                "packageName": "common_pkg",
                "importedName": "word_t"
              },
              {
                "name": "u_child",
                "kind": "Instance",
                "body": {
                  "name": "child",
                  "kind": "InstanceBody",
                  "members": [
                    {
                      "name": "WIDTH",
                      "kind": "Parameter",
                      "type": "int unsigned",
                      "value": "8",
                      "isLocal": false
                    }
                  ]
                }
              },
              {
                "name": "bus",
                "kind": "Instance",
                "body": {
                  "name": "bus_if",
                  "kind": "InstanceBody",
                  "members": []
                }
              }
            ]
          }
        }
      ]
    })JSON";

    const QByteArray cst = R"JSON({
      "kind": "CompilationUnit",
      "members": [
        {"kind": "IncludeDirective", "path": "rtl/include/defs.svh"},
        {"kind": "DefineDirective", "name": "LOCAL_WIDTH", "value": "8"}
      ]
    })JSON";
    const QByteArray diagnostics = R"JSON([
      {
        "severity": "warning",
        "code": "Wexample",
        "message": "fixture warning",
        "location": {"fileName": "top.sv", "line": 12, "column": 4}
      }
    ])JSON";

    const QString astPath = optionValue(arguments, QStringLiteral("--ast-json"));
    const QString cstPath = optionValue(arguments, QStringLiteral("--cst-json"));
    const QString diagnosticsPath = optionValue(arguments, QStringLiteral("--diag-json"));
    const QString dependenciesPath = optionValue(arguments, QStringLiteral("--include-deps"));
    const QByteArray dependency =
        QDir::current().absoluteFilePath(QStringLiteral("rtl/include/defs.svh")).toUtf8()
        + '\n';

    return writeFile(astPath, ast) && writeFile(cstPath, cst)
                   && writeFile(diagnosticsPath, diagnostics)
                   && writeFile(dependenciesPath, dependency)
               ? 0
               : 2;
}
