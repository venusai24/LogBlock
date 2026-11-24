# LogBlock

A log-structured block device in C++20, exposed to Linux over NBD through an nbdkit plugin. Writes
are appended to a log file, and an in-memory map finds the latest version of each block.

**Status:** M1 (storage engine, nbdkit plugin) and M2 (crash recovery and checkpoints) are
implemented, with unit, integration, power-cut simulation and crash tests. The ext4 end-to-end run
(`scripts/e2e.sh`) and the fio baseline (`scripts/bench.sh --only baseline`) need `sudo`, so they are
not part of the automated test run. The benchmark numbers below were measured on M2.

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

Start the server on a Unix socket. The log is created if it is missing, and recovered if it is not
empty:

```sh
nbdkit -f -U /path/to/sock ./build/nbdkit-logblock-plugin.so log=/path/to/log cap=2G
```

`checkpoint_interval=<size>` (default `64M`, at least 4136 bytes) sets how much the log grows between
checkpoints. Checkpoints are kept in `<log>.ckpt`, next to the log. Removing that file only makes the
next start scan the whole log; removing only the log makes the next start refuse, so start over by
removing both.

## Recovery

At startup the server takes an exclusive lock on the log, rebuilds the block map, removes a torn
tail, makes the log durable, and only then listens. It starts from the newest valid checkpoint slot
and replays the records after it, or scans the whole log if no slot is usable. Corruption anywhere
in the scanned region, a checkpoint that disagrees with the log, a valid log over the cap, or a log
that is missing or empty while a valid checkpoint exists all refuse to start, and change nothing.

Startup prints one line on stderr, and a warning for each thing it ignored:

```text
[info] recovery log=<path> source=checkpoint|full_scan|none slot=A|B|- generation=<n> records_replayed=<n> log_bytes_scanned=<n> torn_bytes_removed=<n> unreadable_checkpoints=<n> time_us=<n>
[warning] checkpoint slot=B file=<path> unreadable: <reason>
[warning] torn tail removed log=<path> offset=<n> bytes=<n>
```

- `source` says where the map came from. `checkpoint` means a slot was loaded and only the later
  records were replayed; `full_scan` means the whole log was read; `none` means a new device.
- `log_bytes_scanned` and `records_replayed` are what recovery actually read, so with a checkpoint
  they cover only the log written since it.
- A missing slot prints nothing. An unreadable one (damaged, torn, wrong version, or an I/O error) is
  counted in `unreadable_checkpoints` and ignored.
- `[warning] checkpoint failed ...` can appear at any time while serving. It means a checkpoint could
  not be written; requests still succeed, and checkpoints stay off until restart.

On SIGTERM the server prints a counters line, which now also carries `checkpoints_written`,
`checkpoints_failed`, `checkpoints_unreadable` and the `checkpoint_duration_*` fields. No checkpoint
is written at shutdown.

`scripts/e2e.sh --data-dir <dir>` attaches it to `/dev/nbd0` and runs the ext4 check (needs
`sudo modprobe nbd` first).

## Baseline numbers

Measured with `scripts/bench.sh` on one machine (i7-1360P, 16 GB, Fedora 44, btrfs on NVMe), Release
build, 3 runs per job; each cell is the median of the 3. The comparison is nbdkit's built-in `file`
plugin on the same disk, attached the same way. LogBlock is shown twice: on M2 (this branch), and on
the M1 code (commit 878c114) run in the same session, because the machine's speed changed between
days. The M1 code run yesterday did 6,720 IOPS on 128K writes at iodepth 1 and the same code did 3,625 today, so
only the two same-session columns can be compared with each other. Raw data and setup:
[M2 and `file`](bench/results/20261106T114938Z-9db9f29/) and
[M1 code](bench/results/20261106T120406Z-878c114/). Yesterday's M1 runs are kept in
[`20261105T194243Z-878c114`](bench/results/20261105T194243Z-878c114/) and
[`20261105T195354Z-878c114`](bench/results/20261105T195354Z-878c114/) and are not used below.

| Job | Block | Iodepth | M2 IOPS | M2 p99 (µs) | M1 IOPS | M1 p99 (µs) | file IOPS | file p99 (µs) |
|---|---|---|---|---|---|---|---|---|
| randread | 4K | 1 | 22,915 | 189 | 25,245 | 165 | 21,694 | 234 |
| randread | 4K | 32 | 35,540 | 4,047 | 40,231 | 3,948 | 110,703 | 1,057 |
| randwrite | 4K | 1 | 46,612 | 46 | 51,644 | 34 | 28,618 | 80 |
| randwrite | 4K | 32 | 111,456 | 553 | 122,727 | 586 | 92,827 | 1,036 |
| read | 128K | 1 | 10,064 | 1,028 | 10,139 | 922 | 6,450 | 2,441 |
| read | 128K | 32 | 12,507 | 4,555 | 13,430 | 4,686 | 15,545 | 5,210 |
| write | 128K | 1 | 3,084 | 522 | 3,625 | 477 | 6,024 | 255 |
| write | 128K | 32 | 3,772 | 45,875 | 4,882 | 12,124 | 5,214 | 15,139 |

The `file` columns come from the M2 run. The M1 run's own `file` figures differ, by up to 58% (4K
random reads at iodepth 1), which is the size of the noise in these measurements.

Read these with care:

- **They exclude the cost of durability.** The fio jobs never flush, so writes land in the page
  cache. Neither side is paying for `fsync`, and these numbers say nothing about flushed-write speed.
- **M2 is slower on writes, and checkpoints are the likely cause.** 128K writes lose 15% (iodepth 1) and
  23% (iodepth 32) against the M1 code in the same session, 4K random writes about 10%, and the p99
  of 128K writes at iodepth 32 rose from about 12 ms to about 46 ms. A separate test (128K sequential
  writes over fio's `nbd` engine, not recorded in `bench/results/`) with checkpoints switched off
  by a very large interval ran about 26% faster than the default 64 MiB interval (the default was
  21% slower), with the checkpoints taking about 0.4 s of a 1.7 s run. That fits the writes above, but it is one test.
- **Random reads are 9% to 12% lower on M2 and that is not explained.** M2 does not change the read
  path. M1's own two runs differed by 7% to 8% on random reads, so this may be noise.
- **Iodepth 32 shows queuing, not parallelism.** LogBlock serializes requests, so its latency at
  iodepth 32 mostly measures time in the queue. The `file` plugin's 4K random reads scale far better
  (about 3x at iodepth 32) because LogBlock handles one request at a time.
- **One machine, one disk, three runs per measurement, and the M1 run came about 15 minutes after the
  M2 run.** The file system under both targets is btrfs, which is copy-on-write, so absolute numbers
  may differ on ext4 or xfs.
- **The recorded `git_tree: DIRTY` in the M1 `setup.txt` is a script bug**, noted in the file: the
  results folder itself counted as a change. The tree was otherwise clean.

## Recovery time

Restart time of a log just under 100 MiB and one just under 1 GiB, built with 4K random writes at the
default 64 MiB checkpoint interval, with the log and checkpoint file dropped from the page cache first
(the disk's own cache is not dropped). Each cell is the median of 3 runs, with the range in
brackets; `recovery_us` is the server's own report and excludes process startup. Raw data and setup:
[`bench/results/20261106T114725Z-9db9f29/`](bench/results/20261106T114725Z-9db9f29/).

| Log | Mode | Recovery time | Log bytes scanned | Records replayed |
|---|---|---|---|---|
| ~100 MiB | checkpoint | 28.6 ms (25.5 to 30.6) | 37,745,136 | 9,126 |
| ~100 MiB | full_scan | 52.6 ms (48.1 to 53.7) | 104,855,872 | 25,352 |
| ~1 GiB | checkpoint | 44.4 ms (41.8 to 58.5) | 67,077,648 | 16,218 |
| ~1 GiB | full_scan | 413.2 ms (393.7 to 417.6) | 1,073,738,688 | 259,608 |

With a checkpoint, the 1 GiB log scanned 67,077,648 bytes, under one interval plus one record
(67,113,000). On this run a checkpoint made the 1 GiB restart about 9 times faster, and the 100 MiB one about
1.8 times.

## Latency across a checkpoint

4K random writes at iodepth 32 from an empty log, with no FLUSH, so each run has exactly one checkpoint
(at the default 64 MiB interval). The boundary window is the 1,000 writes around the one that crosses
the interval; the control window is 1,000 writes with no checkpoint in them. Raw data and setup:
[`bench/results/20261106T114732Z-9db9f29/`](bench/results/20261106T114732Z-9db9f29/).

| Run | Boundary p99 | Boundary max | Checkpoint duration | Control p99 |
|---|---|---|---|---|
| 1 | 196.4 ms | 196.6 ms | 196.1 ms | 233 µs |
| 2 | 34.6 ms | 34.7 ms | 34.3 ms | 242 µs |
| 3 | 36.0 ms | 36.1 ms | 35.4 ms | 212 µs |

The boundary p99 includes queueing behind the checkpoint, so it is not the checkpoint's own duration;
the checkpoint duration column is the engine's counter. Run 1's checkpoint took more than 5 times longer
than runs 2 and 3.

Three more invocations of the same mode (9 runs, same disk and default interval, from a git tree with
uncommitted changes outside the engine, so the SHA does not describe the build exactly) did not
reproduce it. Checkpoint durations in ms, runs 1 to 3 of each invocation:

| Invocation | Run 1 | Run 2 | Run 3 | Raw data |
|---|---|---|---|---|
| A | 25.5 | 25.1 | 24.0 | [`20261113T203647Z-9e6ae72/`](bench/results/20261113T203647Z-9e6ae72/) |
| B | 39.8 | 28.1 | 25.0 | [`20261113T203650Z-9e6ae72/`](bench/results/20261113T203650Z-9e6ae72/) |
| C | 25.4 | 26.9 | 27.1 | [`20261113T203700Z-9e6ae72/`](bench/results/20261113T203700Z-9e6ae72/) |

These 9 runs range from 24.0 to 39.8 ms. Run 1 was not consistently slower, and none came near 196 ms.
The engine does create `<log>.ckpt` during the first checkpoint (and syncs the directory) while later
ones overwrite a slot in place, but in these runs that cost was not visible. Every run starts from a
fresh log, so each run's checkpoint is a "first" one; that is why the creation cost cannot explain a
difference between run 1 and runs 2 and 3. The 196 ms outlier is therefore unexplained: it was not
reproduced, and the hypothesis that first-checkpoint file creation causes it is not supported by these
numbers.

## Limitations

- **Power loss is tested only by simulation.** The I/O layer can cut power at a seeded point and
  keep only what was synced, and the tests check that flushed data survives. That is not real
  hardware, and the simulation does not cover writes that reach the disk out of order. A process
  kill, which the crash test also uses, leaves the page cache intact.
- **A partly written final record refuses startup.** A short or zero-filled tail that starts at a
  record boundary is treated as a torn write and removed. A final record that is partly written in
  any other way is treated as corruption, and the server refuses to start rather than guess.
- **Flushed records at the end of the log that were overwritten with zeros** are detected only if a
  checkpoint covers them. Without one, they look like a torn tail and are removed.
- **Corruption inside a checkpoint-covered region does not stop startup.** Recovery does not rescan
  what a checkpoint covers, so damage there shows up as a read error on the affected block, not as a
  refusal.
- **Log space is never reclaimed.** Overwrites and trims all use new log space, and when the log
  reaches its cap, writes and trims fail with a no-space error. GC comes later.
- **A failed flush is permanent for that run.** After one, every later flush returns an error until
  the server restarts, and a restart recovers the log. If a failed append cannot be cleaned up,
  writes, trims and flushes all return errors in the same way. A failed checkpoint is not like this:
  it only turns checkpoints off until restart.
- **Deleting the log while a server runs is not guarded.** The lock belongs to the open file, so
  the old server keeps its lock on the deleted file. A new server then finds no log, reads the
  checkpoint slots without any lock held (this is safe against two servers starting together,
  because `O_EXCL` settles that race), creates a fresh log and runs beside the old one, and both
  write the same `<log>.ckpt`. Revisit this when MiniHCI manages the disks.
- **The log must be on a local file system.** The lock is a `flock`, which is not reliable on every
  network file system.
- **At iodepth 32, p99 includes queueing behind a checkpoint** wherever the checkpoint falls. A
  checkpoint writes about 2 MiB and fsyncs twice while requests wait, so a high percentile there is
  not the cost of one write.
- **One request at a time.** Requests are serialized and I/O is blocking, which limits throughput
  under concurrent I/O.
- **Fixed geometry.** The device is 1 GiB with 4 KiB blocks.
- **Local access only.** The server is reached through a Unix socket, so the device cannot be used
  from another machine.
