// The storage engine: create(), read(), write(), trim() and flush() over the block map and
// the append-only log.
#include "logblock/engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "block_map.h"
#include "checkpoint.h"
#include "log_file.h"
#include "recovery.h"

namespace logblock {

Engine::~Engine() = default;

namespace {

// Rejects any request that is not entirely inside the device. This is the first check
// of every read, write and trim. It is written so that offset + len is never
// computed: that sum can wrap around in 64 bits (offset = UINT64_MAX, len = 2 gives
// 1) and a wrapped value would look like a valid small range.
Status check_range(uint64_t offset, uint64_t len) {
  // offset > size is outside even for len == 0; offset == size with len == 0 is
  // fine. When offset <= size, `size - offset` cannot underflow, and `len` fits if it
  // is not more than what is left.
  if (offset > kDeviceSize || len > kDeviceSize - offset) {
    return {StatusCode::OutOfRange,
            "request at offset " + std::to_string(offset) + " with length " + std::to_string(len) +
                " is outside the device (size " + std::to_string(kDeviceSize) + ")"};
  }
  return Status::Ok();
}

}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): created only by Engine::create.
Engine::Engine(PrivateKey, std::unique_ptr<Io> io, uint64_t cap, uint64_t checkpoint_interval,
               RecoveredState&& recovered)
    : io_(std::move(io)),
      cap_(cap),
      map_(std::move(recovered.map)),
      // Resume where the last valid record ended, so the next append lands there (FR-003).
      log_(std::make_unique<LogFile>(*io_, recovered.end, recovered.next_seq)),
      checkpoint_interval_(checkpoint_interval),
      report_(std::move(recovered.report)) {
  counters_.unreadable = report_.unreadable_checkpoints;
  ckpt_path_ = recovered.ckpt_path;
  ckpt_buffer_.assign(kCheckpointSlotSize, 0);
  ckpt_open_ = recovered.ckpt_open;
  last_checkpoint_end_ = recovered.last_checkpoint_end;
  generation_ = recovered.generation;
  next_slot_ = recovered.next_slot;
  fresh_checkpoint_due_ = recovered.fresh_checkpoint_due;
  startup_record_offset_ = recovered.last_record_offset;
}

Status Engine::create(const std::string& path, uint64_t cap, std::unique_ptr<Io> io,
                      std::unique_ptr<Engine>* out, uint64_t checkpoint_interval) {
  // Check the settings before touching any file, so a bad one never creates a log.
  if (cap < kDataRecordSize) {
    return {StatusCode::OutOfRange,
            "cap " + std::to_string(cap) + " is too small: it must be at least " +
                std::to_string(kDataRecordSize) + " bytes, the size of one DATA record"};
  }
  if (checkpoint_interval < kMinCheckpointInterval) {
    return {StatusCode::OutOfRange, "checkpoint_interval " + std::to_string(checkpoint_interval) +
                                        " is too small: it must be at least " +
                                        std::to_string(kMinCheckpointInterval) +
                                        " bytes, the size of one DATA record"};
  }

  // Startup order (D28, D34, D35): open the log WITHOUT creating it. If it is missing, create it
  // with O_EXCL. Take the lock, and only then read anything. Two servers on one log would both
  // append at the same offsets and silently destroy each other's records, so the second must be
  // refused while nothing has been read or changed.
  // The OS releases the lock when the process dies, so a killed server never blocks the next start.
  CheckpointSlots preread;
  bool found = false;
  if (Status s = io->open_log_existing(path, &found); !s.ok()) {
    return s;  // only "not found" means the log may be created
  }
  if (!found) {
    // A missing log with a valid checkpoint beside it means a device lived here and its log was
    // lost. Creating an empty one would serve zeros, so refuse first. This only reads (FR-009).
    if (Status s = check_checkpoints_without_log(*io, path, &preread); !s.ok()) {
      return s;
    }
    // O_EXCL: if something exists at the path now, we change nothing. We cannot tell which it
    // is: a server that created the log since we looked, or a dangling symlink (O_EXCL refuses
    // to follow it, and the earlier open without O_CREAT saw it as missing).
    bool created = false;
    if (Status s = io->create_log(path, &created); !s.ok()) {
      return s;
    }
    if (!created) {
      return {StatusCode::Failed,
              "the log appeared during startup: " + path +
                  " (another server may have just created it, or the path is a dangling symlink)"};
    }
    // The new name is made durable later, by recover(), which syncs the directory on every
    // successful start (D35). Any refusal after this point (lock, size read, recovery, the
    // directory sync itself) may leave this empty log behind (FR-009).
  }
  if (Status s = io->lock_log(); !s.ok()) {
    return s;
  }

  // Rebuild the map from the log (and, in time, a checkpoint). Nothing is served until this
  // returns, so a client can never see a half-recovered device (FR-004).
  RecoveredState recovered;
  if (Status s = recover(*io, path, cap, &recovered, &preread); !s.ok()) {
    return s;
  }

  auto engine = std::make_unique<Engine>(PrivateKey{}, std::move(io), cap, checkpoint_interval,
                                         std::move(recovered));

  // No slot could be used but some were unreadable: the next restart would scan the whole log
  // again, so write a fresh checkpoint now (FR-016). If it fails the server still starts: the
  // failure is counted and warned about, and the log is intact.
  if (engine->fresh_checkpoint_due_) {
    engine->write_checkpoint(engine->startup_record_offset_);
  }
  *out = std::move(engine);
  return Status::Ok();
}

Status Engine::check_writable() const {
  if (log_->broken()) {
    return {StatusCode::Failed,
            "an earlier append failed and could not be cleaned up; the log accepts no more "
            "writes or trims (reads still work)"};
  }
  return Status::Ok();
}

Status Engine::check_room(uint64_t needed) const {
  // end() never exceeds cap_, but the guard keeps the subtraction safe regardless.
  const uint64_t used = log_->end();
  if (used > cap_ || needed > cap_ - used) {
    return {StatusCode::NoSpace, "the log has " + std::to_string(used > cap_ ? 0 : cap_ - used) +
                                     " bytes left under its cap of " + std::to_string(cap_) +
                                     " and this request needs " + std::to_string(needed)};
  }
  return Status::Ok();
}

Status Engine::load_block(uint64_t block, uint8_t* out) {
  const std::optional<uint64_t> offset = map_->get(block);
  if (!offset) {
    std::fill_n(out, kBlockSize, uint8_t{0});  // never written or trimmed: no I/O at all
    return Status::Ok();
  }
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record record;
  if (Status s = log_->read_record(*offset, &buffer, &record); !s.ok()) {
    return s;
  }
  // read_record already checked rules 1 to 8, so the record is well formed. The map
  // says it is block `block`'s latest record, so it must be a DATA record for exactly
  // this block. A valid record for another block (or a TRIM) means the log or the map
  // is wrong, and returning its payload would hand back some other block's data.
  if (record.header.type != RecordType::Data || record.payload.size() != kBlockSize) {
    return {StatusCode::Corrupt,
            "block " + std::to_string(block) + " points at a record that is not a DATA record"};
  }
  if (record.header.block_no != block || record.header.block_count != 1) {
    return {StatusCode::Corrupt, "block " + std::to_string(block) +
                                     " points at a record for block " +
                                     std::to_string(record.header.block_no)};
  }
  std::ranges::copy(record.payload, out);
  return Status::Ok();
}

Status Engine::read(uint64_t offset, uint64_t len, void* buf) {
  if (Status s = check_range(offset, len); !s.ok()) {
    return s;
  }
  auto* out = static_cast<uint8_t*>(buf);
  const uint64_t end = offset + len;
  std::array<uint8_t, kBlockSize> block_data{};
  // Walk the request one block at a time. `pos` is the next byte still to deliver.
  for (uint64_t pos = offset; pos < end;) {
    const uint64_t block = pos / kBlockSize;
    const uint64_t in_block = pos % kBlockSize;  // where the request starts inside this block
    const uint64_t n = std::min(kBlockSize - in_block, end - pos);
    if (Status s = load_block(block, block_data.data()); !s.ok()) {
      return s;
    }
    std::copy_n(block_data.begin() + static_cast<std::ptrdiff_t>(in_block), n,
                out + (pos - offset));
    pos += n;
  }
  return Status::Ok();  // len == 0 never enters the loop, so it succeeds and does no I/O
}

Status Engine::write(uint64_t offset, uint64_t len, const void* buf) {
  // Order of checks: range, then sticky state, then the zero-length shortcut.
  // The state check is before the shortcut on purpose: in AppendFailed even a request
  // that would append nothing returns Failed.
  if (Status s = check_range(offset, len); !s.ok()) {
    return s;
  }
  if (Status s = check_writable(); !s.ok()) {
    return s;
  }
  if (len == 0) {
    return Status::Ok();
  }
  const auto* in = static_cast<const uint8_t*>(buf);
  const uint64_t end = offset + len;  // cannot overflow: check_range passed
  const uint64_t first_block = offset / kBlockSize;
  const uint64_t last_block = (end - 1) / kBlockSize;

  // The cap check comes before any old-block read or append, so a write that does not
  // fit changes nothing. Every touched block costs a whole DATA record, even for
  // a 1-byte write.
  if (Status s = check_room((last_block - first_block + 1) * kDataRecordSize); !s.ok()) {
    return s;
  }

  // The new contents of one block: its old contents (only when the write does not
  // cover the whole block), with the written bytes laid over them.
  auto build_block = [&](uint64_t block, uint8_t* block_data) -> Status {
    const uint64_t block_start = block * kBlockSize;
    const uint64_t lo = std::max(offset, block_start);
    const uint64_t hi = std::min(end, block_start + kBlockSize);
    if (hi - lo < kBlockSize) {
      if (Status s = load_block(block, block_data); !s.ok()) {
        return s;
      }
    }
    std::copy_n(in + (lo - offset), hi - lo, block_data + (lo - block_start));
    return Status::Ok();
  };

  // Only the first and last block of a write can be partly covered. Build both
  // before the first append, so a failure to read an old block appends nothing.
  std::array<uint8_t, kBlockSize> first_data{};
  std::array<uint8_t, kBlockSize> last_data{};
  if (Status s = build_block(first_block, first_data.data()); !s.ok()) {
    return s;
  }
  if (last_block != first_block) {
    if (Status s = build_block(last_block, last_data.data()); !s.ok()) {
      return s;
    }
  }

  std::array<uint8_t, kBlockSize> middle_data{};
  for (uint64_t block = first_block; block <= last_block; ++block) {
    uint8_t* data = middle_data.data();
    if (block == first_block) {
      data = first_data.data();
    } else if (block == last_block) {
      data = last_data.data();
    } else {
      (void)build_block(block, data);  // fully covered, so nothing is read and it cannot fail
    }
    // The map changes only after the record is fully written.
    uint64_t record_offset = 0;
    if (Status s = log_->append_data(block, std::span<const uint8_t, kBlockSize>(data, kBlockSize),
                                     &record_offset);
        !s.ok()) {
      return s;
    }
    map_->set(block, record_offset);
    // Per record, not per request: a request that crosses the interval checkpoints at the
    // crossing, with a snapshot that includes this record and no later one (FR-011).
    checkpoint_if_due(record_offset);
  }
  return Status::Ok();
}

Status Engine::trim(uint64_t offset, uint64_t len) {
  // Same order as write: range, sticky state, then the no-full-block shortcut.
  if (Status s = check_range(offset, len); !s.ok()) {
    return s;
  }
  if (Status s = check_writable(); !s.ok()) {
    return s;
  }

  // Only blocks the range covers completely are trimmed. Round the start up and the
  // end down to a block boundary: blocks [first_block, end_block) are fully inside.
  const uint64_t first_block = (offset + kBlockSize - 1) / kBlockSize;
  const uint64_t end_block = (offset + len) / kBlockSize;
  if (len == 0 || first_block >= end_block) {
    return Status::Ok();  // no full block: nothing to log and no I/O
  }
  const uint64_t block_count = end_block - first_block;

  // A TRIM record costs 40 bytes whatever it covers. A trim that appends nothing (the
  // shortcut above) needs no room, so it never reaches this check.
  if (Status s = check_room(kTrimRecordSize); !s.ok()) {
    return s;
  }

  // Log first, then change the map, so a failed append leaves the map as it was.
  // Trimming a block that is already unmapped is still logged.
  const uint64_t record_offset = log_->end();
  if (Status s = log_->append_trim(first_block, static_cast<uint32_t>(block_count)); !s.ok()) {
    return s;
  }
  map_->clear_range(first_block, block_count);
  checkpoint_if_due(record_offset);
  return Status::Ok();
}

Status Engine::flush() {
  // AppendFailed: the log may hold torn bytes we could not remove, so there is nothing
  // sound to promise. No fsync is attempted.
  if (log_->broken()) {
    return {StatusCode::Failed,
            "an earlier append failed and could not be cleaned up; flush is refused"};
  }
  // FlushFailed: after a failed fsync the kernel may have dropped dirty pages and
  // cleared the error, so a later fsync could return success for data that never reached
  // the disk. Never call fsync again; every later flush fails.
  if (flush_failed_) {
    return {StatusCode::Failed, "an earlier flush failed; the engine cannot promise durability"};
  }
  if (Status s = io_->fsync(); !s.ok()) {
    flush_failed_ = true;
    return s;
  }
  return Status::Ok();
}

uint64_t Engine::size() const { return kDeviceSize; }

uint64_t Engine::bytes_logged() const { return log_->end(); }

const RecoveryReport& Engine::recovery_report() const { return report_; }

CheckpointCounters Engine::checkpoint_counters() const { return counters_; }

std::vector<std::string> Engine::take_warnings() { return std::exchange(warnings_, {}); }

void Engine::checkpoint_if_due(uint64_t record_offset) {
  if (log_->end() - last_checkpoint_end_ >= checkpoint_interval_) {
    write_checkpoint(record_offset);
  }
}

void Engine::write_checkpoint(uint64_t last_record_offset) {
  // After a failed fsync the log cannot promise durability (D10), and after a failed append it
  // may hold torn bytes, so a checkpoint vouching for the log's tail would be a lie. A failed
  // checkpoint disables the next ones (D27).
  if (checkpoints_disabled_ || flush_failed_ || log_->broken()) {
    return;
  }
  const auto started = std::chrono::steady_clock::now();

  // Any failure ends here: count it, say what failed, and stop checkpointing until restart. The
  // caller's request was not affected (its record is already appended), so nothing is returned.
  auto fail = [&](const char* step, const Status& s) {
    checkpoints_disabled_ = true;
    ++counters_.failed;
    warnings_.push_back("checkpoint failed file=" + ckpt_path_ + ": " + step + " failed: " +
                        s.message() + "; checkpoints are off until restart");
  };

  // 1. The log must be durable through the covered record before a slot may vouch for it. A
  // checkpoint that survives a crash while the records it covers do not would point into bytes
  // that are gone. A failed fsync is sticky, same as for a client flush (D10).
  if (Status s = io_->fsync(); !s.ok()) {
    flush_failed_ = true;
    fail("log fsync", s);
    return;
  }

  // 2. Create the checkpoint file the first time it is needed (recovery only looked for it).
  bool created = false;
  if (!ckpt_open_) {
    bool opened = false;
    if (Status s = io_->open_ckpt(ckpt_path_, /*create=*/true, &opened, &created); !s.ok()) {
      fail("create", s);
      return;
    }
    ckpt_open_ = true;
  }

  // 3. Encode into the preallocated buffer and write the whole slot over the older one. The
  // newer slot is never touched, so a crash mid-write still leaves one good checkpoint.
  const uint64_t generation = generation_ + 1;
  encode_slot({.generation = generation,
               .last_record_offset = last_record_offset,
               .covered_seq = log_->next_seq() - 1},
              map_->snapshot(),
              std::span<uint8_t, kCheckpointSlotSize>(ckpt_buffer_.data(), kCheckpointSlotSize));
  if (Status s = io_->ckpt_pwrite(checkpoint_slot_offset(next_slot_), ckpt_buffer_.data(),
                                  ckpt_buffer_.size());
      !s.ok()) {
    fail("write", s);
    return;
  }
  if (Status s = io_->ckpt_fsync(); !s.ok()) {
    fail("fsync", s);
    return;
  }

  // 4. A new file's name is only durable once its directory is synced (D26).
  if (created) {
    if (Status s = io_->fsync_parent_dir(); !s.ok()) {
      fail("directory fsync", s);
      return;
    }
  }

  // Done: this slot is now the newest, so the other one is next. Only completed checkpoints
  // count toward the durations.
  const auto elapsed = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                            started)
          .count());
  generation_ = generation;
  next_slot_ = 1 - next_slot_;
  last_checkpoint_end_ = log_->end();
  ++counters_.written;
  ++counters_.duration_count;
  counters_.duration_sum_us += elapsed;
  counters_.duration_max_us = std::max(counters_.duration_max_us, elapsed);
}

}  // namespace logblock
