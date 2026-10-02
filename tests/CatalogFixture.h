#pragma once
#include "library/SnapshotLibrary.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>

// Explicitly seed a pre-existing catalog with one saved revision. New-product
// creation tests call SnapshotLibrary::create directly and expect no files/history.
inline xips::SnapshotResult savedCatalogFixture(const QString &library, const xips::CatalogDefinition &definition)
{
    auto result = xips::SnapshotLibrary::create(library, definition);
    if (!result.ok) return result;
    if (definition.source.isEmpty())
    {
        QDir().mkpath(result.asset.root + "/rtl");
        QFile source(result.asset.root + "/rtl/" + definition.name + ".sv");
        if (!source.open(QIODevice::WriteOnly)) qFatal("Cannot seed catalog fixture");
        source.write("module " + definition.name.toUtf8() + "; endmodule\n");
    }
    result = xips::SnapshotLibrary::saveCurrent(result.asset);
    if (!result.ok) return result;
    auto document = result.asset.document;
    document.remove("workingArea"); // Exercise catalogs produced before empty workspaces existed.
    QFile metadata(result.asset.historyRoot + "/.xips.json");
    if (!metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("Cannot seed catalog metadata");
    metadata.write(QJsonDocument(document).toJson());
    metadata.close();
    result.asset = xips::SnapshotLibrary::describe(result.asset).asset;
    return result;
}
