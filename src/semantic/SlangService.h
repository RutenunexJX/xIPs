#pragma once

#include "assetcore/Asset.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>

namespace xips {

class SlangService {
public:
    struct Request {
        AssetRecord asset;
        QString executable;
        qint64 generation = 0;
        int timeoutMs = 120000;
    };

    [[nodiscard]] SemanticResult analyze(
        const Request &request,
        const std::atomic_bool *cancelled = nullptr) const;

    [[nodiscard]] static SemanticResult parseArtifacts(
        const QByteArray &astJson,
        const QByteArray &cstJson,
        const QByteArray &diagnosticsJson,
        const QStringList &includeDependencies,
        const QStringList &commandLineDefines,
        const QString &engineVersion,
        qint64 generation,
        const QString &contentHash);

    [[nodiscard]] static QString locateExecutable(const QString &explicitPath = {});
    [[nodiscard]] static bool supports(const AssetRecord &asset);

private:
    static QString queryVersion(const QString &executable, int timeoutMs);
};

} // namespace xips
