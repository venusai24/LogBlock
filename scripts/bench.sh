#!/usr/bin/env bash
# Benchmarks, in three modes (--only picks one; the default runs all three):
#   baseline  8 fio jobs x 3 runs x 2 targets = 48 runs. Targets: the LogBlock plugin and
#             `nbdkit file`, both on the same disk, both attached to /dev/nbd0 over a Unix
#             socket with a 4 KiB block size. Needs sudo and the nbd module.
#   recovery  restart time of a ~100 MiB and a ~1 GiB log, with and without a checkpoint,
#             caches dropped. No root, no device: the server is driven over its socket.
#   latency   p99 of 4K writes at iodepth 32 across one checkpoint, from fio's nbd engine.
#             No root, no device.
#
# Run this from a clean git tree. The baseline takes a while.
#   sudo modprobe nbd     (baseline only; this script never loads the module)
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/bench.sh --data-dir <dir> [--only baseline|recovery|latency]

  --data-dir <dir>  required; an existing directory on the disk to measure (not
                    tmpfs). A private mode 0700 directory is created inside it
                    for the log, the backing file and the socket, and removed
                    at exit.
  --only <mode>     run one mode; the default runs baseline, recovery, latency.

Builds build-release (CMAKE_BUILD_TYPE=Release) and writes
bench/results/<UTC timestamp>-<short git SHA>/ with setup.txt, raw/ and, per mode,
results.csv (baseline), recovery.csv, latency.csv.

baseline  uses only /dev/nbd0. sudo is used for nbd-client, fio and cache dropping on
          that device; each command is echoed first.
recovery  needs no root. Caches are dropped per file: fsync, then
          posix_fadvise(DONTNEED) on the log and its .ckpt (see scripts/bench_recovery.py).
          The disk's own cache and other files' pages are untouched.
latency   needs no root; uses fio's nbd engine (package fio-engine-nbd).

The baseline numbers exclude the cost of durability (no FLUSH is sent), and iodepth 32
shows queuing, not parallel service (requests are serialized). Latency p99 at depth 32
includes queueing behind the checkpoint; checkpoint_duration_us is the checkpoint itself.

Testing overrides (unset for real results; setup.txt records any that are set):
  BENCH_BUILD_DIR         build directory (default build-release)
  BENCH_RESULTS_DIR       parent of the timestamped results folder (default bench/results)
  BENCH_RECOVERY_SIZES    log size targets in bytes, space separated
                          (default "104857600 1073741824")
  BENCH_RECOVERY_INTERVAL checkpoint_interval in bytes for recovery runs (default: the
                          plugin's default, 64 MiB)
  BENCH_LATENCY_WRITES    writes per latency run (default 24000)
  BENCH_LATENCY_INTERVAL  checkpoint_interval in bytes for latency runs (default 67108864,
                          the plugin's default)
  BENCH_LATENCY_WINDOW    writes per latency window (default 1000)
USAGE
}

CAP=3G         # logblock log cap: a full prefill plus the largest write job
FILE_SIZE=1G   # size of the sparse backing file for the `file` target
RUNS=3

# Recovery and latency constants. 4,136 = 40-byte record header + 4,096 of data: the
# log bytes one 4K write adds (docs/formats.md).
RECORD_BYTES=4136
RECOVERY_SIZES=${BENCH_RECOVERY_SIZES:-"104857600 1073741824"}  # 100 MiB, 1 GiB
LAT_WRITES=${BENCH_LATENCY_WRITES:-24000}   # passes the first boundary (about write 16,225), stops short of the second
LAT_INTERVAL=${BENCH_LATENCY_INTERVAL:-67108864}  # kDefaultCheckpointInterval, 64 MiB
LAT_WINDOW=${BENCH_LATENCY_WINDOW:-1000}
# Empty (the plugin's default interval) unless a test override is set.
RECOVERY_INTERVAL_ARG=()
if [[ -n "${BENCH_RECOVERY_INTERVAL:-}" ]]; then
  RECOVERY_INTERVAL_ARG=("checkpoint_interval=${BENCH_RECOVERY_INTERVAL}")
fi
BUILD_DIR=${BENCH_BUILD_DIR:-build-release}
RESULTS_DIR=${BENCH_RESULTS_DIR:-bench/results}
LAT_JOB=bench/jobs/latency/randwrite-4k-qd32-ckpt.fio

cd "$(dirname "${BASH_SOURCE[0]}")/.."
# shellcheck source=scripts/lib_nbd.sh
source scripts/lib_nbd.sh

# Split our own --only/--help out, then hand the rest to the shared parser.
ONLY=all
args=()
while [[ $# -gt 0 ]]; do
  case $1 in
    -h | --help) usage; exit 0 ;;
    --only)
      [[ $# -ge 2 ]] || die "--only needs baseline, recovery or latency"
      ONLY=$2
      shift 2
      ;;
    *) args+=("$1"); shift ;;
  esac
done
case "${ONLY}" in
  all | baseline | recovery | latency) ;;
  *) die "--only must be baseline, recovery or latency, not '${ONLY}'" ;;
esac
if ((${#args[@]} == 0)); then usage >&2; fi
parse_data_dir_arg "${args[@]}"

# want <mode>: true when this run includes that mode.
want() { [[ "${ONLY}" == all || "${ONLY}" == "$1" ]]; }

# The --data-dir checks always run. The nbd module and /dev/nbd0 checks run only when
# the baseline will attach the device; the other modes use the Unix socket directly.
check_data_dir
if want baseline; then check_nbd_device; fi
if want recovery || want latency; then
  # fio lists its engines; the nbd engine is a separate package on Fedora.
  fio --enghelp=nbd >/dev/null 2>&1 ||
    die "fio has no nbd engine; run: sudo dnf install fio-engine-nbd"
  command -v fincore >/dev/null || echo "warning: fincore not found; cache drops will not be verified" >&2
fi

# Build the Release plugin. Unprivileged, so safe to do before any sudo.
cmake -S . -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}"
PLUGIN="$PWD/${BUILD_DIR}/nbdkit-logblock-plugin.so"
[[ -f "${PLUGIN}" ]] || die "plugin not found: ${PLUGIN}"

# The glob is bench/jobs/*.fio only: the latency job lives in bench/jobs/latency/.
JOBS=(bench/jobs/*.fio)
((${#JOBS[@]} == 8)) || die "expected 8 job files in bench/jobs, found ${#JOBS[@]}"
[[ -f "${LAT_JOB}" ]] || die "latency job not found: ${LAT_JOB}"

# Results folder named by UTC time and short git SHA.
SHA=$(git rev-parse --short HEAD)
OUT="${RESULTS_DIR}/$(date -u +%Y%m%dT%H%M%SZ)-${SHA}"
mkdir -p "${OUT}/raw"

make_run_dir logblock-bench
SERVER_LOG="${RUN_DIR}/server.log"
DATA_FILE="" # the log or backing file of the current run
SERVER_PID=""
ATTACHED=0

# Runs on every exit: detach, stop the server, delete the run directory.
cleanup() {
  local status=$?
  set +e
  detach_nbd
  stop_server
  if ((status != 0)) && [[ -f "${SERVER_LOG}" ]]; then
    echo "--- FAILED (status ${status}); server log tail:" >&2
    tail -n 20 "${SERVER_LOG}" >&2
  fi
  [[ -n "${RUN_DIR}" ]] && rm -rf -- "${RUN_DIR}"
  exit "${status}"
}
trap cleanup EXIT

# --- setup.txt: everything needed to reproduce the run. -------------------------------------
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "modes: ${ONLY}"
  echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -n1)"
  echo "ram: $(awk '/^MemTotal/ {print $2, $3}' /proc/meminfo)"
  echo "kernel: $(uname -r)"
  echo "data_dir: ${DATA_DIR}"
  # findmnt -T finds the mount that holds the path: device and file system type.
  echo "filesystem_device: $(findmnt -n -o SOURCE,FSTYPE -T "${DATA_DIR}")"
  echo "nbdkit: $(nbdkit --version)"
  echo "fio: $(fio --version)"
  echo "git_sha: $(git rev-parse HEAD)"
  # bench/results is excluded: this script has just created OUT there, which would
  # otherwise make every run look dirty.
  if [[ -n "$(git status --porcelain -- . ':!bench/results')" ]]; then
    echo "git_tree: DIRTY (uncommitted changes; the SHA does not describe this build)"
  else
    echo "git_tree: clean"
  fi
  echo "build_type: $(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "${BUILD_DIR}/CMakeCache.txt")"
  if want baseline; then
    echo "baseline_fio_engine: libaio on ${NBD_DEV} (direct=1, nbd-client, 4096-byte blocks)"
    echo "baseline_cache_handling: sudo tee /proc/sys/vm/drop_caches (3) after the prefill, before each job"
    echo "logblock_cap: ${CAP}"
    echo "runs_per_job: ${RUNS}"
  fi
  if want recovery; then
    echo "recovery_fio_engine: nbd (4K random writes, iodepth 32, over the Unix socket; builds the log)"
    echo "recovery_checkpoint_interval: ${BENCH_RECOVERY_INTERVAL:-default (67108864 bytes)}"
    echo "recovery_log_targets_bytes: ${RECOVERY_SIZES}"
    echo "recovery_cache_handling: no root; fsync then posix_fadvise(DONTNEED) on the log and .ckpt before each run; verified with fincore when available; the disk's own cache is not dropped"
    echo "recovery_runs_per_mode: 3"
  fi
  if want latency; then
    echo "latency_fio_engine: nbd (4K random writes, iodepth 32, no FLUSH, over the Unix socket)"
    echo "latency_checkpoint_interval: ${LAT_INTERVAL} bytes"
    echo "latency_writes_per_run: ${LAT_WRITES}"
    echo "latency_window_writes: ${LAT_WINDOW}"
    echo "latency_cache_handling: none (runs start from an empty log; nothing is read back)"
    echo "latency_runs: 3"
  fi
  overrides=""
  for v in BENCH_BUILD_DIR BENCH_RESULTS_DIR BENCH_RECOVERY_SIZES BENCH_RECOVERY_INTERVAL BENCH_LATENCY_WRITES \
    BENCH_LATENCY_INTERVAL BENCH_LATENCY_WINDOW; do
    [[ -n "${!v:-}" ]] && overrides+=" ${v}=${!v}"
  done
  [[ -z "${overrides}" ]] || echo "overrides_set (not a real result):${overrides}"
} >"${OUT}/setup.txt"

# ===== baseline ==================================================================

# drop_caches: flush dirty pages, then free page/dentry/inode caches so reads
# come from the disk, not memory. Needs root.
drop_caches() {
  sync
  echo "+ sudo tee /proc/sys/vm/drop_caches  (writes 3)"
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

# one_run <target> <job file> <run number>
one_run() {
  local target=$1 job=$2 run=$3
  local name
  name=$(basename "${job}" .fio)
  echo "=== ${target} ${name} run ${run}"

  # 1. Fresh server state: empty log, or a 1 GiB sparse file (nbdkit exports
  #    the file's size, so an empty file would be 0 bytes).
  if [[ "${target}" == logblock ]]; then
    DATA_FILE="${RUN_DIR}/log.bin"
    start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" "${PLUGIN}" \
      "log=${DATA_FILE}" "cap=${CAP}"
  else
    DATA_FILE="${RUN_DIR}/backing.img"
    truncate -s "${FILE_SIZE}" "${DATA_FILE}"
    start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" file "${DATA_FILE}"
  fi

  # 2. Attach with a 4 KiB block size.
  attach_nbd

  # 3. Write the whole device once so reads hit real data.
  run_sudo fio --name=prefill --filename="${NBD_DEV}" --ioengine=libaio \
    --direct=1 --rw=write --bs=128k --iodepth=1 --size=100% >/dev/null

  # 4. Sync and drop caches.
  drop_caches

  # 5. The job. fio prints its JSON on stdout; the redirect is done by this
  #    (unprivileged) shell, so the raw file belongs to the invoking user.
  local raw="${OUT}/raw/${target}-${name}-${run}.json"
  echo "+ sudo fio --output-format=json ${job} > ${raw}"
  sudo fio --output-format=json "${job}" >"${raw}"
  python3 -I scripts/bench_csv.py --target "${target}" --run "${run}" "${raw}" \
    >>"${OUT}/results.csv"

  # 6. Disconnect, SIGTERM the server, delete the log or backing file. The plugin
  #    also keeps <log>.ckpt next to the log; the next run refuses to start on a
  #    checkpoint without its log, so it goes too.
  detach_nbd
  stop_server
  rm -f -- "${DATA_FILE}" "${DATA_FILE}.ckpt"
}

run_baseline() {
  python3 -I scripts/bench_csv.py --header >"${OUT}/results.csv"
  local target job run
  for target in logblock file; do
    for job in "${JOBS[@]}"; do
      for run in $(seq 1 "${RUNS}"); do
        one_run "${target}" "${job}" "${run}"
      done
    done
  done
  local rows
  rows=$(($(wc -l <"${OUT}/results.csv") - 1))
  echo "baseline done: ${rows} rows in ${OUT}/results.csv (expected 48)"
  ((rows == 48)) || die "expected 48 rows, got ${rows}"
}

# ===== shared by recovery and latency (no root, no device) =========================

# fio_uri: the Unix-socket URI fio's nbd engine connects to.
fio_uri() { echo "nbd+unix:///?socket=${SOCK}"; }

# ===== recovery ==================================================================

# drop_file_caches <files...>: no-root cache drop for the files named. Sets CACHE_STATE:
#   dropped             fincore found nothing resident
#   dropped_unverified  no fincore to check with
#   partial             fincore still found pages resident (fadvise is only a hint)
drop_file_caches() {
  python3 -I scripts/bench_recovery.py drop-cache "$@"
  CACHE_STATE=dropped_unverified
  if command -v fincore >/dev/null; then
    local resident
    # -b bytes, -n no header, -o RES the resident column only; one line per file.
    resident=$(fincore -b -n -o RES "$@" | awk '{s += $1} END {print s + 0}')
    if ((resident == 0)); then
      CACHE_STATE=dropped
    else
      CACHE_STATE=partial
      echo "warning: ${resident} bytes still cached after the drop; recorded as partial" >&2
    fi
  fi
}

# recovery_cap <target bytes>: a cap with room to spare over a log built under the target.
recovery_cap() { echo $(($1 * 2)); }

# build_log <target bytes> <log>: fill an empty log with the last 4K random write that
# keeps it under the target, then SIGTERM the server.
build_log() {
  local target=$1 log=$2 n
  n=$(python3 -I scripts/bench_recovery.py writes-under "${target}")
  rm -f -- "${log}" "${log}.ckpt"
  start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" "${PLUGIN}" \
    "log=${log}" "cap=$(recovery_cap "${target}")" "${RECOVERY_INTERVAL_ARG[@]}"
  # size = n blocks, so fio's random map visits distinct blocks and ends after n writes.
  fio --name=build --ioengine=nbd --uri="$(fio_uri)" --rw=randwrite --bs=4k \
    --iodepth=32 --size=$((n * 4096)) --number_ios="${n}" \
    --output="${OUT}/raw/recovery-${target}-build-fio.txt"
  stop_server
  cp -- "${SERVER_LOG}" "${OUT}/raw/recovery-${target}-build-server.log"
  local actual
  actual=$(stat -c %s "${log}")
  ((actual == n * RECORD_BYTES)) ||
    die "built log is ${actual} bytes, expected ${n} x ${RECORD_BYTES}"
  ((actual < target)) || die "built log (${actual}) is not under the target (${target})"
}

# recovery_run <target> <log> <log bytes> <mode> <run>
recovery_run() {
  local target=$1 log=$2 bytes=$3 mode=$4 run=$5
  local files=("${log}")
  [[ -f "${log}.ckpt" ]] && files+=("${log}.ckpt")
  drop_file_caches "${files[@]}"
  echo "=== recovery ${bytes} ${mode} run ${run} (cache ${CACHE_STATE})"
  start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" "${PLUGIN}" \
    "log=${log}" "cap=$(recovery_cap "${target}")" "${RECOVERY_INTERVAL_ARG[@]}"
  stop_server
  cp -- "${SERVER_LOG}" "${OUT}/raw/recovery-${target}-${mode}-${run}.log"
  python3 -I scripts/bench_recovery.py recovery-row --log-bytes "${bytes}" \
    --mode "${mode}" --run "${run}" --cache-state "${CACHE_STATE}" "${SERVER_LOG}" \
    >>"${OUT}/recovery.csv"
}

run_recovery() {
  python3 -I scripts/bench_recovery.py header >"${OUT}/recovery.csv"
  local target log="${RUN_DIR}/log.bin" bytes run
  for target in ${RECOVERY_SIZES}; do
    build_log "${target}" "${log}"
    bytes=$(stat -c %s "${log}")
    for run in 1 2 3; do
      recovery_run "${target}" "${log}" "${bytes}" checkpoint "${run}"
    done
    # A log without its checkpoint is scanned in full, and the server does not write a
    # new checkpoint at startup, so all three runs scan fully.
    rm -f -- "${log}.ckpt"
    for run in 1 2 3; do
      recovery_run "${target}" "${log}" "${bytes}" full_scan "${run}"
    done
    rm -f -- "${log}" "${log}.ckpt"
  done
  local rows
  rows=$(($(wc -l <"${OUT}/recovery.csv") - 1))
  local expected=$(($(wc -w <<<"${RECOVERY_SIZES}") * 6))
  echo "recovery done: ${rows} rows in ${OUT}/recovery.csv (expected ${expected})"
  ((rows == expected)) || die "expected ${expected} recovery rows, got ${rows}"
}

# ===== latency ===================================================================

latency_run() {
  local run=$1 log="${RUN_DIR}/log.bin"
  echo "=== latency run ${run}"
  rm -f -- "${log}" "${log}.ckpt"
  start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" "${PLUGIN}" \
    "log=${log}" "cap=$((LAT_WRITES * RECORD_BYTES + (16 << 20)))" \
    "checkpoint_interval=${LAT_INTERVAL}"
  # The job file reads these three from the environment.
  NBD_URI="$(fio_uri)" LAT_WRITES="${LAT_WRITES}" LAT_LOG="${OUT}/raw/latency-${run}" \
    fio --output="${OUT}/raw/latency-${run}-fio.txt" "${LAT_JOB}"
  stop_server # SIGTERM: the plugin prints its counters on the way out
  cp -- "${SERVER_LOG}" "${OUT}/raw/latency-${run}-server.log"
  # Fails (set -e) unless exactly one checkpoint ran.
  local duration
  duration=$(python3 -I scripts/bench_recovery.py checkpoint-duration "${SERVER_LOG}")
  python3 -I scripts/bench_latency.py --interval-bytes "${LAT_INTERVAL}" --run "${run}" \
    --iodepth 32 --checkpoint-duration-us "${duration}" --window-writes "${LAT_WINDOW}" \
    "${OUT}/raw/latency-${run}_lat.1.log" >>"${OUT}/latency.csv"
  rm -f -- "${log}" "${log}.ckpt"
}

run_latency() {
  python3 -I scripts/bench_latency.py --header >"${OUT}/latency.csv"
  local run
  for run in 1 2 3; do
    latency_run "${run}"
  done
  local rows
  rows=$(($(wc -l <"${OUT}/latency.csv") - 1))
  echo "latency done: ${rows} rows in ${OUT}/latency.csv (expected 6)"
  ((rows == 6)) || die "expected 6 latency rows, got ${rows}"
}

if want baseline; then run_baseline; fi
if want recovery; then run_recovery; fi
if want latency; then run_latency; fi
echo "results: ${OUT}"
