#include "support/fault_io.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace logblock::test {
namespace {

using Op = FaultIo::Op;

std::vector<Op> ops_of(const FaultIo& io) {
  std::vector<Op> ops;
  for (const auto& call : io.calls()) {
    ops.push_back(call.op);
  }
  return ops;
}

class FaultIoTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(io_.open_log("fake.log").ok()); }

  Status write(uint64_t offset, const std::string& data) {
    return io_.pwrite(offset, data.data(), data.size());
  }
  std::string read(uint64_t offset, size_t len, size_t* got = nullptr) {
    std::string out(len, '?');
    size_t n = 0;
    EXPECT_TRUE(io_.pread(offset, out.data(), len, &n).ok());
    out.resize(n);
    if (got != nullptr) {
      *got = n;
    }
    return out;
  }

  FaultIo io_;
};

TEST_F(FaultIoTest, WritesAndReadsBackAtOffsets) {
  ASSERT_TRUE(write(0, "hello").ok());
  ASSERT_TRUE(write(5, " world").ok());
  EXPECT_EQ(io_.bytes(), "hello world");
  EXPECT_EQ(read(6, 5), "world");
}

TEST_F(FaultIoTest, WritingPastTheEndZeroFillsTheGap) {
  ASSERT_TRUE(write(4, "ab").ok());
  EXPECT_EQ(io_.bytes(), std::string("\0\0\0\0ab", 6));
}

TEST_F(FaultIoTest, ReadPastTheEndIsAShortRead) {
  ASSERT_TRUE(write(0, "0123456789").ok());
  size_t got = 99;
  EXPECT_EQ(read(5, 20, &got), "56789");
  EXPECT_EQ(got, 5U);
  EXPECT_EQ(read(50, 4, &got), "");
  EXPECT_EQ(got, 0U);
}

TEST_F(FaultIoTest, FileSizeAndFtruncate) {
  ASSERT_TRUE(write(0, "0123456789").ok());
  uint64_t size = 0;
  ASSERT_TRUE(io_.file_size(&size).ok());
  EXPECT_EQ(size, 10U);
  ASSERT_TRUE(io_.ftruncate(4).ok());
  EXPECT_EQ(io_.bytes(), "0123");
}

TEST(FaultIo, PrefilledBytesAreThereAfterOpen) {
  FaultIo io;
  io.bytes() = "existing";  // a non-empty log, for the startup-refusal tests
  ASSERT_TRUE(io.open_log("fake.log").ok());
  uint64_t size = 0;
  ASSERT_TRUE(io.file_size(&size).ok());
  EXPECT_EQ(size, 8U);
  EXPECT_EQ(io.path(), "fake.log");
}

TEST(FaultIo, EveryCallBeforeOpenFails) {
  FaultIo io;
  char buf[4] = {};
  size_t got = 0;
  uint64_t n = 0;
  EXPECT_EQ(io.file_size(&n).code(), StatusCode::IoError);
  EXPECT_EQ(io.pread(0, buf, 4, &got).code(), StatusCode::IoError);
  EXPECT_EQ(io.pwrite(0, buf, 4).code(), StatusCode::IoError);
  EXPECT_EQ(io.fsync().code(), StatusCode::IoError);
  EXPECT_EQ(io.ftruncate(0).code(), StatusCode::IoError);
  EXPECT_EQ(io.fsync_parent_dir().code(), StatusCode::IoError);
  EXPECT_TRUE(io.bytes().empty());
}

TEST_F(FaultIoTest, CallsAreRecordedInOrderWithOffsetsAndLengths) {
  io_.clear_calls();
  ASSERT_TRUE(write(40, "abcd").ok());
  std::string buf(2, '?');
  size_t got = 0;
  ASSERT_TRUE(io_.pread(41, buf.data(), 2, &got).ok());
  ASSERT_TRUE(io_.fsync().ok());
  ASSERT_TRUE(io_.ftruncate(10).ok());
  ASSERT_TRUE(io_.fsync_parent_dir().ok());

  EXPECT_EQ(ops_of(io_),
            (std::vector<Op>{Op::Pwrite, Op::Pread, Op::Fsync, Op::Ftruncate, Op::FsyncParentDir}));
  EXPECT_EQ(io_.calls()[0].offset, 40U);
  EXPECT_EQ(io_.calls()[0].length, 4U);
  EXPECT_EQ(io_.calls()[1].offset, 41U);
  EXPECT_EQ(io_.calls()[1].length, 2U);
  EXPECT_EQ(io_.calls()[3].length, 10U);  // for ftruncate, the new size
  EXPECT_EQ(io_.count(Op::Pwrite), 1U);
  EXPECT_EQ(io_.count(Op::Fsync), 1U);
  io_.clear_calls();
  EXPECT_TRUE(io_.calls().empty());
}

TEST_F(FaultIoTest, FailedPwriteWritesTheFirstKBytesThenFailsOnce) {
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 3);
  Status s = write(0, "abcdef");
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_EQ(io_.bytes(), "abc");  // a torn append: 3 of 6 bytes reached the file

  EXPECT_TRUE(write(3, "def").ok());  // the fault was one-shot
  EXPECT_EQ(io_.bytes(), "abcdef");
}

TEST_F(FaultIoTest, FailedPwriteCanReportNoSpace) {
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace);
  EXPECT_EQ(write(0, "abcdef").code(), StatusCode::NoSpace);
  EXPECT_TRUE(io_.bytes().empty());  // K defaults to 0: nothing written
}

TEST_F(FaultIoTest, FailedCallsAreStillRecorded) {
  io_.clear_calls();
  io_.fail_next(Op::Pwrite);
  EXPECT_FALSE(write(0, "ab").ok());
  EXPECT_EQ(io_.count(Op::Pwrite), 1U);
}

TEST_F(FaultIoTest, EachOtherOperationFailsOnceThenRecovers) {
  ASSERT_TRUE(write(0, "0123456789").ok());
  char buf[4];
  size_t got = 0;
  uint64_t size = 0;

  io_.fail_next(Op::Pread);
  EXPECT_FALSE(io_.pread(0, buf, 4, &got).ok());
  EXPECT_EQ(got, 0U);
  EXPECT_TRUE(io_.pread(0, buf, 4, &got).ok());

  io_.fail_next(Op::Fsync);
  EXPECT_FALSE(io_.fsync().ok());
  EXPECT_TRUE(io_.fsync().ok());

  io_.fail_next(Op::Ftruncate);
  EXPECT_FALSE(io_.ftruncate(4).ok());
  EXPECT_EQ(io_.bytes().size(), 10U);  // a failed truncate changes nothing
  EXPECT_TRUE(io_.ftruncate(4).ok());

  io_.fail_next(Op::FsyncParentDir);
  EXPECT_FALSE(io_.fsync_parent_dir().ok());
  EXPECT_TRUE(io_.fsync_parent_dir().ok());

  io_.fail_next(Op::FileSize);
  EXPECT_FALSE(io_.file_size(&size).ok());
  EXPECT_TRUE(io_.file_size(&size).ok());
}

TEST(FaultIo, OpenLogCanFail) {
  FaultIo io;
  io.fail_next(Op::OpenLog);
  EXPECT_EQ(io.open_log("fake.log").code(), StatusCode::IoError);
  EXPECT_TRUE(io.open_log("fake.log").ok());
}

TEST_F(FaultIoTest, QueuedFaultsAreUsedInOrder) {
  // An append that fails, then the cleanup that fails too.
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace);
  io_.fail_next(Op::Pwrite, StatusCode::IoError);
  EXPECT_EQ(write(0, "a").code(), StatusCode::NoSpace);
  EXPECT_EQ(write(0, "a").code(), StatusCode::IoError);
  EXPECT_TRUE(write(0, "a").ok());
}

}  // namespace
}  // namespace logblock::test
