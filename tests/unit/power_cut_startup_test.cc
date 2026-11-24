// A start that was refused after creating the log must not lose the log's name (D35). The first
// start creates the log, its directory fsync fails and it is refused. The next start finds the
// file, syncs the directory and serves. A block written and flushed after that must survive a
// power cut, whatever the unsynced parts of the files keep.

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>

#include "logblock/engine.h"
#include "support/engine_test_base.h"
#include "support/forwarding_io.h"
#include "support/power_cut_io.h"

namespace logblock::test {
namespace {

constexpr const char* kPath = "startup.log";
constexpr uint64_t kCap = 1U << 20;

Status start_on(PowerCutIo& io, std::unique_ptr<Engine>* out) {
  return Engine::create(kPath, kCap, std::make_unique<ForwardingIo>(io), out,
                        kDefaultCheckpointInterval);
}

// Runs the scenario and reboots with `outcome` for the log and drop-all for the checkpoint file
// (or the seed's own choice when `use_seed_outcomes`). Returns an empty string on success.
std::string run(uint64_t seed, bool use_seed_outcomes, const PowerCutIo::Outcome& outcome) {
  PowerCutIo fresh(seed);
  fresh.fail_next(PowerCutIo::Op::FsyncParentDir, StatusCode::IoError);
  std::unique_ptr<Engine> engine;
  if (start_on(fresh, &engine).ok() || engine != nullptr) {
    return "the first start was not refused";
  }
  auto io = fresh.restart();
  if (Status s = start_on(*io, &engine); !s.ok()) {
    return "second start refused: " + s.message();
  }
  const std::array<uint8_t, kBlockSize> data = [] {
    std::array<uint8_t, kBlockSize> d{};
    d.fill(0x5A);
    return d;
  }();
  if (Status s = engine->write(0, kBlockSize, data.data()); !s.ok()) {
    return "write: " + s.message();
  }
  if (Status s = engine->flush(); !s.ok()) {
    return "flush: " + s.message();
  }
  engine.reset();
  auto after = use_seed_outcomes ? io->reboot() : io->reboot(outcome, PowerCutIo::Outcome{});
  if (Status s = start_on(*after, &engine); !s.ok()) {
    return "start after the cut refused: " + s.message();
  }
  std::array<uint8_t, kBlockSize> got{};
  if (Status s = engine->read(0, kBlockSize, got.data()); !s.ok()) {
    return "read: " + s.message();
  }
  if (got != data) {
    return "the flushed block did not survive the power cut";
  }
  return "";
}

TEST(PowerCutStartup, ARefusedFirstStartDoesNotLoseTheLogNameDropAll) {
  const std::string failure = run(0, false, PowerCutIo::Outcome{});
  EXPECT_EQ(failure, "") << "seed 0, drop-all";
}

TEST(PowerCutStartup, ARefusedFirstStartDoesNotLoseTheLogNameRandomPrefix) {
  for (uint64_t seed = 0; seed < 20; ++seed) {
    const std::string failure = run(seed, true, PowerCutIo::Outcome{});
    EXPECT_EQ(failure, "") << "seed " << seed << ", random outcomes";
  }
}

}  // namespace
}  // namespace logblock::test
