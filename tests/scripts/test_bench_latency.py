"""Tests for scripts/bench_latency.py.

Run from anywhere with:  python3 -I tests/scripts/test_bench_latency.py
or from this directory:  python3 -m unittest test_bench_latency

The command line is run as a child process, exactly as scripts/bench.sh will
run it. The pure functions are imported and called directly.

Sample log data/fio_lat_mixed_60w.log (made by hand, fio 3.x row layout
"time_msec, latency_ns, direction, bs, offset, priority"):

  - 60 write rows (direction 1) and 6 read rows (direction 0, 99 ms each).
    Write i has latency 100000 + 1000 * i ns, except write 12 = 250000 ns
    and write 39 = 5000000 ns. The reads sit inside both windows below, so
    they would change the answer if they were counted.
  - interval 40 * 4136 = 165440 bytes, so the crossing write is n = 39
    (the first n with (n + 1) * 4136 >= 165440).
  - window of 20 writes: boundary = writes 29..48 (centre 39), control =
    writes 9..28 (centre 39 // 2 = 19).
  - boundary max = write 39 = 5000.0 us. control max = write 12 = 250.0 us.
    With 20 values the nearest-rank p99 is rank ceil(19.8) = 20, the max.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "scripts" / "bench_latency.py"
DATA = HERE / "data"
SAMPLE = DATA / "fio_lat_mixed_60w.log"

# -I drops the script directory from sys.path, so add it back to import.
sys.path.insert(0, str(SCRIPT.parent))
import bench_latency  # noqa: E402

HEADER = (
    "window,run,iodepth,window_writes,"
    "write_lat_p99_us,write_lat_max_us,checkpoint_duration_us"
)

RECORD = 4136  # bytes one 4K write adds to the log: 40-byte header + 4096


def run_script(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def sample_args(**overrides: str) -> list[str]:
    args = {
        "--interval-bytes": str(40 * RECORD),
        "--window-writes": "20",
        "--run": "2",
        "--iodepth": "32",
        "--checkpoint-duration-us": "12345",
    }
    args.update(overrides)
    flat = [x for pair in args.items() for x in pair]
    return [*flat, str(SAMPLE)]


def write_log(directory: str, latencies_ns: list[int], reads: bool = False) -> Path:
    """Write a fio-style log with one write row per latency."""
    lines = []
    for i, lat in enumerate(latencies_ns):
        lines.append(f"{i + 1}, {lat}, 1, 4096, {i * 4096}, 0")
        if reads:
            lines.append(f"{i + 1}, 77000000, 0, 4096, 0, 0")
    path = Path(directory) / "lat.log"
    path.write_text("\n".join(lines) + "\n")
    return path


class NearestRankTest(unittest.TestCase):
    def test_p99_of_1_to_100_is_99(self) -> None:
        self.assertEqual(bench_latency.p99_nearest_rank(list(range(1, 101))), 99)

    def test_p99_of_1_to_101_is_100(self) -> None:
        # rank = ceil(0.99 * 101) = ceil(99.99) = 100
        self.assertEqual(bench_latency.p99_nearest_rank(list(range(1, 102))), 100)

    def test_p99_of_1000_values_is_the_990th(self) -> None:
        values = list(range(1000, 0, -1))  # unsorted on purpose
        self.assertEqual(bench_latency.p99_nearest_rank(values), 990)

    def test_p99_of_fewer_than_100_values_is_the_max(self) -> None:
        self.assertEqual(bench_latency.p99_nearest_rank([5, 9, 1, 7]), 9)

    def test_p99_of_one_value(self) -> None:
        self.assertEqual(bench_latency.p99_nearest_rank([42]), 42)

    def test_p99_of_nothing_is_an_error(self) -> None:
        with self.assertRaises(bench_latency.BenchLatencyError):
            bench_latency.p99_nearest_rank([])


class CrossingTest(unittest.TestCase):
    def test_exact_multiple_of_record_size(self) -> None:
        # (n + 1) * 4136 >= 40 * 4136 first holds at n = 39.
        self.assertEqual(bench_latency.crossing_index(40 * RECORD), 39)

    def test_one_byte_over_needs_one_more_write(self) -> None:
        self.assertEqual(bench_latency.crossing_index(40 * RECORD + 1), 40)

    def test_one_byte_under_still_needs_the_same_write(self) -> None:
        self.assertEqual(bench_latency.crossing_index(40 * RECORD - 1), 39)

    def test_first_write_can_cross(self) -> None:
        self.assertEqual(bench_latency.crossing_index(1), 0)

    def test_64_mib_interval(self) -> None:
        # 67108864 / 4136 = 16225.3, so ceil is 16226 and n = 16225.
        self.assertEqual(bench_latency.crossing_index(64 * 1024 * 1024), 16225)

    def test_interval_must_be_positive(self) -> None:
        with self.assertRaises(bench_latency.BenchLatencyError):
            bench_latency.crossing_index(0)


class ParseTest(unittest.TestCase):
    def test_reads_are_skipped_and_write_order_is_kept(self) -> None:
        writes = bench_latency.read_write_latencies(SAMPLE)
        self.assertEqual(len(writes), 60)
        self.assertEqual(writes[0], 100000)
        self.assertEqual(writes[12], 250000)
        self.assertEqual(writes[39], 5000000)
        self.assertNotIn(99000000, writes)

    def test_bad_row_names_the_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.log"
            path.write_text("1, 1000, 1, 4096, 0, 0\n2, fast, 1, 4096, 0, 0\n")
            with self.assertRaises(bench_latency.BenchLatencyError) as ctx:
                bench_latency.read_write_latencies(path)
        self.assertIn("line 2", str(ctx.exception))

    def test_too_few_fields_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.log"
            path.write_text("1, 1000\n")
            with self.assertRaises(bench_latency.BenchLatencyError):
                bench_latency.read_write_latencies(path)

    def test_blank_lines_are_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "blank.log"
            path.write_text("\n1, 1000, 1, 4096, 0, 0\n\n")
            self.assertEqual(bench_latency.read_write_latencies(path), [1000])


class WindowTest(unittest.TestCase):
    def test_boundary_and_control_slices(self) -> None:
        writes = list(range(60))
        rows = bench_latency.window_rows(
            writes, 40 * RECORD, window_writes=20, run=1, iodepth=32,
            checkpoint_duration_us=7,
        )
        self.assertEqual([r.window for r in rows], ["boundary", "control"])
        self.assertEqual(rows[0].first_write, 29)
        self.assertEqual(rows[1].first_write, 9)
        self.assertEqual(rows[0].count, 20)
        self.assertEqual(rows[1].count, 20)

    def test_log_without_room_after_the_crossing_is_too_short(self) -> None:
        # Boundary window is writes 29..48, so 48 writes are one short.
        with self.assertRaises(bench_latency.BenchLatencyError) as ctx:
            bench_latency.window_rows(
                list(range(48)), 40 * RECORD, window_writes=20, run=1,
                iodepth=32, checkpoint_duration_us=7,
            )
        self.assertIn("too short", str(ctx.exception))

    def test_exactly_enough_writes_is_accepted(self) -> None:
        rows = bench_latency.window_rows(
            list(range(49)), 40 * RECORD, window_writes=20, run=1, iodepth=32,
            checkpoint_duration_us=7,
        )
        self.assertEqual(len(rows), 2)

    def test_crossing_too_early_for_a_centred_boundary_window(self) -> None:
        # Crossing at n = 4 with a window of 20 would start before write 0.
        with self.assertRaises(bench_latency.BenchLatencyError):
            bench_latency.window_rows(
                list(range(100)), 5 * RECORD, window_writes=20, run=1,
                iodepth=32, checkpoint_duration_us=7,
            )

    def test_crossing_too_early_for_a_centred_control_window(self) -> None:
        # n = 24, window 30: boundary starts at write 9, which fits, but the
        # control centre is write 12, so its window would start at -3.
        with self.assertRaises(bench_latency.BenchLatencyError) as ctx:
            bench_latency.window_rows(
                list(range(200)), 25 * RECORD, window_writes=30, run=1,
                iodepth=32, checkpoint_duration_us=7,
            )
        self.assertIn("control", str(ctx.exception))


class FormatTest(unittest.TestCase):
    def test_sample_rows_exact(self) -> None:
        rows = bench_latency.window_rows(
            bench_latency.read_write_latencies(SAMPLE),
            40 * RECORD,
            window_writes=20,
            run=2,
            iodepth=32,
            checkpoint_duration_us=12345,
        )
        self.assertEqual(
            [bench_latency.format_row(r) for r in rows],
            [
                "boundary,2,32,20,5000.0,5000.0,12345",
                "control,2,32,20,250.0,250.0,",
            ],
        )


class CommandLineTest(unittest.TestCase):
    def test_header_flag_prints_exact_contract_header(self) -> None:
        result = run_script("--header")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, HEADER + "\n")

    def test_sample_prints_both_rows(self) -> None:
        result = run_script(*sample_args())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "boundary,2,32,20,5000.0,5000.0,12345\ncontrol,2,32,20,250.0,250.0,\n",
        )

    def test_control_row_has_empty_last_field_and_boundary_is_filled(self) -> None:
        result = run_script(*sample_args(**{"--checkpoint-duration-us": "0"}))
        boundary, control = result.stdout.splitlines()
        self.assertEqual(boundary.split(",")[-1], "0")
        self.assertEqual(control.split(",")[-1], "")
        self.assertEqual(len(control.split(",")), len(HEADER.split(",")))

    def test_default_window_is_1000_writes_with_exact_p99_and_max(self) -> None:
        # Crossing n = 1000, so boundary = writes 500..1499 and control =
        # writes 0..999 (centre 500). Write i takes (i + 1) us, except
        # write 1000, the crossing, which takes 90000 us.
        latencies = [(i + 1) * 1000 for i in range(1600)]
        latencies[1000] = 90_000_000
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, latencies, reads=True)
            result = run_script(
                "--interval-bytes", str(1001 * RECORD),
                "--run", "3",
                "--iodepth", "32",
                "--checkpoint-duration-us", "888",
                str(path),
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        # boundary: 999 ordinary values plus the spike. The 990th smallest
        # is ordinary write 1490 = 1491 us. control: 1 to 1000 us, so p99
        # is 990 us and the max is 1000 us.
        self.assertEqual(
            result.stdout,
            "boundary,3,32,1000,1491.0,90000.0,888\n"
            "control,3,32,1000,990.0,1000.0,\n",
        )

    def test_sub_microsecond_latencies_keep_one_decimal(self) -> None:
        # 20 writes of 1500 ns. n = 9 (interval 10 records), window 6:
        # boundary 6..11, control centre 4 gives 1..6, which overlaps no
        # crossing. All values 1500 ns -> 1.5 us.
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, [1500] * 20)
            result = run_script(
                "--interval-bytes", str(10 * RECORD),
                "--window-writes", "6",
                "--run", "1",
                "--iodepth", "32",
                "--checkpoint-duration-us", "5",
                str(path),
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "boundary,1,32,6,1.5,1.5,5\ncontrol,1,32,6,1.5,1.5,\n",
        )


class ErrorTest(unittest.TestCase):
    def assert_clean_failure(
        self, result: subprocess.CompletedProcess, *words: str
    ) -> None:
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("Traceback", result.stderr)
        self.assertTrue(
            result.stderr.startswith("bench_latency: error: "), result.stderr
        )
        for word in words:
            self.assertIn(word, result.stderr)

    def test_log_too_short_for_the_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, [1000] * 48)
            result = run_script(
                "--interval-bytes", str(40 * RECORD),
                "--window-writes", "20",
                "--run", "1",
                "--iodepth", "32",
                "--checkpoint-duration-us", "5",
                str(path),
            )
        self.assert_clean_failure(result, "too short", "49")

    def test_log_that_never_reaches_the_interval(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, [1000] * 10)
            result = run_script(
                "--interval-bytes", str(40 * RECORD),
                "--window-writes", "20",
                "--run", "1",
                "--iodepth", "32",
                "--checkpoint-duration-us", "5",
                str(path),
            )
        self.assert_clean_failure(result, "too short")

    def test_missing_file(self) -> None:
        result = run_script(*sample_args()[:-1], "/no/such/lat.log")
        self.assert_clean_failure(result, "/no/such/lat.log")

    def test_bad_row_in_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.log"
            path.write_text("this is not a fio log\n")
            result = run_script(*sample_args()[:-1], str(path))
        self.assert_clean_failure(result, "line 1")

    def test_log_with_no_writes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "reads.log"
            path.write_text("1, 5000, 0, 4096, 0, 0\n")
            result = run_script(*sample_args()[:-1], str(path))
        self.assert_clean_failure(result, "too short")


class UsageTest(unittest.TestCase):
    def test_missing_arguments_is_a_usage_error(self) -> None:
        result = run_script(str(SAMPLE))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--interval-bytes", result.stderr)

    def test_run_must_be_a_positive_integer(self) -> None:
        result = run_script(*sample_args(**{"--run": "zero"}))
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Traceback", result.stderr)

    def test_checkpoint_duration_must_not_be_negative(self) -> None:
        result = run_script(*sample_args(**{"--checkpoint-duration-us": "-1"}))
        self.assertEqual(result.returncode, 2)

    def test_checkpoint_duration_is_required_with_a_log(self) -> None:
        args = sample_args()
        i = args.index("--checkpoint-duration-us")
        del args[i : i + 2]
        result = run_script(*args)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--checkpoint-duration-us", result.stderr)


if __name__ == "__main__":
    unittest.main()
