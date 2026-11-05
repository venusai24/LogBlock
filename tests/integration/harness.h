#pragma once

// Test harness for the integration tests: starts a real nbdkit with the
// logblock plugin on a Unix socket, connects to it with libnbd, and stops it.
// No root, no kernel NBD device, no TCP.

#include <libnbd.h>
#include <sys/types.h>

#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace logblock::itest {

// How nbdkit ended, plus everything it wrote to stderr.
struct ServerExit {
  bool exited = false;      // true: normal exit with exit_code; false: killed by term_signal
  int exit_code = -1;       // valid when exited
  int term_signal = 0;      // valid when !exited
  std::string stderr_text;  // the whole stderr of the nbdkit process

  [[nodiscard]] bool clean() const { return exited && exit_code == 0; }
};

// Closes the libnbd handle when the test ends, even if an ASSERT returns early.
struct NbdDeleter {
  void operator()(nbd_handle* handle) const { nbd_close(handle); }
};
using NbdHandle = std::unique_ptr<nbd_handle, NbdDeleter>;

// One Harness per test. It owns a private directory (mode 0700) that holds the
// socket, the log and the captured output, and at most one nbdkit process.
// The destructor always kills the process, so a failing or throwing test never
// leaves an nbdkit behind. It deletes the directory only if the test passed.
class Harness {
 public:
  // base_dir is where the per-run directory is made. The default is
  // <build dir>/test-tmp (a compile definition), never /tmp, which is tmpfs on
  // Fedora. Throws std::runtime_error with a clear message if the socket path
  // would be longer than a Unix socket path can be (107 characters).
  explicit Harness(const std::string& base_dir = default_base_dir());
  ~Harness();

  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  [[nodiscard]] static std::string default_base_dir();

  [[nodiscard]] const std::string& dir() const { return dir_; }
  [[nodiscard]] std::string socket_path() const { return dir_ + "/sock"; }
  [[nodiscard]] std::string log_path() const { return dir_ + "/log"; }

  // The two required plugin settings: {"log=<dir>/log", "cap=<cap>"}.
  [[nodiscard]] std::vector<std::string> default_settings(const std::string& cap) const;

  // Runs `nbdkit -f -U <dir>/sock <plugin> <settings...>` with stderr going to a
  // file in the run directory. `plugin` is a file path or the name of one of
  // nbdkit's own plugins (the harness self-tests use "memory"). Waits until the
  // socket file exists or the process has exited. Returns true if nbdkit is
  // serving; false if it exited early (a startup refusal; call stop() for the
  // status and stderr) or did not start in time (a test failure is recorded).
  // It never connects to the socket: with SERIALIZE_CONNECTIONS a probe
  // connection would block the real client.
  [[nodiscard]] bool start(const std::vector<std::string>& settings,
                           const std::string& plugin = plugin_path());

  // Connects with libnbd over the Unix socket. Returns null and records a test
  // failure if it cannot. Libnbd's bounds check is switched off so that
  // out-of-range requests reach the server, which is what we want to test.
  [[nodiscard]] NbdHandle connect();

  // Sends `sig` (SIGTERM is a clean shutdown, SIGKILL a crash) and waits for the
  // process to end. If it already ended, returns the saved result. If it does
  // not end within a few seconds, it is killed and a test failure is recorded.
  ServerExit stop(int sig = SIGTERM);

  [[nodiscard]] static std::string plugin_path();

 private:
  // Collects the exit status if the child has ended. Never blocks.
  bool try_reap();
  void finish(int wait_status);

  std::string dir_;
  pid_t pid_ = -1;
  std::optional<ServerExit> exit_;
};

// Whole file as a string, or "" if it cannot be read.
std::string read_file(const std::string& path);

// sha256 of a file as 64 hex digits, computed by the sha256sum tool (no new
// library dependency). Returns "" if the tool failed.
std::string sha256_file(const std::string& path);

// True if `text` mentions `setting` (like "log" or "cap"). The plugin's own name
// ("logblock") is removed first, so a message that only says "logblock" does not
// count as naming the log setting.
bool mentions_setting(const std::string& text, const std::string& setting);

}  // namespace logblock::itest
