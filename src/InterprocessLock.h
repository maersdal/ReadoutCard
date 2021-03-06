/// \file InterprocessLock.h
/// \brief Definitions for InterprocessLock class
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#ifndef ALICEO2_READOUTCARD_INTERPROCESSMUTEX_H_
#define ALICEO2_READOUTCARD_INTERPROCESSMUTEX_H_

#include <mutex>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/exception/errinfo_errno.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/scoped_ptr.hpp>
#include "Common/System.h"
#include "ExceptionInternal.h"
#include "Utilities/SmartPointer.h"

namespace AliceO2 {
namespace roc {
namespace Interprocess {

/// This lock uses two mutexes internally to help detect inconsistent state.
/// When the process is terminated unexpectedly, the The boost::file_lock is unlocked automatically, while the
/// boost::interprocess_mutex IS NOT unlocked automatically. This gives us some detection ability.
class Lock
{
  public:
    using FileMutex = boost::interprocess::file_lock;
    using NamedMutex = boost::interprocess::named_mutex;
    using FileLock = std::unique_lock<FileMutex>;
    using NamedLock = std::unique_lock<NamedMutex>;

    Lock(const boost::filesystem::path& lockFilePath, const std::string& namedMutexName, bool waitOnLock = false)
    {
      Common::System::touchFile(lockFilePath.string());

      // Construct mutexes
      try {
        Utilities::resetSmartPtr(mFileMutex, lockFilePath.c_str());
      }
      catch (const boost::interprocess::interprocess_exception& e) {
        BOOST_THROW_EXCEPTION(LockException()
          << ErrorInfo::Message(e.what())
          << ErrorInfo::SharedLockFile(lockFilePath.string())
          << ErrorInfo::NamedMutexName(namedMutexName)
          << ErrorInfo::PossibleCauses({"Invalid lock file path"}));
      }
      try {
        Utilities::resetSmartPtr(mNamedMutex, boost::interprocess::open_or_create, namedMutexName.c_str());
      }
      catch (const boost::interprocess::interprocess_exception& e) {
        BOOST_THROW_EXCEPTION(LockException()
          << ErrorInfo::Message(e.what())
          << ErrorInfo::SharedLockFile(lockFilePath.string())
          << ErrorInfo::NamedMutexName(namedMutexName)
          << ErrorInfo::PossibleCauses({"Invalid mutex name (note: it should be a name, not a path)"}));
      }

      // Initialize locks
      mFileLock = FileLock(*mFileMutex.get(), std::defer_lock);
      mNamedLock = NamedLock(*mNamedMutex.get(), std::defer_lock);

      // Attempt to lock mutexes
      if (waitOnLock) {
        // Waits on lock to become available
        try {
          std::lock(mFileLock, mNamedLock);
        } catch (const std::exception& e) {
          BOOST_THROW_EXCEPTION(e); // Rethrow with line number info
        }
      } else {
        // Fails immediately if lock is not available
        int result = std::try_lock(mFileLock, mNamedLock);
        if (result == 0) {
          // First lock failed
          BOOST_THROW_EXCEPTION(FileLockException()
            << ErrorInfo::Message("Failed to acquire file lock")
            << ErrorInfo::SharedLockFile(lockFilePath.string())
            << ErrorInfo::NamedMutexName(namedMutexName));
        } else if (result == 1) {
          // Second lock failed
          BOOST_THROW_EXCEPTION(NamedMutexLockException()
            << ErrorInfo::Message("Failed to acquire named mutex only; file lock was successfully acquired")
            << ErrorInfo::SharedLockFile(lockFilePath.string())
            << ErrorInfo::NamedMutexName(namedMutexName)
            << ErrorInfo::PossibleCauses({
              "Named mutex is owned by other thread in current process",
              "Previous Interprocess::Lock on same objects was not cleanly destroyed"
            }));
        }
      }
    }

  private:
    boost::scoped_ptr<FileMutex> mFileMutex;
    boost::scoped_ptr<NamedMutex> mNamedMutex;
    FileLock mFileLock;
    NamedLock mNamedLock;
};

} // namespace Interprocess
} // namespace roc
} // namespace AliceO2

#endif /* ALICEO2_READOUTCARD_INTERPROCESSMUTEX_H_ */
