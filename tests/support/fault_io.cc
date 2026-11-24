#include "support/fault_io.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace logblock::test {

namespace {
const char* op_name(FaultIo::Op op) {
  switch (op) {
    case FaultIo::Op::FileSize:
      return "file_size";
    case FaultIo::Op::Pread:
      return "pread";
    case FaultIo::Op::Pwrite:
      return "pwrite";
    case FaultIo::Op::Fsync:
      return "fsync";
    case FaultIo::Op::Ftruncate:
      return "ftruncate";
    case FaultIo::Op::FsyncParentDir:
      return "fsync_parent_dir";
    case FaultIo::Op::OpenLogExisting:
      return "open_log_existing";
    case FaultIo::Op::CreateLog:
      return "create_log";
    case FaultIo::Op::LockLog:
      return "lock_log";
    case FaultIo::Op::OpenCkpt:
      return "open_ckpt";
    case FaultIo::Op::CkptSize:
      return "ckpt_size";
    case FaultIo::Op::CkptPread:
      return "ckpt_pread";
    case FaultIo::Op::CkptPwrite:
      return "ckpt_pwrite";
    case FaultIo::Op::CkptFsync:
      return "ckpt_fsync";
  }
  return "unknown";
}
}  // namespace

bool FaultIo::begin(Op op, uint64_t offset, uint64_t length, Fault* fault) {
  calls_.push_back({op, offset, length});
  auto it = faults_.find(op);
  if (it == faults_.end() || it->second.empty()) {
    return false;
  }
  *fault = it->second.front();
  it->second.pop_front();
  return true;
}

Status FaultIo::injected(Op op, const Fault& fault) {
  return {fault.code, std::string("injected failure in ") + op_name(op)};
}

Status FaultIo::check_open() const {
  if (!open_) {
    return {StatusCode::IoError, "log is not open"};
  }
  return Status::Ok();
}

Status FaultIo::check_ckpt_open() const {
  if (!ckpt_open_) {
    return {StatusCode::IoError, "checkpoint file is not open"};
  }
  return Status::Ok();
}

size_t FaultIo::count(Op op) const {
  return static_cast<size_t>(
      std::count_if(calls_.begin(), calls_.end(), [op](const Call& c) { return c.op == op; }));
}

void FaultIo::fail_next(Op op, StatusCode code, size_t partial_bytes) {
  faults_[op].push_back({code, partial_bytes});
}

Status FaultIo::open_log_existing(const std::string& path, bool* found) {
  *found = false;
  Fault fault{};
  if (begin(Op::OpenLogExisting, 0, 0, &fault)) {
    return injected(Op::OpenLogExisting, fault);
  }
  if (!log_exists()) {
    return Status::Ok();  // ENOENT: not an error, nothing created
  }
  open_ = true;
  log_exists_ = true;
  path_ = path;
  *found = true;
  return Status::Ok();
}

Status FaultIo::create_log(const std::string& path, bool* created) {
  *created = false;
  Fault fault{};
  if (begin(Op::CreateLog, 0, 0, &fault)) {
    return injected(Op::CreateLog, fault);
  }
  if (create_race_ || log_exists()) {
    log_exists_ = true;  // somebody else's file now exists; ours is not opened
    return Status::Ok();
  }
  open_ = true;
  log_exists_ = true;
  path_ = path;
  *created = true;
  return Status::Ok();
}

Status FaultIo::file_size(uint64_t* size) {
  Fault fault{};
  bool failing = begin(Op::FileSize, 0, 0, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::FileSize, fault);
  }
  *size = bytes_.size();
  return Status::Ok();
}

Status FaultIo::pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
  Fault fault{};
  bool failing = begin(Op::Pread, offset, len, &fault);
  *bytes_read = 0;
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::Pread, fault);
  }
  if (offset < bytes_.size()) {
    size_t n = std::min<size_t>(len, bytes_.size() - offset);
    std::memcpy(buf, bytes_.data() + offset, n);
    *bytes_read = n;
  }
  return Status::Ok();
}

Status FaultIo::pwrite(uint64_t offset, const void* buf, size_t len) {
  Fault fault{};
  bool failing = begin(Op::Pwrite, offset, len, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  // A failing write still puts its first partial_bytes in the file (a torn write).
  size_t n = failing ? std::min(len, fault.partial_bytes) : len;
  if (n > 0) {
    if (bytes_.size() < offset + n) {
      bytes_.resize(offset + n, '\0');  // like a real file, a gap reads as zeros
    }
    std::memcpy(bytes_.data() + offset, buf, n);
  }
  return failing ? injected(Op::Pwrite, fault) : Status::Ok();
}

Status FaultIo::fsync() {
  Fault fault{};
  bool failing = begin(Op::Fsync, 0, 0, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  return failing ? injected(Op::Fsync, fault) : Status::Ok();
}

Status FaultIo::ftruncate(uint64_t size) {
  Fault fault{};
  bool failing = begin(Op::Ftruncate, 0, size, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::Ftruncate, fault);
  }
  bytes_.resize(size, '\0');
  return Status::Ok();
}

Status FaultIo::fsync_parent_dir() {
  Fault fault{};
  bool failing = begin(Op::FsyncParentDir, 0, 0, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  return failing ? injected(Op::FsyncParentDir, fault) : Status::Ok();
}

Status FaultIo::lock_log() {
  Fault fault{};
  bool failing = begin(Op::LockLog, 0, 0, &fault);
  if (Status s = check_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::LockLog, fault);
  }
  if (lock_held_) {
    return {StatusCode::Failed, "log is in use by another server: " + path_};
  }
  return Status::Ok();
}

Status FaultIo::open_ckpt(const std::string&, bool create, bool* opened, bool* created) {
  Fault fault{};
  bool failing = begin(Op::OpenCkpt, 0, create ? 1 : 0, &fault);
  *opened = false;
  *created = false;
  if (failing) {
    return injected(Op::OpenCkpt, fault);
  }
  if (ckpt_open_) {
    return {StatusCode::IoError, "checkpoint file is already open"};
  }
  if (!ckpt_exists()) {
    if (!create) {
      return Status::Ok();  // missing and not wanted: succeeds, nothing opened
    }
    ckpt_exists_ = true;
    *created = true;
  }
  ckpt_open_ = true;
  *opened = true;
  return Status::Ok();
}

Status FaultIo::ckpt_size(uint64_t* size) {
  Fault fault{};
  bool failing = begin(Op::CkptSize, 0, 0, &fault);
  if (Status s = check_ckpt_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::CkptSize, fault);
  }
  *size = ckpt_bytes_.size();
  return Status::Ok();
}

Status FaultIo::ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
  Fault fault{};
  bool failing = begin(Op::CkptPread, offset, len, &fault);
  *bytes_read = 0;
  if (Status s = check_ckpt_open(); !s.ok()) {
    return s;
  }
  if (failing) {
    return injected(Op::CkptPread, fault);
  }
  if (offset < ckpt_bytes_.size()) {
    size_t n = std::min<size_t>(len, ckpt_bytes_.size() - offset);
    std::memcpy(buf, ckpt_bytes_.data() + offset, n);
    *bytes_read = n;
  }
  return Status::Ok();
}

Status FaultIo::ckpt_pwrite(uint64_t offset, const void* buf, size_t len) {
  Fault fault{};
  bool failing = begin(Op::CkptPwrite, offset, len, &fault);
  if (Status s = check_ckpt_open(); !s.ok()) {
    return s;
  }
  size_t n = failing ? std::min(len, fault.partial_bytes) : len;
  if (n > 0) {
    if (ckpt_bytes_.size() < offset + n) {
      ckpt_bytes_.resize(offset + n, '\0');
    }
    std::memcpy(ckpt_bytes_.data() + offset, buf, n);
  }
  return failing ? injected(Op::CkptPwrite, fault) : Status::Ok();
}

Status FaultIo::ckpt_fsync() {
  Fault fault{};
  bool failing = begin(Op::CkptFsync, 0, 0, &fault);
  if (Status s = check_ckpt_open(); !s.ok()) {
    return s;
  }
  return failing ? injected(Op::CkptFsync, fault) : Status::Ok();
}

}  // namespace logblock::test
