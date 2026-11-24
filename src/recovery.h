#pragma once

// Startup recovery (D28): rebuild the block map from the log, deciding what to trust and what
// to refuse. Engine::create() calls recover() once, before it accepts any request.

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "block_map.h"
#include "checkpoint.h"
#include "logblock/engine.h"
#include "logblock/io.h"
#include "logblock/status.h"
#include "record.h"

namespace logblock {

// What recovery learned and Engine needs to start serving.
struct RecoveredState {
  std::unique_ptr<BlockMap> map;
  // Just past the last valid record, and the seq the next record gets (FR-003).
  uint64_t end = 0;
  uint64_t next_seq = 1;
  RecoveryReport report;
  // Checkpoint bookkeeping. last_checkpoint_end is where the record at the newest valid slot's
  // last_record_offset ends (0 if no slot was used), and generation is that slot's generation
  // (0 if none), so the next checkpoint can number itself generation + 1.
  uint64_t last_checkpoint_end = 0;
  uint64_t generation = 0;
  // Where the next checkpoint goes: the slot that is not the newest valid one (D24). A missing or
  // unreadable slot counts as older, and with no valid slot the answer is A.
  std::string ckpt_path;
  int next_slot = 0;
  // True if recovery already opened the checkpoint file (an Io opens it only once).
  bool ckpt_open = false;
  // Where the last valid record starts (meaningful when end > 0): a checkpoint taken now names it.
  uint64_t last_record_offset = 0;
  // No slot was usable but at least one was unreadable and the log has records, so the engine
  // writes a fresh checkpoint before serving (FR-016).
  bool fresh_checkpoint_due = false;
};

// The checkpoint file that belongs to a log: "<log>.ckpt" (D22).
inline std::string checkpoint_path(const std::string& log_path) { return log_path + ".ckpt"; }

// Applies one log record to the block map: DATA points its block at the record's offset, and
// TRIM unmaps the blocks it covers (D29). Replay calls this for every scanned record, so the
// live write path and recovery cannot disagree about what a record means.
void apply_record(BlockMap& map, const RecordHeader& header, uint64_t record_offset);

// Both checkpoint slots as read from disk. The checkpoint file can be opened only once per Io, so
// when Engine::create has to look at the slots before the log exists, it hands what it read on to
// recover() instead of reading again.
struct CheckpointSlots {
  std::array<DecodedSlot, 2> slot;
  bool read = false;
  bool ckpt_open = false;  // open_ckpt succeeded and found the file
};

// Recovers the log that `io` has open. `log_path` names it in messages. On success *out holds the
// state to serve from; on failure nothing has been served, and the files are as they were
// except for the torn-tail exceptions in FR-009.
// `preread`, if given, holds slots check_checkpoints_without_log() already read.
Status recover(Io& io, const std::string& log_path, uint64_t cap, RecoveredState* out,
               CheckpointSlots* preread = nullptr);

// For a log file that does not exist yet: refuses (and changes nothing) if the checkpoint file
// holds a slot that proves a device once lived here. Starting an empty device over a checkpoint
// would quietly lose that data, so the operator has to decide (data-model.md, startup decisions).
// Call it before open_log, which would create the file. An I/O error reading the checkpoint file
// counts as unreadable slots, not as a refusal (D32).
Status check_checkpoints_without_log(Io& io, const std::string& log_path,
                                     CheckpointSlots* slots);

}  // namespace logblock
