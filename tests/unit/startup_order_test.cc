// The order of startup file work (D35): open the log WITHOUT creating it. If it exists, lock it,
// then read the checkpoint slots. If it is missing, read the slots first (a valid slot means
// refuse), otherwise create it with O_EXCL (losing the race refuses), then lock. Recovery ends
// with the log fsync and then the parent-directory fsync, on every successful start.
//
// Implemented in Engine::create() (src/engine.cc) over Io::open_log_existing() and
// Io::create_log() (src/io_posix.cc).
#include <gtest/gtest.h>

#include <string>

#include "logblock/engine.h"
#include "support/checkpoint_builder.h"
#include "support/recovery_fixture.h"

namespace logblock {
namespace {

using test::kCkptPath;
using test::kLogPath;
using test::make_slot;
using test::Op;
using test::put_slot;

class StartupOrderTest : public test::RecoveryFixture {
 protected:
  // The log and checkpoint file exactly as the test set them, to check a refusal changed nothing.
  void expect_nothing_created_or_changed(const std::string& ckpt_before) {
    EXPECT_EQ(io_->count(Op::CreateLog), 0U);
    EXPECT_EQ(io_->count(Op::LockLog), 0U);
    EXPECT_EQ(io_->count(Op::FsyncParentDir), 0U);
    EXPECT_EQ(io_->count(Op::Pwrite), 0U);
    EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
    EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
    EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
  }

  test::LogBuilder log_of(uint64_t n) {
    test::LogBuilder log;
    for (uint64_t b = 0; b < n; ++b) {
      log.data(b, static_cast<uint8_t>(b + 1));
    }
    return log;
  }
};

// --- An existing log: open it, lock it, only then look at the slots -----------------------------

TEST_F(StartupOrderTest, AnExistingLogIsOpenedWithoutCreatingAndLockedBeforeAnythingIsRead) {
  test::LogBuilder log = log_of(5);
  load(log);
  put_slot(*io_, 0, make_slot(log.bytes(), 1, 3));
  ASSERT_TRUE(start().ok());

  EXPECT_EQ(io_->count(Op::OpenLogExisting), 1U);
  EXPECT_EQ(io_->count(Op::CreateLog), 0U);
  const int open = first_call(Op::OpenLogExisting);
  const int lock = first_call(Op::LockLog);
  ASSERT_GE(lock, 0);
  EXPECT_LT(open, lock);
  // Nothing about the log's content or the checkpoint file is looked at before the lock is held.
  for (Op op : {Op::FileSize, Op::Pread, Op::OpenCkpt, Op::CkptSize, Op::CkptPread}) {
    const int at = first_call(op);
    if (at >= 0) {
      EXPECT_LT(lock, at) << "a call ran before lock_log()";
    }
  }
}

TEST_F(StartupOrderTest,
       AnExistingEmptyLogWithAValidSlotRefusesAfterTheLockWithoutCreatingAnything) {
  test::LogBuilder log = log_of(5);
  put_slot(*io_, 0, make_slot(log.bytes(), 1, 3));
  io_->set_log_exists(true);  // present but empty
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->count(Op::CreateLog), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
  EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
}

// --- A missing log -------------------------------------------------------------------------------

TEST_F(StartupOrderTest, AMissingLogWithAValidSlotRefusesBeforeCreatingOrLockingAnything) {
  test::LogBuilder log = log_of(5);
  put_slot(*io_, 0, make_slot(log.bytes(), 1, 3));
  io_->set_log_exists(false);
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
  EXPECT_FALSE(io_->log_exists()) << "a refused start must not leave a new empty log behind";
  EXPECT_EQ(io_->count(Op::OpenLogExisting), 1U) << "the log is looked for with open_log_existing";
  expect_nothing_created_or_changed(ckpt_before);
}

TEST_F(StartupOrderTest,
       AMissingLogWithNoValidSlotIsCreatedThenLockedThenItsDirectorySyncedAfterRecovery) {
  io_->set_log_exists(false);
  ASSERT_TRUE(start().ok());
  const int open = first_call(Op::OpenLogExisting);
  const int create = first_call(Op::CreateLog);
  const int dir = first_call(Op::FsyncParentDir);
  const int lock = first_call(Op::LockLog);
  ASSERT_GE(open, 0);
  ASSERT_GE(create, 0);
  ASSERT_GE(dir, 0);
  ASSERT_GE(lock, 0);
  EXPECT_LT(open, create);
  EXPECT_LT(create, lock);
  EXPECT_LT(lock, dir) << "the name is made durable by recovery, after the lock (D35)";
  EXPECT_EQ(io_->count(Op::CreateLog), 1U);
  EXPECT_TRUE(io_->log_exists());
  expect_zero_block(0);
}

TEST_F(StartupOrderTest, AMissingLogWithOnlyUnreadableSlotsIsCreatedAsANewDevice) {
  test::LogBuilder log = log_of(5);
  std::string a = make_slot(log.bytes(), 1, 3);
  a[100] ^= 0x01;
  put_slot(*io_, 0, a);
  io_->set_log_exists(false);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->count(Op::CreateLog), 1U);
  EXPECT_EQ(engine_->recovery_report().unreadable_checkpoints, 1U);
  EXPECT_EQ(engine_->recovery_report().source, RecoverySource::None);
}

TEST_F(StartupOrderTest, LosingTheCreationRaceRefusesAndChangesNothing) {
  // Another server created the log between our look and our O_EXCL create: create_log() reports
  // created == false. We must not open, lock or change anything, and say why.
  io_->set_log_exists(false);
  io_->set_create_race(true);
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("appeared during startup"), std::string::npos) << s.message();
  EXPECT_EQ(io_->count(Op::CreateLog), 1U);
  EXPECT_EQ(io_->count(Op::LockLog), 0U);
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 0U) << "the winner syncs its own directory entry";
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
}

TEST_F(StartupOrderTest, AnErrorOtherThanNotFoundFromTheOpenRefusesWithoutCreating) {
  io_->set_log_exists(false);
  io_->fail_next(Op::OpenLogExisting, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->count(Op::CreateLog), 0U) << "only ENOENT means the log may be created";
  EXPECT_FALSE(io_->log_exists());
}

TEST_F(StartupOrderTest, AFailedCreateRefusesAndTheNextStartCanSucceed) {
  io_->set_log_exists(false);
  io_->fail_next(Op::CreateLog, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->count(Op::LockLog), 0U);
  ASSERT_TRUE(start().ok());
}

}  // namespace
}  // namespace logblock
