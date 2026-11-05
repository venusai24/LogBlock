#include "block_map.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "logblock/engine.h"

namespace logblock {
namespace {

using Offset = std::optional<uint64_t>;
constexpr Offset kNone = std::nullopt;

// Blocks 0..count-1 mapped to offset 1000 + block, so each has a distinct value.
void map_first(BlockMap& map, uint64_t count) {
  for (uint64_t b = 0; b < count; ++b) {
    map.set(b, 1000 + b);
  }
}

TEST(BlockMap, HasOneEntryPerDeviceBlock) {
  BlockMap map;
  EXPECT_EQ(map.size(), kBlockCount);
  EXPECT_EQ(map.size(), 262144U);
}

TEST(BlockMap, EveryBlockStartsUnmapped) {
  BlockMap map;
  uint64_t mapped = 0;
  for (uint64_t b = 0; b < map.size(); ++b) {
    if (map.get(b).has_value()) {
      ++mapped;
    }
  }
  EXPECT_EQ(mapped, 0U);
}

TEST(BlockMap, SetThenGetAtTheFirstAndLastBlock) {
  BlockMap map;
  map.set(0, 111);
  map.set(kBlockCount - 1, 222);
  EXPECT_EQ(map.get(0), Offset(111));
  EXPECT_EQ(map.get(kBlockCount - 1), Offset(222));
  EXPECT_EQ(map.get(1), kNone);  // neighbours are untouched
  EXPECT_EQ(map.get(kBlockCount - 2), kNone);
}

TEST(BlockMap, OffsetZeroIsAValidMappedValue) {
  // The first record in a log is at offset 0. It must not be confused with
  // "unmapped".
  BlockMap map;
  map.set(5, 0);
  ASSERT_TRUE(map.get(5).has_value());
  EXPECT_EQ(*map.get(5), 0U);
  EXPECT_EQ(map.get(4), kNone);
  EXPECT_EQ(map.get(6), kNone);
}

TEST(BlockMap, SetOverwritesThePreviousOffset) {
  BlockMap map;
  map.set(9, 4136);
  map.set(9, 8272);
  EXPECT_EQ(map.get(9), Offset(8272));
}

TEST(BlockMap, KeepsFullSixtyFourBitOffsets) {
  // A log offset is a uint64_t. The largest value that is not the sentinel must
  // survive unchanged, and nothing may be truncated to 32 bits.
  BlockMap map;
  map.set(1, (1ULL << 40) + 7);
  map.set(2, BlockMap::kUnmapped - 1);
  EXPECT_EQ(map.get(1), Offset((1ULL << 40) + 7));
  EXPECT_EQ(map.get(2), Offset(BlockMap::kUnmapped - 1));
}

TEST(BlockMap, SetDoesNotDisturbOtherBlocks) {
  BlockMap map;
  map_first(map, 10);
  map.set(4, 9999);
  for (uint64_t b = 0; b < 10; ++b) {
    EXPECT_EQ(map.get(b), Offset(b == 4 ? 9999 : 1000 + b)) << "block " << b;
  }
}

TEST(BlockMap, ClearRangeUnmapsOnlyThatRange) {
  BlockMap map;
  map_first(map, 10);
  map.clear_range(3, 4);  // blocks 3, 4, 5, 6
  for (uint64_t b = 0; b < 10; ++b) {
    bool cleared = b >= 3 && b <= 6;
    EXPECT_EQ(map.get(b), cleared ? kNone : Offset(1000 + b)) << "block " << b;
  }
}

TEST(BlockMap, ClearRangeOfOneBlock) {
  BlockMap map;
  map_first(map, 3);
  map.clear_range(1, 1);
  EXPECT_EQ(map.get(0), Offset(1000));
  EXPECT_EQ(map.get(1), kNone);
  EXPECT_EQ(map.get(2), Offset(1002));
}

TEST(BlockMap, ClearRangeAtTheEndsOfTheDevice) {
  BlockMap map;
  map.set(0, 1);
  map.set(1, 2);
  map.set(kBlockCount - 2, 3);
  map.set(kBlockCount - 1, 4);
  map.clear_range(0, 1);
  map.clear_range(kBlockCount - 1, 1);
  EXPECT_EQ(map.get(0), kNone);
  EXPECT_EQ(map.get(1), Offset(2));
  EXPECT_EQ(map.get(kBlockCount - 2), Offset(3));
  EXPECT_EQ(map.get(kBlockCount - 1), kNone);
}

TEST(BlockMap, ClearRangeOfTheWholeDevice) {
  // A whole-device trim is one TRIM record covering all 262,144 blocks.
  BlockMap map;
  map.set(0, 1);
  map.set(12345, 2);
  map.set(kBlockCount - 1, 3);
  map.clear_range(0, kBlockCount);
  uint64_t mapped = 0;
  for (uint64_t b = 0; b < map.size(); ++b) {
    if (map.get(b).has_value()) {
      ++mapped;
    }
  }
  EXPECT_EQ(mapped, 0U);
}

TEST(BlockMap, ClearRangeOfZeroBlocksChangesNothing) {
  BlockMap map;
  map_first(map, 4);
  map.clear_range(2, 0);
  map.clear_range(kBlockCount, 0);  // an empty range at the very end is allowed
  for (uint64_t b = 0; b < 4; ++b) {
    EXPECT_EQ(map.get(b), Offset(1000 + b)) << "block " << b;
  }
}

TEST(BlockMap, ClearRangeOnUnmappedBlocksIsHarmless) {
  BlockMap map;
  map.clear_range(100, 50);
  EXPECT_EQ(map.get(100), kNone);
  map.set(120, 7);
  EXPECT_EQ(map.get(120), Offset(7));
}

TEST(BlockMap, ABlockCanBeMappedAgainAfterBeingCleared) {
  BlockMap map;
  map.set(8, 100);
  map.clear_range(8, 1);
  EXPECT_EQ(map.get(8), kNone);
  map.set(8, 200);
  EXPECT_EQ(map.get(8), Offset(200));
}

// The preconditions are checked with assert(), so they fail loudly in the Debug
// builds that every test uses. Release builds define NDEBUG, where an out-of-range
// call would be undefined behaviour, so these tests exist only when asserts do.
#ifndef NDEBUG
TEST(BlockMapDeathTest, BlockNumbersPastTheEndAreCaught) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  BlockMap map;
  EXPECT_DEATH((void)map.get(kBlockCount), "block_map\\.cc:[0-9]+:.*Assertion");
  EXPECT_DEATH(map.set(kBlockCount, 1), "block_map\\.cc:[0-9]+:.*Assertion");
  EXPECT_DEATH((void)map.get(std::numeric_limits<uint64_t>::max()),
               "block_map\\.cc:[0-9]+:.*Assertion");
}

TEST(BlockMapDeathTest, TheSentinelIsNotAValidLogOffset) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  BlockMap map;
  EXPECT_DEATH(map.set(0, BlockMap::kUnmapped), "block_map\\.cc:[0-9]+:.*Assertion");
}

TEST(BlockMapDeathTest, RangesPastTheEndAreCaught) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  BlockMap map;
  EXPECT_DEATH(map.clear_range(kBlockCount - 1, 2), "block_map\\.cc:[0-9]+:.*Assertion");
  EXPECT_DEATH(map.clear_range(0, kBlockCount + 1), "block_map\\.cc:[0-9]+:.*Assertion");
  EXPECT_DEATH(map.clear_range(kBlockCount + 1, 0), "block_map\\.cc:[0-9]+:.*Assertion");
  // first_block + count wraps to 1 in 64 bits, which must not look like a valid range.
  EXPECT_DEATH(map.clear_range(std::numeric_limits<uint64_t>::max(), 2),
               "block_map\\.cc:[0-9]+:.*Assertion");
}
#endif  // NDEBUG

}  // namespace
}  // namespace logblock
