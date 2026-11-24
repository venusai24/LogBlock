#pragma once

// Builds checkpoint slots that are consistent with a log image, for tests that start an engine on
// a checkpoint file. Test-only, header-only.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "block_map.h"
#include "checkpoint.h"
#include "log_scan.h"
#include "recovery.h"
#include "support/fault_io.h"

namespace logblock::test {

// Where each slot starts in the checkpoint file.
inline uint64_t slot_offset(int index) { return static_cast<uint64_t>(index) * kCheckpointSlotSize; }

// What a checkpoint taken just after the record with seq `upto_seq` holds: the block map after
// replaying records 1..upto_seq of `log`, the offset that record starts at, and the offset it ends.
struct MapAt {
  std::vector<uint64_t> map;
  uint64_t last_record_offset = 0;
  uint64_t end = 0;
};

inline MapAt map_at(const std::string& log, uint64_t upto_seq) {
  ScanResult scan = scan_log(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(log.data()), log.size()));
  BlockMap map;
  MapAt result;
  for (const ScannedRecord& r : scan.records) {
    if (r.header.seq > upto_seq) {
      break;
    }
    apply_record(map, r.header, r.offset);
    result.last_record_offset = r.offset;
    result.end = r.offset + (r.header.type == RecordType::Data ? kDataRecordSize : kTrimRecordSize);
  }
  auto snapshot = map.snapshot();
  result.map.assign(snapshot.begin(), snapshot.end());
  return result;
}

// One whole encoded slot for a checkpoint taken after record `upto_seq`.
inline std::string make_slot(const std::string& log, uint64_t generation, uint64_t upto_seq) {
  MapAt at = map_at(log, upto_seq);
  std::string slot(kCheckpointSlotSize, '\0');
  encode_slot({generation, at.last_record_offset, upto_seq},
              std::span<const uint64_t, kBlockCount>(at.map.data(), kBlockCount),
              std::span<uint8_t, kCheckpointSlotSize>(reinterpret_cast<uint8_t*>(slot.data()),
                                                      kCheckpointSlotSize));
  return slot;
}

// Puts `slot` in slot A (index 0) or B (index 1) of the fake checkpoint file, growing the file
// with zeros if slot B is written before slot A.
inline void put_slot(FaultIo& io, int index, const std::string& slot) {
  std::string& file = io.ckpt_bytes();
  if (file.size() < slot_offset(index) + slot.size()) {
    file.resize(slot_offset(index) + slot.size(), '\0');
  }
  file.replace(slot_offset(index), slot.size(), slot);
}

}  // namespace logblock::test
