// Tests for Engine::create: what it accepts, what it refuses, and
// which I/O calls it makes. Uses FaultIo, so no real file is touched.
//
// Refusals use fixed codes (cap -> OutOfRange, non-empty
// log -> Failed) and a message that says why. The I/O error tests check that the
// Io's own code is passed on.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "logblock/engine.h"
#include "support/fault_io.h"

namespace logblock {
namespace {

using test::FaultIo;
using Op = FaultIo::Op;

constexpr std::string_view kPath = "engine-create.log";

// Engine::create takes ownership of its Io and destroys it if creation fails. The
// tests still need to look at the FaultIo afterwards (its calls and bytes), so the
// engine gets this wrapper and the test keeps the FaultIo itself.
class ForwardingIo final : public Io {
 public:
  explicit ForwardingIo(Io& target) : target_(target) {}
  Status open_log(const std::string& path) override { return target_.open_log(path); }
  Status file_size(uint64_t* size) override { return target_.file_size(size); }
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override {
    return target_.pread(offset, buf, len, bytes_read);
  }
  Status pwrite(uint64_t offset, const void* buf, size_t len) override {
    return target_.pwrite(offset, buf, len);
  }
  Status fsync() override { return target_.fsync(); }
  Status ftruncate(uint64_t size) override { return target_.ftruncate(size); }
  Status fsync_parent_dir() override { return target_.fsync_parent_dir(); }

 private:
  Io& target_;
};

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
  // open the log at all.
  EXPECT_FALSE(create(kDataRecordSize - 1).ok());
  EXPECT_EQ(io_.count(Op::OpenLog), 0U);
  EXPECT_FALSE(changed_the_file());
}

// --- The log file -----------------------------------------------------------

TEST_F(EngineCreateTest, AcceptsAMissingOrEmptyLog) {
  // FaultIo creates the file in open_log, as PosixIo does for a missing path, so a
  // missing log and an empty one look the same to the engine.
  Status s = create();
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_NE(engine_, nullptr);
}

TEST_F(EngineCreateTest, OpensTheLogAtTheGivenPathOnce) {
  ASSERT_TRUE(create().ok());
  EXPECT_EQ(io_.path(), std::string(kPath));
  EXPECT_EQ(io_.count(Op::OpenLog), 1U);
}

TEST_F(EngineCreateTest, RefusesANonEmptyLogAndSaysWhy) {
  io_.bytes() = "x";  // even one byte is non-empty
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::Failed) << s.message();
  EXPECT_NE(s.message().find("empty"), std::string::npos) << s.message();
  EXPECT_EQ(engine_, nullptr);
}

TEST_F(EngineCreateTest, LeavesARefusedLogByteForByteUnchanged) {
  const std::string before = "records from an older run";
  io_.bytes() = before;
  EXPECT_FALSE(create().ok());
  EXPECT_EQ(io_.bytes(), before);
  EXPECT_FALSE(changed_the_file());  // no pwrite, ftruncate or fsync
}

TEST_F(EngineCreateTest, ReportsAnOpenFailure) {
  io_.fail_next(Op::OpenLog, StatusCode::IoError);
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
  // If the size cannot be read, the log cannot be known to be empty, so refuse.
  io_.fail_next(Op::FileSize, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_FALSE(changed_the_file());
  EXPECT_EQ(io_.count(Op::FsyncParentDir), 0U);
}

TEST_F(EngineCreateTest, CreatingAnEngineWritesNothingToTheLog) {
  ASSERT_TRUE(create().ok());
  EXPECT_FALSE(changed_the_file());
  EXPECT_TRUE(io_.bytes().empty());
}

// --- The parent directory ------------------------------------

TEST_F(EngineCreateTest, SyncsTheParentDirectoryOnceAfterOpeningTheLog) {
  ASSERT_TRUE(create().ok());
  std::vector<size_t> opened;
  std::vector<size_t> synced;
  for (size_t i = 0; i < io_.calls().size(); ++i) {
    if (io_.calls()[i].op == Op::OpenLog) {
      opened.push_back(i);
    }
    if (io_.calls()[i].op == Op::FsyncParentDir) {
      synced.push_back(i);
    }
  }
  ASSERT_EQ(opened.size(), 1U);
  ASSERT_EQ(synced.size(), 1U) << "fsync_parent_dir must be called exactly once";
  EXPECT_LT(opened[0], synced[0]) << "the file's name only exists after open_log";
}

TEST_F(EngineCreateTest, ReportsAParentDirectorySyncFailure) {
  io_.fail_next(Op::FsyncParentDir, StatusCode::IoError);
  Status s = create();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
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
