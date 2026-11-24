#!/usr/bin/env python3
"""Turn one fio per-I/O latency log into the two latency.csv rows of one run.

scripts/bench.sh calls this once with --header to start latency.csv, then once
per run to append two rows (boundary, then control):

    bench_latency.py --header
    bench_latency.py --interval-bytes 67108864 --run 1 --iodepth 32 \\
        --checkpoint-duration-us 8120 raw/latency-1_lat.1.log

Input: the log fio writes with --write_lat_log and no log_avg_msec, one row per
I/O: "time_msec, latency, direction, bs, offset[, priority]". Decisions:

- Units. fio 3.x logs latency in nanoseconds; the CSV columns are
  microseconds with one decimal, as in bench_csv.py. Either the lat or clat
  log works; the script does not care which one it is given.
- Writes only. Rows whose direction is not 1 (reads, trims) are skipped, so
  they never count towards n or the window.
- Order. fio logs rows in completion order. n counts write rows from 0 in that
  order. At queue depth 32 the order can differ from submission order by up to
  32 writes, which a window of 1,000 absorbs (research R8).
- Crossing write. The log grows 4,136 bytes per 4K write (40-byte record
  header plus 4,096 bytes of data). The checkpoint fires on the first write
  with (n + 1) * 4,136 >= interval, so n = ceil(interval / 4,136) - 1.
- Windows. A window of W writes centred on write c is writes [c - W/2,
  c + W/2). The boundary window is centred on the crossing write n, the
  control window on n // 2. The control window is checked to start at write 0
  or later, which also keeps it clear of the crossing.
- p99. Nearest rank: the value at index ceil(0.99 * N) - 1 of the sorted list.
  Integer arithmetic is used, so no floating-point rounding can move the rank.

Only the standard library is used, so it runs anywhere python3 does.
"""

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

HEADER = (
    "window,run,iodepth,window_writes,"
    "write_lat_p99_us,write_lat_max_us,checkpoint_duration_us"
)

# Bytes one 4K write adds to the log: 40-byte record header + 4,096 of data.
RECORD_BYTES = 4136

DEFAULT_WINDOW_WRITES = 1000

# fio's direction column: 0 = read, 1 = write, 2 = trim.
DIR_WRITE = 1


class BenchLatencyError(Exception):
    """An expected failure: bad input. main() prints it without a traceback."""


@dataclass(frozen=True)
class WindowRow:
    window: str  # "boundary" or "control"
    run: int
    iodepth: int
    first_write: int  # index of the window's first write, for tests
    count: int
    p99_ns: int
    max_ns: int
    checkpoint_duration_us: int | None  # None on control rows


def p99_nearest_rank(values: list[int]) -> int:
    """Nearest-rank 99th percentile: sorted[ceil(0.99 * N) - 1]."""
    if not values:
        raise BenchLatencyError("cannot take a percentile of zero writes")
    # ceil(99 * N / 100) in integers: -(-a // b) is ceil(a / b).
    rank = -(-99 * len(values) // 100)
    return sorted(values)[rank - 1]


def crossing_index(interval_bytes: int) -> int:
    """Index n (from 0) of the first write with (n + 1) * 4,136 >= interval."""
    if interval_bytes < 1:
        raise BenchLatencyError(f"interval must be positive, got {interval_bytes}")
    return -(-interval_bytes // RECORD_BYTES) - 1


def read_write_latencies(path: Path) -> list[int]:
    """Latencies in ns of the write rows of a fio latency log, in log order."""
    try:
        text = path.read_text()
    except OSError as err:
        raise BenchLatencyError(f"cannot read {path}: {err.strerror}") from err

    latencies = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 3:
            raise BenchLatencyError(
                f"{path} line {number}: expected 'time, latency, direction, ...'"
            )
        try:
            latency_ns = int(fields[1])
            direction = int(fields[2])
        except ValueError as err:
            raise BenchLatencyError(
                f"{path} line {number}: latency and direction must be integers"
            ) from err
        if direction == DIR_WRITE:
            latencies.append(latency_ns)
    return latencies


def make_window(
    name: str,
    writes: list[int],
    centre: int,
    window_writes: int,
    run: int,
    iodepth: int,
    checkpoint_duration_us: int | None,
) -> WindowRow:
    start = centre - window_writes // 2
    end = start + window_writes
    if start < 0:
        raise BenchLatencyError(
            f"the {name} window would start at write {start}; "
            f"the interval is too small for {window_writes} writes"
        )
    if end > len(writes):
        raise BenchLatencyError(
            f"log too short for the {name} window: need {end} writes, "
            f"found {len(writes)}"
        )
    chosen = writes[start:end]
    return WindowRow(
        window=name,
        run=run,
        iodepth=iodepth,
        first_write=start,
        count=len(chosen),
        p99_ns=p99_nearest_rank(chosen),
        max_ns=max(chosen),
        checkpoint_duration_us=checkpoint_duration_us,
    )


def window_rows(
    writes: list[int],
    interval_bytes: int,
    window_writes: int,
    run: int,
    iodepth: int,
    checkpoint_duration_us: int,
) -> list[WindowRow]:
    """The boundary row (with the checkpoint duration) and the control row."""
    crossing = crossing_index(interval_bytes)
    boundary = make_window(
        "boundary", writes, crossing, window_writes, run, iodepth,
        checkpoint_duration_us,
    )  # fmt: skip
    # Halfway to the crossing; the checkpoint has not run by then.
    control = make_window(
        "control", writes, crossing // 2, window_writes, run, iodepth, None
    )
    return [boundary, control]


def format_row(row: WindowRow) -> str:
    duration = "" if row.checkpoint_duration_us is None else str(row.checkpoint_duration_us)
    return ",".join(
        [
            row.window,
            str(row.run),
            str(row.iodepth),
            str(row.count),
            f"{row.p99_ns / 1000:.1f}",
            f"{row.max_ns / 1000:.1f}",
            duration,
        ]
    )


def positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text!r} is not a positive integer")
    return value


def non_negative_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = -1
    if value < 0:
        raise argparse.ArgumentTypeError(f"{text!r} is not a non-negative integer")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print the boundary and control latency.csv rows for one "
        "fio per-I/O latency log.",
    )
    parser.add_argument(
        "--header", action="store_true", help="print the CSV header and exit"
    )
    parser.add_argument(
        "--interval-bytes",
        type=positive_int,
        help="checkpoint_interval the server ran with, in bytes",
    )
    parser.add_argument("--run", type=positive_int, help="run number, 1 to 3")
    parser.add_argument("--iodepth", type=positive_int, help="fio iodepth")
    parser.add_argument(
        "--checkpoint-duration-us",
        type=non_negative_int,
        help="the run's single checkpoint, from the engine's counters",
    )
    parser.add_argument(
        "--window-writes",
        type=positive_int,
        default=DEFAULT_WINDOW_WRITES,
        help=f"writes per window (default {DEFAULT_WINDOW_WRITES})",
    )
    parser.add_argument("log_file", nargs="?", type=Path, help="fio latency log")
    args = parser.parse_args(argv)
    if not args.header:
        required = {
            "--interval-bytes": args.interval_bytes,
            "--run": args.run,
            "--iodepth": args.iodepth,
            "--checkpoint-duration-us": args.checkpoint_duration_us,
            "log_file": args.log_file,
        }
        missing = [name for name, value in required.items() if value is None]
        if missing:
            parser.error(f"{', '.join(missing)} required without --header")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.header:
        print(HEADER)
        return 0
    try:
        writes = read_write_latencies(args.log_file)
        rows = window_rows(
            writes,
            args.interval_bytes,
            args.window_writes,
            args.run,
            args.iodepth,
            args.checkpoint_duration_us,
        )
    except BenchLatencyError as err:
        print(f"bench_latency: error: {err}", file=sys.stderr)
        return 1
    # Print only after both rows exist, so a failure leaves stdout empty.
    for row in rows:
        print(format_row(row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
