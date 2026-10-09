#include "ArchiveRuntime.h"
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <dlfcn.h>
#endif
namespace xips::archive {
QString bundledCompressor()
{
    QString file;
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&bundledCompressor), &module)) return {};
    wchar_t path[32768];
    const auto size = GetModuleFileNameW(module, path, 32768);
    if (size && size < 32768) file = QString::fromWCharArray(path, int(size));
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&bundledCompressor), &info) && info.dli_fname)
        file = QString::fromLocal8Bit(info.dli_fname);
#endif
    return file.isEmpty() ? QString() : QFileInfo(file).absolutePath() + "/tools/7zip/7za.exe";
}
}
