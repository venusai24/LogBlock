// Crash test. Usage: crash_test [iterations] [seed]
//
// Seeds run in order from the base seed. EVEN seeds are kill iterations and ODD seeds are
// power-cut iterations, so a run of 200 does 100 of each.
//
// A kill iteration starts a real nbdkit with the logblock plugin and sends it single-block
// requests: writes, overwrites, trims and flushes, so one request is one log record. Then it
// SIGKILLs nbdkit with more requests in flight and checks:
//   (c) from the log file alone: scan_log reports Clean or a torn tail (never corruption), at
//       least every record covered by a completed flush is present, and record i is exactly the
//       i-th request;
//   (d) after a restart on the same log (a second Harness): every block reads back through libnbd
//       and the whole device equals the state after the first R requests, where R is the number
//       of valid records the log held (FR-025).
// Some iterations repeat kill and restart on one log, dropping the requests that did not survive
// each time (FR-026). Half use a tiny checkpoint_interval (the checkpoint path) and half the
// default (the full-scan path).
//
// A power-cut iteration calls power_cut_run() (tests/support/power_cut_run.h), which cuts power in
// the simulated I/O layer at a seeded operation. SIGKILL leaves the page cache intact, so only
// that half tests what power loss does.
//
// The run counts how many iterations reached each recovery path, file outcome and crash-point
// class, prints the counts, and fails if any is zero (FR-026, SC-002), except when it runs a
// single iteration (a replay of one seed).

#include <gtest/gtest.h>
#include <libnbd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "log_scan.h"
#include "logblock/engine.h"
#include "logblock/io.h"
#include "support/power_cut_run.h"

namespace logblock::itest {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kBlocksUsed = 64;
constexpr const char* kCap = "64M";  // ~15,000 records; each iteration writes far fewer
// Four DATA records: a few hundred requests cross it dozens of times (the checkpoint path).
constexpr const char* kSmallInterval = "16544";
// The default: far more than any iteration writes, so recovery is a full scan.
constexpr const char* kLargeInterval = "64M";

// --- The workload ---------------------------------------------------------------------------

struct Step {
  enum class Kind : uint8_t { Write, Trim, Flush };
  Kind kind = Kind::Write;
  uint64_t block = 0;
  bool operator==(const Step&) const = default;
};

// What one cycle of an iteration does. A pure function of (seed, cycle), so the same seed always
// makes the same workload (FR-034, SC-010).
struct Plan {
  std::vector<Step> sync_steps;  // sent one at a time, each waited for
  std::vector<Step> in_flight;   // Write or Trim, sent without waiting, then the kill
  uint64_t kill_delay_us = 0;
  bool operator==(const Plan&) const = default;
};

Plan plan_for(uint64_t seed, uint64_t cycle) {
  std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ULL + cycle);
  Plan plan;
  const uint64_t sync_count = 10 + rng() % 190;
  for (uint64_t i = 0; i < sync_count; ++i) {
    const uint64_t roll = rng() % 100;
    const uint64_t block = rng() % kBlocksUsed;
    if (roll < 70) {
      plan.sync_steps.push_back({Step::Kind::Write, block});
    } else if (roll < 85) {
      plan.sync_steps.push_back({Step::Kind::Trim, block});
    } else {
      plan.sync_steps.push_back({Step::Kind::Flush, 0});
    }
  }
  // Always end with a flush, so each cycle has a durable part.
  plan.sync_steps.push_back({Step::Kind::Flush, 0});
  const uint64_t in_flight = 1 + rng() % 512;
  for (uint64_t i = 0; i < in_flight; ++i) {
    const bool trim = rng() % 100 < 15;
    plan.in_flight.push_back({trim ? Step::Kind::Trim : Step::Kind::Write, rng() % kBlocksUsed});
  }
  plan.kill_delay_us = rng() % 300;
  return plan;
}

// The request that put record k in the log. `seed` is the cycle's seed, which the payload uses.
struct Mutation {
  bool trim = false;
  uint64_t block = 0;
  uint64_t seed = 0;
};
using History = std::vector<Mutation>;

std::vector<uint8_t> payload_of(uint64_t seed, uint64_t k) {
  std::vector<uint8_t> data(kBlockSize);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(seed + k * 131 + i * 7);
  }
  // The first 8 bytes hold k itself, so a block read back says which write put it there.
  for (std::size_t i = 0; i < 8; ++i) data[i] = static_cast<uint8_t>(k >> (8 * i));
  return data;
}

// --- Small helpers --------------------------------------------------------------------------

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool fail(const std::string& message) {
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

// The value of "<key>=<word>" in the text, or an empty string.
std::string field(const std::string& text, const std::string& key) {
  const std::regex pattern("\\b" + key + "=(\\S+)");
  std::smatch match;
  return std::regex_search(text, match, pattern) ? match[1].str() : std::string();
}

// --- Coverage -------------------------------------------------------------------------------

class Coverage {
 public:
  void hit(const std::string& key) { ++counts_[key]; }

  // Prints every count, and returns false if a required one is zero.
  bool report(bool require_all) const {
    std::cout << "coverage:\n";
    bool ok = true;
    for (const std::string& key : required_keys()) {
      const auto found = counts_.find(key);
      const int count = found == counts_.end() ? 0 : found->second;
      std::cout << "  " << key << " = " << count << "\n";
      if (count == 0 && require_all) {
        std::cerr << "FAIL: no iteration reached '" << key
                  << "'; run more iterations, or the seeds no longer cycle through it\n";
        ok = false;
      }
    }
    return ok;
  }

 private:
  static std::vector<std::string> required_keys() {
    std::vector<std::string> keys = {"path:checkpoint", "path:full_scan", "path:torn_tail"};
    for (size_t i = 0; i < test::kFileOutcomeCount; ++i) {
      keys.push_back(std::string("outcome:") + test::to_string(static_cast<test::FileOutcome>(i)));
    }
    for (size_t i = 0; i < test::kCrashPointCount; ++i) {
      keys.push_back(std::string("class:") + test::to_string(static_cast<test::CrashPoint>(i)));
    }
    return keys;
  }

  std::map<std::string, int> counts_;
};

// Counts the recovery path a restarted server reported on its stderr.
void count_recovery(const std::string& stderr_text, Coverage& coverage) {
  const std::string source = field(stderr_text, "source");
  if (source == "checkpoint") coverage.hit("path:checkpoint");
  if (source == "full_scan") coverage.hit("path:full_scan");
  const std::string torn = field(stderr_text, "torn_bytes_removed");
  if (!torn.empty() && torn != "0") coverage.hit("path:torn_tail");
}

// --- Failure files --------------------------------------------------------------------------

fs::path failure_dir(uint64_t seed) {
  return fs::path(LOGBLOCK_CRASH_FAILURE_DIR) / ("seed-" + std::to_string(seed));
}

void copy_if_exists(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (fs::exists(from, ec)) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

void write_text(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

// --- Kill iterations ------------------------------------------------------------------------

// The device after the first `count` mutations: block -> index of the mutation that wrote it.
std::map<uint64_t, uint64_t> model_after(const History& history, uint64_t count) {
  std::map<uint64_t, uint64_t> state;
  for (uint64_t k = 0; k < count; ++k) {
    if (history[k].trim) {
      state.erase(history[k].block);
    } else {
      state[history[k].block] = k;
    }
  }
  return state;
}

// Reads every block through libnbd. Returns false (with a message) on an I/O error.
bool read_device(nbd_handle* nbd, std::vector<std::vector<uint8_t>>* device) {
  device->assign(kBlocksUsed, std::vector<uint8_t>(kBlockSize));
  for (uint64_t b = 0; b < kBlocksUsed; ++b) {
    if (nbd_pread(nbd, (*device)[b].data(), kBlockSize, b * kBlockSize, 0) != 0) {
      return fail(std::string("read after restart failed: ") + nbd_get_error());
    }
  }
  return true;
}

// Compares what was read with the state after the first `count` mutations.
bool device_matches(const History& history, uint64_t count,
                    const std::vector<std::vector<uint8_t>>& device, std::string* why) {
  const auto expected = model_after(history, count);
  for (uint64_t b = 0; b < kBlocksUsed; ++b) {
    const std::vector<uint8_t>& got = device[b];
    const bool zero = std::all_of(got.begin(), got.end(), [](uint8_t x) { return x == 0; });
    const auto want = expected.find(b);
    if (want == expected.end()) {
      if (!zero) {
        *why = "block " + std::to_string(b) + " should read as zeros after " +
               std::to_string(count) + " requests, but holds data";
        return false;
      }
      continue;
    }
    const uint64_t k = want->second;
    if (got != payload_of(history[k].seed, k)) {
      *why = "block " + std::to_string(b) + " should hold request " + std::to_string(k) +
             " after " + std::to_string(count) + " requests, but does not";
      return false;
    }
  }
  return true;
}

// Sends one cycle's workload to the server, then kills it. Appends every request that was sent
// to `history`, and sets *flushed to how many requests a completed flush covers.
bool send_workload_and_kill(Harness& server, uint64_t cycle_seed, const Plan& plan,
                            History* history, uint64_t* flushed, ServerExit* killed) {
  NbdHandle nbd = server.connect();
  if (!nbd) return fail("could not connect");

  for (const Step& step : plan.sync_steps) {
    if (step.kind == Step::Kind::Flush) {
      if (nbd_flush(nbd.get(), 0) != 0)
        return fail(std::string("flush failed: ") + nbd_get_error());
      *flushed = history->size();
      continue;
    }
    const uint64_t k = history->size();
    const bool trim = step.kind == Step::Kind::Trim;
    history->push_back({trim, step.block, cycle_seed});
    int rc = 0;
    if (trim) {
      rc = nbd_trim(nbd.get(), kBlockSize, step.block * kBlockSize, 0);
    } else {
      const auto data = payload_of(cycle_seed, k);
      rc = nbd_pwrite(nbd.get(), data.data(), data.size(), step.block * kBlockSize, 0);
    }
    if (rc != 0) return fail(std::string("request failed: ") + nbd_get_error());
  }

  // Submit without waiting for replies, then kill at once (after 0 to 0.3 ms), so the kill
  // usually lands while the plugin is writing. These requests are never flushed.
  std::vector<std::vector<uint8_t>> keep_alive;  // libnbd reads these buffers later
  keep_alive.reserve(plan.in_flight.size());
  for (const Step& step : plan.in_flight) {
    const uint64_t k = history->size();
    const bool trim = step.kind == Step::Kind::Trim;
    history->push_back({trim, step.block, cycle_seed});
    int64_t rc = 0;
    if (trim) {
      rc = nbd_aio_trim(nbd.get(), kBlockSize, step.block * kBlockSize, NBD_NULL_COMPLETION, 0);
    } else {
      keep_alive.push_back(payload_of(cycle_seed, k));
      rc = nbd_aio_pwrite(nbd.get(), keep_alive.back().data(), kBlockSize, step.block * kBlockSize,
                          NBD_NULL_COMPLETION, 0);
    }
    if (rc == -1) return fail(std::string("aio request failed: ") + nbd_get_error());
  }
  std::this_thread::sleep_for(std::chrono::microseconds(plan.kill_delay_us));
  *killed = server.stop(SIGKILL);
  if (killed->exited || killed->term_signal != SIGKILL) {
    return fail("nbdkit was not ended by SIGKILL (it exited on its own?):\n" + killed->stderr_text);
  }
  return true;
}

// Checks the log file against the history: no corruption, record i is request i, and everything
// a completed flush covered is there. Returns the number of valid records through *records.
bool check_log(const std::string& log, const History& history, uint64_t flushed,
               uint64_t* records) {
  const std::vector<uint8_t> bytes = read_bytes(log);
  const ScanResult scan = scan_log(bytes);
  std::cerr << "  log " << bytes.size() << " bytes, " << scan.records.size() << " records, scan "
            << scan_stop_name(scan.stop) << ", flushed " << flushed << " of " << history.size()
            << " requests\n";
  if (scan.stop == ScanStop::Corrupt) return fail("scan reports corruption: " + scan.detail);
  if (scan.records.size() < flushed) {
    return fail("only " + std::to_string(scan.records.size()) + " records found, but " +
                std::to_string(flushed) + " were flushed before the kill");
  }
  if (scan.records.size() > history.size()) {
    return fail("the log holds " + std::to_string(scan.records.size()) + " records, but only " +
                std::to_string(history.size()) + " requests were sent");
  }
  for (std::size_t i = 0; i < scan.records.size(); ++i) {
    const ScannedRecord& rec = scan.records[i];
    const Mutation& want = history[i];
    const std::string where =
        "record " + std::to_string(i) + " at offset " + std::to_string(rec.offset) + ": ";
    if (rec.header.seq != i + 1) return fail(where + "seq is " + std::to_string(rec.header.seq));
    if (want.trim) {
      if (rec.header.type != RecordType::Trim || rec.header.block_no != want.block ||
          rec.header.block_count != 1) {
        return fail(where + "expected a TRIM of block " + std::to_string(want.block));
      }
      continue;
    }
    if (rec.header.type != RecordType::Data) return fail(where + "not a DATA record");
    if (rec.header.block_no != want.block) return fail(where + "wrong block number");
    const auto payload = payload_of(want.seed, i);
    const uint8_t* got = bytes.data() + rec.offset + kRecordHeaderSize;
    if (!std::equal(payload.begin(), payload.end(), got)) return fail(where + "wrong payload");
  }
  *records = scan.records.size();
  return true;
}

// Per-seed choices: even seeds are kill iterations, numbered 0, 1, 2, ... by seed / 2.
const char* interval_for(uint64_t seed) {
  return (seed / 2) % 2 == 0 ? kSmallInterval : kLargeInterval;
}
uint64_t cycles_for(uint64_t seed) { return (seed / 2) % 3 == 2 ? 3 : 1; }

bool run_kill_iteration(uint64_t seed, Coverage& coverage) {
  const fs::path keep = failure_dir(seed);
  std::error_code ec;
  fs::remove_all(keep, ec);  // an iteration clears its directory first
  fs::create_directories(keep, ec);

  const std::string interval = std::string("checkpoint_interval=") + interval_for(seed);
  const uint64_t cycles = cycles_for(seed);
  History history;
  uint64_t durable = 0;  // requests that are certainly in the log: flushed, or survived a restart

  // Each server gets its own Harness (its own socket directory). The first one's directory holds
  // the log for the whole iteration, so the harnesses are kept until the end.
  std::vector<std::unique_ptr<Harness>> servers;
  servers.push_back(std::make_unique<Harness>());
  const std::string log = servers.front()->log_path();
  const std::vector<std::string> settings = {"log=" + log, std::string("cap=") + kCap, interval};
  if (!servers.back()->start(settings)) {
    return fail("nbdkit did not start:\n" + servers.back()->stop().stderr_text);
  }

  for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
    const uint64_t cycle_seed = seed + cycle * 1000003;
    const Plan plan = plan_for(seed, cycle);
    uint64_t flushed = durable;
    ServerExit killed;
    if (!send_workload_and_kill(*servers.back(), cycle_seed, plan, &history, &flushed, &killed)) {
      return false;
    }
    if (cycle > 0) count_recovery(killed.stderr_text, coverage);

    // Keep what recovery is about to see, so a failure can be replayed from the files (the
    // checkpoint stays "<log>.ckpt" beside the log).
    copy_if_exists(log, keep / "log");
    copy_if_exists(log + ".ckpt", keep / "log.ckpt");
    write_text(keep / ("server-stderr-cycle" + std::to_string(cycle) + ".txt"), killed.stderr_text);

    uint64_t records = 0;
    if (!check_log(log, history, flushed, &records)) return false;

    // Restart on the same log.
    servers.push_back(std::make_unique<Harness>());
    Harness& restarted = *servers.back();
    if (!restarted.start(settings)) {
      return fail("restart on the log was refused:\n" + restarted.stop().stderr_text);
    }
    std::vector<std::vector<uint8_t>> device;
    {
      NbdHandle after = restarted.connect();
      if (!after) return fail("could not connect after the restart");
      if (!read_device(after.get(), &device)) return false;
    }
    // The device must be exactly the state after the R requests whose records survived. R is at
    // least everything a completed flush covered, which check_log() has already required.
    std::string why;
    if (!device_matches(history, records, device, &why)) return fail(why);

    // The requests after R did not survive. What survived was made durable by recovery's fsync
    // (D28), so the next cycle starts from it.
    history.resize(records);
    durable = records;
    if (cycle + 1 == cycles) {
      const ServerExit stopped = restarted.stop(SIGTERM);
      if (!stopped.clean()) {
        return fail("restarted server did not stop cleanly:\n" + stopped.stderr_text);
      }
      count_recovery(stopped.stderr_text, coverage);
      // A full scan replays every record; a checkpoint replays only the ones after it.
      const std::string source = field(stopped.stderr_text, "source");
      const std::string replayed = field(stopped.stderr_text, "records_replayed");
      if (source == "full_scan" && replayed != std::to_string(records)) {
        return fail("recovery line says records_replayed=" + replayed + ", but the log holds " +
                    std::to_string(records) + ":\n" + stopped.stderr_text);
      }
      if (source == "checkpoint" && std::stoull(replayed) > records) {
        return fail("recovery replayed more records than the log holds:\n" + stopped.stderr_text);
      }
    }
  }
  fs::remove_all(keep, ec);  // passed: nothing to keep
  return true;
}

// --- Power-cut iterations -------------------------------------------------------------------

bool run_power_cut_iteration(uint64_t seed, Coverage& coverage) {
  const fs::path keep = failure_dir(seed);
  std::error_code ec;
  fs::remove_all(keep, ec);

  const test::PowerCutResult result = test::power_cut_run(seed);
  if (!result.ok()) {
    fs::create_directories(keep, ec);
    write_text(keep / "failure.txt", result.failure + "\n");
    // The files as the power left them, before the failing recovery, under their usual names.
    write_text(keep / "log", result.pre_recovery_log);
    write_text(keep / "log.ckpt", result.pre_recovery_ckpt);
    return fail(result.failure);
  }
  coverage.hit(std::string("class:") + test::to_string(result.crash_point));
  coverage.hit(std::string("outcome:") + test::to_string(result.log_outcome));
  coverage.hit(std::string("outcome:") + test::to_string(result.ckpt_outcome));
  if (result.recovery_path == test::RecoveryPath::Checkpoint) coverage.hit("path:checkpoint");
  if (result.recovery_path == test::RecoveryPath::FullScan) coverage.hit("path:full_scan");
  if (result.torn_tail) coverage.hit("path:torn_tail");
  std::cerr << "  power cut at " << test::to_string(result.crash_point) << " (log "
            << test::to_string(result.log_outcome) << ", ckpt "
            << test::to_string(result.ckpt_outcome) << "), recovered by "
            << test::to_string(result.recovery_path) << (result.torn_tail ? " + torn tail" : "")
            << ", prefix " << result.matched_prefix << " of " << result.requests_issued
            << " requests, flushed " << result.flushed_prefix << "\n";
  return true;
}

// --- Self-checks that the test itself is repeatable -----------------------------------------

// FR-034, SC-010: one seed makes the same workload twice, and different seeds differ.
bool check_workload_is_deterministic() {
  for (uint64_t seed : {0ULL, 2ULL, 12ULL, 1234568ULL}) {
    for (uint64_t cycle = 0; cycle < 3; ++cycle) {
      if (!(plan_for(seed, cycle) == plan_for(seed, cycle))) {
        return fail("plan_for(" + std::to_string(seed) + ") differs between two calls");
      }
    }
  }
  if (plan_for(2, 0) == plan_for(4, 0)) return fail("two different seeds made the same workload");
  if (plan_for(2, 0) == plan_for(2, 1))
    return fail("two cycles of one seed made the same workload");
  return true;
}

// FR-034, SC-010: recovering twice from separate copies of one set of files gives the same
// report (apart from the time taken) and the same blocks.
bool check_recovery_is_repeatable() {
  const fs::path base = fs::path(LOGBLOCK_TEST_TMP_DIR) / "crash-repeat";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "orig", ec);
  fs::create_directories(base / "a", ec);
  fs::create_directories(base / "b", ec);

  {  // Build a log and a checkpoint file with a real engine, then end it without a shutdown.
    std::unique_ptr<Engine> engine;
    const Status s = Engine::create((base / "orig" / "log").string(), 64 << 20, make_posix_io(),
                                    &engine, 3 * kDataRecordSize);
    if (!s.ok()) return fail("cannot build the repeat-test files: " + s.message());
    std::vector<uint8_t> data(kBlockSize);
    for (uint64_t k = 0; k < 40; ++k) {
      std::fill(data.begin(), data.end(), static_cast<uint8_t>(k + 1));
      if (!engine->write((k * 7 % kBlocksUsed) * kBlockSize, kBlockSize, data.data()).ok() ||
          (k % 9 == 8 && !engine->trim((k % kBlocksUsed) * kBlockSize, kBlockSize).ok())) {
        return fail("cannot write the repeat-test files");
      }
    }
    if (!engine->flush().ok()) return fail("cannot flush the repeat-test files");
  }
  for (const char* dir : {"a", "b"}) {
    copy_if_exists(base / "orig" / "log", base / dir / "log");
    copy_if_exists(base / "orig" / "log.ckpt", base / dir / "log.ckpt");
  }

  struct Recovered {
    RecoveryReport report;
    std::vector<std::vector<uint8_t>> blocks;
  };
  Recovered results[2];
  int index = 0;
  for (const char* dir : {"a", "b"}) {
    std::unique_ptr<Engine> engine;
    const Status s = Engine::create((base / dir / "log").string(), 64 << 20, make_posix_io(),
                                    &engine, 3 * kDataRecordSize);
    if (!s.ok()) return fail(std::string("recovery from copy ") + dir + " failed: " + s.message());
    results[index].report = engine->recovery_report();
    results[index].report.time_us = 0;  // the only field allowed to differ
    results[index].blocks.assign(kBlocksUsed, std::vector<uint8_t>(kBlockSize));
    for (uint64_t b = 0; b < kBlocksUsed; ++b) {
      if (!engine->read(b * kBlockSize, kBlockSize, results[index].blocks[b].data()).ok()) {
        return fail("read failed in the repeat test");
      }
    }
    ++index;
  }
  const RecoveryReport& x = results[0].report;
  const RecoveryReport& y = results[1].report;
  if (x.source != y.source || x.slot != y.slot || x.generation != y.generation ||
      x.records_replayed != y.records_replayed || x.log_bytes_scanned != y.log_bytes_scanned ||
      x.torn_bytes_removed != y.torn_bytes_removed ||
      x.unreadable_checkpoints != y.unreadable_checkpoints || x.warnings != y.warnings) {
    return fail("two recoveries from copies of the same files gave different reports");
  }
  if (results[0].blocks != results[1].blocks) {
    return fail("two recoveries from copies of the same files gave different blocks");
  }
  if (x.source != RecoverySource::Checkpoint) {
    return fail("the repeat test expected recovery from a checkpoint");
  }
  fs::remove_all(base, ec);
  return true;
}

}  // namespace
}  // namespace logblock::itest

int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape): a throw aborts the run, which is a failure
  using namespace logblock::itest;
  if (argc > 3) {
    std::cerr << "usage: crash_test [iterations] [seed]\n";
    return 2;
  }
  const int iterations = argc > 1 ? static_cast<int>(std::strtol(argv[1], nullptr, 10)) : 200;
  // The seed is printed so a failing run can be repeated exactly.
  const uint64_t base_seed =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : std::random_device{}() & 0x7FFFFFFFU;
  std::cout << "crash_test: " << iterations << " iterations, seed " << base_seed << "\n";

  if (!check_workload_is_deterministic() || !check_recovery_is_repeatable()) {
    std::cerr << "crash_test FAILED its own repeatability checks\n";
    return 1;
  }

  Coverage coverage;
  for (int i = 0; i < iterations; ++i) {
    // Each iteration has its own seed, so one failure reproduces alone with
    // `crash_test 1 <iteration seed>`. Even seeds kill a server, odd seeds cut simulated power.
    const uint64_t seed = base_seed + static_cast<uint64_t>(i);
    const bool kill = seed % 2 == 0;
    std::cout << "iteration " << i + 1 << "/" << iterations << " (seed " << seed
              << (kill ? ", kill" : ", power cut") << ")\n";
    const bool passed =
        kill ? run_kill_iteration(seed, coverage) : run_power_cut_iteration(seed, coverage);
    if (!passed || ::testing::Test::HasFailure()) {
      std::cerr << "crash_test FAILED at iteration " << i + 1 << "; reproduce with: crash_test 1 "
                << seed << "\nkept files: " << failure_dir(seed).string() << "\n";
      return 1;
    }
  }
  // A single iteration cannot reach every path, so only a real run is held to it.
  if (!coverage.report(iterations > 1)) {
    std::cerr << "crash_test FAILED: some paths were never reached\n";
    return 1;
  }
  std::cout << "crash_test: all " << iterations << " iterations passed\n";
  return 0;
}
