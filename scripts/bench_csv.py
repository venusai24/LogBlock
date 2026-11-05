#!/usr/bin/env python3
"""Turn one fio JSON file into one results.csv row.

The columns are fixed; see --header.
scripts/bench.sh calls this once with --header to start results.csv, then once
per run to append a row:

    bench_csv.py --header
    bench_csv.py --target logblock --run 1 raw/logblock-randread-4k-qd1-1.json

Only the standard library is used, so it runs anywhere python3 does.
"""

import argparse
import json
import sys
from pathlib import Path

HEADER = (
    "target,pattern,block_size,iodepth,run,"
    "read_iops,read_bw_kib_s,read_lat_mean_us,read_lat_p99_us,"
    "write_iops,write_bw_kib_s,write_lat_mean_us,write_lat_p99_us"
)

TARGETS = ("logblock", "file")

# fio keys percentiles by a six-decimal string.
P99_KEY = "99.000000"


class BenchCsvError(Exception):
    """An expected failure: bad input. main() prints it without a traceback."""


def load_job(path: Path) -> dict:
    """Return the single job object from a fio --output-format=json file."""
    try:
        text = path.read_text()
    except OSError as err:
        raise BenchCsvError(f"cannot read {path}: {err.strerror}") from err
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as err:
        # fio prints plain text instead of JSON when a job fails to start.
        raise BenchCsvError(f"{path} is not valid JSON: {err}") from err

    jobs = doc.get("jobs") if isinstance(doc, dict) else None
    if not isinstance(jobs, list):
        raise BenchCsvError(f"{path} has no 'jobs' list; is it fio JSON output?")
    # One job file is one job. Several jobs would need per-job rows.
    if len(jobs) != 1:
        raise BenchCsvError(f"expected exactly one job in {path}, found {len(jobs)}")
    job = jobs[0]
    if job.get("error", 0) != 0:
        raise BenchCsvError(f"fio reported error {job['error']} for the job")
    return job


def number(section: dict, direction: str, *keys: str) -> float:
    """Look up section[keys[0]][keys[1]]... and check it is a number."""
    value = section
    for key in keys:
        if not isinstance(value, dict) or key not in value:
            name = ".".join((direction, *keys))
            raise BenchCsvError(f"missing field {name}")
        value = value[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        name = ".".join((direction, *keys))
        raise BenchCsvError(f"field {name} is not a number: {value!r}")
    return value


def direction_fields(job: dict, direction: str) -> list[str]:
    """Four CSV fields for 'read' or 'write', or four empty ones if unused.

    fio always emits both sections, with zeros in the one the job did not use.
    A zero latency would look like a measurement, so an unused direction is
    left empty instead.
    """
    section = job.get(direction)
    if not isinstance(section, dict):
        raise BenchCsvError(f"missing section {direction}")
    if number(section, direction, "total_ios") == 0:
        return ["", "", "", ""]

    iops = number(section, direction, "iops")
    bw_kib_s = number(section, direction, "bw")  # fio's "bw" is KiB/s
    # Completion latency (clat) is used for both mean and p99 because fio only
    # gives percentiles for clat. At queue depth 1 the submit time (slat) is
    # tiny next to it; the difference is not what this baseline is about.
    clat_mean_ns = number(section, direction, "clat_ns", "mean")
    percentiles = section["clat_ns"].get("percentile")
    if not isinstance(percentiles, dict) or P99_KEY not in percentiles:
        raise BenchCsvError(f"missing p99 latency in {direction}.clat_ns.percentile")
    p99_ns = number(percentiles, direction, P99_KEY)

    return [
        f"{iops:.1f}",
        f"{bw_kib_s:.0f}",
        f"{clat_mean_ns / 1000:.1f}",
        f"{p99_ns / 1000:.1f}",
    ]


def job_option(job: dict, name: str) -> str:
    """Read one setting fio echoes back from the job file."""
    options = job.get("job options")
    if not isinstance(options, dict) or name not in options:
        raise BenchCsvError(f"missing job option '{name}'")
    return str(options[name])


def make_row(job: dict, target: str, run: int) -> str:
    # pattern, block size and iodepth come from the job file via fio's own
    # output, so the CSV row cannot disagree with the job that actually ran.
    pattern = job_option(job, "rw")
    block_size = job_option(job, "bs").upper()  # "4k" -> "4K", as in the contract
    iodepth = job_option(job, "iodepth")
    if not iodepth.isdigit():
        raise BenchCsvError(f"job option 'iodepth' is not a number: {iodepth!r}")

    read = direction_fields(job, "read")
    write = direction_fields(job, "write")
    if not any(read) and not any(write):
        raise BenchCsvError("the job did no I/O in either direction")

    fields = [target, pattern, block_size, str(int(iodepth)), str(run), *read, *write]
    return ",".join(fields)


def positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value < 1:
        raise argparse.ArgumentTypeError(f"{text!r} is not a positive integer")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print one results.csv row for one fio JSON file.",
    )
    parser.add_argument(
        "--header", action="store_true", help="print the CSV header and exit"
    )
    parser.add_argument("--target", choices=TARGETS, help="which server was tested")
    parser.add_argument("--run", type=positive_int, help="run number, 1 to 3")
    parser.add_argument("json_file", nargs="?", type=Path, help="fio JSON output")
    args = parser.parse_args(argv)
    if not args.header and (
        args.target is None or args.run is None or args.json_file is None
    ):
        parser.error("--target, --run and json_file are required without --header")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.header:
        print(HEADER)
        return 0
    try:
        print(make_row(load_job(args.json_file), args.target, args.run))
    except BenchCsvError as err:
        print(f"bench_csv: error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
