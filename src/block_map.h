#pragma once

// The in-memory block map: for each of the device's blocks, the log offset of
// its latest DATA record, or "unmapped", which reads as zeros.

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

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

 private:
  std::vector<uint64_t> offsets_;
};

}  // namespace logblock
