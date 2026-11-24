#include "checkpoint.h"

#include <crc32c/crc32c.h>

#include <array>

#include <cstring>
#include <format>
#include <utility>

namespace logblock {
namespace {

// Byte offsets of the slot header fields (docs/formats.md, "Checkpoint format v1").
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffGeneration = 8;
constexpr size_t kOffLastRecordOffset = 16;
constexpr size_t kOffCoveredSeq = 24;
constexpr size_t kOffBlockCount = 32;
constexpr size_t kOffBlockSize = 40;
constexpr size_t kOffCrc = 44;  // the CRC covers everything before this, then the map
constexpr size_t kOffMap = 48;  // == kCheckpointHeaderSize

constexpr std::array<uint8_t, 4> kMagic = {'L', 'B', 'C', 'K'};

// Fields are written and read one at a time, least significant byte first, so the format never
// depends on struct padding or the host's byte order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a value and a byte width, always named.
void put_le(uint8_t* p, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    p[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

uint64_t get_le(const uint8_t* p, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return value;
}

// CRC32C over header bytes 0-43, then `block_count` map entries. The CRC's own four bytes (44-47)
// are skipped, as in a log record. block_count must already be known to be at most the slot
// capacity, so 8 * block_count cannot overflow.
uint32_t slot_crc(const uint8_t* slot, uint64_t block_count) {
  uint32_t crc = crc32c::Crc32c(slot, kOffCrc);
  return crc32c::Extend(crc, slot + kOffMap, 8 * block_count);
}

DecodedSlot unreadable(std::string reason) {
  DecodedSlot result;
  result.state = SlotState::Unreadable;
  result.reason = std::move(reason);
  return result;
}

}  // namespace

void encode_slot(const SlotHeader& header, std::span<const uint64_t, kBlockCount> map,
                 std::span<uint8_t, kCheckpointSlotSize> out) {
  uint8_t* p = out.data();
  std::memcpy(p + kOffMagic, kMagic.data(), kMagic.size());  // 4 plain bytes, not a struct
  put_le(p + kOffVersion, kCheckpointVersion, 4);
  put_le(p + kOffGeneration, header.generation, 8);
  put_le(p + kOffLastRecordOffset, header.last_record_offset, 8);
  put_le(p + kOffCoveredSeq, header.covered_seq, 8);
  put_le(p + kOffBlockCount, kBlockCount, 8);
  put_le(p + kOffBlockSize, kBlockSize, 4);
  for (uint64_t i = 0; i < kBlockCount; ++i) {
    put_le(p + kOffMap + 8 * i, map[i], 8);
  }
  // Last, because the CRC covers the fields and entries written above.
  put_le(p + kOffCrc, slot_crc(p, kBlockCount), 4);
}

DecodedSlot decode_slot(std::span<const uint8_t> bytes) {
  const uint8_t* p = bytes.data();

  // Step 1: presence. No bytes means the file is absent or ends before this slot.
  if (bytes.empty()) {
    return {};  // state defaults to Missing
  }

  // Step 2: magic and version come first, so an unknown format is rejected before any other
  // field is trusted.
  if (bytes.size() < kOffGeneration) {
    return unreadable("file ends before magic and version");
  }
  if (std::memcmp(p + kOffMagic, kMagic.data(), kMagic.size()) != 0) {
    return unreadable("wrong magic");
  }
  const uint64_t version = get_le(p + kOffVersion, 4);
  if (version != kCheckpointVersion) {
    return unreadable(std::format("unknown version {}", version));
  }

  // Step 3: block_count must fit a slot before it is used in any length. A corrupt value near
  // UINT64_MAX would otherwise overflow 8 * block_count, or ask for a read far past the slot.
  if (bytes.size() < kCheckpointHeaderSize) {
    return unreadable("file ends inside the header, before block_count and the crc");
  }
  const uint64_t block_count = get_le(p + kOffBlockCount, 8);
  if (block_count > kCheckpointMaxBlockCount) {
    return unreadable(std::format("block_count {} is above the slot capacity {}", block_count,
                                  kCheckpointMaxBlockCount));
  }

  // Step 4: the CRC, over the length block_count implies. The file must hold all of it.
  const uint64_t needed = kCheckpointHeaderSize + 8 * block_count;
  if (bytes.size() < needed) {
    return unreadable(
        std::format("file ends partway through the map ({} of {} bytes)", bytes.size(), needed));
  }
  const auto stored = static_cast<uint32_t>(get_le(p + kOffCrc, 4));
  const uint32_t computed = slot_crc(p, block_count);
  if (stored != computed) {
    return unreadable(
        std::format("crc32c mismatch (stored {:#010x}, computed {:#010x})", stored, computed));
  }

  // Step 5: only now are block_size and block_count trusted, so one damaged bit in either
  // makes a slot unreadable (above) and never refuses startup.
  DecodedSlot result;
  const uint64_t block_size = get_le(p + kOffBlockSize, 4);
  if (block_size != kBlockSize) {
    result.state = SlotState::GeometryMismatch;
    result.reason =
        std::format("block_size {} differs from this build's {}", block_size, kBlockSize);
    return result;
  }
  if (block_count != kBlockCount) {
    result.state = SlotState::GeometryMismatch;
    result.reason =
        std::format("block_count {} differs from this build's {}", block_count, kBlockCount);
    return result;
  }

  result.state = SlotState::Valid;
  result.header.generation = get_le(p + kOffGeneration, 8);
  result.header.last_record_offset = get_le(p + kOffLastRecordOffset, 8);
  result.header.covered_seq = get_le(p + kOffCoveredSeq, 8);
  result.map.resize(block_count);
  for (uint64_t i = 0; i < block_count; ++i) {
    result.map[i] = get_le(p + kOffMap + 8 * i, 8);
  }
  return result;
}

}  // namespace logblock
