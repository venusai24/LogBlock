#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "logblock/io.h"

namespace logblock::test {

// An in-memory Io for unit tests. The "file" is a std::string, so tests can
// read and change its bytes directly, and every call is recorded so tests can
// check what the engine did and in what order. Faults are queued per operation
// and each one is used up by the next matching call.
//
// Test-only: this is never linked into the plugin.
class FaultIo final : public Io {
 public:
  enum class Op {
    FileSize,
    Pread,
    Pwrite,
    Fsync,
    Ftruncate,
    FsyncParentDir,
    // Added in M2
    OpenLogExisting,
    CreateLog,
    LockLog,
    OpenCkpt,
    CkptSize,
    CkptPread,
    CkptPwrite,
    CkptFsync
  };

  // One recorded call. For Pread, Pwrite, CkptPread and CkptPwrite, offset and length are the
  // arguments. For Ftruncate, length is the new size. For OpenCkpt, length is 1 when create is
  // true. Otherwise both are 0.
  struct Call {
    Op op;
    uint64_t offset;
    uint64_t length;
  };

  // --- Io ------------------------------------------------------------------
  Status open_log_existing(const std::string& path, bool* found) override;
  Status create_log(const std::string& path, bool* created) override;
  Status file_size(uint64_t* size) override;
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override;
  Status pwrite(uint64_t offset, const void* buf, size_t len) override;
  Status fsync() override;
  Status ftruncate(uint64_t size) override;
  Status fsync_parent_dir() override;
  Status lock_log() override;
  Status open_ckpt(const std::string& path, bool create, bool* opened, bool* created) override;
  Status ckpt_size(uint64_t* size) override;
  Status ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override;
  Status ckpt_pwrite(uint64_t offset, const void* buf, size_t len) override;
  Status ckpt_fsync() override;

  // --- The fake file -------------------------------------------------------
  // Direct access to the file's contents. Fill it before the log is opened to start
  // with a non-empty log, or change it later to corrupt a record.
  std::string& bytes() { return bytes_; }
  [[nodiscard]] const std::string& bytes() const { return bytes_; }
  [[nodiscard]] const std::string& path() const { return path_; }

  // --- The fake checkpoint file --------------------------------------------
  // Its own bytes, independent of the log. A test can put slots here before the engine starts.
  std::string& ckpt_bytes() { return ckpt_bytes_; }
  [[nodiscard]] const std::string& ckpt_bytes() const { return ckpt_bytes_; }

  // Whether each file exists. A file also exists if it has any bytes, so filling bytes() or
  // ckpt_bytes() is enough. The setters make an empty file exist (or not). create_log() creates the
  // log; open_ckpt(create = true) creates the checkpoint file.
  [[nodiscard]] bool log_exists() const { return log_exists_ || !bytes_.empty(); }
  [[nodiscard]] bool ckpt_exists() const { return ckpt_exists_ || !ckpt_bytes_.empty(); }
  void set_log_exists(bool v) { log_exists_ = v; }
  void set_ckpt_exists(bool v) { ckpt_exists_ = v; }

  // Makes create_log() lose the race: it reports created = false and opens nothing, as if
  // another server had created the file between open_log_existing() and create_log().
  void set_create_race(bool v) { create_race_ = v; }

  // Makes lock_log() fail with Failed, as if another server held the lock.
  void set_lock_held(bool v) { lock_held_ = v; }

  // --- Call record ---------------------------------------------------------
  [[nodiscard]] const std::vector<Call>& calls() const { return calls_; }
  [[nodiscard]] size_t count(Op op) const;
  void clear_calls() { calls_.clear(); }

  // --- Fault injection -----------------------------------------------------
  // Makes the next call of this operation fail with `code`. Several faults for
  // one operation are used in the order they were added.
  //
  // For Op::Pwrite and Op::CkptPwrite, `partial_bytes` is how many of the bytes reach the file
  // before the failure, which makes a torn append (the write is cut short at
  // that point, or all of it if it is shorter). Other operations ignore it, and
  // a failed call changes nothing.
  void fail_next(Op op, StatusCode code = StatusCode::IoError, size_t partial_bytes = 0);

 private:
  struct Fault {
    StatusCode code;
    size_t partial_bytes;
  };

  // Records the call, and returns the fault to apply to it, if one is queued.
  bool begin(Op op, uint64_t offset, uint64_t length, Fault* fault);
  [[nodiscard]] Status check_open() const;
  static Status injected(Op op, const Fault& fault);

  [[nodiscard]] Status check_ckpt_open() const;

  bool open_ = false;
  std::string path_;
  std::string bytes_;
  bool log_exists_ = false;
  bool ckpt_open_ = false;
  bool ckpt_exists_ = false;
  bool lock_held_ = false;
  bool create_race_ = false;
  std::string ckpt_bytes_;
  std::vector<Call> calls_;
  std::map<Op, std::deque<Fault>> faults_;
};

}  // namespace logblock::test
