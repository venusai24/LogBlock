#include <gtest/gtest.h>

#include "logblock/engine.h"

namespace logblock {
namespace {

// The numbers every other test and the spec rely on: the device geometry and
// the record sizes.
TEST(EngineConstants, DeviceGeometry) {
  EXPECT_EQ(kBlockSize, 4096U);
  EXPECT_EQ(kBlockCount, 262144U);
  EXPECT_EQ(kDeviceSize, 1073741824U);  // exactly 1 GiB
}

TEST(EngineConstants, RecordSizes) {
  EXPECT_EQ(kRecordHeaderSize, 40U);
  EXPECT_EQ(kDataRecordSize, 4136U);  // 40-byte header + 4096-byte payload
  EXPECT_EQ(kTrimRecordSize, 40U);    // header only
}

}  // namespace
}  // namespace logblock
