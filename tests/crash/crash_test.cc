// Crash test. Usage: crash_test [iterations] [seed]
//
// Each iteration starts a real nbdkit with the logblock plugin, writes known
// 4 KiB blocks with periodic flushes, then SIGKILLs nbdkit while more writes
// are in flight. It then checks, from the log file alone:
//   (a) a restart on the non-empty log is refused,
//   (b) the refused restart left the log byte-for-byte unchanged (sha256),
//   (c) scan_log reports Clean or a torn tail (never corruption), and every
//       record acknowledged by a completed flush is present with the expected
//       seq, block and payload.
// It does not test power loss: SIGKILL leaves the page cache intact.

#include <gtest/gtest.h>
#include <libnbd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "log_scan.h"
#include "logblock/engine.h"

namespace logblock::itest {
namespace {

constexpr uint64_t kBlocksUsed = 1000;  // writes go to blocks 0..999 (repeats are fine)
constexpr const char* kCap = "64M";     // ~15,000 records; each iteration writes far fewer

// What write number `k` (0-based) of an iteration writes. Both the writer and
// the checker call these, so they cannot disagree. The seed makes iterations
// differ; the payload depends on k, so a record in the wrong place is caught.
uint64_t block_of(uint64_t seed, uint64_t k) { return (seed * 31 + k * 17) % kBlocksUsed; }

std::vector<uint8_t> payload_of(uint64_t seed, uint64_t k) {
  std::vector<uint8_t> data(kBlockSize);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(seed + k * 131 + i * 7);
  }
  return data;
}

// Whole file as bytes (the scanner takes a span of bytes).
std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Prints and returns false, so the caller can stop at the first problem.
bool fail(const std::string& message) {
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

// One iteration. Returns true if every check passed.
bool run_iteration(uint64_t seed) {
  std::mt19937_64 rng(seed);
  Harness harness;
  if (!harness.start(harness.default_settings(kCap))) return fail("nbdkit did not start");
  NbdHandle nbd = harness.connect();
  if (!nbd) return fail("could not connect");

  // Phase 1: synchronous writes with a flush every 1 to 8 writes, plus a final
  // flush. `flushed` counts writes covered by a flush that returned success.
  const uint64_t total = 10 + rng() % 190;
  uint64_t flushed = 0;
  uint64_t next_flush_at = 1 + rng() % 8;
  for (uint64_t k = 0; k < total; ++k) {
    const auto data = payload_of(seed, k);
    if (nbd_pwrite(nbd.get(), data.data(), data.size(), block_of(seed, k) * kBlockSize, 0) != 0) {
      return fail(std::string("write failed: ") + nbd_get_error());
    }
    if (k + 1 == next_flush_at || k + 1 == total) {
      if (nbd_flush(nbd.get(), 0) != 0)
        return fail(std::string("flush failed: ") + nbd_get_error());
      flushed = k + 1;
      next_flush_at = k + 1 + 1 + rng() % 8;
    }
  }

  // Phase 2: submit up to 512 writes without waiting for replies, then kill at once
  // (after 0 to 0.3 ms), so the kill usually lands while the plugin is writing.
  // These writes are never flushed, so they may or may not be in the log.
  const uint64_t in_flight = 1 + rng() % 512;
  std::vector<std::vector<uint8_t>> keep_alive;  // libnbd reads these buffers later
  for (uint64_t k = total; k < total + in_flight; ++k) {
    keep_alive.push_back(payload_of(seed, k));
    if (nbd_aio_pwrite(nbd.get(), keep_alive.back().data(), kBlockSize,
                       block_of(seed, k) * kBlockSize, NBD_NULL_COMPLETION, 0) == -1) {
      return fail(std::string("aio write failed: ") + nbd_get_error());
    }
  }
  std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
  const ServerExit killed = harness.stop(SIGKILL);
  if (killed.exited || killed.term_signal != SIGKILL) {
    return fail("nbdkit was not ended by SIGKILL (it exited on its own?):\n" + killed.stderr_text);
  }
  nbd.reset();

  // (c) Scan the log. Records may have been written beyond `flushed`; those
  // are checked too, but only the first `flushed` are required.
  const std::string log = harness.log_path();
  const std::vector<uint8_t> bytes = read_bytes(log);
  const ScanResult scan = scan_log(bytes);
  std::cerr << "  killed after " << flushed << " flushed + " << in_flight << " in flight; log "
            << bytes.size() << " bytes, " << scan.records.size() << " records, scan "
            << scan_stop_name(scan.stop) << "\n";
  if (scan.stop == ScanStop::Corrupt) return fail("scan reports corruption: " + scan.detail);
  if (scan.records.size() < flushed) {
    return fail("only " + std::to_string(scan.records.size()) + " records found, but " +
                std::to_string(flushed) + " were flushed before the kill");
  }
  for (std::size_t i = 0; i < scan.records.size(); ++i) {
    const ScannedRecord& rec = scan.records[i];
    const std::string where =
        "record " + std::to_string(i) + " at offset " + std::to_string(rec.offset) + ": ";
    if (rec.header.type != RecordType::Data) return fail(where + "not a DATA record");
    if (rec.header.seq != i + 1) return fail(where + "seq is " + std::to_string(rec.header.seq));
    if (rec.header.block_no != block_of(seed, i)) return fail(where + "wrong block number");
    const auto want = payload_of(seed, i);
    const uint8_t* got = bytes.data() + rec.offset + kRecordHeaderSize;
    if (!std::equal(want.begin(), want.end(), got)) return fail(where + "wrong payload");
  }

  // (a) and (b): restart on this non-empty log must be refused, and must not
  // change a byte of it.
  const std::string before = sha256_file(log);
  if (before.empty()) return fail("sha256 of the log failed");
  // A Harness can start only one server, so a second Harness (its own socket
  // directory) is pointed at the first one's log file through the settings.
  Harness restart;
  if (restart.start({"log=" + log, std::string("cap=") + kCap})) {
    return fail("restart on a non-empty log was accepted");
  }
  const ServerExit refused = restart.stop();
  if (!refused.exited || refused.exit_code == 0) {
    return fail("refused restart did not exit with a non-zero status:\n" + refused.stderr_text);
  }
  if (refused.stderr_text.find(log) == std::string::npos) {
    return fail("refusal message does not name the log:\n" + refused.stderr_text);
  }
  if (sha256_file(log) != before) return fail("the refused restart changed the log");
  return true;
}

}  // namespace
}  // namespace logblock::itest

int main(int argc, char** argv) {
  using namespace logblock::itest;
  if (argc > 3) {
    std::cerr << "usage: crash_test [iterations] [seed]\n";
    return 2;
  }
  const int iterations = argc > 1 ? static_cast<int>(std::strtol(argv[1], nullptr, 10)) : 20;
  // The seed is printed so a failing run can be repeated exactly.
  const uint64_t base_seed =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : std::random_device{}();
  std::cout << "crash_test: " << iterations << " iterations, seed " << base_seed << "\n";
  for (int i = 0; i < iterations; ++i) {
    // Each iteration has its own seed, so one failure reproduces alone with
    // `crash_test 1 <iteration seed>`.
    const uint64_t seed = base_seed + static_cast<uint64_t>(i);
    std::cout << "iteration " << i + 1 << "/" << iterations << " (seed " << seed << ")\n";
    if (!run_iteration(seed) || ::testing::Test::HasFailure()) {
      std::cerr << "crash_test FAILED at iteration " << i + 1 << "; reproduce with: crash_test 1 "
                << seed << "\n";
      return 1;
    }
  }
  std::cout << "crash_test: all " << iterations << " iterations passed\n";
  return 0;
}
