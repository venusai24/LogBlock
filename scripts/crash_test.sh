#!/usr/bin/env bash
# Build and run the crash test.
#
#   scripts/crash_test.sh [iterations]      default 20
#   BUILD_DIR=build-asan scripts/crash_test.sh 5
#
# BUILD_DIR must already be configured (cmake -S . -B <dir> ...). Stops at the
# first failed iteration and exits non-zero. Needs no root and no /dev/nbd*.
set -euo pipefail

# Work from the repository root, wherever the script is started from.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

iterations=${1:-20}
build_dir=${BUILD_DIR:-build}

cmake --build "${build_dir}" --target crash_test
# crash_test itself stops at the first failing iteration and prints its seed.
"${build_dir}/tests/crash/crash_test" "${iterations}"
