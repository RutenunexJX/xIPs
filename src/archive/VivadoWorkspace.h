#pragma once
#include "ArchiveEngine.h"
#include <QProcessEnvironment>
#include <functional>

namespace xips::archive {
using Check = std::function<void()>;
using Log = std::function<void(const QString&)>;
// Destination must not exist. Validates references before copying; never opens the source in Vivado.
ProjectInfo isolateProject(const QString& xpr, const QString& destination, const Check& check = [] {}, const Log& log = [](const QString&) {});
// Contained child process with an isolated startup profile. Caller supplies only owned working files.
void runBatch(const Installation& vivado, const QString& control, const QByteArray& script,
              const QMap<QString, QString>& variables, const Check& check, const Log& log);
void requirePlainPath(const QString& path);
bool isWithin(const QString& path, const QString& root);
}
