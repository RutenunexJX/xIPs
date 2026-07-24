#pragma once

#include "assetcore/Asset.h"
#include "manifest/ManifestService.h"

#include <QList>
#include <QString>

#include <atomic>

namespace xips {

class AssetScanner {
public:
    ScanResult scan(const QList<LibraryRoot> &roots,
                    const std::atomic_bool *cancelled = nullptr) const;

    static QString contentHash(const Manifest &manifest, const QString &assetRoot);

private:
    void discoverManifests(const QString &directory,
                           QStringList &manifests,
                           const std::atomic_bool *cancelled) const;

    ManifestService m_manifestService;
};

} // namespace xips
