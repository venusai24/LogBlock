// The in-memory block map: a flat array indexed by block number, holding
// the log offset of each block's latest DATA record.
#include "block_map.h"

#include <algorithm>
#include <cassert>

#include "logblock/engine.h"

namespace logblock {

// A dense array is the simplest structure that fits: block numbers are
// 0 .. 262,143, so lookup is one index and the whole map is 2 MiB.
BlockMap::BlockMap() : offsets_(kBlockCount, kUnmapped) {}

uint64_t BlockMap::size() const { return offsets_.size(); }

std::optional<uint64_t> BlockMap::get(uint64_t block) const {
  assert(block < offsets_.size());
  const uint64_t offset = offsets_[block];
  if (offset == kUnmapped) {
    return std::nullopt;  // never written, or trimmed: reads as zeros
  }
  return offset;
}

void BlockMap::set(uint64_t block, uint64_t log_offset) {
  assert(block < offsets_.size());
  // Storing the sentinel would silently unmap the block.
  assert(log_offset != kUnmapped);
  offsets_[block] = log_offset;
}

void BlockMap::clear_range(uint64_t first_block, uint64_t count) {
  // The range check is written as two comparisons, not first_block + count <=
  // size(), because that sum can wrap around in 64 bits (UINT64_MAX + 2 is 1)
  // and then look valid. count <= size() makes size() - count safe to compute.
  assert(count <= offsets_.size() && first_block <= offsets_.size() - count);
  std::fill_n(offsets_.begin() + static_cast<std::ptrdiff_t>(first_block), count, kUnmapped);
}

}  // namespace logblock
