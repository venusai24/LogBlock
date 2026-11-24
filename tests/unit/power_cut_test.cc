// Fixed-seed power-cut runs (T028, FR-032 to FR-034). Each seed cuts the power at one operation of
// a short workload, restarts, and requires the device to hold some unbroken prefix of the requests,
// including everything a completed flush covered. See tests/support/power_cut_run.h.

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>
#include <vector>

#include "support/power_cut_run.h"

namespace logblock::test {
namespace {

// 50 fixed seeds. Class is seed % 7 and the log outcome is seed % 5, so seeds 0 to 34 meet every
// (class, log outcome) pair, and 35 to 49 repeat the first fifteen with new workloads.
constexpr uint64_t kSeedCount = 50;

// Runs every fixed seed. Each run writes whole 2 MiB checkpoint slots, so it is slow in a Debug
// build, and CTest runs every TEST in its own process. The tests below therefore each run the
// seeds they need only once, and the ones that need the whole set share a single TEST.
std::vector<PowerCutResult> run_all_seeds() {
  std::vector<PowerCutResult> out;
  out.reserve(kSeedCount);
  for (uint64_t seed = 0; seed < kSeedCount; ++seed) {
    out.push_back(power_cut_run(seed));
  }
  return out;
}

// --- The workload and the prefix check -------------------------------------------------------

TEST(PowerCutWorkload, ASeedGivesTheSameWorkloadEveryTime) {
  EXPECT_EQ(make_workload(7, true), make_workload(7, true));
  EXPECT_NE(make_workload(7, true), make_workload(8, true));
}

TEST(PowerCutWorkload, StartsWithEnoughWritesToCheckpointAndThenAFlush) {
  for (uint64_t seed = 0; seed < kSeedCount; ++seed) {
    auto w = make_workload(seed, true);
    ASSERT_GE(w.size(), 11U);
    for (size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(w[i].kind, Request::Kind::Write) << "seed " << seed;
    }
    EXPECT_EQ(w[4].kind, Request::Kind::Flush) << "seed " << seed;
  }
}

TEST(PowerCutWorkload, WithoutFlushesThereIsNoFlush) {
  for (uint64_t seed = 0; seed < kSeedCount; ++seed) {
    for (const Request& r : make_workload(seed, false)) {
      EXPECT_NE(r.kind, Request::Kind::Flush) << "seed " << seed;
    }
  }
}

TEST(PowerCutPrefix, AcceptsAnyUnbrokenPrefixInTheAllowedRange) {
  // write block 0 (id 1), write block 1 (id 2), trim block 0, flush
  const std::vector<Request> w = {{Request::Kind::Write, 0},
                                  {Request::Kind::Write, 1},
                                  {Request::Kind::Trim, 0},
                                  {Request::Kind::Flush, 0}};
  size_t matched = 99;
  EXPECT_TRUE(matches_some_prefix(w, {}, 0, 4, &matched));
  EXPECT_EQ(matched, 0U);
  EXPECT_TRUE(matches_some_prefix(w, {{0, 1}}, 0, 4, &matched));
  EXPECT_EQ(matched, 1U);
  EXPECT_TRUE(matches_some_prefix(w, {{0, 1}, {1, 2}}, 0, 4, &matched));
  EXPECT_EQ(matched, 2U);
  EXPECT_TRUE(matches_some_prefix(w, {{1, 2}}, 0, 4, &matched));
  EXPECT_EQ(matched, 3U);
}

TEST(PowerCutPrefix, RejectsStatesThatSkipARequest) {
  const std::vector<Request> w = {{Request::Kind::Write, 0},
                                  {Request::Kind::Write, 1},
                                  {Request::Kind::Write, 0}};
  size_t matched = 0;
  // Request 2 (id 3) without request 1 (id 2): the later write survived, the earlier did not.
  EXPECT_FALSE(matches_some_prefix(w, {{0, 3}}, 0, 3, &matched));
  // Block 1 written but block 0 empty: request 1 without request 0.
  EXPECT_FALSE(matches_some_prefix(w, {{1, 2}}, 0, 3, &matched));
  // A value no request wrote.
  EXPECT_FALSE(matches_some_prefix(w, {{0, 9}}, 0, 3, &matched));
}

TEST(PowerCutPrefix, RejectsAStateOlderThanTheFlushedPrefix) {
  const std::vector<Request> w = {{Request::Kind::Write, 0},
                                  {Request::Kind::Write, 1},
                                  {Request::Kind::Flush, 0},
                                  {Request::Kind::Write, 0}};
  size_t matched = 0;
  // The flush covered the first three requests, so losing request 1 is not allowed.
  EXPECT_FALSE(matches_some_prefix(w, {{0, 1}}, 3, 4, &matched));
  EXPECT_TRUE(matches_some_prefix(w, {{0, 1}, {1, 2}}, 3, 4, &matched));
  EXPECT_EQ(matched, 3U);
  EXPECT_TRUE(matches_some_prefix(w, {{0, 4}, {1, 2}}, 3, 4, &matched));
  EXPECT_EQ(matched, 4U);
}

TEST(PowerCutPrefix, RejectsAStateNewerThanTheRequestsIssued) {
  const std::vector<Request> w = {{Request::Kind::Write, 0}, {Request::Kind::Write, 1}};
  size_t matched = 0;
  EXPECT_FALSE(matches_some_prefix(w, {{0, 1}, {1, 2}}, 0, 1, &matched));
}

// --- Fixed seeds -----------------------------------------------------------------------------

TEST(PowerCutRun, EveryFixedSeedRecoversToAnUnbrokenPrefixAndTheSeedsCoverEverything) {
  const std::vector<PowerCutResult> all = run_all_seeds();

  // Every seed passes: the device holds an unbroken prefix that includes the flushed requests.
  for (const PowerCutResult& r : all) {
    EXPECT_TRUE(r.ok()) << r.failure;
    EXPECT_TRUE(r.cut_reached) << "seed " << r.seed;
    EXPECT_GE(r.requests_issued, 1U) << "seed " << r.seed;
    EXPECT_LE(r.flushed_prefix, r.matched_prefix) << "seed " << r.seed;
    EXPECT_LE(r.matched_prefix, r.requests_issued) << "seed " << r.seed;
  }

  // Together they reach every crash-point class and give both files every outcome.
  std::set<CrashPoint> classes;
  std::set<FileOutcome> log_outcomes;
  std::set<FileOutcome> ckpt_outcomes;
  size_t checkpoint = 0;
  size_t full_scan = 0;
  size_t torn = 0;
  size_t covered = 0;
  size_t uncovered = 0;
  std::set<std::string> shapes;
  for (const PowerCutResult& r : all) {
    classes.insert(r.crash_point);
    log_outcomes.insert(r.log_outcome);
    ckpt_outcomes.insert(r.ckpt_outcome);
    checkpoint += r.recovery_path == RecoveryPath::Checkpoint ? 1 : 0;
    full_scan += r.recovery_path == RecoveryPath::FullScan ? 1 : 0;
    torn += r.torn_tail ? 1 : 0;
    // A cut at a client flush's fsync leaves that flush incomplete; later cuts keep it covered.
    covered += r.flushed_prefix > 0 ? 1 : 0;
    uncovered += r.flushed_prefix == 0 ? 1 : 0;
    shapes.insert(std::to_string(r.requests_issued) + "/" + std::to_string(r.matched_prefix) +
                  "/" + to_string(r.recovery_path) + (r.torn_tail ? "/torn" : ""));
  }
  EXPECT_EQ(classes.size(), kCrashPointCount);
  EXPECT_EQ(log_outcomes.size(), kFileOutcomeCount);
  EXPECT_EQ(ckpt_outcomes.size(), kFileOutcomeCount);
  EXPECT_GT(checkpoint, 0U) << "no seed recovered from a checkpoint";
  EXPECT_GT(full_scan, 0U) << "no seed recovered by a full scan";
  EXPECT_GT(torn, 0U) << "no seed removed a torn tail";
  EXPECT_GT(covered, 0U);
  EXPECT_GT(uncovered, 0U);
  EXPECT_GT(shapes.size(), 5U) << "the seeds should not all behave the same";
}

TEST(PowerCutRun, ASeedRunTwiceGivesIdenticalResults) {
  // Seeds 0 to 6 are one cycle of crash-point classes; the whole set is not run twice only to keep
  // the suite fast.
  for (uint64_t seed = 0; seed < 7; ++seed) {
    const PowerCutResult first = power_cut_run(seed);
    EXPECT_TRUE(first.ok()) << first.failure;
    EXPECT_EQ(power_cut_run(seed), first) << "seed " << seed;
  }
}

TEST(PowerCutRun, NamesArePrintableAndDistinct) {
  std::set<std::string> names;
  for (size_t i = 0; i < kCrashPointCount; ++i) {
    names.insert(to_string(static_cast<CrashPoint>(i)));
  }
  for (size_t i = 0; i < kFileOutcomeCount; ++i) {
    names.insert(to_string(static_cast<FileOutcome>(i)));
  }
  for (RecoveryPath p : {RecoveryPath::None, RecoveryPath::Checkpoint, RecoveryPath::FullScan}) {
    names.insert(to_string(p));
  }
  EXPECT_EQ(names.size(), kCrashPointCount + kFileOutcomeCount + 3);
  EXPECT_EQ(names.count("unknown"), 0U);
}

}  // namespace
}  // namespace logblock::test
