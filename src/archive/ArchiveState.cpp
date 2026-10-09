#include "ArchiveState.h"
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
namespace xips::archive {
namespace { constexpr auto key = "tools/project.archive/state"; }
ArchiveState::ArchiveState(QString file, QString legacyFile)
    : file_(std::move(file)), legacyFile_(std::move(legacyFile))
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (file_.isEmpty()) file_ = base + "/xIPs/archive.ini";
    if (legacyFile_.isEmpty()) legacyFile_ = base + "/FpgaToolbox/settings.ini";
}
QVariantMap ArchiveState::load() const
{
    QSettings own(file_, QSettings::IniFormat);
    if (own.contains(key)) return own.value(key).toMap();
    if (!QFileInfo::exists(legacyFile_)) return {};
    const QSettings legacy(legacyFile_, QSettings::IniFormat);
    const auto state = legacy.value(key).toMap();
    if (state.value("schema").toInt() != 1) return {};
    save(state);
    return state;
}
bool ArchiveState::save(const QVariantMap& state) const
{
    QSettings own(file_, QSettings::IniFormat);
    own.setValue(key, state);
    own.sync();
    return own.status() == QSettings::NoError;
}
}
