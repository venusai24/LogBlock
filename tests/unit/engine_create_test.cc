// Tests for Engine::create: what it accepts, what it refuses, and
// which I/O calls it makes. Uses FaultIo, so no real file is touched.
//
// Refusals use fixed codes (cap -> OutOfRange, non-empty
// log -> Failed) and a message that says why. The I/O error tests check that the
// Io's own code is passed on.
#include <gtest/gtest.h>

#include <stdlib.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "logblock/engine.h"
#include "support/fault_io.h"
#include "support/forwarding_io.h"
#include "support/log_builder.h"

namespace logblock {
namespace {

using test::FaultIo;
using test::ForwardingIo;
using Op = FaultIo::Op;

constexpr std::string_view kPath = "engine-create.log";

class EngineCreateTest : public ::testing::Test {
 protected:
  Status create(uint64_t cap = kDeviceSize) {
    return Engine::create(std::string(kPath), cap, std::make_unique<ForwardingIo>(io_), &engine_);
  }

  // True if the engine did anything that changes the file.
  [[nodiscard]] bool changed_the_file() const {
    return io_.count(Op::Pwrite) + io_.count(Op::Ftruncate) + io_.count(Op::Fsync) > 0;
  }

  FaultIo io_;
  std::unique_ptr<Engine> engine_;
};

// --- The cap ----------------------------------------------------------------

TEST_F(EngineCreateTest, RejectsACapOneByteBelowOneDataRecord) {
  Status s = create(kDataRecordSize - 1);  // 4,135
  EXPECT_EQ(s.code(), StatusCode::OutOfRange) << s.message();
  EXPECT_NE(s.message().find("cap"), std::string::npos) << s.message();
  EXPECT_EQ(engine_, nullptr);
}

TEST_F(EngineCreateTest, AcceptsACapOfExactlyOneDataRecord) {
  Status s = create(kDataRecordSize);  // 4,136
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_NE(engine_, nullptr);
}

TEST_F(EngineCreateTest, RejectsACapOfZero) {
  EXPECT_FALSE(create(0).ok());
  EXPECT_EQ(engine_, nullptr);
}

TEST_F(EngineCreateTest, ARejectedCapDoesNotTouchTheLog) {
  // A bad setting must not leave a new empty log behind, so the engine must not
  // open or create the log at all.
  EXPECT_FALSE(create(kDataRecordSize - 1).ok());
  EXPECT_EQ(io_.count(Op::OpenLogExisting), 0U);
  EXPECT_EQ(io_.count(Op::CreateLog), 0U);
  EXPECT_FALSE(changed_the_file());
}

// --- The log file -----------------------------------------------------------

TEST_F(EngineCreateTest, AcceptsAMissingOrEmptyLog) {
  // The engine creates a missing log (create_log) and opens an existing one, so a missing log
  // and an empty one both start as a new device.
  Status s = create();
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_NE(engine_, nullptr);
}

TEST_F(EngineCreateTest, LooksForTheLogAtTheGivenPathOnceAndCreatesItWhenMissing) {
  ASSERT_TRUE(create().ok());
  EXPECT_EQ(io_.path(), std::string(kPath));
  EXPECT_EQ(io_.count(Op::OpenLogExisting), 1U);
  EXPECT_EQ(io_.count(Op::CreateLog), 1U);
}

TEST_F(EngineCreateTest, OpensAnExistingLogWithoutCreatingIt) {
  io_.set_log_exists(true);
  ASSERT_TRUE(create().ok());
  EXPECT_EQ(io_.count(Op::OpenLogExisting), 1U);
  EXPECT_EQ(io_.count(Op::CreateLog), 0U);
}

TEST_F(EngineCreateTest, RecoversAValidNonEmptyLogInsteadOfRefusingIt) {
  // M1 refused any non-empty log. M2 rebuilds the map from it (user story 1).
  test::LogBuilder log;
  log.data(3, 7);
  io_.bytes() = log.bytes();
  Status s = create();
  EXPECT_TRUE(s.ok()) << s.message();
  ASSERT_NE(engine_, nullptr);
  EXPECT_EQ(engine_->bytes_logged(), log.end());
}

TEST_F(EngineCreateTest, LeavesARefusedLogByteForByteUnchanged) {
  // A log that is refused (here, a complete first record that fails its checks) is not touched.
  test::LogBuilder log;
  log.data(3, 7);
  std::string damaged = log.bytes();
  damaged[100] ^= 0x01;  // inside the payload, so the CRC no longer matches
  io_.bytes() = damaged;
  EXPECT_FALSE(create().ok());
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_.bytes(), damaged);
  EXPECT_FALSE(changed_the_file());  // no pwrite, ftruncate or fsync
}

TEST_F(EngineCreateTest, ReportsAnOpenFailure) {
  io_.fail_next(Op::OpenLogExisting, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_FALSE(changed_the_file());
  // create must stop at the failure. Without this, a create that ignored the error
  // would still fail later, because FaultIo rejects every call on an unopened log.
  EXPECT_EQ(io_.count(Op::FileSize), 0U);
  EXPECT_EQ(io_.count(Op::FsyncParentDir), 0U);
}

TEST_F(EngineCreateTest, ReportsAFileSizeFailure) {
  // If the size cannot be read, the log cannot be known to be empty, so refuse. The refusal comes
  // before recovery's directory sync, so none happens.
  io_.set_log_exists(true);
  io_.fail_next(Op::FileSize, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_FALSE(changed_the_file());
  EXPECT_EQ(io_.count(Op::FsyncParentDir), 0U);
}

// A start that created the log and is then refused may leave that empty log. The next start
// treats it as empty and syncs its name.
void expect_empty_log_left_and_next_start_syncs(FaultIo& io, std::unique_ptr<Engine>& engine,
                                                const std::function<Status()>& create) {
  EXPECT_EQ(engine, nullptr);
  EXPECT_TRUE(io.log_exists());
  EXPECT_TRUE(io.bytes().empty());
  EXPECT_FALSE(io.ckpt_exists());
  io.clear_calls();
  Status s = create();
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_NE(engine, nullptr);
  EXPECT_EQ(io.count(Op::FsyncParentDir), 1U);
}

TEST_F(EngineCreateTest, AFileSizeFailureOnAMissingLogLeavesAnEmptyLogAndTheNextStartSucceeds) {
  io_.fail_next(Op::FileSize, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  expect_empty_log_left_and_next_start_syncs(io_, engine_, [&] { return create(); });
}

TEST_F(EngineCreateTest, ALockErrorOnAMissingLogLeavesAnEmptyLogAndTheNextStartSucceeds) {
  io_.fail_next(Op::LockLog, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  expect_empty_log_left_and_next_start_syncs(io_, engine_, [&] { return create(); });
}

TEST_F(EngineCreateTest, CreatingAnEngineWritesNothingToTheLog) {
  ASSERT_TRUE(create().ok());
  EXPECT_FALSE(changed_the_file());
  EXPECT_TRUE(io_.bytes().empty());
}

// --- The parent directory ------------------------------------

TEST_F(EngineCreateTest, SyncsTheParentDirectoryOnceAfterCreatingTheLog) {
  ASSERT_TRUE(create().ok());
  std::vector<size_t> opened;
  std::vector<size_t> synced;
  for (size_t i = 0; i < io_.calls().size(); ++i) {
    if (io_.calls()[i].op == Op::CreateLog) {
      opened.push_back(i);
    }
    if (io_.calls()[i].op == Op::FsyncParentDir) {
      synced.push_back(i);
    }
  }
  ASSERT_EQ(opened.size(), 1U);
  ASSERT_EQ(synced.size(), 1U) << "fsync_parent_dir must be called exactly once";
  EXPECT_LT(opened[0], synced[0]) << "the file's name only exists after create_log";
}

TEST_F(EngineCreateTest, ReportsAParentDirectorySyncFailure) {
  io_.fail_next(Op::FsyncParentDir, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
}

// --- O_EXCL finds the log --------------------------------------------------

constexpr std::string_view kAppeared =
    "another server may have just created it, or the path is a dangling symlink";

TEST_F(EngineCreateTest, ALogThatAppearsDuringStartupIsRefusedWithAnHonestMessage) {
  io_.set_log_exists(false);
  io_.set_create_race(true);
  Status s = create();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(std::string(kPath)), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("appeared during startup"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kAppeared), std::string::npos) << s.message();
}

TEST(EngineCreatePosixTest, ADanglingSymlinkAtTheLogPathIsRefusedAndItsTargetNotCreated) {
  namespace fs = std::filesystem;
  fs::path base = LOGBLOCK_TEST_TMP_DIR;
  fs::create_directories(base);
  std::string pattern = (base / "create-XXXXXX").string();
  ASSERT_NE(mkdtemp(pattern.data()), nullptr);
  const fs::path dir = pattern;
  const fs::path target = dir / "target";
  const std::string link = (dir / "log").string();
  fs::create_symlink(target, link);

  std::unique_ptr<Engine> engine;
  Status s = Engine::create(link, kDeviceSize, make_posix_io(), &engine);
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine, nullptr);
  EXPECT_NE(s.message().find(link), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kAppeared), std::string::npos) << s.message();
  EXPECT_FALSE(fs::exists(target)) << "O_EXCL must not follow the symlink";
  fs::remove_all(dir);
}

// --- A new engine -----------------------------------------------------------

TEST_F(EngineCreateTest, ANewEngineIsOneGibibyteAndHasLoggedNothing) {
  ASSERT_TRUE(create().ok());
  ASSERT_NE(engine_, nullptr);
  EXPECT_EQ(engine_->size(), 1073741824U);  // the literal, not kDeviceSize
  EXPECT_EQ(engine_->bytes_logged(), 0U);
}

TEST_F(EngineCreateTest, SizeDoesNotDependOnTheCap) {
  ASSERT_TRUE(create(kDataRecordSize).ok());
  ASSERT_NE(engine_, nullptr);
  EXPECT_EQ(engine_->size(), 1073741824U);
}

}  // namespace
}  // namespace logblock
