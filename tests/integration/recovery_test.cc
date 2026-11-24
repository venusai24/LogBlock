// Recovery through the real plugin (US1, FR-001, FR-004, FR-005, FR-023).
//
// A "restart" is a second Harness whose nbdkit is pointed at the FIRST harness's
// log file. Each Harness can start only one server and owns its own socket
// directory, so the second one is a fresh server process on the old log. The
// first Harness is declared first, so it is destroyed last and its directory
// (which holds the log) outlives the second server.
//
// The recovery line is documented in specs/002-crash-recovery/contracts/plugin-cli.md:
//   [info] recovery log=<path> source=checkpoint|full_scan|none slot=A|B|-
//          generation=<n> records_replayed=<n> log_bytes_scanned=<n>
//          torn_bytes_removed=<n> unreadable_checkpoints=<n> time_us=<n>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "harness.h"
#include "logblock/engine.h"

namespace logblock::itest {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr const char* kCap = "4M";  // room for ~1000 records; the tests write far fewer

// A block's contents that differ per block and per "version", so a block served
// from the wrong place or from a stale version is caught.
Bytes pattern(uint64_t block, uint8_t version) {
  Bytes data(kBlockSize);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(block * 37 + size_t{version} * 101 + i * 3 + 1);  // never all zero
  }
  return data;
}

void write_block(nbd_handle* nbd, uint64_t block, const Bytes& data) {
  ASSERT_EQ(nbd_pwrite(nbd, data.data(), data.size(), block * kBlockSize, 0), 0) << nbd_get_error();
}

// Fails the test (and returns empty) if the read fails.
Bytes read_block(nbd_handle* nbd, uint64_t block) {
  Bytes data(kBlockSize);
  if (nbd_pread(nbd, data.data(), data.size(), block * kBlockSize, 0) != 0) {
    ADD_FAILURE() << nbd_get_error();
    return {};
  }
  return data;
}

// A first run: write `blocks` blocks (version 1), flush, then end the server with
// `sig` while the client is still connected for SIGKILL, closed for SIGTERM.
void run_and_end(Harness& first, uint64_t blocks, int sig) {
  ASSERT_TRUE(first.start(first.default_settings(kCap))) << first.stop().stderr_text;
  NbdHandle nbd = first.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t b = 0; b < blocks; ++b) {
    write_block(nbd.get(), b, pattern(b, 1));
  }
  ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
  if (sig == SIGTERM) nbd.reset();  // nbdkit has no open connection to wait for
  const ServerExit ended = first.stop(sig);
  if (sig == SIGKILL) {
    EXPECT_FALSE(ended.exited);
    EXPECT_EQ(ended.term_signal, SIGKILL);
  } else {
    EXPECT_TRUE(ended.clean()) << ended.stderr_text;
  }
}

// Starts a server on the log of `first`. Returns false (and records a failure) if
// it did not start.
bool restart_on(Harness& first, Harness& second) {
  const bool serving = second.start({"log=" + first.log_path(), std::string("cap=") + kCap});
  if (!serving) ADD_FAILURE() << "restart refused:\n" << second.stop().stderr_text;
  return serving;
}

// The fields of the recovery line, as strings.
using RecoveryLine = std::map<std::string, std::string>;

std::optional<RecoveryLine> parse_recovery_line(const std::string& stderr_text) {
  std::size_t start = 0;
  while (start < stderr_text.size()) {
    std::size_t end = stderr_text.find('\n', start);
    if (end == std::string::npos) end = stderr_text.size();
    const std::string line = stderr_text.substr(start, end - start);
    start = end + 1;
    if (line.find("recovery log=") == std::string::npos) continue;

    RecoveryLine fields;
    const std::regex pair("([a-z_]+)=(\\S+)");
    for (std::sregex_iterator it(line.begin(), line.end(), pair), done; it != done; ++it) {
      fields[(*it)[1].str()] = (*it)[2].str();
    }
    // All ten fields of the contract must be there.
    for (const char* key :
         {"log", "source", "slot", "generation", "records_replayed", "log_bytes_scanned",
          "torn_bytes_removed", "unreadable_checkpoints", "time_us"}) {
      if (fields.count(key) == 0) return std::nullopt;
    }
    return fields;
  }
  return std::nullopt;
}

// --- US1 scenarios 1, 5, 4, 2 -------------------------------------------------

TEST(Recovery, FlushedWritesSurviveSigkill) {
  Harness first;
  run_and_end(first, 10, SIGKILL);

  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  NbdHandle nbd = second.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t b = 0; b < 10; ++b) {
    EXPECT_EQ(read_block(nbd.get(), b), pattern(b, 1)) << "block " << b;
  }
  EXPECT_EQ(read_block(nbd.get(), 500), Bytes(kBlockSize, 0)) << "a block never written";
}

TEST(Recovery, DataSurvivesCleanShutdown) {
  Harness first;
  run_and_end(first, 10, SIGTERM);

  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  NbdHandle nbd = second.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t b = 0; b < 10; ++b) {
    EXPECT_EQ(read_block(nbd.get(), b), pattern(b, 1)) << "block " << b;
  }
}

TEST(Recovery, DataWrittenAfterARestartSurvivesTheNextKill) {
  Harness first;
  run_and_end(first, 5, SIGKILL);

  // Second run: add blocks 5..9 and overwrite block 2, flush, kill again.
  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  {
    NbdHandle nbd = second.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 5; b < 10; ++b) write_block(nbd.get(), b, pattern(b, 1));
    write_block(nbd.get(), 2, pattern(2, 2));
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
    EXPECT_EQ(second.stop(SIGKILL).term_signal, SIGKILL);
  }

  // Third run sees both generations of data, with the newer block 2.
  Harness third;
  ASSERT_TRUE(restart_on(first, third));
  NbdHandle nbd = third.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t b = 0; b < 10; ++b) {
    const uint8_t version = b == 2 ? 2 : 1;
    EXPECT_EQ(read_block(nbd.get(), b), pattern(b, version)) << "block " << b;
  }
}

TEST(Recovery, TrimmedBlockStaysZeroAfterRestart) {
  Harness first;
  ASSERT_TRUE(first.start(first.default_settings(kCap))) << first.stop().stderr_text;
  {
    NbdHandle nbd = first.connect();
    ASSERT_TRUE(nbd);
    write_block(nbd.get(), 3, pattern(3, 1));
    write_block(nbd.get(), 4, pattern(4, 1));
    ASSERT_EQ(nbd_trim(nbd.get(), kBlockSize, 3 * kBlockSize, 0), 0) << nbd_get_error();
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
    first.stop(SIGKILL);
  }

  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  NbdHandle nbd = second.connect();
  ASSERT_TRUE(nbd);
  EXPECT_EQ(read_block(nbd.get(), 3), Bytes(kBlockSize, 0));
  EXPECT_EQ(read_block(nbd.get(), 4), pattern(4, 1));
}

// --- FR-004 -------------------------------------------------------------------

// "No request is served until recovery has finished." Recovery runs in the
// plugin's get_ready, and nbdkit creates its socket only after get_ready returns.
// A timing test ("connect early and fail") would be flaky, so this test checks the
// two facts that make early service impossible instead:
//  1. The moment the socket file exists (which is what Harness::start waits for),
//     the recovery line is already on stderr. So recovery had finished by then.
//  2. The very first connection already reads the recovered data, with no window
//     in which blocks look empty.
TEST(Recovery, NothingIsServedBeforeRecoveryHasFinished) {
  Harness first;
  run_and_end(first, 200, SIGKILL);  // 200 records: enough for a real scan

  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  const std::string stderr_so_far = read_file(second.dir() + "/stderr.txt");
  const auto line = parse_recovery_line(stderr_so_far);
  ASSERT_TRUE(line.has_value()) << "no recovery line yet although the socket exists; stderr:\n"
                                << stderr_so_far;
  EXPECT_EQ(line->at("records_replayed"), "200");

  NbdHandle nbd = second.connect();
  ASSERT_TRUE(nbd);
  EXPECT_EQ(read_block(nbd.get(), 0), pattern(0, 1));
  EXPECT_EQ(read_block(nbd.get(), 199), pattern(199, 1));
}

// --- FR-005, FR-023 -----------------------------------------------------------

TEST(Recovery, NewDeviceReportsNoRecovery) {
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings(kCap))) << harness.stop().stderr_text;
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;

  const auto line = parse_recovery_line(result.stderr_text);
  ASSERT_TRUE(line.has_value()) << "stderr:\n" << result.stderr_text;
  EXPECT_EQ(line->at("log"), harness.log_path());
  EXPECT_EQ(line->at("source"), "none");
  EXPECT_EQ(line->at("slot"), "-");
  EXPECT_EQ(line->at("records_replayed"), "0");
  EXPECT_EQ(line->at("log_bytes_scanned"), "0");
  EXPECT_EQ(line->at("torn_bytes_removed"), "0");
  EXPECT_EQ(line->at("unreadable_checkpoints"), "0");
}

TEST(Recovery, RestartReportsAFullScanWithItsCounts) {
  Harness first;
  run_and_end(first, 7, SIGKILL);

  Harness second;
  ASSERT_TRUE(restart_on(first, second));
  const ServerExit result = second.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;

  const auto line = parse_recovery_line(result.stderr_text);
  ASSERT_TRUE(line.has_value()) << "stderr:\n" << result.stderr_text;
  EXPECT_EQ(line->at("log"), first.log_path());
  EXPECT_EQ(line->at("source"), "full_scan");
  EXPECT_EQ(line->at("slot"), "-");
  EXPECT_EQ(line->at("records_replayed"), "7");
  EXPECT_EQ(line->at("log_bytes_scanned"), std::to_string(7 * kDataRecordSize));
  EXPECT_EQ(line->at("torn_bytes_removed"), "0");
  EXPECT_EQ(line->at("unreadable_checkpoints"), "0");
  // No warnings for a healthy log.
  EXPECT_EQ(result.stderr_text.find("[warning]"), std::string::npos) << result.stderr_text;
}

// --- US3: starting from a checkpoint -------------------------------------------------

TEST(Recovery, ARestartAfterSeveralCheckpointsStartsFromTheNewestAndReadsEverythingBack) {
  // An interval of two DATA records: 19 one-block writes checkpoint after records 2, 4, ... 18,
  // so the newest is generation 9 (in slot A) and one record is left to replay.
  constexpr const char* kInterval = "checkpoint_interval=8272";
  Harness first;
  ASSERT_TRUE(first.start({"log=" + first.log_path(), std::string("cap=") + kCap, kInterval}))
      << first.stop().stderr_text;
  {
    NbdHandle nbd = first.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 0; b < 19; ++b) {
      write_block(nbd.get(), b, pattern(b, 1));
    }
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
  }
  const ServerExit killed = first.stop(SIGKILL);
  EXPECT_FALSE(killed.exited);

  Harness second;
  ASSERT_TRUE(second.start({"log=" + first.log_path(), std::string("cap=") + kCap, kInterval}))
      << second.stop().stderr_text;
  {
    NbdHandle nbd = second.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 0; b < 19; ++b) {
      EXPECT_EQ(read_block(nbd.get(), b), pattern(b, 1)) << "block " << b;
    }
  }
  const ServerExit result = second.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
  const auto line = parse_recovery_line(result.stderr_text);
  ASSERT_TRUE(line.has_value()) << "stderr:\n" << result.stderr_text;
  EXPECT_EQ(line->at("source"), "checkpoint");
  EXPECT_EQ(line->at("slot"), "A");
  EXPECT_EQ(line->at("generation"), "9");
  EXPECT_EQ(line->at("records_replayed"), "1");
  EXPECT_EQ(line->at("log_bytes_scanned"), std::to_string(kDataRecordSize));
  EXPECT_EQ(line->at("unreadable_checkpoints"), "0");
  EXPECT_EQ(result.stderr_text.find("[warning]"), std::string::npos) << result.stderr_text;
}

TEST(Recovery, ADamagedNewestSlotWarnsAndTheOlderOneIsUsed) {
  constexpr const char* kInterval = "checkpoint_interval=8272";
  Harness first;
  ASSERT_TRUE(first.start({"log=" + first.log_path(), std::string("cap=") + kCap, kInterval}))
      << first.stop().stderr_text;
  {
    NbdHandle nbd = first.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 0; b < 8; ++b) {  // checkpoints after records 2, 4, 6, 8: generation 4 is B
      write_block(nbd.get(), b, pattern(b, 1));
    }
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
  }
  EXPECT_FALSE(first.stop(SIGKILL).exited);

  // Flip one byte of slot B's map: its CRC no longer matches.
  {
    std::fstream ckpt(first.log_path() + ".ckpt", std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(ckpt.good());
    constexpr std::streamoff kSlotSize = 48 + 8 * 262144;
    ckpt.seekp(kSlotSize + 48 + 100);
    ckpt.put('\x5A');
  }

  Harness second;
  ASSERT_TRUE(second.start({"log=" + first.log_path(), std::string("cap=") + kCap, kInterval}))
      << second.stop().stderr_text;
  {
    NbdHandle nbd = second.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 0; b < 8; ++b) {
      EXPECT_EQ(read_block(nbd.get(), b), pattern(b, 1)) << "block " << b;
    }
  }
  const ServerExit result = second.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
  const auto line = parse_recovery_line(result.stderr_text);
  ASSERT_TRUE(line.has_value()) << "stderr:\n" << result.stderr_text;
  EXPECT_EQ(line->at("source"), "checkpoint");
  EXPECT_EQ(line->at("slot"), "A");
  EXPECT_EQ(line->at("generation"), "3");
  EXPECT_EQ(line->at("records_replayed"), "2");
  EXPECT_EQ(line->at("unreadable_checkpoints"), "1");
  EXPECT_NE(result.stderr_text.find("[warning] checkpoint slot=B file=" + first.log_path() +
                                    ".ckpt unreadable:"),
            std::string::npos)
      << result.stderr_text;
}

}  // namespace
}  // namespace logblock::itest
