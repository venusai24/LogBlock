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
  enum class Op { OpenLog, FileSize, Pread, Pwrite, Fsync, Ftruncate, FsyncParentDir };

  // One recorded call. For Pread and Pwrite, offset and length are the
  // arguments. For Ftruncate, length is the new size. Otherwise both are 0.
  struct Call {
    Op op;
    uint64_t offset;
    uint64_t length;
  };

  // --- Io ------------------------------------------------------------------
  Status open_log(const std::string& path) override;
  Status file_size(uint64_t* size) override;
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override;
  Status pwrite(uint64_t offset, const void* buf, size_t len) override;
  Status fsync() override;
  Status ftruncate(uint64_t size) override;
  Status fsync_parent_dir() override;

  // --- The fake file -------------------------------------------------------
  // Direct access to the file's contents. Fill it before open_log() to start
  // with a non-empty log, or change it later to corrupt a record.
  std::string& bytes() { return bytes_; }
  [[nodiscard]] const std::string& bytes() const { return bytes_; }
  [[nodiscard]] const std::string& path() const { return path_; }

  // --- Call record ---------------------------------------------------------
  [[nodiscard]] const std::vector<Call>& calls() const { return calls_; }
  [[nodiscard]] size_t count(Op op) const;
  void clear_calls() { calls_.clear(); }

  // --- Fault injection -----------------------------------------------------
  // Makes the next call of this operation fail with `code`. Several faults for
  // one operation are used in the order they were added.
  //
  // For Op::Pwrite, `partial_bytes` is how many of the bytes reach the file
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

  bool open_ = false;
  std::string path_;
  std::string bytes_;
  std::vector<Call> calls_;
  std::map<Op, std::deque<Fault>> faults_;
};

}  // namespace logblock::test
