#include "support/fault_io.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "support/io_helpers.h"

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
  void SetUp() override { ASSERT_TRUE(test::open_or_create_log(io_, "fake.log").ok()); }

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
  ASSERT_TRUE(test::open_or_create_log(io, "fake.log").ok());
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

TEST(FaultIo, OpenLogExistingCanFail) {
  FaultIo io;
  io.fail_next(Op::OpenLogExisting);
  EXPECT_EQ(test::open_or_create_log(io, "fake.log").code(), StatusCode::IoError);
  EXPECT_TRUE(test::open_or_create_log(io, "fake.log").ok());
}

TEST_F(FaultIoTest, QueuedFaultsAreUsedInOrder) {
  // An append that fails, then the cleanup that fails too.
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace);
  io_.fail_next(Op::Pwrite, StatusCode::IoError);
  EXPECT_EQ(write(0, "a").code(), StatusCode::NoSpace);
  EXPECT_EQ(write(0, "a").code(), StatusCode::IoError);
  EXPECT_TRUE(write(0, "a").ok());
}

// --- M2 additions: the checkpoint file, existence, the lock ------------------

class FaultIoCkptTest : public FaultIoTest {
 protected:
  // Opens the checkpoint file, creating it.
  void open_ckpt_created() {
    bool opened = false;
    bool created = false;
    ASSERT_TRUE(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).ok());
    ASSERT_TRUE(opened);
  }
  Status ckpt_write(uint64_t offset, const std::string& data) {
    return io_.ckpt_pwrite(offset, data.data(), data.size());
  }
};

TEST(FaultIo, LogExistenceFollowsCreateAndTheSetter) {
  FaultIo io;
  EXPECT_FALSE(io.log_exists());

  io.set_log_exists(true);  // an empty log that is already there
  EXPECT_TRUE(io.log_exists());
  EXPECT_TRUE(io.bytes().empty());

  FaultIo opened;
  bool created = false;
  ASSERT_TRUE(opened.create_log("fake.log", &created).ok());  // creating makes the log
  EXPECT_TRUE(created);
  EXPECT_TRUE(opened.log_exists());
}

TEST(FaultIo, PrefilledLogBytesCountAsAnExistingLog) {
  FaultIo io;
  io.bytes() = "x";
  EXPECT_TRUE(io.log_exists());
}

TEST(FaultIo, CheckpointExistenceFollowsTheSetterAndTheBytes) {
  FaultIo io;
  EXPECT_FALSE(io.ckpt_exists());
  io.set_ckpt_exists(true);
  EXPECT_TRUE(io.ckpt_exists());
  io.set_ckpt_exists(false);
  io.ckpt_bytes() = "slot";  // bytes imply the file is there
  EXPECT_TRUE(io.ckpt_exists());
}

TEST_F(FaultIoCkptTest, OpenWithoutCreateOnAMissingFileSucceedsNotOpened) {
  bool opened = true;
  bool created = true;
  ASSERT_TRUE(io_.open_ckpt("fake.log.ckpt", false, &opened, &created).ok());
  EXPECT_FALSE(opened);
  EXPECT_FALSE(created);
  EXPECT_FALSE(io_.ckpt_exists()) << "create = false must not create the file";
  uint64_t n = 0;
  EXPECT_EQ(io_.ckpt_size(&n).code(), StatusCode::IoError) << "nothing is open";
}

TEST_F(FaultIoCkptTest, OpenWithCreateMakesTheFileAndReportsItWasNew) {
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).ok());
  EXPECT_TRUE(opened);
  EXPECT_TRUE(created);
  EXPECT_TRUE(io_.ckpt_exists());
  EXPECT_TRUE(io_.ckpt_bytes().empty());
}

TEST_F(FaultIoCkptTest, OpenOnAnExistingFileKeepsItsBytesAndReportsNotNew) {
  for (bool create : {false, true}) {
    FaultIo io;
    io.ckpt_bytes() = "slotdata";
    ASSERT_TRUE(test::open_or_create_log(io, "fake.log").ok());
    bool opened = false;
    bool created = true;
    ASSERT_TRUE(io.open_ckpt("fake.log.ckpt", create, &opened, &created).ok());
    EXPECT_TRUE(opened) << "create=" << create;
    EXPECT_FALSE(created) << "create=" << create;
    EXPECT_EQ(io.ckpt_bytes(), "slotdata");
  }
}

TEST_F(FaultIoCkptTest, OpeningTwiceIsAnError) {
  open_ckpt_created();
  bool opened = false;
  bool created = false;
  EXPECT_EQ(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).code(), StatusCode::IoError);
}

TEST_F(FaultIoCkptTest, CheckpointWritesReadsAndSizeWorkAtOffsets) {
  open_ckpt_created();
  ASSERT_TRUE(ckpt_write(4, "abcd").ok());
  EXPECT_EQ(io_.ckpt_bytes(), std::string("\0\0\0\0abcd", 8)) << "a gap reads as zeros";
  uint64_t n = 0;
  ASSERT_TRUE(io_.ckpt_size(&n).ok());
  EXPECT_EQ(n, 8U);

  std::string out(20, '?');
  size_t got = 99;
  ASSERT_TRUE(io_.ckpt_pread(5, out.data(), out.size(), &got).ok());
  EXPECT_EQ(got, 3U);
  EXPECT_EQ(out.substr(0, got), "bcd");
  ASSERT_TRUE(io_.ckpt_pread(50, out.data(), out.size(), &got).ok());
  EXPECT_EQ(got, 0U);
}

TEST_F(FaultIoCkptTest, TheTwoFilesAreIndependent) {
  open_ckpt_created();
  ASSERT_TRUE(write(0, "log").ok());
  ASSERT_TRUE(ckpt_write(0, "ckpt!").ok());
  EXPECT_EQ(io_.bytes(), "log");
  EXPECT_EQ(io_.ckpt_bytes(), "ckpt!");
  uint64_t n = 0;
  ASSERT_TRUE(io_.file_size(&n).ok());
  EXPECT_EQ(n, 3U);
}

TEST_F(FaultIoCkptTest, FailedCheckpointWriteIsTornAndOneShot) {
  open_ckpt_created();
  io_.fail_next(Op::CkptPwrite, StatusCode::IoError, 2);
  EXPECT_EQ(ckpt_write(0, "abcd").code(), StatusCode::IoError);
  EXPECT_EQ(io_.ckpt_bytes(), "ab") << "2 of 4 bytes reached the file";
  EXPECT_TRUE(ckpt_write(0, "abcd").ok());
  EXPECT_EQ(io_.ckpt_bytes(), "abcd");
}

TEST_F(FaultIoCkptTest, EachCheckpointOperationFailsOnceThenRecovers) {
  open_ckpt_created();
  ASSERT_TRUE(ckpt_write(0, "0123456789").ok());
  char buf[4];
  size_t got = 7;
  uint64_t n = 0;

  io_.fail_next(Op::CkptPread);
  EXPECT_FALSE(io_.ckpt_pread(0, buf, 4, &got).ok());
  EXPECT_EQ(got, 0U);
  EXPECT_TRUE(io_.ckpt_pread(0, buf, 4, &got).ok());

  io_.fail_next(Op::CkptSize);
  EXPECT_FALSE(io_.ckpt_size(&n).ok());
  EXPECT_TRUE(io_.ckpt_size(&n).ok());

  io_.fail_next(Op::CkptFsync);
  EXPECT_FALSE(io_.ckpt_fsync().ok());
  EXPECT_TRUE(io_.ckpt_fsync().ok());
}

TEST_F(FaultIoCkptTest, OpenCkptCanFailAndLeavesNothingOpen) {
  io_.fail_next(Op::OpenCkpt);
  bool opened = true;
  bool created = true;
  EXPECT_EQ(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).code(), StatusCode::IoError);
  EXPECT_FALSE(opened);
  EXPECT_FALSE(created);
  EXPECT_FALSE(io_.ckpt_exists()) << "a failed create must not create the file";
  EXPECT_TRUE(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).ok());
}

TEST(FaultIo, EveryCheckpointCallBeforeItsOpenFails) {
  FaultIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "fake.log").ok());  // the log being open does not help
  char buf[4] = {};
  size_t got = 0;
  uint64_t n = 0;
  EXPECT_EQ(io.ckpt_size(&n).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_pread(0, buf, 4, &got).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_pwrite(0, buf, 4).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_fsync().code(), StatusCode::IoError);
  EXPECT_TRUE(io.ckpt_bytes().empty());
}

TEST(FaultIo, LockLogSucceedsUnlessHeld) {
  FaultIo io;
  EXPECT_EQ(io.lock_log().code(), StatusCode::IoError) << "needs an open log";
  ASSERT_TRUE(test::open_or_create_log(io, "fake.log").ok());
  EXPECT_TRUE(io.lock_log().ok());

  io.set_lock_held(true);
  Status s = io.lock_log();
  EXPECT_EQ(s.code(), StatusCode::Failed);
  EXPECT_NE(s.message().find("fake.log"), std::string::npos) << s.message();

  io.set_lock_held(false);
  EXPECT_TRUE(io.lock_log().ok());
}

TEST(FaultIo, LockLogCanFailWithAnInjectedCode) {
  FaultIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "fake.log").ok());
  io.fail_next(Op::LockLog, StatusCode::IoError);
  EXPECT_EQ(io.lock_log().code(), StatusCode::IoError);
  EXPECT_TRUE(io.lock_log().ok());
}

TEST_F(FaultIoCkptTest, CallOrderAcrossBothFilesIsRecorded) {
  io_.clear_calls();
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(io_.lock_log().ok());
  ASSERT_TRUE(io_.open_ckpt("fake.log.ckpt", true, &opened, &created).ok());
  ASSERT_TRUE(io_.fsync().ok());
  ASSERT_TRUE(ckpt_write(8, "abcd").ok());
  ASSERT_TRUE(io_.ckpt_fsync().ok());
  ASSERT_TRUE(io_.fsync_parent_dir().ok());
  uint64_t n = 0;
  ASSERT_TRUE(io_.ckpt_size(&n).ok());
  char buf[2];
  size_t got = 0;
  ASSERT_TRUE(io_.ckpt_pread(8, buf, 2, &got).ok());

  EXPECT_EQ(ops_of(io_), (std::vector<Op>{Op::LockLog, Op::OpenCkpt, Op::Fsync, Op::CkptPwrite,
                                         Op::CkptFsync, Op::FsyncParentDir, Op::CkptSize,
                                         Op::CkptPread}));
  EXPECT_EQ(io_.calls()[1].length, 1U) << "for open_ckpt, length is 1 when create = true";
  EXPECT_EQ(io_.calls()[3].offset, 8U);
  EXPECT_EQ(io_.calls()[3].length, 4U);
  EXPECT_EQ(io_.calls()[7].offset, 8U);
  EXPECT_EQ(io_.calls()[7].length, 2U);
  EXPECT_EQ(io_.count(Op::CkptPwrite), 1U);
}

}  // namespace
}  // namespace logblock::test
