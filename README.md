# LogBlock

A log-structured block device in C++20, exposed to Linux over NBD through an nbdkit plugin. Writes
are appended to a log file, and an in-memory map finds the latest version of each block.

**Status:** M1 is implemented: the storage engine, the nbdkit plugin, and unit, integration and crash
tests. The ext4 end-to-end run (`scripts/e2e.sh`) and the fio baseline (`scripts/bench.sh`) need
`sudo`, so they are not part of the automated test run. Baseline numbers are below.

## Build and run

Install the dependencies (Fedora), then:

```sh
sudo dnf install gcc-c++ clang clang-tools-extra cmake ninja-build gdb valgrind perf fio \
  nbdkit nbdkit-devel nbd lz4-devel gtest-devel google-benchmark-devel spdlog-devel fmt-devel \
  cli11-devel libnbd-devel google-crc32c-devel libasan libubsan libtsan
```

Then build and test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure   # unit, integration and script tests
scripts/check.sh                              # all three builds, plus the crash test
```

Start the server on a Unix socket, with a new or empty log:

```sh
nbdkit -f -U /path/to/sock ./build/nbdkit-logblock-plugin.so log=/path/to/log cap=2G
```

`scripts/e2e.sh --data-dir <dir>` attaches it to `/dev/nbd0` and runs the ext4 check (needs
`sudo modprobe nbd` first).

## Baseline numbers

Measured with `scripts/bench.sh` on one machine (i7-1360P, 16 GB, Fedora 44, btrfs on NVMe), Release
build, 3 runs per job; the figures are the median run. The comparison is nbdkit's built-in `file`
plugin on the same disk, attached the same way. Raw data and setup:
[`bench/results/20261105T194243Z-878c114/`](bench/results/20261105T194243Z-878c114/).

| Job | Block | Iodepth | LogBlock IOPS | LogBlock p99 (µs) | file IOPS | file p99 (µs) |
|---|---|---|---|---|---|---|
| randread | 4K | 1 | 20,221 | 204 | 13,454 | 510 |
| randread | 4K | 32 | 36,188 | 4,080 | 112,605 | 1,073 |
| randwrite | 4K | 1 | 59,687 | 35 | 41,930 | 41 |
| randwrite | 4K | 32 | 166,758 | 510 | 135,405 | 643 |
| read | 128K | 1 | 7,907 | 1,286 | 5,472 | 2,867 |
| read | 128K | 32 | 11,942 | 6,062 | 11,410 | 6,521 |
| write | 128K | 1 | 6,559 | 313 | 8,127 | 206 |
| write | 128K | 32 | 8,342 | 4,080 | 4,180 | 19,268 |

Read these with care:

- **They exclude the cost of durability.** The fio jobs never flush, so writes land in the page
  cache. Neither side is paying for `fsync`, and these numbers say nothing about flushed-write speed.
- **Iodepth 32 shows queuing, not parallelism.** LogBlock serializes requests, so its latency at
  iodepth 32 mostly measures time in the queue. The `file` plugin's 4K random reads scale far better
  (112,605 vs 36,188 IOPS) because LogBlock handles one request at a time.
- **One machine, one disk, three runs.** Run-to-run spread is visible in the CSV (for example the
  `file` 128K write at iodepth 1 ranges from 7,969 to 11,146 IOPS). The file system under both
  targets is btrfs, which is copy-on-write, so absolute numbers may differ on ext4 or xfs.
- **The recorded `git_tree: DIRTY` in `setup.txt` is a script bug**, noted in the file: the results
  folder itself counted as a change. The tree was otherwise clean.

## Limitations

These describe M1 as designed.

- **Not crash-safe yet.** The block map lives only in memory, so after a crash even flushed data
  cannot be found again, and the server refuses to start on a non-empty log. Recovery arrives in M2.
- **The crash test covers a process kill, not power loss.** Killing the process leaves the page
  cache intact, so the test does not prove that flushed data survives power loss or a kernel crash.
- **Log space is never reclaimed.** Overwrites and trims all use new log space, and when the log
  reaches its cap, writes and trims fail with a no-space error. GC comes later.
- **A failed flush is permanent for that run.** After one, every later flush returns an error until
  the server restarts with a new or empty log. If a failed append cannot be cleaned up, writes,
  trims and flushes all return errors in the same way.
- **One request at a time.** Requests are serialized and I/O is blocking, which limits throughput
  under concurrent I/O.
- **Fixed geometry.** The device is 1 GiB with 4 KiB blocks.
- **Local access only.** The server is reached through a Unix socket, so the device cannot be used
  from another machine.
