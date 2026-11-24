#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "logblock/status.h"

namespace logblock {

// The engine does all its file work through this interface, so tests can swap in a fake that
// injects failures and short writes. One Io object works on one log file and its checkpoint file:
// open_log_existing() or create_log() opens the log and open_ckpt() opens the checkpoint file. Both
// are closed when the object is destroyed.
//
// Every log call before a successful open of the log returns IoError, and every checkpoint call
// (ckpt_*) before a successful open_ckpt() that opened a file returns IoError.
class Io {
 public:
  Io() = default;
  virtual ~Io() = default;
  Io(const Io&) = delete;
  Io& operator=(const Io&) = delete;

  // Opens the log for reading and writing only if it already exists; never creates it. A missing
  // file is not an error: *found is false and nothing is created or opened. Any other failure
  // (permissions, a directory in the way) is an error. An existing file is never changed.
  // The startup order is open_log_existing, then (if missing) create_log, then lock_log, then slot
  // reads (D35).
  virtual Status open_log_existing(const std::string& path, bool* found) = 0;

  // Creates the log with O_CREAT | O_EXCL (mode 0600) and opens it. If the file appeared since
  // open_log_existing() looked (another server won the race), *created is false, nothing is
  // opened and the call succeeds; the caller refuses to start. Needs the directory to exist.
  virtual Status create_log(const std::string& path, bool* created) = 0;

  // Current size of the log file in bytes.
  virtual Status file_size(uint64_t* size) = 0;

  // Reads up to len bytes at offset. Keeps reading until len bytes are read or
  // the end of the file is reached, so *bytes_read < len means end of file, and
  // the caller decides whether that is corruption.
  virtual Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) = 0;

  // Writes exactly len bytes at offset, or fails. Short writes and EINTR are
  // retried. ENOSPC and EDQUOT return NoSpace; anything else returns IoError.
  virtual Status pwrite(uint64_t offset, const void* buf, size_t len) = 0;

  // Flushes the file's data to disk.
  virtual Status fsync() = 0;

  // Sets the file size. The engine uses this to cut off a failed append.
  virtual Status ftruncate(uint64_t size) = 0;

  // Flushes the directory that holds the log, so a file's name is durable too. Startup calls it on
  // every successful start, whether or not that start created the log (D35). Also used for the
  // checkpoint file, which lives in the same directory.
  virtual Status fsync_parent_dir() = 0;

  // --- Added in M2 ---------------------------------------------------------

  // Takes an exclusive, non-blocking flock() on the open log, so a second server on the same log
  // is refused instead of corrupting it. If another open file description holds the lock, returns
  // Failed (naming the log) at once. The lock is released when the file is closed, which includes
  // the process dying.
  virtual Status lock_log() = 0;

  // Opens the checkpoint file for reading and writing.
  //   create = false, file missing: succeeds with *opened = false (nothing is created).
  //   create = true:  creates a missing file with mode 0600.
  // *opened says whether a file is now open. *created says whether this call made it, because
  // the caller must fsync the directory after a new file (FR-014).
  virtual Status open_ckpt(const std::string& path, bool create, bool* opened, bool* created) = 0;

  // The checkpoint file's calls behave like size, pread, pwrite and fsync on the log.
  virtual Status ckpt_size(uint64_t* size) = 0;
  virtual Status ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) = 0;
  virtual Status ckpt_pwrite(uint64_t offset, const void* buf, size_t len) = 0;
  virtual Status ckpt_fsync() = 0;
};

// The production Io, which calls the system calls directly.
std::unique_ptr<Io> make_posix_io();

// A single system call attempt: returns the byte count, or -1 with errno set,
// like pwrite(2) and pread(2).
using RawWriteFn = std::function<ssize_t(const void* buf, size_t len, uint64_t offset)>;
using RawReadFn = std::function<ssize_t(void* buf, size_t len, uint64_t offset)>;

// The retry loops behind PosixIo, separate from the system calls so tests can
// feed them short returns and errors.
//
// write_fully repeats write_once until all len bytes are written, retrying after
// short writes and EINTR. A zero-byte write counts as an error (it would loop
// forever). ENOSPC and EDQUOT give NoSpace, other errors give IoError.
Status write_fully(const RawWriteFn& write_once, const void* buf, size_t len, uint64_t offset);

// read_fully repeats read_once until len bytes are read or it returns 0 (end of
// file). *bytes_read is how many bytes were read.
Status read_fully(const RawReadFn& read_once, void* buf, size_t len, uint64_t offset,
                  size_t* bytes_read);

}  // namespace logblock
