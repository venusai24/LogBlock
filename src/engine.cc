// The storage engine: create(), read(), write(), trim() and flush() over the block map and
// the append-only log.
#include "logblock/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "block_map.h"
#include "log_file.h"

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

Engine::Engine(PrivateKey, std::unique_ptr<Io> io, uint64_t cap)
    : io_(std::move(io)),
      cap_(cap),
      map_(std::make_unique<BlockMap>()),
      log_(std::make_unique<LogFile>(*io_)) {}

Status Engine::create(const std::string& path, uint64_t cap, std::unique_ptr<Io> io,
                      std::unique_ptr<Engine>* out) {
  // Check the setting before touching the file, so a bad cap never creates a log.
  if (cap < kDataRecordSize) {
    return {StatusCode::OutOfRange,
            "cap " + std::to_string(cap) + " is too small: it must be at least " +
                std::to_string(kDataRecordSize) + " bytes, the size of one DATA record"};
  }

  if (Status s = io->open_log(path); !s.ok()) {
    return s;
  }

  // M1 has no recovery, so a log that already holds records is refused as it is.
  // Only reads happen here: the file must stay byte-for-byte unchanged.
  uint64_t existing_size = 0;
  if (Status s = io->file_size(&existing_size); !s.ok()) {
    return s;
  }
  if (existing_size != 0) {
    return {StatusCode::Failed, "log '" + path + "' is not empty (" +
                                    std::to_string(existing_size) +
                                    " bytes) and M1 has no recovery; give a new or empty log"};
  }

  // A newly created file's name is only durable once its directory is synced
  //, so do it before reporting that the engine exists.
  if (Status s = io->fsync_parent_dir(); !s.ok()) {
    return s;
  }

  *out = std::make_unique<Engine>(PrivateKey{}, std::move(io), cap);
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
  std::copy(record.payload.begin(), record.payload.end(), out);
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
  if (Status s = log_->append_trim(first_block, static_cast<uint32_t>(block_count)); !s.ok()) {
    return s;
  }
  map_->clear_range(first_block, block_count);
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

}  // namespace logblock
