#pragma once

#include <QByteArray>
#include <QtCore/qglobal.h>
class QWidget;
class QObject;

// Native surface v1. Load only when the ABI string matches the host's Qt,
// pointer size and compiler. Deploy xips-browser-impl.dll and XipsEla.dll beside
// xips-browser.dll. The entry point loads dependencies from its own directory; do not
// substitute a host's ElaWidgetTools.dll. Keep the library loaded for the host
// process lifetime (QLibrary::PreventUnloadHint), including background workers.
// Call on the QApplication thread. The factory initializes its private Ela
// runtime and preserves the host application identity/font. Failure returns null.
// The returned QWidget exposes these public Qt invokables:
// setContext(QString library, QString workspace), collectPaths(QStringList),
// importWorkingFiles(QStringList), isCatalogBusy()->bool, setDarkTheme(bool),
// revealAsset(QString), refresh(), saveState()->QVariantMap,
// restoreState(QVariantMap).
// Empty library retains the panel library; initial fallback is XIPS_LIBRARY or
// xIPs settings. State includes library, workingChecks and page; old state works.
// Context/state switches queue together while an operation is active. Workspace
// is always supplied by the host, never restored from a saved state.
// Optional host invokables: destinationError(QString)->QString,
// exportCompleted(QVariantMap)->QString (empty on success),
// collectionSources()->QStringList (saved source files, empty on cancellation).
using XipsCreateBrowserV1 = QWidget *(*)(QWidget *, QObject *);
using XipsBrowserAbiV1 = const char *(*)();
// Optional JSON capability/provenance export; no QWidget creation is required.
using XipsBrowserCapabilitiesV1 = const char *(*)();
// Optional xips_browser_last_error_v1(): diagnostic for a null factory result.
using XipsBrowserLastErrorV1 = const char *(*)();

inline QByteArray xipsExpectedBrowserAbi()
{
    QByteArray result = "xips-browser/v1;qt=" QT_VERSION_STR ";bits=";
    result += QByteArray::number(sizeof(void *) * 8);
#if defined(__GNUC__)
    result += ";gcc=" + QByteArray::number(__GNUC__);
#elif defined(_MSC_VER)
    result += ";msvc=" + QByteArray::number(_MSC_VER);
#else
    result += ";compiler=unknown";
#endif
    result += ";ela=454cac2d-p27";
    return result;
}

// Additive Archive project surface (native ABI remains v1):
// openArchiveProject() opens/reuses the BrowserPanel-owned tool without a library.
// isCatalogBusy() includes archive/scanning work; hosts must keep the panel alive
// and leave Cancel reachable until this returns false. setContext/restoreState
// defer during this work. Forced panel destruction cancels and joins its workers.
// Resolve helper files using the layout in xips-native-runtime.json.
