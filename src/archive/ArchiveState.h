#pragma once
#include <QString>
#include <QVariantMap>
namespace xips::archive {
class ArchiveState final {
public:
    explicit ArchiveState(QString file = {}, QString legacyFile = {});
    QVariantMap load() const;
    bool save(const QVariantMap& state) const;
    const QString& fileName() const { return file_; }
private:
    QString file_, legacyFile_;
};
}
