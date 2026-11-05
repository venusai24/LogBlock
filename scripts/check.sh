#!/usr/bin/env bash
# Build and test everything, in all three build types. This is the only test
# command CI runs (see .github/workflows/ci.yml), so whatever later tasks add
# here (integration tests, the crash test) runs on every push too.
#
#   build       Debug, no sanitizer: all tests
#   build-asan  Debug, AddressSanitizer + UBSan: all tests (integration with LD_PRELOAD)
#   build-tsan  Debug, ThreadSanitizer: unit tests only
#
# Exits non-zero on the first build error, test failure or sanitizer report.
set -euo pipefail

# Work from the repository root, wherever the script is started from.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# A sanitizer report must fail the test, not just print. UBSan is already built
# with -fno-sanitize-recover; these make ASan and TSan stop at the first report.
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}"

# check <build dir> <sanitizer or ""> [extra ctest arguments...]
check() {
  local dir=$1 sanitize=$2
  shift 2
  echo
  echo "=== ${dir}: configure, build, test ${sanitize:+(SANITIZE=${sanitize})}"
  # SANITIZE is always passed, so a build directory can never keep an old value.
  cmake -S . -B "${dir}" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE="${sanitize}"
  cmake --build "${dir}"
  # --no-tests=error: a build that finds zero tests is a failure, not a pass.
  ctest --test-dir "${dir}" --output-on-failure --no-tests=error --timeout 120 "$@"
}

# Plain build: everything, including the integration tests that start a real nbdkit.
check build ""
# The kill test: SIGKILL a real server mid-run and scan the log. It needs no
# root and no /dev/nbd*, so it runs in CI too. A failure makes check.sh exit non-zero.
BUILD_DIR=build scripts/crash_test.sh

# ASan build: the unit tests run as usual. The integration tests load the plugin into
# nbdkit, which is not built with ASan, so the ASan runtime must be first in nbdkit's
# library list: LD_PRELOAD. Leak checking is off for that run, because
# nbdkit itself leaves small allocations behind at exit. libasan.so is a linker
# script, so ask gcc for the real shared library.
check build-asan address -LE integration
echo
echo "=== build-asan: integration tests with the ASan runtime preloaded"
asan_lib=$(gcc -print-file-name=libasan.so.8)
if [[ ! -f "${asan_lib}" ]]; then
  echo "check.sh: cannot find libasan.so.8 (gcc printed '${asan_lib}')" >&2
  exit 1
fi
LD_PRELOAD="${asan_lib}" ASAN_OPTIONS="halt_on_error=1:detect_leaks=0" \
  ctest --test-dir build-asan --output-on-failure --no-tests=error --timeout 120 -L integration
LD_PRELOAD="${asan_lib}" ASAN_OPTIONS="halt_on_error=1:detect_leaks=0" \
  BUILD_DIR=build-asan scripts/crash_test.sh

# TSan runs only the unit tests: nbdkit is not built with TSan.
check build-tsan thread -L unit

echo
echo "check.sh: all builds and tests passed"
