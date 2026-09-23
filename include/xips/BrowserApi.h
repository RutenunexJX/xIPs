#pragma once

#include <QByteArray>
#include <QtCore/qglobal.h>
class QWidget;
class QObject;

// Native surface v1. Load only when the ABI string matches the host's Qt,
// pointer size and compiler. Keep the library loaded until its widgets die.
// The returned QWidget exposes these public Qt invokables:
// setContext(QString library, QString workspace), collectPaths(QStringList),
// revealAsset(QString), refresh(), saveState()->QVariantMap,
// restoreState(QVariantMap).
// Optional host invokables: destinationError(QString)->QString,
// exportCompleted(QVariantMap)->QString (empty on success),
// collectionSources()->QStringList (saved source files, empty on cancellation).
using XipsCreateBrowserV1 = QWidget *(*)(QWidget *, QObject *);
using XipsBrowserAbiV1 = const char *(*)();

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
    return result;
}
