// Tests for Engine::flush and the FlushFailed state. A flush is one fsync of the log. If that fsync
// fails, the engine cannot know which writes reached the disk (after a failed fsync the
// kernel may have dropped the dirty pages), so every later flush returns Failed and
// never calls fsync again: a later "success" could report lost data as durable.
// Reads, writes and trims keep working in FlushFailed. Only a failed append whose
// ftruncate also fails moves the engine on to AppendFailed.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "support/engine_test_base.h"

namespace logblock {
namespace {

using test::Bytes;
using test::pattern;
using test::SameBytes;
using test::zeros;
using Op = test::FaultIo::Op;

class EngineFlushTest : public test::EngineTestBase {
 protected:
  // Writes one whole block with data derived from `seed`.
  Status write_block(uint64_t block, uint8_t seed) {
    return write(block * kBlockSize, pattern(seed, kBlockSize));
  }

  // The Pwrite and Fsync calls so far, in order. Other calls (pread, the open at
  // startup) are left out because they say nothing about durability.
  std::vector<Op> pwrite_fsync_order() const {
    std::vector<Op> order;
    for (const auto& call : io_->calls()) {
      if (call.op == Op::Pwrite || call.op == Op::Fsync) {
        order.push_back(call.op);
      }
    }
    return order;
  }

  // Makes the next flush fail and returns what it returned. The engine is in
  // FlushFailed afterwards. Block 0 holds known data before that.
  Status break_flush() {
    EXPECT_TRUE(write_block(0, 1).ok());
    io_->fail_next(Op::Fsync, StatusCode::IoError);
    return engine_->flush();
  }

  // What every flush must say once the engine is in FlushFailed or AppendFailed: Failed,
  // and no call to fsync, even though the injected failure was used up long ago.
  void ExpectFlushesFailWithoutFsync() {
    io_->clear_calls();
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed);
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed) << "the state is sticky";
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed);
    EXPECT_EQ(io_->count(Op::Fsync), 0U) << "a failed engine must not call fsync again";
  }
};

// --- The normal case ------------------------------------------------------------

TEST_F(EngineFlushTest, FlushAfterWritesCallsFsyncOnceAfterTheLastPwriteAndReturnsOk) {
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write(5 * kBlockSize, pattern(2, 3 * kBlockSize)).ok());  // three more records
  ASSERT_TRUE(engine_->trim(0, kBlockSize).ok());
  io_->clear_calls();

  Status s = engine_->flush();

  EXPECT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(io_->count(Op::Fsync), 1U);
  // The fsync is the last thing that happened: nothing was written after it, and (since
  // the calls were cleared) every pwrite of the writes above came before it.
  ASSERT_FALSE(io_->calls().empty());
  EXPECT_EQ(io_->calls().back().op, Op::Fsync);
}

TEST_F(EngineFlushTest, FsyncComesAfterEveryPwriteOfTheEarlierWrites) {
  // Same check without clear_calls(): the whole call list since startup. Every pwrite
  // must come before the one fsync, or the sync would not cover that write.
  ASSERT_TRUE(write(0, pattern(1, 4 * kBlockSize)).ok());
  ASSERT_TRUE(engine_->flush().ok());

  const std::vector<Op> order = pwrite_fsync_order();
  ASSERT_EQ(order.size(), 5U);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(order[i], Op::Pwrite) << "call " << i;
  }
  EXPECT_EQ(order[4], Op::Fsync);
}

TEST_F(EngineFlushTest, FlushChangesNothingInTheLogOrTheData) {
  const Bytes data = pattern(1, 2 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  const uint64_t logged = engine_->bytes_logged();
  const std::string file_before = io_->bytes();

  ASSERT_TRUE(engine_->flush().ok());

  EXPECT_EQ(engine_->bytes_logged(), logged);
  EXPECT_EQ(io_->bytes(), file_before);
  EXPECT_TRUE(SameBytes(read(0, 2 * kBlockSize), data));
}

TEST_F(EngineFlushTest, FlushOnAnEmptyLogReturnsOk) {
  // A flush covers "all data logged before it", and there is none. Calling fsync anyway
  // and skipping it are both defensible, so the test only asserts the result.
  Status s = engine_->flush();
  EXPECT_TRUE(s.ok()) << s.to_string();
}

TEST_F(EngineFlushTest, TwoFlushesInARowBothCallFsync) {
  ASSERT_TRUE(write_block(0, 1).ok());
  io_->clear_calls();

  EXPECT_TRUE(engine_->flush().ok());
  EXPECT_TRUE(engine_->flush().ok());

  // Each flush is its own sync. The engine does not remember "already synced"
  // (that would be a cache that could go stale), so the count is two.
  EXPECT_EQ(io_->count(Op::Fsync), 2U);
}

TEST_F(EngineFlushTest, WritesAfterAFlushAreCoveredByTheNextFlush) {
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(engine_->flush().ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  ASSERT_TRUE(engine_->trim(0, kBlockSize).ok());
  ASSERT_TRUE(engine_->flush().ok());

  // write, flush, write, trim, flush: each fsync follows the pwrites it must cover.
  const std::vector<Op> want = {Op::Pwrite, Op::Fsync, Op::Pwrite, Op::Pwrite, Op::Fsync};
  EXPECT_EQ(pwrite_fsync_order(), want);
}

TEST_F(EngineFlushTest, FlushWorksOnAFullLog) {
  // Room for exactly two DATA records. A flush appends nothing, so it needs no room.
  make_engine(2 * kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  ASSERT_EQ(write_block(2, 3).code(), StatusCode::NoSpace) << "the log is full";
  io_->clear_calls();

  Status s = engine_->flush();

  EXPECT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(io_->count(Op::Fsync), 1U);
  // A full log is not a failed state: a rejected write did not make flushes fail.
  EXPECT_TRUE(engine_->flush().ok());
}

// --- A failed fsync: FlushFailed -----------------------------------------

TEST_F(EngineFlushTest, AFailedFsyncReturnsIoError) {
  Status s = break_flush();

  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  EXPECT_EQ(io_->count(Op::Fsync), 1U);
}

TEST_F(EngineFlushTest, EveryLaterFlushReturnsFailedWithoutCallingFsyncAgain) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  // The injected failure was one-shot and is gone: a real fsync would now succeed in
  // FaultIo. The engine must still refuse.
  ExpectFlushesFailWithoutFsync();
}

TEST_F(EngineFlushTest, ReadsStillWorkInFlushFailed) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), zeros(kBlockSize)));
  Bytes across(2 * kBlockSize);
  EXPECT_TRUE(engine_->read(0, across.size(), across.data()).ok());
}

TEST_F(EngineFlushTest, WritesAndTrimsStillWorkInFlushFailed) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  EXPECT_TRUE(write_block(1, 2).ok());
  EXPECT_TRUE(write(100, pattern(3, 10)).ok());  // a partial write needs a read-modify-write
  EXPECT_TRUE(engine_->trim(kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(engine_->trim(0, 0).ok());

  // The data of those requests is really there.
  const Bytes block0 = read(0, kBlockSize);
  EXPECT_TRUE(SameBytes(test::slice(block0, 100, 10), pattern(3, 10)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), zeros(kBlockSize))) << "block 1 was trimmed";
}

TEST_F(EngineFlushTest, AWriteAfterAFailedFlushDoesNotMakeTheNextFlushWork) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  ASSERT_TRUE(write_block(1, 2).ok());
  io_->clear_calls();

  // New data was logged after the failure, but the engine cannot tell the earlier
  // writes are safe, so it still cannot say "everything logged so far is durable".
  EXPECT_EQ(engine_->flush().code(), StatusCode::Failed);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
}

TEST_F(EngineFlushTest, AFailedFlushOnAFullLogAlsoSticks) {
  // A full log: room for exactly one DATA record, which block 0 uses.
  make_engine(kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  ASSERT_EQ(engine_->flush().code(), StatusCode::IoError);

  ExpectFlushesFailWithoutFsync();
  // The log is still full, and that still says NoSpace, not Failed: FlushFailed
  // does not touch writes.
  EXPECT_EQ(write_block(1, 2).code(), StatusCode::NoSpace);
}

// --- FlushFailed -> AppendFailed, and no way back ------------------------------

TEST_F(EngineFlushTest, AFailedAppendWithAFailedCutBackMovesFlushFailedToAppendFailed) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);  // FlushFailed, block 0 written
  ASSERT_TRUE(write_block(1, 2).ok());                   // still allowed in FlushFailed
  const uint64_t good_end = engine_->bytes_logged();

  // The next append fails after 10 bytes and the ftruncate that should clean up fails too.
  // The two faults sit in different queues, so the order of these calls does not matter.
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 10);
  io_->fail_next(Op::Ftruncate, StatusCode::IoError);
  Status s = write_block(2, 3);

  EXPECT_EQ(s.code(), StatusCode::IoError)
      << "the request that broke the log reports its own error";
  EXPECT_EQ(engine_->bytes_logged(), good_end) << "torn bytes are not counted";

  // Now AppendFailed: writes and trims fail as well, not only flushes.
  io_->clear_calls();
  EXPECT_EQ(write_block(3, 4).code(), StatusCode::Failed);
  EXPECT_EQ(write(100, pattern(5, 10)).code(), StatusCode::Failed);
  EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::Failed);
  EXPECT_EQ(io_->count(Op::Pwrite), 0U) << "a rejected request must not touch the log";
  ExpectFlushesFailWithoutFsync();

  // Reads still work.
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineFlushTest, AFailedAppendWithAWorkingCutBackKeepsFlushFailedAndWritesWorking) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  io_->fail_next(Op::Pwrite, StatusCode::IoError, 10);  // the ftruncate works
  ASSERT_EQ(write_block(1, 2).code(), StatusCode::IoError);

  // Still FlushFailed and not AppendFailed: writes work, flushes do not.
  EXPECT_TRUE(write_block(1, 2).ok());
  io_->clear_calls();
  EXPECT_EQ(engine_->flush().code(), StatusCode::Failed);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
}

TEST_F(EngineFlushTest, NothingTakesTheEngineBackToHealthy) {
  ASSERT_EQ(break_flush().code(), StatusCode::IoError);

  // Many successful requests of every kind. The injected fault is used up, so a real
  // fsync would succeed, but no flush works afterwards.
  for (uint8_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(write_block(i, i).ok());
    ASSERT_TRUE(engine_->trim(i * kBlockSize, kBlockSize).ok());
    EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed) << "round " << static_cast<int>(i);
  }
  ExpectFlushesFailWithoutFsync();

  // Break the log as well (AppendFailed): still no way back.
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 10);
  io_->fail_next(Op::Ftruncate, StatusCode::IoError);
  ASSERT_EQ(write_block(7, 7).code(), StatusCode::IoError);
  ExpectFlushesFailWithoutFsync();
  EXPECT_EQ(write_block(8, 8).code(), StatusCode::Failed);
}

}  // namespace
}  // namespace logblock
