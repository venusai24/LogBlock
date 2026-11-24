// The exclusive lock on the log, through real nbdkit processes (US6, FR-030, D20).
//
// Two servers on one log would each append at their own idea of the end and
// destroy each other's data, so the second must refuse. A "second server" is a
// second Harness whose nbdkit is pointed at the FIRST harness's log file (each
// Harness owns its own socket directory). The first Harness is declared first, so
// it is destroyed last and its directory (which holds the log) outlives the second.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "harness.h"
#include "logblock/engine.h"

namespace logblock::itest {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr const char* kCap = "1M";

// The message the engine gives when the lock is held; it must name the log.
constexpr const char* kLockMessage = "in use by another server";
// When two servers start on a missing log, the loser may instead lose the
// exclusive create and say the log appeared during startup.
constexpr const char* kCreateRaceMessage = "appeared during startup";

std::vector<std::string> settings_for(const std::string& log_path) {
  return {"log=" + log_path, std::string("cap=") + kCap};
}

Bytes block_pattern(uint8_t fill) { return Bytes(kBlockSize, fill); }  // NOLINT(modernize-return-braced-init-list): braces would build a 2-element vector

void write_block(nbd_handle* nbd, uint64_t block, const Bytes& data) {
  ASSERT_EQ(nbd_pwrite(nbd, data.data(), data.size(), block * kBlockSize, 0), 0) << nbd_get_error();
  ASSERT_EQ(nbd_flush(nbd, 0), 0) << nbd_get_error();
}

Bytes read_block(nbd_handle* nbd, uint64_t block) {
  Bytes data(kBlockSize);
  if (nbd_pread(nbd, data.data(), data.size(), block * kBlockSize, 0) != 0) {
    ADD_FAILURE() << nbd_get_error();
    return {};
  }
  return data;
}

// A refused server must not have started serving, must have exited with a
// non-zero status, and must have named the log and the lock in its message.
void expect_lock_refusal(Harness& loser, const std::string& log_path, bool may_lose_create_race = false) {
  const ServerExit result = loser.stop();
  EXPECT_TRUE(result.exited) << "killed by signal " << result.term_signal;
  EXPECT_NE(result.exit_code, 0) << "stderr:\n" << result.stderr_text;
  const bool lock_message = result.stderr_text.find(kLockMessage) != std::string::npos;
  const bool race_message =
      may_lose_create_race && result.stderr_text.find(kCreateRaceMessage) != std::string::npos;
  EXPECT_TRUE(lock_message || race_message)
      << "the message should say the log is in use (or appeared during startup, where allowed); stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find(log_path), std::string::npos)
      << "the message should name the log; stderr:\n"
      << result.stderr_text;
}

TEST(Lock, SecondServerIsRefusedAtOnceAndTheFirstKeepsServing) {
  Harness first;
  ASSERT_TRUE(first.start(settings_for(first.log_path()))) << first.stop().stderr_text;
  NbdHandle nbd = first.connect();
  ASSERT_TRUE(nbd);
  write_block(nbd.get(), 3, block_pattern(0x5a));
  const std::string before = sha256_file(first.log_path());
  ASSERT_EQ(before.size(), 64u) << "sha256sum failed";

  Harness second;
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(second.start(settings_for(first.log_path())));
  // "At once": the refusal must not wait for the first server (the lock is not
  // taken with blocking). The harness itself would wait 10 s for a socket.
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
  expect_lock_refusal(second, first.log_path());

  // The refusal changed nothing, and the first server still answers.
  EXPECT_EQ(sha256_file(first.log_path()), before);
  EXPECT_EQ(read_block(nbd.get(), 3), block_pattern(0x5a));
  write_block(nbd.get(), 4, block_pattern(0x6b));
  nbd.reset();
  EXPECT_TRUE(first.stop(SIGTERM).clean());
}

TEST(Lock, RestartRightAfterSigkillWorks) {
  // The operating system releases the lock when the server dies, however it dies.
  Harness first;
  ASSERT_TRUE(first.start(settings_for(first.log_path()))) << first.stop().stderr_text;
  {
    NbdHandle nbd = first.connect();
    ASSERT_TRUE(nbd);
    write_block(nbd.get(), 7, block_pattern(0x33));
    const ServerExit ended = first.stop(SIGKILL);  // the client is still connected
    EXPECT_FALSE(ended.exited);
    EXPECT_EQ(ended.term_signal, SIGKILL);
  }

  Harness second;
  ASSERT_TRUE(second.start(settings_for(first.log_path()))) << second.stop().stderr_text;
  NbdHandle nbd = second.connect();
  ASSERT_TRUE(nbd);
  EXPECT_EQ(read_block(nbd.get(), 7), block_pattern(0x33));
  nbd.reset();
  EXPECT_TRUE(second.stop(SIGTERM).clean());
}

TEST(Lock, TwoServersStartedAtOnceOnAMissingLogEndWithOneRunningAndOneRefused) {
  // Both servers are launched before either is waited for, so their startups
  // overlap. Which one wins is up to the scheduler; the outcome must always be
  // one serving and one refused for the lock. Repeated so a bad race is likely to
  // show. FR-009: the loser may have created the (empty) log the winner uses.
  constexpr int kRounds = 15;
  int a_won = 0;
  for (int round = 0; round < kRounds; ++round) {
    Harness a;
    Harness b;
    ASSERT_FALSE(fs::exists(a.log_path()));
    const std::vector<std::string> settings = settings_for(a.log_path());
    ASSERT_TRUE(a.launch(settings));
    ASSERT_TRUE(b.launch(settings));

    const bool a_serving = a.wait_until_serving();
    const bool b_serving = b.wait_until_serving();
    ASSERT_NE(a_serving, b_serving)
        << "round " << round << ": exactly one server must run\n--- a ---\n"
        << a.stop(SIGKILL).stderr_text << "\n--- b ---\n"
        << b.stop(SIGKILL).stderr_text;
    Harness& winner = a_serving ? a : b;
    Harness& loser = a_serving ? b : a;
    a_won += a_serving ? 1 : 0;

    expect_lock_refusal(loser, a.log_path(), true);
    // The winner works: write, read back, clean shutdown.
    {
      NbdHandle nbd = winner.connect();
      ASSERT_TRUE(nbd);
      write_block(nbd.get(), round, block_pattern(static_cast<uint8_t>(round + 1)));
      EXPECT_EQ(read_block(nbd.get(), round), block_pattern(static_cast<uint8_t>(round + 1)));
    }
    EXPECT_TRUE(winner.stop(SIGTERM).clean());
  }
  RecordProperty("a_won", a_won);  // shows in the XML output how the races went
}

TEST(Lock, ARelativeLogPathStillFindsItsCheckpointFileAfterNbdkitForks) {
  // A daemon nbdkit changes directory to "/" after get_ready. The log's fd is already open, but
  // the checkpoint file is created later by path, so a relative log= must have been made
  // absolute at startup or the first checkpoint lands in the wrong directory (or fails).
  Harness daemon;
  const std::string relative =
      std::filesystem::relative(daemon.log_path(), std::filesystem::current_path()).string();
  ASSERT_NE(relative.front(), '/') << "the test needs a relative path";
  std::vector<std::string> settings = {"log=" + relative, "cap=4M", "checkpoint_interval=8272"};
  ASSERT_TRUE(daemon.start_background(settings)) << read_file(daemon.dir() + "/stderr.txt");
  {
    NbdHandle nbd = daemon.connect();
    ASSERT_TRUE(nbd);
    for (uint64_t b = 0; b < 4; ++b) {  // checkpoints after records 2 and 4
      write_block(nbd.get(), b, block_pattern(static_cast<uint8_t>(b + 1)));
    }
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
  }
  EXPECT_TRUE(daemon.stop_background());
  EXPECT_TRUE(std::filesystem::exists(daemon.log_path() + ".ckpt"))
      << "the checkpoint file should sit beside the log";
  EXPECT_GT(std::filesystem::file_size(daemon.log_path() + ".ckpt"), 0U);
}

TEST(Lock, BackgroundServerKeepsTheLockAfterNbdkitForks) {
  // Without -f nbdkit forks after get_ready and the foreground process exits. The
  // lock belongs to the open file description, which the child inherits, so it must
  // still be held by the daemon (research R4).
  Harness daemon;
  ASSERT_TRUE(daemon.start_background(settings_for(daemon.log_path())))
      << read_file(daemon.dir() + "/stderr.txt");
  ASSERT_GT(daemon.background_pid(), 0);
  // The process that was started has exited; the daemon is a different process.
  EXPECT_NE(daemon.background_pid(), daemon.launched_pid());

  Harness second;
  EXPECT_FALSE(second.start(settings_for(daemon.log_path())));
  expect_lock_refusal(second, daemon.log_path());

  // The daemon serves, too.
  {
    NbdHandle nbd = daemon.connect();
    ASSERT_TRUE(nbd);
    write_block(nbd.get(), 1, block_pattern(0x77));
    EXPECT_EQ(read_block(nbd.get(), 1), block_pattern(0x77));
  }
  EXPECT_TRUE(daemon.stop_background());

  // After the daemon is gone the lock is free again.
  Harness third;
  ASSERT_TRUE(third.start(settings_for(daemon.log_path()))) << third.stop().stderr_text;
  NbdHandle nbd = third.connect();
  ASSERT_TRUE(nbd);
  EXPECT_EQ(read_block(nbd.get(), 1), block_pattern(0x77));
  nbd.reset();
  EXPECT_TRUE(third.stop(SIGTERM).clean());
}

}  // namespace
}  // namespace logblock::itest
