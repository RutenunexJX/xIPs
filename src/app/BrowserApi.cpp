#include "xips/BrowserApi.h"
#include "BuildCapabilities.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QThread>
#include <bit>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
thread_local QByteArray lastError;
QString componentDirectory()
{
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&componentDirectory), &module)) return {};
    wchar_t path[32768];
    const auto size = GetModuleFileNameW(module, path, 32768);
    return size && size < 32768 ? QFileInfo(QString::fromWCharArray(path, int(size))).absolutePath() : QString();
#else
    Dl_info info{};
    return dladdr(reinterpret_cast<void *>(&componentDirectory), &info) && info.dli_fname
        ? QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath() : QString();
#endif
}
XipsCreateBrowserV1 implementation()
{
    static XipsCreateBrowserV1 factory = nullptr;
    if (factory) return factory;
    const auto directory = componentDirectory();
    if (directory.isEmpty()) { lastError = "Cannot locate the xIPs component directory."; return nullptr; }
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(directory + "/xips-browser-impl.dll");
    // Keep this loader reference for the process lifetime: workers can outlive a panel.
    // Resolve private dependencies here and shared Qt in the host executable directory,
    // without consulting CWD/PATH or changing the host's DLL search configuration.
    static HMODULE module = nullptr;
    if (!module) module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    {
        lastError = QStringLiteral("Cannot load %1 (Windows error %2). Deploy xips-browser-impl.dll and XipsEla.dll together, with matching Qt dependencies in the component or application directory.")
            .arg(path).arg(GetLastError()).toUtf8();
        return nullptr;
    }
    factory = std::bit_cast<XipsCreateBrowserV1>(GetProcAddress(module, "xips_create_browser_impl_v1"));
#else
    static QLibrary module(directory + "/xips-browser-impl");
    module.setLoadHints(QLibrary::PreventUnloadHint);
    if (!module.load()) { lastError = module.errorString().toUtf8(); return nullptr; }
    factory = reinterpret_cast<XipsCreateBrowserV1>(module.resolve("xips_create_browser_impl_v1"));
#endif
    if (!factory) lastError = "The xIPs implementation does not export its factory.";
    return factory;
}
}

extern "C" Q_DECL_EXPORT const char *xips_browser_last_error_v1()
{
    return lastError.constData();
}

extern "C" Q_DECL_EXPORT const char *xips_browser_capabilities_v1()
{
    return xips::buildCapabilities;
}

extern "C" Q_DECL_EXPORT const char *xips_browser_abi_v1()
{
    static const QByteArray abi = xipsExpectedBrowserAbi();
    return abi.constData();
}

extern "C" Q_DECL_EXPORT QWidget *xips_create_browser_v1(QWidget *parent, QObject *host)
{
    const auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    lastError.clear();
    if (!application || QThread::currentThread() != application->thread() ||
        (host && host->thread() != application->thread()))
    {
        lastError = "Create the xIPs browser and host bridge on the QApplication thread.";
        return nullptr;
    }
    try
    {
        const auto factory = implementation();
        return factory ? factory(parent, host) : nullptr;
    }
    catch (...)
    {
        lastError = "Cannot construct the xIPs browser.";
        return nullptr;
    }
}
