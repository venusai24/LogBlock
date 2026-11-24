#pragma once

// The in-memory block map: for each of the device's blocks, the log offset of
// its latest DATA record, or "unmapped", which reads as zeros.

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "logblock/engine.h"

namespace logblock {

class BlockMap {
 public:
  // The value stored for an unmapped block. It can never be a real log offset,
  // so offset 0 (the first record in the log) is still a valid mapped value.
  static constexpr uint64_t kUnmapped = std::numeric_limits<uint64_t>::max();

  // One entry per block of the device (kBlockCount), all unmapped.
  BlockMap();

  // Number of entries: kBlockCount.
  [[nodiscard]] uint64_t size() const;

  // The log offset of the block's latest DATA record, or std::nullopt if the
  // block is unmapped. Precondition: block < size().
  [[nodiscard]] std::optional<uint64_t> get(uint64_t block) const;

  // Points the block at a DATA record. Preconditions: block < size(), and
  // log_offset != kUnmapped.
  void set(uint64_t block, uint64_t log_offset);

  // Unmaps `count` blocks starting at first_block, so they read as zeros.
  // Precondition: the whole range is inside the map. count may be 0.
  void clear_range(uint64_t first_block, uint64_t count);

  // All entries, indexed by block number; unmapped blocks hold kUnmapped. This
  // is a view of the live map, not a copy: it sees later set()/clear_range()/
  // load() calls and stays valid for the life of the BlockMap (the vector never
  // resizes). A checkpoint writer must copy it if the map can change meanwhile.
  [[nodiscard]] std::span<const uint64_t, kBlockCount> snapshot() const;

  // Replaces every entry with `entries` (kUnmapped marks an unmapped block).
  // The caller passes entries from a validated checkpoint.
  void load(std::span<const uint64_t, kBlockCount> entries);

 private:
  std::vector<uint64_t> offsets_;
};

}  // namespace logblock
