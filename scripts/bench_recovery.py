#!/usr/bin/env python3
"""Helpers for scripts/bench.sh recovery and latency modes (bench-output contract).

Subcommands:

    bench_recovery.py header
        Print the recovery.csv header.
    bench_recovery.py recovery-row --log-bytes N --mode checkpoint|full_scan \\
            --run R --cache-state S server.log
        Print one recovery.csv row from the server's "[info] recovery ..." line.
        Fails if the line is missing or repeated, or if its source= is not the
        mode being measured (a "full_scan" run that found a checkpoint, or a
        "checkpoint" run that fell back to a scan, is not a valid measurement).
    bench_recovery.py checkpoint-duration server.log
        Print the run's single checkpoint duration in microseconds, from the
        "[info] counters ..." line. Fails if checkpoint_duration_count is not 1.
    bench_recovery.py writes-under BYTES
        Print how many 4K writes fit strictly under BYTES of log (each adds
        4,136 bytes: a 40-byte record header plus 4,096 of data).
    bench_recovery.py drop-cache FILE...
        Drop FILE's clean pages from the page cache without root: fsync, then
        posix_fadvise(DONTNEED). dd iflag=nocache does the same thing.

Only the standard library is used.
"""

import argparse
import os
import re
import sys
from pathlib import Path

HEADER = (
    "log_bytes,mode,run,recovery_us,log_bytes_scanned,records_replayed,cache_state"
)

MODES = ("checkpoint", "full_scan")

# Bytes one 4K write adds to the log: 40-byte record header + 4,096 of data.
RECORD_BYTES = 4136


class BenchRecoveryError(Exception):
    """An expected failure: bad input. main() prints it without a traceback."""


def parse_fields(line: str) -> dict[str, int | str]:
    """key=value pairs of a report line. Digits-only values become ints."""
    fields: dict[str, int | str] = {}
    for key, value in re.findall(r"\b([a-z_]+)=(\S+)", line):
        fields[key] = int(value) if value.isdigit() else value
    return fields


def find_line(text: str, tag: str) -> list[str]:
    """Lines holding the "[info] <tag> " report, in order."""
    return [line for line in text.splitlines() if f"[info] {tag} " in line]


def parse_recovery(text: str) -> dict[str, int | str]:
    lines = find_line(text, "recovery")
    if not lines:
        raise BenchRecoveryError("no recovery line in the server log")
    if len(lines) > 1:
        raise BenchRecoveryError("more than one recovery line in the server log")
    return parse_fields(lines[0])


def recovery_row(
    text: str, log_bytes: int, mode: str, run: int, cache_state: str
) -> str:
    fields = parse_recovery(text)
    if fields.get("source") != mode:
        raise BenchRecoveryError(
            f"a {mode} run needs source={mode}, but the server reported "
            f"source={fields.get('source')}"
        )
    try:
        values = [
            log_bytes, mode, run,
            fields["time_us"], fields["log_bytes_scanned"], fields["records_replayed"],
            cache_state,
        ]  # fmt: skip
    except KeyError as err:
        raise BenchRecoveryError(f"recovery line has no {err.args[0]}=") from err
    return ",".join(str(v) for v in values)


def checkpoint_duration_us(text: str) -> int:
    lines = find_line(text, "counters")
    if not lines:
        raise BenchRecoveryError("no counters line in the server log")
    fields = parse_fields(lines[-1])
    try:
        count = fields["checkpoint_duration_count"]
        total = fields["checkpoint_duration_sum_us"]
    except KeyError as err:
        raise BenchRecoveryError(f"counters line has no {err.args[0]}=") from err
    if count != 1:
        raise BenchRecoveryError(
            f"expected exactly one checkpoint, got checkpoint_duration_count={count}"
        )
    return int(total)


def writes_under(target_bytes: int) -> int:
    """Largest n with n * 4,136 < target_bytes."""
    if target_bytes < 1:
        raise BenchRecoveryError("target must be positive")
    return (target_bytes - 1) // RECORD_BYTES


def drop_cache(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError as err:
        raise BenchRecoveryError(f"cannot open {path}: {err.strerror}") from err
    try:
        # DONTNEED only drops clean pages, so write dirty ones out first. fsync
        # works on a read-only descriptor on Linux. offset 0, length 0 = whole file.
        os.fsync(fd)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    except OSError as err:
        raise BenchRecoveryError(f"cannot drop cache of {path}: {err.strerror}") from err
    finally:
        os.close(fd)


def read_text(path: Path) -> str:
    try:
        return path.read_text()
    except OSError as err:
        raise BenchRecoveryError(f"cannot read {path}: {err.strerror}") from err


def positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text!r} is not a positive integer")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("header", help="print the recovery.csv header")

    row = sub.add_parser("recovery-row", help="one recovery.csv row")
    row.add_argument("--log-bytes", type=positive_int, required=True)
    row.add_argument("--mode", choices=MODES, required=True)
    row.add_argument("--run", type=positive_int, required=True)
    row.add_argument("--cache-state", required=True)
    row.add_argument("server_log", type=Path)

    dur = sub.add_parser("checkpoint-duration", help="the single checkpoint's us")
    dur.add_argument("server_log", type=Path)

    writes = sub.add_parser("writes-under", help="4K writes that fit under BYTES")
    writes.add_argument("target_bytes", type=positive_int)

    drop = sub.add_parser("drop-cache", help="drop files from the page cache")
    drop.add_argument("files", nargs="+", type=Path)

    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.command == "header":
            print(HEADER)
        elif args.command == "recovery-row":
            print(
                recovery_row(
                    read_text(args.server_log),
                    args.log_bytes,
                    args.mode,
                    args.run,
                    args.cache_state,
                )
            )
        elif args.command == "checkpoint-duration":
            print(checkpoint_duration_us(read_text(args.server_log)))
        elif args.command == "writes-under":
            print(writes_under(args.target_bytes))
        elif args.command == "drop-cache":
            for path in args.files:
                drop_cache(path)
    except BenchRecoveryError as err:
        print(f"bench_recovery: error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
