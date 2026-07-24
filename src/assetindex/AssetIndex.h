#pragma once

#include "assetcore/Asset.h"

#include <QList>
#include <QString>

namespace xips {

enum class SemanticPublishStatus {
    Published,
    Stale,
    Missing,
    Error
};

enum class TestPublishStatus {
    Published,
    Stale,
    Missing,
    Error
};

class AssetIndex {
public:
    explicit AssetIndex(QString databasePath);

    [[nodiscard]] QString databasePath() const;
    bool initialize(QString *error = nullptr) const;
    qint64 reserveGeneration(QString *error = nullptr) const;
    bool rebuild(const QList<AssetRecord> &assets,
                 qint64 generation,
                 QString *error = nullptr) const;
    bool updateAssets(const QList<AssetRecord> &assets,
                      qint64 generation,
                      QString *error = nullptr) const;
    bool removeAssets(const QStringList &assetIds,
                      qint64 generation,
                      QString *error = nullptr) const;
    bool markUsed(const QString &assetId,
                  const QDateTime &when,
                  QString *error = nullptr) const;
    SemanticPublishStatus publishSemantic(const QString &assetId,
                                          const QString &expectedContentHash,
                                          qint64 expectedGeneration,
                                          const SemanticResult &semantic,
                                          QString *error = nullptr) const;
    TestPublishStatus publishTestResult(const QString &assetId,
                                        const QString &expectedContentHash,
                                        qint64 expectedGeneration,
                                        const TestResult &result,
                                        QString *error = nullptr) const;
    QList<TestResult> testResults(const QString &assetId,
                                  QString *error = nullptr) const;

    QList<SearchHit> search(const QString &query,
                            AssetType typeFilter = AssetType::Unknown,
                            int limit = 250,
                            QString *error = nullptr) const;
    QList<AssetRecord> allAssets(QString *error = nullptr) const;

private:
    QString m_databasePath;
};

} // namespace xips
