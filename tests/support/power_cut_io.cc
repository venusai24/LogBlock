#include "support/power_cut_io.h"

#include <algorithm>
#include <cstring>

namespace logblock::test {

Status PowerCutIo::begin(Op op) {
  size_t number = ops_.size();
  ops_.push_back(op);
  if (cut_ || (cut_at_.has_value() && number >= *cut_at_)) {
    cut_ = true;  // from here on every call fails, and none takes effect
    return {StatusCode::IoError, "power cut"};
  }
  if (injected_.has_value() && injected_->op == op) {
    StatusCode code = injected_->code;
    injected_.reset();
    return {code, "injected failure"};
  }
  return Status::Ok();
}

Status PowerCutIo::check_open(const File& f, const char* what) {
  if (!f.open) {
    return {StatusCode::IoError, std::string(what) + " is not open"};
  }
  return Status::Ok();
}

// Applies one pending operation to a file image. Used for the running view now, and for the
// partial outcomes in T004, which apply only some of a pending list to the durable image.
void PowerCutIo::apply(const PendingOp& op, std::string* image) {
  if (op.is_truncate) {
    image->resize(op.size, '\0');  // growing reads as zeros, like a real file
    return;
  }
  if (image->size() < op.offset + op.data.size()) {
    image->resize(op.offset + op.data.size(), '\0');  // a gap reads as zeros
  }
  std::memcpy(image->data() + op.offset, op.data.data(), op.data.size());
}

Status PowerCutIo::write_file(File& f, uint64_t offset, const void* buf, size_t len) {
  PendingOp op;
  op.offset = offset;
  op.data.assign(static_cast<const char*>(buf), len);
  apply(op, &f.live);
  f.pending.push_back(std::move(op));
  return Status::Ok();
}

Status PowerCutIo::truncate_file(File& f, uint64_t size) {
  PendingOp op;
  op.is_truncate = true;
  op.size = size;
  apply(op, &f.live);
  f.pending.push_back(std::move(op));
  return Status::Ok();
}

Status PowerCutIo::read_file(const File& f, uint64_t offset, void* buf, size_t len, size_t* got) {
  *got = 0;
  if (offset < f.live.size()) {
    size_t n = std::min<size_t>(len, f.live.size() - offset);
    std::memcpy(buf, f.live.data() + offset, n);
    *got = n;
  }
  return Status::Ok();
}

void PowerCutIo::sync_file(File& f) {
  // The pending list has already been applied to `live`, so the durable image becomes it.
  f.durable = f.live;
  f.pending.clear();
}

// --- Setup --------------------------------------------------------------------

void PowerCutIo::set_log_image(std::string bytes) {
  log_ = File{};
  log_.exists = true;
  log_.name_durable = true;
  log_.durable = bytes;
  log_.live = std::move(bytes);
}

void PowerCutIo::set_ckpt_image(std::string bytes) {
  ckpt_ = File{};
  ckpt_.exists = true;
  ckpt_.name_durable = true;
  ckpt_.durable = bytes;
  ckpt_.live = std::move(bytes);
}

// --- Restart ------------------------------------------------------------------

PowerCutIo::Outcome PowerCutIo::pick_outcome() {
  Outcome o;
  o.kind = static_cast<Kind>(rng_() % 3);
  // Always drawn, so the number of draws does not depend on the kind. A tear has no zero-fill
  // variant (the contract lists it for drop-all and random prefix only).
  bool zero = (rng_() % 2) == 1;
  o.zero_fill = zero && o.kind != Kind::TearLast;
  return o;
}

std::string PowerCutIo::survivor(const File& f, Outcome* o) {
  std::string image = f.durable;
  const size_t n = f.pending.size();
  size_t k = 0;
  const std::optional<size_t> wanted_tear = o->tear_bytes;  // what the caller asked for, if any
  o->tear_bytes.reset();  // refilled below only if a write is actually torn

  switch (o->kind) {
    case Kind::DropAll:
      break;  // k stays 0: no pending operation reaches the disk
    case Kind::RandomPrefix:
      k = o->prefix_ops.has_value() ? std::min(*o->prefix_ops, n) : rng_() % (n + 1);
      break;
    case Kind::TearLast:
      if (n > 0) {
        k = o->prefix_ops.has_value() ? std::min(*o->prefix_ops, n - 1) : rng_() % n;
      }
      break;
  }
  for (size_t i = 0; i < k; ++i) {
    apply(f.pending[i], &image);
  }
  o->prefix_ops = k;

  if (o->kind == Kind::TearLast && k < n && !f.pending[k].is_truncate &&
      !f.pending[k].data.empty()) {
    // The write that was in flight when the power went: only part of it lands, always less than
    // the whole. An in-place tear therefore leaves the new bytes followed by the old ones.
    const size_t len = f.pending[k].data.size();
    const size_t t = wanted_tear.has_value() ? std::min(*wanted_tear, len - 1) : rng_() % len;
    PendingOp torn;
    torn.offset = f.pending[k].offset;
    torn.data = f.pending[k].data.substr(0, t);
    apply(torn, &image);
    o->tear_bytes = t;
  }

  if (o->zero_fill && o->kind != Kind::TearLast) {
    // The file system recorded the size, but not all the data blocks made it.
    image.resize(f.live.size(), '\0');
  } else {
    o->zero_fill = false;
  }
  return image;
}

std::unique_ptr<PowerCutIo> PowerCutIo::reboot() {
  Outcome log = pick_outcome();
  Outcome ckpt = pick_outcome();
  return reboot(log, ckpt);
}

std::unique_ptr<PowerCutIo> PowerCutIo::reboot(const Outcome& log, const Outcome& ckpt) {
  Outcomes used{log, ckpt};
  // The log is decided before the checkpoint file, so the draws come in a fixed order.
  std::optional<std::string> log_image;
  std::optional<std::string> ckpt_image;
  // A file whose name is not durable does not survive, whatever its outcome.
  if (log_.exists && log_.name_durable) {
    log_image = survivor(log_, &used.log);
  }
  if (ckpt_.exists && ckpt_.name_durable) {
    ckpt_image = survivor(ckpt_, &used.ckpt);
  }
  auto next = std::make_unique<PowerCutIo>(rng_());
  if (log_image.has_value()) {
    next->set_log_image(std::move(*log_image));
  }
  if (ckpt_image.has_value()) {
    next->set_ckpt_image(std::move(*ckpt_image));
  }
  next->boot_outcomes_ = used;
  return next;
}

std::unique_ptr<PowerCutIo> PowerCutIo::restart() {
  auto next = std::make_unique<PowerCutIo>(rng_());
  // Copy each file as it is, then close it: the old process held the descriptors.
  next->log_ = log_;
  next->log_.open = false;
  next->ckpt_ = ckpt_;
  next->ckpt_.open = false;
  return next;
}

// --- Io: the log --------------------------------------------------------------

Status PowerCutIo::open_log_existing(const std::string&, bool* found) {
  *found = false;
  if (Status s = begin(Op::OpenLogExisting); !s.ok()) {
    return s;
  }
  if (log_.open) {
    return {StatusCode::IoError, "log is already open"};
  }
  if (!log_.exists) {
    return Status::Ok();
  }
  log_.open = true;
  *found = true;
  return Status::Ok();
}

Status PowerCutIo::create_log(const std::string&, bool* created) {
  *created = false;
  if (Status s = begin(Op::CreateLog); !s.ok()) {
    return s;
  }
  if (log_.open) {
    return {StatusCode::IoError, "log is already open"};
  }
  if (log_.exists) {
    return Status::Ok();  // O_EXCL loses
  }
  log_.exists = true;  // the name is not durable until fsync_parent_dir()
  log_.name_durable = false;
  log_.open = true;
  *created = true;
  return Status::Ok();
}

Status PowerCutIo::file_size(uint64_t* size) {
  if (Status s = begin(Op::FileSize); !s.ok()) {
    return s;
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  *size = log_.live.size();
  return Status::Ok();
}

Status PowerCutIo::pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
  *bytes_read = 0;
  if (Status s = begin(Op::Pread); !s.ok()) {
    return s;
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  return read_file(log_, offset, buf, len, bytes_read);
}

Status PowerCutIo::pwrite(uint64_t offset, const void* buf, size_t len) {
  if (Status s = begin(Op::Pwrite); !s.ok()) {
    return s;
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  return write_file(log_, offset, buf, len);
}

Status PowerCutIo::fsync() {
  if (Status s = begin(Op::Fsync); !s.ok()) {
    return s;  // an fsync at the cut has not completed: its pending list stays pending
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  if (!lying_log_fsync_) {
    sync_file(log_);
  }
  return Status::Ok();
}

Status PowerCutIo::ftruncate(uint64_t size) {
  if (Status s = begin(Op::Ftruncate); !s.ok()) {
    return s;
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  return truncate_file(log_, size);
}

Status PowerCutIo::fsync_parent_dir() {
  if (Status s = begin(Op::FsyncParentDir); !s.ok()) {
    return s;
  }
  if (Status s = check_open(log_, "log"); !s.ok()) {
    return s;
  }
  // Each name in the directory is durable or not on its own. One directory fsync covers every
  // entry that exists, except that the lying mode skips the checkpoint's entry: it reports
  // success, and the log's name still becomes durable, as it would on a disk that dropped just
  // that one directory update.
  if (log_.exists) {
    log_.name_durable = true;
  }
  if (ckpt_.exists && !lying_dir_fsync_) {
    ckpt_.name_durable = true;
  }
  return Status::Ok();
}

Status PowerCutIo::lock_log() {
  if (Status s = begin(Op::LockLog); !s.ok()) {
    return s;
  }
  // flock state is not modelled: a power cut ends every holder, so there is nothing to lose.
  return check_open(log_, "log");
}

// --- Io: the checkpoint file --------------------------------------------------

Status PowerCutIo::open_ckpt(const std::string&, bool create, bool* opened, bool* created) {
  *opened = false;
  *created = false;
  if (Status s = begin(Op::OpenCkpt); !s.ok()) {
    return s;
  }
  if (ckpt_.open) {
    return {StatusCode::IoError, "checkpoint file is already open"};
  }
  if (!ckpt_.exists) {
    if (!create) {
      return Status::Ok();  // missing and not wanted: succeeds, nothing opened
    }
    ckpt_.exists = true;
    ckpt_.name_durable = false;  // durable only after fsync_parent_dir()
    *created = true;
  }
  ckpt_.open = true;
  *opened = true;
  return Status::Ok();
}

Status PowerCutIo::ckpt_size(uint64_t* size) {
  if (Status s = begin(Op::CkptSize); !s.ok()) {
    return s;
  }
  if (Status s = check_open(ckpt_, "checkpoint file"); !s.ok()) {
    return s;
  }
  *size = ckpt_.live.size();
  return Status::Ok();
}

Status PowerCutIo::ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
  *bytes_read = 0;
  if (Status s = begin(Op::CkptPread); !s.ok()) {
    return s;
  }
  if (Status s = check_open(ckpt_, "checkpoint file"); !s.ok()) {
    return s;
  }
  return read_file(ckpt_, offset, buf, len, bytes_read);
}

Status PowerCutIo::ckpt_pwrite(uint64_t offset, const void* buf, size_t len) {
  if (Status s = begin(Op::CkptPwrite); !s.ok()) {
    return s;
  }
  if (Status s = check_open(ckpt_, "checkpoint file"); !s.ok()) {
    return s;
  }
  return write_file(ckpt_, offset, buf, len);
}

Status PowerCutIo::ckpt_fsync() {
  if (Status s = begin(Op::CkptFsync); !s.ok()) {
    return s;
  }
  if (Status s = check_open(ckpt_, "checkpoint file"); !s.ok()) {
    return s;
  }
  if (!lying_ckpt_fsync_) {
    sync_file(ckpt_);
  }
  return Status::Ok();
}

}  // namespace logblock::test
