#include <gtest/gtest.h>

// Smoke test: proves that the unit test executable builds, links against
// logblock_core, is discovered by CTest and runs under every build type
// (normal, ASan/UBSan and TSan).
TEST(Smoke, TestsRunInThisBuild) { EXPECT_EQ(2 + 2, 4); }
