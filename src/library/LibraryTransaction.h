#pragma once
#include <QString>
#include <memory>
class QLockFile;
namespace xips
{
// All catalog writers share this lock. Nested calls on the same thread reuse
// it.
class LibraryTransaction final
{
  public:
    explicit LibraryTransaction(const QString &library);
    ~LibraryTransaction();
    LibraryTransaction(const LibraryTransaction &) = delete;
    LibraryTransaction &operator=(const LibraryTransaction &) = delete;

  private:
    QString key;
    std::unique_ptr<QLockFile> lock;
};
} // namespace xips
