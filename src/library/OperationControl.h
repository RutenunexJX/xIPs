#pragma once

#include <QString>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace xips
{
class OperationCancelled final : public std::runtime_error
{
  public:
    OperationCancelled()
        : std::runtime_error("Operation cancelled before publication. Existing "
                             "revisions and destinations are unchanged.")
    {
    }
};

// One control per worker. Cancellation and publication compete through a single
// CAS. Immutable unreferenced content objects may remain after an interrupted
// save.
class OperationControl
{
  public:
    enum State
    {
        Running,
        CancelRequested,
        Publishing
    };
    std::atomic<State> state{Running};
    bool cancel()
    {
        auto expected = Running;
        return state.compare_exchange_strong(expected, CancelRequested);
    }
    QString status() const
    {
        std::lock_guard lock(m_mutex);
        return m_status;
    }
    void progress(const QString &phase, qint64 done = -1, qint64 total = -1)
    {
        if (state.load() == CancelRequested)
            throw OperationCancelled();
        if (!phase.isEmpty())
        {
            std::lock_guard lock(m_mutex);
            m_status = done < 0
                           ? phase
                           : QStringLiteral("%1 · %2 / %3 bytes").arg(phase).arg(done).arg(total);
        }
    }
    void publish()
    {
        auto expected = Running;
        if (!state.compare_exchange_strong(expected, Publishing) && expected == CancelRequested)
            throw OperationCancelled();
        progress(QStringLiteral("Publishing verified result…"));
    }

  private:
    mutable std::mutex m_mutex;
    QString m_status;
};

class OperationScope
{
  public:
    explicit OperationScope(OperationControl *control) : previous(active) { active = control; }
    ~OperationScope() { active = previous; }
    OperationScope(const OperationScope &) = delete;
    OperationScope &operator=(const OperationScope &) = delete;
    static void checkpoint(const QString &phase = {}, qint64 done = -1, qint64 total = -1)
    {
        if (active)
            active->progress(phase, done, total);
    }
    static void publish()
    {
        if (active)
            active->publish();
    }

  private:
    OperationControl *previous;
    inline static thread_local OperationControl *active = nullptr;
};
} // namespace xips
