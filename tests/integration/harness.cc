#include "harness.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

// Both come from tests/integration/CMakeLists.txt.
#ifndef LOGBLOCK_TEST_TMP_DIR
#error "LOGBLOCK_TEST_TMP_DIR must be defined by CMake"
#endif
#ifndef LOGBLOCK_PLUGIN_PATH
#error "LOGBLOCK_PLUGIN_PATH must be defined by CMake"
#endif

namespace logblock::itest {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// How long we wait for nbdkit to start serving, and to stop after a signal.
// Generous, because a loaded CI machine can be slow; ctest's own TIMEOUT is the
// final guard against a hang.
constexpr milliseconds kStartTimeout{10000};
constexpr milliseconds kStopTimeout{5000};
constexpr milliseconds kPollInterval{10};

// sizeof(sockaddr_un::sun_path) is 108 on Linux, and the last byte is the NUL.
constexpr std::size_t kMaxSocketPath = sizeof(sockaddr_un::sun_path) - 1;  // 107

bool is_socket(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

}  // namespace

std::string Harness::default_base_dir() { return LOGBLOCK_TEST_TMP_DIR; }

std::string Harness::plugin_path() { return LOGBLOCK_PLUGIN_PATH; }

Harness::Harness(const std::string& base_dir) {
  fs::create_directories(base_dir);
  // mkdtemp picks a unique name and creates the directory with mode 0700, so
  // nobody else can reach the socket. Parallel tests cannot collide.
  std::string pattern = base_dir + "/it-XXXXXX";
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw std::runtime_error("mkdtemp failed for " + pattern + ": " + std::strerror(errno));
  }
  dir_ = pattern;

  // A Unix socket path longer than sun_path is silently unusable, so fail now
  // with a message that says what to change.
  const std::string sock = socket_path();
  if (sock.size() > kMaxSocketPath) {
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
    throw std::runtime_error("socket path is " + std::to_string(sock.size()) +
                             " characters, but a Unix socket path can hold at most " +
                             std::to_string(kMaxSocketPath) +
                             "; use a shorter build directory path: " + sock);
  }
}

Harness::~Harness() {
  // Never leave nbdkit running, whatever the test did. SIGKILL cannot be
  // ignored, and waitpid stops it from becoming a zombie.
  if (pid_ > 0 && !exit_) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  if (dir_.empty()) return;
  // This destructor runs at the end of the test body, so HasFailure() already
  // knows if an assertion failed. Keep the directory then, for debugging.
  if (::testing::Test::HasFailure()) {
    std::cerr << "[harness] test failed; files kept in " << dir_ << "\n";
  } else {
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
  }
}

std::vector<std::string> Harness::default_settings(const std::string& cap) const {
  return {"log=" + log_path(), "cap=" + cap};
}

bool Harness::start(const std::vector<std::string>& settings, const std::string& plugin) {
  EXPECT_EQ(pid_, -1) << "this Harness already started a server";

  // Build the argument list before fork(): after fork only async-signal-safe
  // calls are allowed, and std::string allocation is not one of them.
  std::vector<std::string> args = {"nbdkit", "-f", "-U", socket_path(), plugin};
  args.insert(args.end(), settings.begin(), settings.end());
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const std::string stderr_file = dir_ + "/stderr.txt";
  const std::string stdout_file = dir_ + "/stdout.txt";

  pid_ = ::fork();
  if (pid_ < 0) {
    ADD_FAILURE() << "fork failed: " << std::strerror(errno);
    return false;
  }
  if (pid_ == 0) {
    // Child. stderr goes to a file, not a pipe: a pipe could fill up and block
    // nbdkit if we are not reading it, and the file stays for debugging.
    const int null_fd = ::open("/dev/null", O_RDONLY);
    const int out_fd = ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int err_fd = ::open(stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (null_fd < 0 || out_fd < 0 || err_fd < 0) _exit(126);
    ::dup2(null_fd, STDIN_FILENO);
    ::dup2(out_fd, STDOUT_FILENO);
    ::dup2(err_fd, STDERR_FILENO);
    ::execvp("nbdkit", argv.data());
    _exit(127);  // exec failed: nbdkit is not installed or not on PATH
  }

  // Wait for the socket file, or for nbdkit to give up. We only look at the
  // file system and the process state; we do not connect (see the header).
  const auto deadline = Clock::now() + kStartTimeout;
  while (Clock::now() < deadline) {
    if (try_reap()) return false;  // startup refusal: stop() returns the details
    if (is_socket(socket_path())) return true;
    std::this_thread::sleep_for(kPollInterval);
  }
  ADD_FAILURE() << "nbdkit did not create " << socket_path() << " within " << kStartTimeout.count()
                << " ms; stderr so far:\n"
                << read_file(stderr_file);
  stop(SIGKILL);
  return false;
}

NbdHandle Harness::connect() {
  // The socket file appears when nbdkit calls bind(), a moment before listen().
  // A refused connection in that gap is harmless, so retry briefly with a fresh
  // handle (a handle that failed to connect cannot be reused).
  std::string last_error;
  for (int attempt = 0; attempt < 100; ++attempt) {
    NbdHandle handle(nbd_create());
    if (!handle) {
      ADD_FAILURE() << "nbd_create failed: " << nbd_get_error();
      return nullptr;
    }
    // By default libnbd rejects requests outside the export itself, before they
    // reach the server. Turn that check off: we want nbdkit and the plugin to
    // answer out-of-range requests, because that is what is under test.
    nbd_set_strict_mode(handle.get(), nbd_get_strict_mode(handle.get()) & ~LIBNBD_STRICT_BOUNDS);
    if (nbd_connect_unix(handle.get(), socket_path().c_str()) == 0) return handle;
    last_error = nbd_get_error();
    if (try_reap()) break;  // the server died, so more attempts are pointless
    std::this_thread::sleep_for(kPollInterval);
  }
  ADD_FAILURE() << "could not connect to " << socket_path() << ": " << last_error;
  return nullptr;
}

ServerExit Harness::stop(int sig) {
  if (exit_) return *exit_;
  if (pid_ <= 0) {
    ADD_FAILURE() << "stop() called but no server was started";
    return {};
  }
  ::kill(pid_, sig);
  const auto deadline = Clock::now() + kStopTimeout;
  while (Clock::now() < deadline) {
    if (try_reap()) return *exit_;
    std::this_thread::sleep_for(kPollInterval);
  }
  ADD_FAILURE() << "nbdkit did not exit within " << kStopTimeout.count() << " ms after signal "
                << sig << "; killing it";
  ::kill(pid_, SIGKILL);
  int status = 0;
  ::waitpid(pid_, &status, 0);
  finish(status);
  return *exit_;
}

bool Harness::try_reap() {
  if (exit_) return true;
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  if (result != pid_) return false;  // 0: still running
  finish(status);
  return true;
}

void Harness::finish(int wait_status) {
  ServerExit result;
  result.exited = WIFEXITED(wait_status);
  if (result.exited) {
    result.exit_code = WEXITSTATUS(wait_status);
  } else if (WIFSIGNALED(wait_status)) {
    result.term_signal = WTERMSIG(wait_status);
  }
  result.stderr_text = read_file(dir_ + "/stderr.txt");
  exit_ = result;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string sha256_file(const std::string& path) {
  // Test-only code, so shelling out to sha256sum is fine. The path comes from
  // the harness and never contains a single quote.
  const std::string command = "sha256sum '" + path + "' 2>/dev/null";
  // NOLINTNEXTLINE(bugprone-command-processor): test-only; the path is one the harness made.
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) return "";
  char digest[65] = {};
  const std::size_t got = std::fread(digest, 1, 64, pipe);
  ::pclose(pipe);
  return got == 64 ? std::string(digest, 64) : "";
}

bool mentions_setting(const std::string& text, const std::string& setting) {
  std::string cleaned = text;
  const std::string plugin_name = "logblock";
  for (std::size_t pos = cleaned.find(plugin_name); pos != std::string::npos;
       pos = cleaned.find(plugin_name, pos)) {
    cleaned.erase(pos, plugin_name.size());
  }
  return cleaned.find(setting) != std::string::npos;
}

}  // namespace logblock::itest
