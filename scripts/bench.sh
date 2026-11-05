#!/usr/bin/env bash
# Baseline benchmark: 8 fio jobs x 3 runs x 2 targets = 48 runs.
# Targets: the LogBlock plugin and `nbdkit file`, both on the same disk, both
# attached to /dev/nbd0 over a Unix socket with a 4 KiB block size.
#
# Run this from a clean git tree. It needs sudo and takes a while.
#   sudo modprobe nbd     (this script never loads the module)
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/bench.sh --data-dir <dir>

  --data-dir <dir>  required; an existing directory on the disk to measure (not
                    tmpfs). A private mode 0700 directory is created inside it
                    for the log, the backing file and the socket, and removed
                    at exit.

Builds build-release (CMAKE_BUILD_TYPE=Release) and writes
bench/results/<UTC timestamp>-<short git SHA>/ with results.csv, setup.txt, raw/.
Uses only /dev/nbd0. sudo is used for nbd-client, fio and cache dropping on that
device; each command is echoed first.

These numbers exclude the cost of durability (no FLUSH is sent), and iodepth 32
shows queuing, not parallel service (M1 serializes requests).
USAGE
}

CAP=3G         # logblock log cap: a full prefill plus the largest write job
FILE_SIZE=1G   # size of the sparse backing file for the `file` target
RUNS=3

cd "$(dirname "${BASH_SOURCE[0]}")/.."
# shellcheck source=scripts/lib_nbd.sh
source scripts/lib_nbd.sh

for a in "$@"; do
  if [[ "$a" == -h || "$a" == --help ]]; then usage; exit 0; fi
done
if (($# == 0)); then usage >&2; fi
parse_data_dir_arg "$@"
check_environment

# Build the Release plugin. Unprivileged, so safe to do before any sudo.
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
PLUGIN="$PWD/build-release/nbdkit-logblock-plugin.so"
[[ -f "${PLUGIN}" ]] || die "plugin not found: ${PLUGIN}"

JOBS=(bench/jobs/*.fio)
((${#JOBS[@]} == 8)) || die "expected 8 job files in bench/jobs, found ${#JOBS[@]}"

# Results folder named by UTC time and short git SHA.
SHA=$(git rev-parse --short HEAD)
OUT="bench/results/$(date -u +%Y%m%dT%H%M%SZ)-${SHA}"
mkdir -p "${OUT}/raw"
python3 -I scripts/bench_csv.py --header >"${OUT}/results.csv"

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
  echo "build_type: $(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' build-release/CMakeCache.txt)"
  echo "logblock_cap: ${CAP}"
  echo "runs_per_job: ${RUNS}"
} >"${OUT}/setup.txt"

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

  # 6. Disconnect, SIGTERM the server, delete the log or backing file.
  detach_nbd
  stop_server
  rm -f -- "${DATA_FILE}"
}

for target in logblock file; do
  for job in "${JOBS[@]}"; do
    for run in $(seq 1 "${RUNS}"); do
      one_run "${target}" "${job}" "${run}"
    done
  done
done

rows=$(($(wc -l <"${OUT}/results.csv") - 1))
echo "done: ${rows} rows in ${OUT}/results.csv (expected 48)"
((rows == 48)) || die "expected 48 rows, got ${rows}"
