#!/usr/bin/env bash
# Shared helpers for scripts/e2e.sh and scripts/bench.sh. Source it, do not run it:
#
#   source "$(dirname "${BASH_SOURCE[0]}")/lib_nbd.sh"
#
# The caller sets `set -euo pipefail` itself. Everything here uses /dev/nbd0 and
# no other device. sudo is used only by run_sudo, and run_sudo echoes the exact
# command first, so you see every privileged command before it runs.

NBD_DEV=/dev/nbd0
NBD_SYS=/sys/block/nbd0

# Print a message to stderr and stop.
die() {
  echo "error: $*" >&2
  exit 1
}

# Echo a command, then run it with sudo. Used for every privileged step.
run_sudo() {
  echo "+ sudo $*" >&2  # stderr, so $(run_sudo ...) captures only the command's output
  sudo "$@"
}

# parse_data_dir_arg "$@": the only option both scripts share is --data-dir.
# Sets DATA_DIR (absolute path). Extra arguments are an error, so a typo such as
# --datadir cannot be silently ignored.
parse_data_dir_arg() {
  DATA_DIR=""
  while [[ $# -gt 0 ]]; do
    case $1 in
      --data-dir)
        [[ $# -ge 2 ]] || die "--data-dir needs a directory"
        DATA_DIR=$2
        shift 2
        ;;
      *) die "unknown argument: $1 (use --help)" ;;
    esac
  done
}

# check_environment: the refusals shared by both scripts. Nothing here changes
# the system or uses sudo; it only reads.
#   - --data-dir given, an existing directory, and not on tmpfs/ramfs
#   - the nbd kernel module is loaded (we never load it ourselves)
#   - /dev/nbd0 exists and is not already attached to a server
check_environment() {
  [[ -n "${DATA_DIR}" ]] || die "--data-dir <dir> is required"
  [[ -d "${DATA_DIR}" ]] || die "--data-dir is not a directory: ${DATA_DIR}"
  DATA_DIR=$(realpath "${DATA_DIR}")

  # tmpfs lives in RAM, so a disk benchmark or a "survives on disk" test there
  # would be meaningless. stat -f reports the file system type of the path.
  local fstype
  fstype=$(stat -f -c %T "${DATA_DIR}")
  if [[ "${fstype}" == tmpfs || "${fstype}" == ramfs ]]; then
    die "--data-dir ${DATA_DIR} is on ${fstype}; use a directory on a real disk"
  fi

  # lsmod lists loaded modules, one per line, name first.
  if ! lsmod | awk '{print $1}' | grep -qx nbd; then
    die "the nbd kernel module is not loaded; run: sudo modprobe nbd"
  fi

  [[ -b "${NBD_DEV}" ]] || die "${NBD_DEV} does not exist"
  # While a client is attached, the kernel exposes the client's pid here.
  if [[ -e "${NBD_SYS}/pid" ]]; then
    die "${NBD_DEV} is already in use (client pid $(cat "${NBD_SYS}/pid")); detach it first"
  fi
  # A mounted /dev/nbd0 counts as in use even if the pid file is gone.
  if grep -q "^${NBD_DEV} " /proc/mounts; then
    die "${NBD_DEV} is mounted; unmount it first"
  fi
}

# make_run_dir <name prefix>: create a private per-run directory under DATA_DIR
# and set RUN_DIR and SOCK. mktemp -d already creates mode 0700; the chmod makes
# the requirement explicit. A Unix socket path must fit in 107 bytes
# (sockaddr_un.sun_path is 108 including the final NUL), so check the length.
make_run_dir() {
  RUN_DIR=$(mktemp -d -p "${DATA_DIR}" "$1.XXXXXX")
  chmod 0700 "${RUN_DIR}"
  SOCK="${RUN_DIR}/nbd.sock"
  if ((${#SOCK} > 107)); then
    die "socket path is ${#SOCK} characters, over the 107 limit; use a shorter --data-dir"
  fi
}

# start_server <stderr log> <command...>: run the server in the background
# (nbdkit -f stays in the foreground, so $! is the real nbdkit pid), then wait
# until its socket exists. Sets SERVER_PID.
start_server() {
  local log=$1
  shift
  rm -f -- "${SOCK}"  # never mistake a stale socket from an earlier run for the new one
  echo "+ $* (stderr -> ${log})"
  "$@" 2>"${log}" &
  SERVER_PID=$!
  local i
  for ((i = 0; i < 100; i++)); do # up to 10 s
    [[ -S "${SOCK}" ]] && return 0
    kill -0 "${SERVER_PID}" 2>/dev/null || {
      cat "${log}" >&2
      die "server exited before creating ${SOCK}"
    }
    sleep 0.1
  done
  die "timed out waiting for ${SOCK}"
}

# stop_server: SIGTERM is nbdkit's clean shutdown (the plugin prints counters).
# `wait` collects the exit status; a signal exit is expected, so ignore it.
stop_server() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
  fi
  # nbdkit can leave its socket file behind after SIGTERM. A stale file makes the next
  # server fail with "Address already in use", and also fools start_server's wait
  # for the socket, so remove it.
  rm -f -- "${SOCK}"
}

# attach_nbd: connect /dev/nbd0 to the server's socket with a 4 KiB block size.
# ATTACHED is set before the command so the cleanup trap detaches even if
# nbd-client fails half way (detach is harmless when nothing is attached).
attach_nbd() {
  ATTACHED=1
  run_sudo nbd-client -unix "${SOCK}" -b 4096 "${NBD_DEV}"
}

# detach_nbd: disconnect, then wait until the kernel says the device is free.
detach_nbd() {
  if [[ "${ATTACHED:-0}" == 1 ]]; then
    run_sudo nbd-client -d "${NBD_DEV}" || true
    local i
    for ((i = 0; i < 100; i++)); do
      [[ -e "${NBD_SYS}/pid" ]] || break
      sleep 0.1
    done
    ATTACHED=0
  fi
}
