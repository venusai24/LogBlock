#include "recovery.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "checkpoint.h"
#include "log_scan.h"

namespace logblock {

void apply_record(BlockMap& map, const RecordHeader& header, uint64_t record_offset) {
  // The scanner has already checked the record, including that its block range is inside the
  // device (rule 6), so the map calls' preconditions hold.
  switch (header.type) {
    case RecordType::Data:
      map.set(header.block_no, record_offset);
      break;
    case RecordType::Trim:
      map.clear_range(header.block_no, header.block_count);
      break;
  }
}

namespace {

// Reads one slot's bytes and decodes them. A slot is missing when the file ends at or before its
// first byte. Any I/O error makes it unreadable (D32), never a refusal: the other slot, or a full
// scan, can still serve.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a file size and a slot index.
DecodedSlot read_slot(Io& io, uint64_t ckpt_size, int index) {
  const uint64_t offset = static_cast<uint64_t>(index) * kCheckpointSlotSize;
  if (ckpt_size <= offset) {
    return {};  // Missing
  }
  // At most one slot: bytes after it belong to the other slot.
  const uint64_t want = std::min<uint64_t>(ckpt_size - offset, kCheckpointSlotSize);
  std::vector<uint8_t> bytes(want);
  uint64_t got = 0;
  while (got < want) {
    size_t n = 0;
    if (Status s = io.ckpt_pread(offset + got, bytes.data() + got, want - got, &n); !s.ok()) {
      DecodedSlot bad;
      bad.state = SlotState::Unreadable;
      bad.reason = "read failed: " + s.message();
      return bad;
    }
    if (n == 0) {
      break;  // the file ended sooner than its size said; decode sees a short slot
    }
    got += n;
  }
  bytes.resize(got);
  return decode_slot(bytes);
}

// Reads both slots. A checkpoint file that is absent (ENOENT) gives two missing slots, silently.
// A file that exists but cannot be opened refuses the start (D34): the operator can fix that, and
// guessing "no checkpoint" could hide a device whose log was lost. The error is returned bare:
// the right advice depends on whether the log exists, so each caller adds its own. Once the file is open, a failed
// size or read only makes the slots unreadable (D32), because the log alone can still rebuild.
Status read_slots(Io& io, const std::string& log_path, CheckpointSlots* slots) {
  slots->read = true;
  DecodedSlot* out = slots->slot.data();
  const std::string path = checkpoint_path(log_path);
  bool opened = false;
  bool created = false;
  if (Status s = io.open_ckpt(path, /*create=*/false, &opened, &created); !s.ok()) {
    return {s.code(), "cannot open checkpoint file '" + path + "': " + s.message()};
  }
  if (!opened) {
    return Status::Ok();  // both stay Missing
  }
  slots->ckpt_open = true;
  uint64_t size = 0;
  if (Status s = io.ckpt_size(&size); !s.ok()) {
    for (int i = 0; i < 2; ++i) {
      out[i].state = SlotState::Unreadable;
      out[i].reason = "cannot read the checkpoint file: " + s.message();
    }
    return Status::Ok();
  }
  for (int i = 0; i < 2; ++i) {
    out[i] = read_slot(io, size, i);
  }
  return Status::Ok();
}

char slot_name(int index) { return index == 0 ? 'A' : 'B'; }

Status inconsistent_slot(const std::string& log_path, int index, const std::string& why) {
  return {StatusCode::Corrupt, "checkpoint '" + checkpoint_path(log_path) +
                                   "' slot=" + slot_name(index) + " is inconsistent: " + why +
                                   " (nothing was changed; an inconsistent slot is never skipped, " +
                                   "so the start is refused even if the other slot is usable)"};
}

Status log_missing_but_checkpoint_valid(const std::string& log_path, int index,
                                        const std::string& detail) {
  return {StatusCode::Corrupt,
          "log '" + log_path + "' is missing or empty, but checkpoint '" +
              checkpoint_path(log_path) + "' slot=" + slot_name(index) + " " + detail +
              "; starting an empty device would lose that data, so the start was refused (nothing "
              "was changed). Restore the log, or to start over remove '" +
              checkpoint_path(log_path) + "'"};
}

// Reads up to `len` bytes at `offset` of the log, stopping early only at the end of the file.
Status read_log_bytes(Io& io, uint64_t offset, std::vector<uint8_t>* out, size_t len) {
  out->assign(len, 0);
  size_t got = 0;
  while (got < len) {
    size_t n = 0;
    if (Status s = io.pread(offset + got, out->data() + got, len - got, &n); !s.ok()) {
      return s;
    }
    if (n == 0) {
      break;
    }
    got += n;
  }
  out->resize(got);
  return Status::Ok();
}

// Checks a slot whose CRC passed against the log (read order step 6). Returns a non-ok Status only
// for an I/O error. If the slot disagrees with the log, *problem says how, and is empty otherwise.
// *covered_end is where the covered record ends, which is where replay starts.
Status check_against_log(Io& io, const DecodedSlot& slot, uint64_t log_size, std::string* problem,
                         uint64_t* covered_end) {
  const SlotHeader& h = slot.header;

  // A map entry is a record that was already in the log when the checkpoint was taken, so it
  // starts at or before the last covered record.
  for (uint64_t block = 0; block < slot.map.size(); ++block) {
    const uint64_t entry = slot.map[block];
    if (entry != BlockMap::kUnmapped && entry > h.last_record_offset) {
      *problem = "block " + std::to_string(block) + " maps to offset " + std::to_string(entry) +
                 ", past last_record_offset " + std::to_string(h.last_record_offset);
      return Status::Ok();
    }
  }

  // A valid record with covered_seq must start exactly at last_record_offset.
  if (h.last_record_offset >= log_size) {
    *problem = "last_record_offset " + std::to_string(h.last_record_offset) +
               " is not inside the log, which holds " + std::to_string(log_size) + " bytes";
    return Status::Ok();
  }
  std::vector<uint8_t> bytes;
  if (Status s = read_log_bytes(io, h.last_record_offset, &bytes, kDataRecordSize); !s.ok()) {
    return s;
  }
  Record record;
  if (Status s = decode_record(bytes, &record); !s.ok()) {
    *problem = "no valid record starts at last_record_offset " +
               std::to_string(h.last_record_offset) + ": " + s.message();
    return Status::Ok();
  }
  if (record.header.seq != h.covered_seq) {
    *problem = "the record at last_record_offset " + std::to_string(h.last_record_offset) +
               " has seq " + std::to_string(record.header.seq) + ", not covered_seq " +
               std::to_string(h.covered_seq);
    return Status::Ok();
  }
  *covered_end = h.last_record_offset +
                 (record.header.type == RecordType::Data ? kDataRecordSize : kTrimRecordSize);
  return Status::Ok();
}

}  // namespace

Status check_checkpoints_without_log(Io& io, const std::string& log_path,
                                     CheckpointSlots* checkpoint_slots) {
  if (Status s = read_slots(io, log_path, checkpoint_slots); !s.ok()) {
    // The log is missing, so the checkpoint file may be the only sign a device lived here.
    return {s.code(), s.message() + "; the log is missing ('" + log_path +
                          "'), and the checkpoint file may be the only sign a device "
                          "lived here (nothing was changed; fix the file, or remove it to start "
                          "an empty device)"};
  }
  const DecodedSlot* slots = checkpoint_slots->slot.data();
  for (int i = 0; i < 2; ++i) {
    if (slots[i].state == SlotState::Valid) {
      return log_missing_but_checkpoint_valid(
          log_path, i,
          "holds a valid checkpoint (generation " + std::to_string(slots[i].header.generation) +
              ")");
    }
    if (slots[i].state == SlotState::GeometryMismatch) {
      return inconsistent_slot(log_path, i, slots[i].reason);
    }
  }
  return Status::Ok();
}

Status recover(Io& io, const std::string& log_path, uint64_t cap, RecoveredState* out,
               CheckpointSlots* preread) {
  const auto started = std::chrono::steady_clock::now();
  RecoveryReport& report = out->report;

  uint64_t file_size = 0;
  if (Status s = io.file_size(&file_size); !s.ok()) {
    return s;
  }

  // Look at both checkpoint slots first. Nothing here changes a byte (FR-009).
  CheckpointSlots local;
  CheckpointSlots& read = preread != nullptr ? *preread : local;
  if (!read.read) {
    if (Status s = read_slots(io, log_path, &read); !s.ok()) {
      // The log exists, so a full scan rebuilds everything the checkpoint would have.
      return {s.code(), s.message() + " (nothing was changed; it is safe to remove the checkpoint "
                                      "file, which only costs a full scan)"};
    }
  }
  DecodedSlot* slots = read.slot.data();
  for (int i = 0; i < 2; ++i) {
    if (slots[i].state == SlotState::Unreadable) {
      ++report.unreadable_checkpoints;
      report.warnings.push_back("checkpoint slot=" + std::string(1, slot_name(i)) +
                                " file=" + checkpoint_path(log_path) +
                                " unreadable: " + slots[i].reason);
    }
  }

  // A slot with a good CRC but another geometry belongs to a different device. It refuses even if
  // the other slot is fine (FR-017): ignoring it could mean serving the wrong device.
  for (int i = 0; i < 2; ++i) {
    if (slots[i].state == SlotState::GeometryMismatch) {
      return inconsistent_slot(log_path, i, slots[i].reason);
    }
  }

  // Slots that the log vouches for, and where each one's covered record ends.
  int newest = -1;
  std::array<uint64_t, 2> ckpt_end = {0, 0};
  for (int i = 0; i < 2; ++i) {
    if (slots[i].state != SlotState::Valid) {
      continue;
    }
    if (file_size == 0) {
      return log_missing_but_checkpoint_valid(
          log_path, i,
          "holds a valid checkpoint (generation " + std::to_string(slots[i].header.generation) +
              ")");
    }
    std::string problem;
    if (Status s = check_against_log(io, slots[i], file_size, &problem, &ckpt_end[i]); !s.ok()) {
      return s;
    }
    if (!problem.empty()) {
      return inconsistent_slot(log_path, i, problem);
    }
    if (newest < 0 || slots[i].header.generation > slots[newest].header.generation) {
      newest = i;
    }
  }

  // Replay from the newest valid checkpoint's map, or from an empty map at the start of the log.
  // The scan streams through Io in chunks, so a 1 GiB log does not need 1 GiB of memory.
  auto map = std::make_unique<BlockMap>();
  uint64_t scan_from = 0;
  uint64_t expected_seq = 1;
  if (newest >= 0) {
    map->load(std::span<const uint64_t, kBlockCount>(slots[newest].map.data(), kBlockCount));
    scan_from = ckpt_end[newest];
    expected_seq = slots[newest].header.covered_seq + 1;
  }
  uint64_t replayed = 0;
  uint64_t last_record_offset = newest >= 0 ? slots[newest].header.last_record_offset : 0;
  StreamScanResult scan;
  Status s = scan_log_stream(
      io, scan_from, expected_seq,
      [&](const ScannedRecord& record) {
        apply_record(*map, record.header, record.offset);
        last_record_offset = record.offset;
        ++replayed;
      },
      &scan);
  if (!s.ok()) {
    return s;  // an Io error is the Io's status, never a classification of the log
  }

  // From here to the cap check nothing may change a byte: a refused start leaves the files as
  // they were (FR-009). The tail is only removed once every refusal has been ruled out.

  // Corruption anywhere in the scanned region refuses. Corrupt records are never skipped (FR-008).
  if (scan.stop == ScanStop::Corrupt) {
    return {StatusCode::Corrupt,
            "log '" + log_path + "' is corrupt at offset " + std::to_string(scan.valid_end) +
                ": " + scan.detail + "; last good seq " + std::to_string(scan.next_seq - 1)};
  }

  // The cap is judged on the valid log. A torn tail is about to be cut off, so it does not count.
  if (scan.valid_end > cap) {
    return {StatusCode::Failed,
            "log '" + log_path + "' holds " + std::to_string(scan.valid_end) +
                " bytes of valid records, which is more than the configured cap of " +
                std::to_string(cap) + " bytes; raise the cap to start (nothing was changed)"};
  }

  const bool torn = scan.stop == ScanStop::TornWrite;
  if (torn) {
    // Remove the torn tail (FR-006). If this fails the start is refused (FR-007): the bytes
    // are unchanged if the truncate itself failed, and the next start sees the same tail again.
    if (Status t = io.ftruncate(scan.valid_end); !t.ok()) {
      return t;
    }
    report.torn_offset = scan.valid_end;
    report.torn_bytes_removed = file_size - scan.valid_end;
    report.warnings.push_back("torn tail removed log=" + log_path +
                              " offset=" + std::to_string(report.torn_offset) +
                              " bytes=" + std::to_string(report.torn_bytes_removed));
  }

  // Make the log durable before the first new record lands after it (D28). That covers the
  // truncate above, and also any records a killed server left only in the page cache, which
  // this recovery has just served as current. An empty log that needed no truncate has nothing
  // to make durable.
  // If a later step fails after the truncate, the tail is already gone from the file even though
  // the start is refused (FR-007). Say so, so the operator knows the file differs (FR-009).
  auto refuse_after_step = [&](const Status& failed, const std::string& what) -> Status {
    if (!torn) {
      return failed;
    }
    return {failed.code(), "cannot " + what + " after removing a torn tail: " + failed.message() +
                               " (the torn tail was already removed, " +
                               std::to_string(report.torn_bytes_removed) +
                               " bytes; the next start handles the log normally)"};
  };
  if (torn || scan.valid_end > 0) {
    if (Status f = io.fsync(); !f.ok()) {
      return refuse_after_step(f, "make log '" + log_path + "' durable");
    }
  }
  // The log's name is made durable on every successful start, whether or not this start created
  // the file: an earlier start may have created it and been refused or killed before this sync
  // (D35). It is idempotent, and nothing is served before it succeeds.
  if (Status d = io.fsync_parent_dir(); !d.ok()) {
    return refuse_after_step(d, "sync the directory of log '" + log_path + "'");
  }

  if (newest >= 0) {
    report.source = RecoverySource::Checkpoint;
    report.slot = slot_name(newest);
    report.generation = slots[newest].header.generation;
    out->last_checkpoint_end = ckpt_end[newest];
    out->generation = report.generation;
  } else {
    report.source = scan.valid_end > 0 ? RecoverySource::FullScan : RecoverySource::None;
  }
  out->ckpt_path = checkpoint_path(log_path);
  out->next_slot = newest >= 0 ? 1 - newest : 0;
  out->ckpt_open = read.ckpt_open;
  out->last_record_offset = last_record_offset;
  out->fresh_checkpoint_due =
      newest < 0 && report.unreadable_checkpoints > 0 && scan.valid_end > 0;
  report.records_replayed = replayed;
  report.log_bytes_scanned = scan.valid_end - scan_from;
  report.time_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count());
  out->map = std::move(map);
  out->end = scan.valid_end;
  out->next_seq = scan.next_seq;
  return Status::Ok();
}

}  // namespace logblock
