#pragma once

// On-disk checkpoint slot: encode and decode (checkpoint format version 1).
// The layout, the CRC rule and the read order are in docs/formats.md ("Checkpoint format v1").
//
// OWNER: human. T007 implements encode_slot() and decode_slot() in checkpoint.cc. The names and
// signatures here are proposals, and the owner may change them (tests/unit/checkpoint_test.cc
// then changes with them).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "logblock/engine.h"

namespace logblock {

inline constexpr uint32_t kCheckpointVersion = 1;
inline constexpr uint64_t kCheckpointHeaderSize = 48;
// What a slot in this build can hold: one u64 per block of the device.
inline constexpr uint64_t kCheckpointMaxBlockCount = kBlockCount;
// 48 + 8 * 262,144 = 2,097,200 bytes. Slot B starts at this offset in the file.
inline constexpr uint64_t kCheckpointSlotSize =
    kCheckpointHeaderSize + 8 * kCheckpointMaxBlockCount;

// Where slot A (index 0) or slot B (index 1) starts in the checkpoint file.
inline constexpr uint64_t checkpoint_slot_offset(int index) {
  return static_cast<uint64_t>(index) * kCheckpointSlotSize;
}

// The header fields a caller needs. magic, version, block_size, block_count and the CRC are fixed
// by the format and this build, so they are not stored here: encoding writes them, and decoding
// checks them.
struct SlotHeader {
  uint64_t generation = 0;
  uint64_t last_record_offset = 0;  // where the last covered record starts in the log
  uint64_t covered_seq = 0;         // that record's seq
};

// Writes one whole slot into `out`: the header, then the map entries, with the CRC filled in.
// `map` has one entry per block: the log offset of the block's latest DATA record, or
// 0xFFFFFFFFFFFFFFFF for a block that reads as zeros. block_size is 4096 and block_count is
// 262,144, this build's geometry.
void encode_slot(const SlotHeader& header, std::span<const uint64_t, kBlockCount> map,
                 std::span<uint8_t, kCheckpointSlotSize> out);

// What a slot turned out to be. Valid, Missing and Unreadable are the first three rows of the
// slot-state table in docs/formats.md. GeometryMismatch is the CRC-passed row that decode can
// see on its own; the checks against the log (map entries, `last_record_offset`, `covered_seq`)
// belong to recovery, which needs the log.
enum class SlotState : uint8_t {
  Missing,           // no bytes: the file is absent or ends at or before the slot's first byte
  Unreadable,        // fails read-order steps 2 to 4
  GeometryMismatch,  // the CRC passed, but block_size or block_count differs from this build's
  Valid
};

struct DecodedSlot {
  SlotState state = SlotState::Missing;
  // Why the slot is not Valid, for the warning or the refusal message. Empty when it is.
  std::string reason;
  // Filled in only when state is Valid.
  SlotHeader header;
  std::vector<uint64_t> map;  // kBlockCount entries
};

// Decodes the slot that starts at bytes[0], following the read order in docs/formats.md and
// stopping at the first failure.
//
// `bytes` is what the file holds from the slot's first byte, at most one slot long. Bytes after
// kCheckpointSlotSize are never looked at, so a caller may pass the rest of the file. An empty
// `bytes` is Missing. Nothing is read past the end of `bytes`, however large block_count claims
// to be: a corrupt field must not turn into an out-of-bounds read or an overflowed length.
DecodedSlot decode_slot(std::span<const uint8_t> bytes);

}  // namespace logblock
