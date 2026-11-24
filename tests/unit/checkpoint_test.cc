#include "checkpoint.h"

#include <crc32c/crc32c.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace logblock {
namespace {

using Bytes = std::vector<uint8_t>;

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// Byte offsets of the slot header fields (docs/formats.md, "Checkpoint format v1").
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffGeneration = 8;
constexpr size_t kOffLastRecordOffset = 16;
constexpr size_t kOffCoveredSeq = 24;
constexpr size_t kOffBlockCount = 32;
constexpr size_t kOffBlockSize = 40;
constexpr size_t kOffCrc = 44;
constexpr size_t kOffMap = 48;

constexpr uint64_t kSentinel = std::numeric_limits<uint64_t>::max();

// ---------------------------------------------------------------------------
// Helpers that build slots by hand, so the decode tests do not depend on the
// encode code, and each rejection test breaks exactly one rule.
// ---------------------------------------------------------------------------

void put_le(Bytes& b, size_t offset, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    b[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// The test map: every 7th block is unmapped, the others point at "record" i * 4136, so block 0
// is unmapped and block 1 sits at offset 4136. Every value is predictable from its index.
uint64_t map_entry(uint64_t i) { return i % 7 == 0 ? kSentinel : i * 4136; }

std::vector<uint64_t> pattern_map(uint64_t count = kBlockCount) {
  std::vector<uint64_t> m(count);
  for (uint64_t i = 0; i < count; ++i) {
    m[i] = map_entry(i);
  }
  return m;
}

// Recomputes the CRC over header bytes 0-43 followed by the map entries, and stores it.
void seal(Bytes& slot) {
  uint32_t crc = crc32c::Crc32c(slot.data(), kOffCrc);
  if (slot.size() > kOffMap) {
    crc = crc32c::Extend(crc, slot.data() + kOffMap, slot.size() - kOffMap);
  }
  put_le(slot, kOffCrc, crc, 4);
}

// A sealed slot built field by field. The defaults are a valid slot for this build.
struct SlotSpec {
  uint32_t magic_byte0 = 'L';  // magic is "LBCK"; tests change one byte at a time
  uint32_t version = 1;
  uint64_t generation = 0x1122334455667788ULL;
  uint64_t last_record_offset = 0x0000000100000002ULL;
  uint64_t covered_seq = 0x0A0B0C0DULL;
  uint64_t block_count = kBlockCount;  // also the number of entries written
  uint32_t block_size = kBlockSize;
};

Bytes build_slot(const SlotSpec& spec = {}) {
  Bytes slot(kOffMap + 8 * spec.block_count, 0);
  slot[kOffMagic + 0] = static_cast<uint8_t>(spec.magic_byte0);
  slot[kOffMagic + 1] = 'B';
  slot[kOffMagic + 2] = 'C';
  slot[kOffMagic + 3] = 'K';
  put_le(slot, kOffVersion, spec.version, 4);
  put_le(slot, kOffGeneration, spec.generation, 8);
  put_le(slot, kOffLastRecordOffset, spec.last_record_offset, 8);
  put_le(slot, kOffCoveredSeq, spec.covered_seq, 8);
  put_le(slot, kOffBlockCount, spec.block_count, 8);
  put_le(slot, kOffBlockSize, spec.block_size, 4);
  for (uint64_t i = 0; i < spec.block_count; ++i) {
    put_le(slot, kOffMap + size_t{8} * i, map_entry(i), 8);
  }
  seal(slot);
  return slot;
}

// The decoder gets a copy of exactly these bytes, so an out-of-bounds read is a heap overflow
// that AddressSanitizer reports, instead of reading a neighbouring byte unnoticed.
DecodedSlot decode_exact(const Bytes& bytes) {
  Bytes copy = bytes;
  copy.shrink_to_fit();
  return decode_slot(std::span<const uint8_t>(copy.data(), copy.size()));
}

Bytes prefix(const Bytes& b, size_t n) {
  return Bytes(b.begin(), b.begin() + static_cast<long>(n));
}

// Compares two byte strings and, on a difference, names the first offset instead of dumping
// two megabytes.
void expect_same_bytes(const Bytes& want, const Bytes& got) {
  ASSERT_EQ(want.size(), got.size());
  auto [w, g] = std::mismatch(want.begin(), want.end(), got.begin());
  if (w != want.end()) {
    ADD_FAILURE() << "first difference at byte " << (w - want.begin()) << ": expected 0x"
                  << std::hex << int{*w} << ", got 0x" << int{*g};
  }
}

std::span<const uint64_t, kBlockCount> as_map_span(const std::vector<uint64_t>& m) {
  return std::span<const uint64_t, kBlockCount>(m.data(), kBlockCount);
}

Bytes encode(const SlotHeader& header, const std::vector<uint64_t>& map, uint8_t fill = 0) {
  Bytes out(kCheckpointSlotSize, fill);
  encode_slot(header, as_map_span(map),
              std::span<uint8_t, kCheckpointSlotSize>(out.data(), kCheckpointSlotSize));
  return out;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

TEST(CheckpointFormat, SlotSizeIsTheHeaderPlusEightBytesPerBlock) {
  EXPECT_EQ(kCheckpointHeaderSize, 48U);
  EXPECT_EQ(kCheckpointMaxBlockCount, 262144U);
  EXPECT_EQ(kCheckpointSlotSize, 2097200U);
}

// ---------------------------------------------------------------------------
// Encoding: exact bytes.
// ---------------------------------------------------------------------------

TEST(CheckpointEncode, FreshMapExactBytes) {
  // A map with every block unmapped (the sentinel), for the first checkpoint of a device:
  // generation 1, the first record at offset 0 with seq 1. Header written out by hand from
  // docs/formats.md.
  const Bytes header = {
      0x4C, 0x42, 0x43, 0x4B,                          // magic "LBCK"
      0x01, 0x00, 0x00, 0x00,                          // version 1 (u32)
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // generation 1
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // last_record_offset 0
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // covered_seq 1
      0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,  // block_count 262144 = 0x40000
      0x00, 0x10, 0x00, 0x00,                          // block_size 4096
  };
  // CRC32C over those 44 bytes plus the 2 MiB of 0xFF entries, computed outside this program
  // with an independent bitwise implementation (checked against the published vectors for
  // "123456789" and 32 zero bytes): 0x85FCB592.
  const Bytes crc_le = {0x92, 0xB5, 0xFC, 0x85};

  Bytes expected = header;
  expected.insert(expected.end(), crc_le.begin(), crc_le.end());
  expected.insert(expected.end(), 8 * kBlockCount, 0xFF);
  ASSERT_EQ(expected.size(), kCheckpointSlotSize);

  std::vector<uint64_t> map(kBlockCount, kSentinel);
  expect_same_bytes(expected, encode({1, 0, 1}, map));
}

TEST(CheckpointEncode, FullSlotExactBytes) {
  // Distinct values make a byte-order or field-order mistake visible: generation, offset and
  // seq must each appear least significant byte first, in the order of the table.
  const Bytes header = {
      0x4C, 0x42, 0x43, 0x4B,                          // magic "LBCK"
      0x01, 0x00, 0x00, 0x00,                          // version 1
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // generation 0x1122334455667788
      0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,  // last_record_offset 0x100000002
      0x0D, 0x0C, 0x0B, 0x0A, 0x00, 0x00, 0x00, 0x00,  // covered_seq 0x0A0B0C0D
      0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,  // block_count 262144
      0x00, 0x10, 0x00, 0x00,                          // block_size 4096
  };
  // Computed the same way, over those 44 bytes plus the entries of map_entry(): 0x061EC149.
  const Bytes crc_le = {0x49, 0xC1, 0x1E, 0x06};

  Bytes expected = header;
  expected.insert(expected.end(), crc_le.begin(), crc_le.end());
  expected.resize(kCheckpointSlotSize);
  for (uint64_t i = 0; i < kBlockCount; ++i) {
    put_le(expected, kOffMap + size_t{8} * i, map_entry(i), 8);
  }

  expect_same_bytes(expected,
                    encode({0x1122334455667788ULL, 0x100000002ULL, 0x0A0B0C0DULL}, pattern_map()));
}

TEST(CheckpointEncode, WritesEveryByteOfTheSlot) {
  // The encoder must not leave any byte of `out` as it found it.
  SlotHeader h{7, 4136, 9};
  std::vector<uint64_t> map = pattern_map();
  expect_same_bytes(encode(h, map, 0x00), encode(h, map, 0xAB));
}

TEST(CheckpointEncode, EncodedBytesMatchAHandBuiltSlot) {
  SlotSpec spec;
  spec.generation = 42;
  spec.last_record_offset = 8272;
  spec.covered_seq = 3;
  expect_same_bytes(build_slot(spec), encode({42, 8272, 3}, pattern_map()));
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(CheckpointRoundTrip, DecodeGivesBackWhatWasEncoded) {
  SlotHeader h{5, 123456, 99};
  std::vector<uint64_t> map = pattern_map();
  DecodedSlot d = decode_exact(encode(h, map));
  ASSERT_EQ(d.state, SlotState::Valid) << d.reason;
  EXPECT_TRUE(d.reason.empty());
  EXPECT_EQ(d.header.generation, 5U);
  EXPECT_EQ(d.header.last_record_offset, 123456U);
  EXPECT_EQ(d.header.covered_seq, 99U);
  EXPECT_EQ(d.map, map);
}

TEST(CheckpointRoundTrip, EdgeValuesSurvive) {
  // Offset 0 is a real mapped value, the sentinel is not an offset, and the largest header
  // values fit their fields.
  std::vector<uint64_t> map(kBlockCount, kSentinel);
  map[0] = 0;
  map[1] = kSentinel - 1;
  map[kBlockCount - 1] = 4136;
  SlotHeader h{std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(),
               std::numeric_limits<uint64_t>::max()};
  DecodedSlot d = decode_exact(encode(h, map));
  ASSERT_EQ(d.state, SlotState::Valid) << d.reason;
  EXPECT_EQ(d.header.generation, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(d.header.last_record_offset, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(d.header.covered_seq, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(d.map, map);
}

// ---------------------------------------------------------------------------
// Decoding a hand-built valid slot
// ---------------------------------------------------------------------------

TEST(CheckpointDecode, HandBuiltSlotIsReadFieldByField) {
  DecodedSlot d = decode_exact(build_slot());
  ASSERT_EQ(d.state, SlotState::Valid) << d.reason;
  EXPECT_EQ(d.header.generation, 0x1122334455667788ULL);
  EXPECT_EQ(d.header.last_record_offset, 0x0000000100000002ULL);
  EXPECT_EQ(d.header.covered_seq, 0x0A0B0C0DULL);
  EXPECT_EQ(d.map, pattern_map());
}

TEST(CheckpointDecode, DoesNotCheckTheMapAgainstTheLog) {
  // Whether a map entry points past last_record_offset, or covered_seq matches a record, is for
  // recovery to judge: it needs the log. The decoder only vouches for the bytes.
  Bytes slot = build_slot();
  put_le(slot, kOffMap + 8, 0x7000000000000000ULL, 8);  // far past last_record_offset
  seal(slot);
  EXPECT_EQ(decode_exact(slot).state, SlotState::Valid);
}

TEST(CheckpointDecode, BytesAfterTheSlotAreNeverLookedAt) {
  // The caller may hand over the rest of the file. Slot B's bytes follow slot A's.
  Bytes slot = build_slot();
  Bytes two = slot;
  two.insert(two.end(), kCheckpointSlotSize, 0xEE);
  DecodedSlot d = decode_slot(std::span<const uint8_t>(two.data(), two.size()));
  EXPECT_EQ(d.state, SlotState::Valid) << d.reason;
  EXPECT_EQ(d.map, pattern_map());
}

// ---------------------------------------------------------------------------
// Read order, one failure at a time (docs/formats.md, "Read order")
// ---------------------------------------------------------------------------

// Step 1
TEST(CheckpointDecode, NoBytesIsMissing) {
  DecodedSlot d = decode_slot(std::span<const uint8_t>());
  EXPECT_EQ(d.state, SlotState::Missing);
}

// Step 2
TEST(CheckpointDecode, FileEndingBeforeMagicAndVersionIsUnreadable) {
  Bytes slot = build_slot();
  ASSERT_EQ(decode_exact(slot).state, SlotState::Valid) << "control: the untouched slot must decode";
  for (size_t n = 1; n < kOffGeneration; ++n) {
    DecodedSlot d = decode_exact(prefix(slot, n));
    EXPECT_EQ(d.state, SlotState::Unreadable) << "length " << n;
    EXPECT_FALSE(d.reason.empty()) << "length " << n;
  }
}

TEST(CheckpointDecode, WrongMagicIsUnreadable) {
  for (uint32_t first : {uint32_t{'X'}, uint32_t{0}, uint32_t{'l'}}) {
    SlotSpec spec;
    spec.magic_byte0 = first;
    DecodedSlot d = decode_exact(build_slot(spec));  // sealed, so only the magic is wrong
    EXPECT_EQ(d.state, SlotState::Unreadable) << "first byte " << first;
    EXPECT_TRUE(contains(d.reason, "magic")) << d.reason;
  }
  // The log's own magic is not a checkpoint's.
  Bytes slot = build_slot();
  slot[kOffMagic + 2] = 'L';  // "LBLK"
  seal(slot);
  EXPECT_EQ(decode_exact(slot).state, SlotState::Unreadable);
  // A magic that is wrong in each of the four bytes.
  for (size_t i = 0; i < 4; ++i) {
    Bytes bad = build_slot();
    bad[kOffMagic + i] ^= 0x20;
    seal(bad);
    DecodedSlot d = decode_exact(bad);
    EXPECT_EQ(d.state, SlotState::Unreadable) << "magic byte " << i;
    EXPECT_TRUE(contains(d.reason, "magic")) << d.reason;
  }
}

TEST(CheckpointDecode, UnknownVersionIsUnreadable) {
  for (uint32_t version : {0U, 2U, 0xFFFFFFFFU, 0x00010001U /* the upper half set */}) {
    SlotSpec spec;
    spec.version = version;
    DecodedSlot d = decode_exact(build_slot(spec));
    EXPECT_EQ(d.state, SlotState::Unreadable) << "version " << version;
    EXPECT_TRUE(contains(d.reason, "version")) << d.reason;
  }
}

TEST(CheckpointDecode, MagicAndVersionAreCheckedBeforeAnyOtherField) {
  // An unknown version means a layout this build does not know, so no other field can be
  // trusted, including a block_count that would also be rejected, and a CRC that is wrong.
  Bytes header(kOffMap, 0);  // only the 48 header bytes exist
  header[0] = 'L';
  header[1] = 'B';
  header[2] = 'C';
  header[3] = 'K';
  put_le(header, kOffVersion, 2, 4);
  put_le(header, kOffBlockCount, std::numeric_limits<uint64_t>::max(), 8);
  put_le(header, kOffBlockSize, 12345, 4);
  DecodedSlot d = decode_exact(header);
  EXPECT_EQ(d.state, SlotState::Unreadable);
  EXPECT_TRUE(contains(d.reason, "version")) << d.reason;

  // And the magic is checked before the version.
  header[0] = 'X';
  d = decode_exact(header);
  EXPECT_TRUE(contains(d.reason, "magic")) << d.reason;
}

TEST(CheckpointDecode, FileEndingInsideTheHeaderIsUnreadable) {
  Bytes slot = build_slot();
  ASSERT_EQ(decode_exact(slot).state, SlotState::Valid) << "control: the untouched slot must decode";
  for (size_t n = kOffGeneration; n < kOffMap; ++n) {
    DecodedSlot d = decode_exact(prefix(slot, n));
    EXPECT_EQ(d.state, SlotState::Unreadable) << "length " << n;
  }
}

// Step 3
TEST(CheckpointDecode, BlockCountAboveCapacityIsUnreadableAndReadsNothingPastTheHeader) {
  // Only the 48 header bytes exist, in a buffer of exactly that size. Any attempt to read the
  // map the count implies is an out-of-bounds read, which AddressSanitizer reports. 2^61 is
  // there because 8 * 2^61 wraps to 0 in 64 bits, so a length computed before the range check
  // would look like an empty map.
  const std::vector<uint64_t> counts = {kBlockCount + 1,
                                        uint64_t{1} << 32,
                                        (uint64_t{1} << 32) + kBlockCount,
                                        uint64_t{1} << 61,
                                        uint64_t{1} << 63,
                                        std::numeric_limits<uint64_t>::max() - 3,
                                        std::numeric_limits<uint64_t>::max()};
  for (uint64_t count : counts) {
    Bytes header = prefix(build_slot(), kOffMap);
    put_le(header, kOffBlockCount, count, 8);
    seal(header);  // a CRC that matches the 48 bytes, so only block_count is wrong
    DecodedSlot d = decode_exact(header);
    EXPECT_EQ(d.state, SlotState::Unreadable) << "block_count " << count;
    EXPECT_TRUE(contains(d.reason, "block_count")) << d.reason;
  }
}

TEST(CheckpointDecode, BlockCountJustAboveCapacityIsUnreadableEvenWithAFullBody) {
  // The file really is long enough here (a slot plus a following slot), and the CRC is made to
  // match whatever the decoder might cover. The capacity check still refuses it.
  Bytes slot = build_slot();
  slot.insert(slot.end(), kCheckpointSlotSize, 0x11);
  put_le(slot, kOffBlockCount, kBlockCount + 1, 8);
  seal(slot);
  DecodedSlot d = decode_slot(std::span<const uint8_t>(slot.data(), slot.size()));
  EXPECT_EQ(d.state, SlotState::Unreadable);
  EXPECT_TRUE(contains(d.reason, "block_count")) << d.reason;
}

TEST(CheckpointDecode, BlockCountIsRangeCheckedBeforeTheCrcIsUsed) {
  // Not sealed: the CRC is wrong too, but the reason must be the earlier step.
  Bytes header = prefix(build_slot(), kOffMap);
  put_le(header, kOffBlockCount, std::numeric_limits<uint64_t>::max(), 8);
  DecodedSlot d = decode_exact(header);
  EXPECT_EQ(d.state, SlotState::Unreadable);
  EXPECT_TRUE(contains(d.reason, "block_count")) << d.reason;
}

// Step 4
TEST(CheckpointDecode, FileEndingPartwayThroughTheMapIsUnreadable) {
  Bytes slot = build_slot();
  ASSERT_EQ(decode_exact(slot).state, SlotState::Valid) << "control: the untouched slot must decode";
  for (size_t n : {kOffMap, kOffMap + 1, kOffMap + 7, kOffMap + 8, kOffMap + size_t{8} * 100,
                   slot.size() / 2, slot.size() - 8, slot.size() - 1}) {
    DecodedSlot d = decode_exact(prefix(slot, n));
    EXPECT_EQ(d.state, SlotState::Unreadable) << "length " << n;
    EXPECT_FALSE(d.reason.empty()) << "length " << n;
  }
}

TEST(CheckpointDecode, ASmallerBlockCountSlotCutShortIsUnreadableNotAMismatch) {
  // block_count 1000 would be a geometry mismatch if the slot were whole, but its file ends
  // partway, so step 4 fails first.
  SlotSpec spec;
  spec.block_count = 1000;
  Bytes slot = build_slot(spec);
  ASSERT_EQ(decode_exact(slot).state, SlotState::GeometryMismatch)
      << "control: the whole slot is a mismatch";
  DecodedSlot d = decode_exact(prefix(slot, slot.size() - 1));
  EXPECT_EQ(d.state, SlotState::Unreadable);
}

TEST(CheckpointDecode, ABitFlippedInAnyHeaderByteIsUnreadable) {
  // Not resealed, so the CRC no longer matches. Whatever the flipped field is, the slot is
  // unreadable: never Valid, and never a geometry mismatch (that would refuse startup over one
  // damaged bit). Every one of the 48 header bytes and all 8 bits of each are tried, including
  // the CRC's own bytes and the bits of block_count and block_size.
  const Bytes good = build_slot();
  ASSERT_EQ(decode_exact(good).state, SlotState::Valid) << "control: the untouched slot must decode";
  for (size_t byte = 0; byte < kOffMap; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes bad = good;
      bad[byte] ^= static_cast<uint8_t>(1U << bit);
      DecodedSlot d = decode_exact(bad);
      EXPECT_EQ(d.state, SlotState::Unreadable) << "header byte " << byte << " bit " << bit;
      EXPECT_FALSE(d.reason.empty());
    }
  }
}

TEST(CheckpointDecode, ABitFlippedInAMapEntryIsUnreadable) {
  const Bytes good = build_slot();
  // The first byte, one in the middle, the last, and a high bit of an entry.
  for (size_t byte : {kOffMap, kOffMap + size_t{8} * 131072 + 3, good.size() - 1, kOffMap + 7}) {
    for (int bit : {0, 7}) {
      Bytes bad = good;
      bad[byte] ^= static_cast<uint8_t>(1U << bit);
      DecodedSlot d = decode_exact(bad);
      EXPECT_EQ(d.state, SlotState::Unreadable) << "byte " << byte << " bit " << bit;
      EXPECT_TRUE(contains(d.reason, "crc")) << d.reason;
    }
  }
}

TEST(CheckpointDecode, AChangedMapEntryWithAMatchingCrcIsValid) {
  // Checks the test's own seal(): the CRC covers the map, so resealing after a change is what
  // makes a changed slot readable again.
  Bytes slot = build_slot();
  put_le(slot, kOffMap + size_t{8} * 5, 4136, 8);
  seal(slot);
  DecodedSlot d = decode_exact(slot);
  ASSERT_EQ(d.state, SlotState::Valid) << d.reason;
  EXPECT_EQ(d.map[5], 4136U);
}

// Step 5
TEST(CheckpointDecode, GoodCrcWithADifferentBlockSizeIsAGeometryMismatch) {
  for (uint32_t block_size : {0U, 512U, 4097U, 8192U, 0xFFFFFFFFU}) {
    SlotSpec spec;
    spec.block_size = block_size;
    DecodedSlot d = decode_exact(build_slot(spec));
    EXPECT_EQ(d.state, SlotState::GeometryMismatch) << "block_size " << block_size;
    EXPECT_TRUE(contains(d.reason, "block_size")) << d.reason;
  }
}

TEST(CheckpointDecode, GoodCrcWithASmallerBlockCountIsAGeometryMismatch) {
  for (uint64_t count : {uint64_t{0}, uint64_t{1}, uint64_t{131072}, kBlockCount - 1}) {
    SlotSpec spec;
    spec.block_count = count;  // the body is that many entries long, and the CRC matches it
    DecodedSlot d = decode_exact(build_slot(spec));
    EXPECT_EQ(d.state, SlotState::GeometryMismatch) << "block_count " << count;
    EXPECT_TRUE(contains(d.reason, "block_count")) << d.reason;
  }
}

TEST(CheckpointDecode, ADeviceOfAnotherSizeAndBlockSizeIsAGeometryMismatch) {
  SlotSpec spec;
  spec.block_count = 1024;
  spec.block_size = 512;
  EXPECT_EQ(decode_exact(build_slot(spec)).state, SlotState::GeometryMismatch);
}

TEST(CheckpointDecode, TheCrcIsCheckedBeforeGeometryIsTrusted) {
  // Wrong block_size and a bad CRC: one damaged slot, not a device of another geometry.
  Bytes slot = build_slot();
  ASSERT_EQ(decode_exact(slot).state, SlotState::Valid) << "control";
  put_le(slot, kOffBlockSize, 8192, 4);  // not resealed
  EXPECT_EQ(decode_exact(slot).state, SlotState::Unreadable);

  // The same for a smaller block_count whose body is long enough but whose CRC is wrong.
  SlotSpec spec;
  spec.block_count = 1000;
  Bytes small = build_slot(spec);
  small[kOffMap + 3] ^= 0x01;
  EXPECT_EQ(decode_exact(small).state, SlotState::Unreadable);
}

TEST(CheckpointDecode, ASlotThatIsNotValidHasNoDecodedContents) {
  SlotSpec spec;
  spec.block_size = 8192;
  DecodedSlot d = decode_exact(build_slot(spec));
  ASSERT_EQ(d.state, SlotState::GeometryMismatch);
  EXPECT_TRUE(d.map.empty());
}

}  // namespace
}  // namespace logblock
