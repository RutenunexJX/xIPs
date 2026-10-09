#pragma once
#include <QThread>
#include <QStringList>

namespace xips::archive {
struct DiscoveredProject { QString xpr; QString version; QString error; };
class ArchiveScan final : public QThread {
    Q_OBJECT
public:
    explicit ArchiveScan(QString root, QObject* parent = nullptr);
    ~ArchiveScan() override;
    QList<DiscoveredProject> projects;
    QString error;
    int skippedDirectories = 0;
    bool cancelled = false;
signals:
    void progress(int found, const QString& directory);
protected:
    void run() override;
private:
    QString root_;
};
}
