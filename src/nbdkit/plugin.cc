// The nbdkit plugin: a thin adapter between nbdkit's C callbacks and the storage
// engine. It parses settings, creates the engine, forwards each request,
// maps Status codes to errno values, and counts requests. It holds no storage
// logic: block maps, logs and caps all live in the engine.

#define NBDKIT_API_VERSION 2
// NBDKIT_REGISTER_PLUGIN needs THREAD_MODEL. The most restrictive model: one
// connection at a time, all requests from one thread.
#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_CONNECTIONS
#include <nbdkit-plugin.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "logblock/engine.h"
#include "logblock/io.h"
#include "logblock/status.h"

namespace {

using logblock::Engine;
using logblock::Status;
using logblock::StatusCode;

// --- Plugin state -------------------------------------------------------------
// nbdkit callbacks are plain C functions with no user-data pointer for the plugin
// as a whole, so the settings, engine and counters live in file-level variables.

std::string g_log_path;
uint64_t g_cap = 0;
bool g_have_log = false;
bool g_have_cap = false;
uint64_t g_checkpoint_interval = logblock::kDefaultCheckpointInterval;
std::unique_ptr<Engine> g_engine;

// One logger for the whole plugin, on the stderr sink (spdlog's default logger
// writes to stdout). It is shared by startup (the recovery report) and
// logblock_unload (the counters) because spdlog throws if the same logger name
// is registered twice.
std::shared_ptr<spdlog::logger> g_logger;

spdlog::logger& logger() {
  if (!g_logger) {
    g_logger = spdlog::stderr_logger_st("logblock");
    g_logger->set_pattern("[%l] %v");
  }
  return *g_logger;
}

// Plain integers are enough: with SERIALIZE_CONNECTIONS nbdkit runs one request at
// a time, so no two threads ever touch these at once.
struct Counters {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t trims = 0;
  uint64_t flushes = 0;
};
Counters g_counters;

// --- Helpers --------------------------------------------------------------------

// Turns an engine Status into nbdkit's convention: 0 on success, otherwise set the
// errno the NBD client will see and return -1.
int report(const Status& status) {
  if (status.ok()) {
    return 0;
  }
  int err = EIO;
  switch (status.code()) {
    case StatusCode::NoSpace:
      err = ENOSPC;
      break;
    case StatusCode::OutOfRange:
      err = EINVAL;
      break;
    case StatusCode::Ok:
    case StatusCode::Corrupt:
    case StatusCode::IoError:
    case StatusCode::Failed:
      err = EIO;
      break;
  }
  nbdkit_error("%s", status.to_string().c_str());
  nbdkit_set_error(err);
  return -1;
}

// Prints the warnings the engine raised while serving (a failed checkpoint), once each. The
// request that triggered a failed checkpoint still succeeds, so this is the only place the
// operator hears about it (D27).
void log_engine_warnings() {
  for (const std::string& warning : g_engine->take_warnings()) {
    logger().warn("{}", warning);
  }
  logger().flush();
}

// Runs `body` (which returns a Status) and never lets an exception escape into
// nbdkit's C code, where it would be undefined behaviour.
// An unexpected exception becomes EIO. Calls `on_ok` only when the request worked,
// so a failed request is not counted.
template <typename Body, typename OnOk>
int run(Body body, OnOk on_ok) {
  try {
    const Status status = body();
    if (status.ok()) {
      on_ok();
    }
    log_engine_warnings();  // a checkpoint that failed during this request
    return report(status);
  } catch (const std::exception& e) {
    nbdkit_error("unexpected exception: %s", e.what());
  } catch (...) {
    nbdkit_error("unexpected exception of unknown type");
  }
  nbdkit_set_error(EIO);
  return -1;
}

// --- Settings and lifecycle ------------------------------------------------

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the signature is fixed by nbdkit.
int logblock_config(const char* key, const char* value) {
  if (std::strcmp(key, "log") == 0) {
    // Made absolute now: a daemonized nbdkit changes directory to "/" after get_ready, and the
    // checkpoint file is created later by path, next to the log.
    char* absolute = nbdkit_absolute_path(value);
    if (absolute == nullptr) {
      return -1;  // nbdkit has reported the reason
    }
    g_log_path = absolute;
    std::free(absolute);
    g_have_log = true;
  } else if (std::strcmp(key, "cap") == 0) {
    // nbdkit_parse_size understands suffixes like 1M and 2G. It reports its own
    // error and returns -1 on a bad value, but that message does not say which
    // setting was wrong, so add our own.
    const int64_t size = nbdkit_parse_size(value);
    if (size < 0) {
      nbdkit_error("invalid value '%s' for the cap setting", value);
      return -1;
    }
    g_cap = static_cast<uint64_t>(size);
    g_have_cap = true;
  } else if (std::strcmp(key, "checkpoint_interval") == 0) {
    const int64_t size = nbdkit_parse_size(value);
    if (size < 0) {
      nbdkit_error("invalid value '%s' for the checkpoint_interval setting", value);
      return -1;
    }
    g_checkpoint_interval = static_cast<uint64_t>(size);
  } else {
    nbdkit_error(
        "unknown setting '%s' (the settings are log=<path>, cap=<size> and "
        "checkpoint_interval=<size>)",
        key);
    return -1;
  }
  return 0;
}

int logblock_config_complete() {
  if (!g_have_log) {
    nbdkit_error("the log setting is required: log=<path>");
    return -1;
  }
  if (!g_have_cap) {
    nbdkit_error("the cap setting is required: cap=<size>");
    return -1;
  }
  if (g_cap < logblock::kDataRecordSize) {
    nbdkit_error("the cap setting is too small: %llu bytes, the minimum is %llu",
                 static_cast<unsigned long long>(g_cap),
                 static_cast<unsigned long long>(logblock::kDataRecordSize));
    return -1;
  }
  if (g_checkpoint_interval < logblock::kMinCheckpointInterval) {
    nbdkit_error("the checkpoint_interval setting is too small: %llu bytes, the minimum is %llu",
                 static_cast<unsigned long long>(g_checkpoint_interval),
                 static_cast<unsigned long long>(logblock::kMinCheckpointInterval));
    return -1;
  }
  return 0;
}

// Prints how startup went (FR-023): the recovery line, then each warning. The format
// is fixed by specs/002-crash-recovery/contracts/plugin-cli.md.
void log_recovery_report(const std::string& log_path, const logblock::RecoveryReport& report) {
  const char* source = "none";
  if (report.source == logblock::RecoverySource::Checkpoint) {
    source = "checkpoint";
  } else if (report.source == logblock::RecoverySource::FullScan) {
    source = "full_scan";
  }
  logger().info(
      "recovery log={} source={} slot={} generation={} records_replayed={} "
      "log_bytes_scanned={} torn_bytes_removed={} unreadable_checkpoints={} time_us={}",
      log_path, source, report.slot, report.generation, report.records_replayed,
      report.log_bytes_scanned, report.torn_bytes_removed, report.unreadable_checkpoints,
      report.time_us);
  for (const std::string& warning : report.warnings) {
    logger().warn("{}", warning);
  }
  logger().flush();
}

// get_ready runs before nbdkit forks into the background and before it creates the
// socket. Engine::create recovers the log inside this call, so no client can connect
// until recovery is done (FR-004), and a refusal (corrupt log, lock held, unopenable
// log) still reaches the terminal and the exit status. Engine::create also creates
// the log file if it is missing.
int logblock_get_ready() {
  try {
    const Status status = Engine::create(g_log_path, g_cap, logblock::make_posix_io(), &g_engine,
                                         g_checkpoint_interval);
    if (!status.ok()) {
      // The engine's text already names the log and the reason.
      nbdkit_error("%s", status.message().c_str());
      return -1;
    }
    log_recovery_report(g_log_path, g_engine->recovery_report());
    log_engine_warnings();  // the fresh checkpoint at startup may have failed (FR-016)
    return 0;
  } catch (const std::exception& e) {
    nbdkit_error("cannot create the engine: %s", e.what());
  } catch (...) {
    nbdkit_error("cannot create the engine: unknown exception");
  }
  return -1;
}

// nbdkit calls unload on a clean shutdown (SIGTERM, SIGINT, or the last client
// leaving). The counters go out as one structured line on stderr, through the same
// logger as the recovery report.
void logblock_unload() {
  if (!g_engine) {
    return;  // startup was refused: there is nothing to report
  }
  try {
    const logblock::CheckpointCounters c = g_engine->checkpoint_counters();
    logger().info(
        "counters reads={} writes={} trims={} flushes={} bytes_logged={} checkpoints_written={} "
        "checkpoints_failed={} checkpoints_unreadable={} checkpoint_duration_count={} "
        "checkpoint_duration_max_us={} checkpoint_duration_sum_us={}",
        g_counters.reads, g_counters.writes, g_counters.trims, g_counters.flushes,
        g_engine->bytes_logged(), c.written, c.failed, c.unreadable, c.duration_count,
        c.duration_max_us, c.duration_sum_us);
    logger().flush();
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Nothing useful can be done while shutting down.
  }
  g_engine.reset();
}

// --- Data callbacks --------------------------------------------------------

// nbdkit needs a non-null handle per connection. The engine is global, so the
// handle carries nothing; its address is just a non-null value.
void* logblock_open(int /*readonly*/) {
  static int handle;
  return &handle;
}

int64_t logblock_get_size(void* /*handle*/) { return static_cast<int64_t>(logblock::kDeviceSize); }

// can_fua NONE: nbdkit then never sends FUA flags to us, and it emulates FUA
// itself by calling flush afterwards. So the flags arguments below are ignored.
int logblock_can_fua(void* /*handle*/) { return NBDKIT_FUA_NONE; }

int logblock_pread(void* /*handle*/, void* buf, uint32_t count, uint64_t offset,
                   uint32_t /*flags*/) {
  return run([&] { return g_engine->read(offset, count, buf); }, [] { ++g_counters.reads; });
}

int logblock_pwrite(void* /*handle*/, const void* buf, uint32_t count, uint64_t offset,
                    uint32_t /*flags*/) {
  return run([&] { return g_engine->write(offset, count, buf); }, [] { ++g_counters.writes; });
}

int logblock_trim(void* /*handle*/, uint32_t count, uint64_t offset, uint32_t /*flags*/) {
  return run([&] { return g_engine->trim(offset, count); }, [] { ++g_counters.trims; });
}

int logblock_flush(void* /*handle*/, uint32_t /*flags*/) {
  return run([&] { return g_engine->flush(); }, [] { ++g_counters.flushes; });
}

// nbdkit's man page builds the struct in a function for C++, because C++ does
// not allow the C-style designated initializers out of order.
nbdkit_plugin create_plugin() {
  nbdkit_plugin plugin = nbdkit_plugin();
  plugin.name = "logblock";
  plugin.longname = "LogBlock log-structured block device";
  plugin.config = logblock_config;
  plugin.config_complete = logblock_config_complete;
  plugin.config_help =
      "log=<path>   Log file (created if missing, recovered if not empty)\n"
      "cap=<size>   Maximum log size, at least 4136 (for example 2G)\n"
      "checkpoint_interval=<size>\n"
      "             Log growth between checkpoints, at least 4136 (default 64M)";
  plugin.get_ready = logblock_get_ready;
  plugin.unload = logblock_unload;
  plugin.open = logblock_open;
  plugin.get_size = logblock_get_size;
  plugin.can_fua = logblock_can_fua;
  plugin.pread = logblock_pread;
  plugin.pwrite = logblock_pwrite;
  plugin.trim = logblock_trim;
  plugin.flush = logblock_flush;
  // No .zero: nbdkit falls back to .pwrite.
  return plugin;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): create_plugin only fills a struct.
static struct nbdkit_plugin plugin = create_plugin();
NBDKIT_REGISTER_PLUGIN(plugin)
