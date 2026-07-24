#pragma once

#include <QString>

#include <atomic>

namespace xips {

struct GitInfo {
    bool available = false;
    bool dirty = false;
    bool timedOut = false;
    bool cancelled = false;
    QString repositoryRoot;
    QString commit;
    QString tag;
    QString branch;
    QString error;
};

class GitService {
public:
    explicit GitService(QString executable = {});

    [[nodiscard]] GitInfo query(
        const QString &path,
        int timeoutMs = 3000,
        const std::atomic_bool *cancelled = nullptr) const;

private:
    QString m_executable;
};

} // namespace xips
