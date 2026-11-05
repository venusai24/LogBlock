#!/usr/bin/env bash
# End-to-end test: LogBlock behind /dev/nbd0 holds an ext4
# file system, copies ~100 MB of random files, unmounts, remounts and compares
# sha256 checksums, all in one server run.
#
# This needs sudo. Before running:
#   cmake --build build            (the plugin must exist)
#   sudo modprobe nbd              (this script never loads the module)
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/e2e.sh --data-dir <dir> [--cap <size>]

  --data-dir <dir>  required; an existing directory on a real disk (not tmpfs).
                    A private mode 0700 directory is created inside it for the
                    log, socket, test files and mount point, and removed at exit.
  --cap <size>      log size cap passed to the plugin (default 206M, set from a run that
                    measures bytes_logged and sets measured + 50%).

Uses only /dev/nbd0. sudo is used for nbd-client, blockdev, mkfs.ext4, mount, umount,
chown and nothing else; each command is echoed first. SELinux is never changed.
USAGE
}

# Default cap: the measured value plus 50% headroom.
CAP=206M  # measured 143,846,024 bytes_logged + 50% = 215,769,036; 206 MiB = 216,006,656

cd "$(dirname "${BASH_SOURCE[0]}")/.."
# shellcheck source=scripts/lib_nbd.sh
source scripts/lib_nbd.sh

# Split our own --cap/--help out, then hand the rest to the shared parser.
args=()
while [[ $# -gt 0 ]]; do
  case $1 in
    -h | --help) usage; exit 0 ;;
    --cap)
      [[ $# -ge 2 ]] || die "--cap needs a size"
      CAP=$2
      shift 2
      ;;
    *) args+=("$1"); shift ;;
  esac
done
if ((${#args[@]} == 0)); then
  usage >&2
fi
parse_data_dir_arg "${args[@]}"
check_environment

PLUGIN="$PWD/build/nbdkit-logblock-plugin.so"
[[ -f "${PLUGIN}" ]] || die "plugin not found: ${PLUGIN} (run: cmake --build build)"

make_run_dir logblock-e2e
LOG="${RUN_DIR}/log.bin"          # the LogBlock log
SERVER_LOG="${RUN_DIR}/server.log" # nbdkit stderr, including the counters line
SRC="${RUN_DIR}/src"              # generated random files
MNT="${RUN_DIR}/mnt"              # mount point for /dev/nbd0
mkdir "${SRC}" "${MNT}"

SERVER_PID=""
ATTACHED=0
MOUNTED=0
OK=0

# The cleanup trap runs on every exit, success or failure (set -e included).
# Order matters: unmount first (needs the device), then detach (needs the
# server), then stop the server, then delete the run directory.
cleanup() {
  local status=$?
  set +e
  if [[ "${MOUNTED}" == 1 ]]; then
    run_sudo umount "${MNT}"
    MOUNTED=0
  fi
  detach_nbd
  stop_server
  echo "--- server counters:"
  grep counters "${SERVER_LOG}" || echo "(no counters line; see below)"
  if [[ "${OK}" != 1 ]]; then
    echo "--- FAILED (status ${status}); server log tail:" >&2
    tail -n 20 "${SERVER_LOG}" >&2
  fi
  # RUN_DIR is non-empty here (set by make_run_dir); rm only what we created.
  [[ -n "${RUN_DIR}" ]] && rm -rf -- "${RUN_DIR}"
  exit "${status}"
}
trap cleanup EXIT

# 1. Start the server from an empty log. -f foreground, -U Unix socket.
start_server "${SERVER_LOG}" nbdkit -f -U "${SOCK}" "${PLUGIN}" "log=${LOG}" "cap=${CAP}"

# 2. Attach and check the geometry: 1 GiB, 4096-byte sectors.
attach_nbd
size=$(run_sudo blockdev --getsize64 "${NBD_DEV}")
ss=$(run_sudo blockdev --getss "${NBD_DEV}")
echo "size=${size} sector_size=${ss}"
[[ "${size}" == 1073741824 ]] || die "expected 1073741824 bytes, got ${size}"
[[ "${ss}" == 4096 ]] || die "expected sector size 4096, got ${ss}"

# 3. Generate the files: 1 x 50 MiB, 40 x 1 MiB, 1000 x 10 KiB (~100 MB).
echo "generating files in ${SRC}"
head -c $((50 * 1024 * 1024)) /dev/urandom >"${SRC}/big.bin"
mkdir "${SRC}/medium" "${SRC}/small"
for i in $(seq 1 40); do
  head -c $((1024 * 1024)) /dev/urandom >"${SRC}/medium/m${i}.bin"
done
for i in $(seq 1 1000); do
  head -c $((10 * 1024)) /dev/urandom >"${SRC}/small/s${i}.bin"
done
# Checksums with relative paths, so the same file works from the mount point.
(cd "${SRC}" && find . -type f | sort | xargs sha256sum) >"${RUN_DIR}/sums.txt"

# 4. mkfs, mount, hand the new root directory to the current user, copy.
run_sudo mkfs.ext4 -q "${NBD_DEV}"
run_sudo mount "${NBD_DEV}" "${MNT}"
MOUNTED=1
run_sudo chown "$(id -u):$(id -g)" "${MNT}"
cp -r "${SRC}/." "${MNT}/"
sync

# 5. Unmount and mount again, so the files must come back from the log, not
#    from the page cache; then compare.
run_sudo umount "${MNT}"
MOUNTED=0
run_sudo mount "${NBD_DEV}" "${MNT}"
MOUNTED=1
(cd "${MNT}" && sha256sum --quiet -c "${RUN_DIR}/sums.txt")
echo "OK: $(wc -l <"${RUN_DIR}/sums.txt") files match"
OK=1
# The trap now unmounts, detaches, sends SIGTERM and prints the counters.
