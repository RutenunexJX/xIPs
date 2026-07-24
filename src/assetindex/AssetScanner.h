#pragma once

#include "assetcore/Asset.h"
#include "manifest/ManifestService.h"

#include <QString>
#include <QStringList>

#include <atomic>

namespace xips {

class AssetScanner {
public:
    [[nodiscard]] ScanResult scan(
        const QString &libraryRoot,
        const std::atomic_bool *cancelled = nullptr) const;

    [[nodiscard]] static QStringList assetFiles(const QString &assetRoot);
    [[nodiscard]] static QString contentHash(const Manifest &manifest,
                                             const QString &assetRoot);

private:
    void discoverManifests(const QString &directory,
                           QStringList &manifests,
                           const std::atomic_bool *cancelled) const;

    ManifestService m_manifestService;
};

} // namespace xips
