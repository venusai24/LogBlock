#!/usr/bin/env bash
# Build and run the crash test.
#
#   scripts/crash_test.sh [iterations]      default 200 (the full run)
#   BUILD_DIR=build-asan scripts/crash_test.sh 200
#
# Even seeds kill a real server; odd seeds cut power in the simulated I/O layer. The run counts
# how many iterations reached each recovery path, outcome and crash-point class, and fails if
# any count is zero, so a short run (under about 100 iterations) can fail for that reason alone.
# Replay one failing seed with: scripts/crash_test.sh 1 <seed>
#
# BUILD_DIR must already be configured (cmake -S . -B <dir> ...). Stops at the
# first failed iteration and exits non-zero. Needs no root and no /dev/nbd*.
set -euo pipefail

# Work from the repository root, wherever the script is started from.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

iterations=${1:-200}
shift || true
# An optional second argument is the starting seed, for replaying one iteration.
seed_args=("$@")
build_dir=${BUILD_DIR:-build}

cmake --build "${build_dir}" --target crash_test
# crash_test itself stops at the first failing iteration and prints its seed.
"${build_dir}/tests/crash/crash_test" "${iterations}" "${seed_args[@]}"
