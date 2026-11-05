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
// injects failures and short writes. One Io object works on one log file: open_log() opens it, and
// the file is closed when the object is destroyed.
//
// Every call before a successful open_log() returns IoError.
class Io {
 public:
  Io() = default;
  virtual ~Io() = default;
  Io(const Io&) = delete;
  Io& operator=(const Io&) = delete;

  // Opens the log for reading and writing, creating it with mode 0600 if it is
  // missing. An existing file is never truncated or changed. Not O_APPEND:
  // the engine writes at explicit offsets.
  virtual Status open_log(const std::string& path) = 0;

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

  // Flushes the directory that holds the log, so a newly created file's name is
  // durable too.
  virtual Status fsync_parent_dir() = 0;
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
