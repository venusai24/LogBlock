// The durability order, proven by test (T029, FR-035, SC-011). A flush-free workload makes every
// log fsync come from a checkpoint. After each cut, and before any recovery, the surviving files
// are checked directly:
//   1. no valid slot may claim a record the surviving log does not hold;
//   2. the newest checkpoint whose steps all returned must still be readable.
// The correct order passes every seed. Each lying fsync must trip one of the checks, and the
// test asserts which one, so a check that never fires cannot hide.

#include <gtest/gtest.h>

#include <vector>

#include "support/power_cut_run.h"

namespace logblock::test {
namespace {

// Each run writes whole 2 MiB slots, which is slow in a Debug build, so the set is kept small.
// CTest runs each TEST in its own process, so every test below runs the seeds it needs once.
constexpr uint64_t kSeedCount = 16;

std::vector<OrderingResult> run_all(const Lies& lies) {
  std::vector<OrderingResult> out;
  out.reserve(kSeedCount);
  for (uint64_t seed = 0; seed < kSeedCount; ++seed) {
    out.push_back(power_cut_ordering_run(seed, lies));
  }
  return out;
}

int count(const std::vector<OrderingResult>& results, OrderingCheck check) {
  int n = 0;
  for (const OrderingResult& r : results) {
    n += r.check == check ? 1 : 0;
  }
  return n;
}

void expect_harness_ok(const std::vector<OrderingResult>& results) {
  for (const OrderingResult& r : results) {
    EXPECT_TRUE(r.failure.empty()) << r.failure;
  }
}

// A lost-checkpoint report is only meaningful once a checkpoint completed, and every report
// carries its reason.
void expect_details(const std::vector<OrderingResult>& results) {
  for (const OrderingResult& r : results) {
    if (r.check == OrderingCheck::Passed) {
      EXPECT_TRUE(r.detail.empty()) << "seed " << r.seed;
      continue;
    }
    EXPECT_FALSE(r.detail.empty()) << "seed " << r.seed;
    EXPECT_GE(r.completed_checkpoints, 1U) << "seed " << r.seed << ": " << r.detail;
  }
}

TEST(PowerCutOrdering, TheCorrectOrderPassesEverySeed) {
  const auto all = run_all({});
  expect_harness_ok(all);
  for (const OrderingResult& r : all) {
    EXPECT_EQ(r.check, OrderingCheck::Passed) << "seed " << r.seed << ": " << r.detail;
  }

  // A pass proves nothing unless some seeds cut after checkpoints completed, and some before.
  bool none = false;
  bool some = false;
  bool several = false;
  bool uncut = false;
  for (const OrderingResult& r : all) {
    none = none || r.completed_checkpoints == 0;
    some = some || r.completed_checkpoints >= 1;
    several = several || r.completed_checkpoints >= 3;
    uncut = uncut || !r.cut_reached;
  }
  EXPECT_TRUE(none);
  EXPECT_TRUE(some);
  EXPECT_TRUE(several);
  EXPECT_TRUE(uncut) << "one seed should let the workload finish and cut at the end";
}

TEST(PowerCutOrdering, ASeedRunTwiceGivesIdenticalResults) {
  // A lying run, since that is the one that produces a detail string.
  Lies lie;
  lie.log_fsync = true;
  for (uint64_t seed = 0; seed < 8; ++seed) {
    EXPECT_EQ(power_cut_ordering_run(seed, lie), power_cut_ordering_run(seed, lie))
        << "seed " << seed;
  }
}

TEST(PowerCutOrdering, ALyingLogFsyncLeavesASlotClaimingALostRecord) {
  Lies lie;
  lie.log_fsync = true;
  const auto all = run_all(lie);
  expect_harness_ok(all);
  expect_details(all);
  EXPECT_GT(count(all, OrderingCheck::SlotClaimsLostRecord), 0)
      << "no seed noticed a lying log fsync";
  // Only this check may fire: the checkpoint file itself is honest.
  EXPECT_EQ(count(all, OrderingCheck::NewestCheckpointLost), 0);
}

TEST(PowerCutOrdering, ALyingCheckpointFsyncLosesTheNewestCheckpoint) {
  Lies lie;
  lie.ckpt_fsync = true;
  const auto all = run_all(lie);
  expect_harness_ok(all);
  expect_details(all);
  EXPECT_GT(count(all, OrderingCheck::NewestCheckpointLost), 0)
      << "no seed noticed a lying checkpoint fsync";
  // The log fsync before each slot is honest, so no slot may claim a lost record.
  EXPECT_EQ(count(all, OrderingCheck::SlotClaimsLostRecord), 0);
}

TEST(PowerCutOrdering, ALyingDirectoryFsyncLosesOnlyTheCheckpointFile) {
  Lies lie;
  lie.dir_fsync = true;
  const auto all = run_all(lie);
  expect_harness_ok(all);
  expect_details(all);

  // The lie drops one directory entry, so every run that completed a checkpoint reports exactly
  // this code, and never the other one.
  int lost = 0;
  for (const OrderingResult& r : all) {
    EXPECT_NE(r.check, OrderingCheck::SlotClaimsLostRecord) << "seed " << r.seed << ": " << r.detail;
    if (r.completed_checkpoints == 0) {
      EXPECT_EQ(r.check, OrderingCheck::Passed) << "seed " << r.seed;
      continue;
    }
    EXPECT_EQ(r.check, OrderingCheck::NewestCheckpointLost) << "seed " << r.seed;
    ++lost;
    // Only the .ckpt entry is lost. The log's name is durable and its checkpointed records are
    // there, so this is not the whole device vanishing.
    EXPECT_FALSE(r.ckpt_survived) << "seed " << r.seed;
    EXPECT_TRUE(r.log_survived) << "seed " << r.seed;
    EXPECT_GE(r.log_records, kPowerCutCheckpointInterval / kDataRecordSize) << "seed " << r.seed;
  }
  EXPECT_GT(lost, 0) << "no seed completed a checkpoint before the cut";
}

}  // namespace
}  // namespace logblock::test
