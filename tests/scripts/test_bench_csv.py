"""Tests for scripts/bench_csv.py.

Run from anywhere with:  python3 -I tests/scripts/test_bench_csv.py
or from this directory:  python3 -m unittest test_bench_csv

The script is always run as a child process, exactly as scripts/bench.sh will
run it, so these tests also cover argument parsing and exit codes.

The expected numbers are worked out by hand from the two saved fio outputs in
data/ (fio 3.40, run once on a small regular file; the filename was changed to
/dev/nbd0 afterwards). fio reports latency in nanoseconds, the CSV uses
microseconds with one decimal:

  fio_randread_4k_qd32.json   (job options: rw=randread bs=4k iodepth=32)
    read iops 68266.666667   -> 68266.7
    read bw   273066 KiB/s   -> 273066
    read clat mean 315766.654297 ns -> 315.8 us
    read clat p99  1466368 ns       -> 1466.4 us
    write section is empty (total_ios 0) -> four empty fields

  fio_write_128k_qd1.json     (job options: rw=write bs=128k iodepth=1)
    write iops 2000.0        -> 2000.0
    write bw   256000 KiB/s  -> 256000
    write clat mean 3903.59375 ns -> 3.9 us
    write clat p99  23168 ns      -> 23.2 us
    read section is empty -> four empty fields
"""

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "scripts" / "bench_csv.py"
DATA = HERE / "data"
READ_SAMPLE = DATA / "fio_randread_4k_qd32.json"
WRITE_SAMPLE = DATA / "fio_write_128k_qd1.json"

# The exact CSV header.
HEADER = (
    "target,pattern,block_size,iodepth,run,"
    "read_iops,read_bw_kib_s,read_lat_mean_us,read_lat_p99_us,"
    "write_iops,write_bw_kib_s,write_lat_mean_us,write_lat_p99_us"
)

READ_ROW = "logblock,randread,4K,32,1,68266.7,273066,315.8,1466.4,,,,"
WRITE_ROW = "file,write,128K,1,2,,,,,2000.0,256000,3.9,23.2"


def run_script(*args: str) -> subprocess.CompletedProcess:
    # -I: ignore PYTHONPATH and the current directory, like the CTest run.
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


class HeaderTest(unittest.TestCase):
    def test_header_flag_prints_exact_contract_header(self) -> None:
        result = run_script("--header")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, HEADER + "\n")


class RowTest(unittest.TestCase):
    def test_read_job_fills_read_columns_and_leaves_write_empty(self) -> None:
        result = run_script(
            "--target", "logblock", "--run", "1", str(READ_SAMPLE)
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, READ_ROW + "\n")

    def test_write_job_fills_write_columns_and_leaves_read_empty(self) -> None:
        result = run_script("--target", "file", "--run", "2", str(WRITE_SAMPLE))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, WRITE_ROW + "\n")

    def test_row_has_same_number_of_fields_as_header(self) -> None:
        result = run_script("--target", "file", "--run", "3", str(WRITE_SAMPLE))
        self.assertEqual(
            len(result.stdout.rstrip("\n").split(",")), len(HEADER.split(","))
        )

    def test_job_with_both_directions_fills_all_eight_metrics(self) -> None:
        # A mixed job (rw=randrw) has data in both sections. Build one by
        # copying the read section of one sample into the other.
        doc = json.loads(WRITE_SAMPLE.read_text())
        read_doc = json.loads(READ_SAMPLE.read_text())
        doc["jobs"][0]["read"] = read_doc["jobs"][0]["read"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mixed.json"
            path.write_text(json.dumps(doc))
            result = run_script("--target", "file", "--run", "1", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "file,write,128K,1,1,68266.7,273066,315.8,1466.4,2000.0,256000,3.9,23.2\n",
        )


class ErrorTest(unittest.TestCase):
    """Expected failures: one-line message on stderr, exit 1, no traceback."""

    def assert_clean_failure(
        self, result: subprocess.CompletedProcess, *words: str
    ) -> None:
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("Traceback", result.stderr)
        self.assertTrue(result.stderr.startswith("bench_csv: error: "), result.stderr)
        for word in words:
            self.assertIn(word, result.stderr)

    def run_on_doc(self, doc, raw: str | None = None):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fio.json"
            path.write_text(raw if raw is not None else json.dumps(doc))
            return run_script("--target", "file", "--run", "1", str(path))

    def sample(self) -> dict:
        return copy.deepcopy(json.loads(READ_SAMPLE.read_text()))

    def test_missing_file(self) -> None:
        result = run_script("--target", "file", "--run", "1", "/no/such/fio.json")
        self.assert_clean_failure(result, "/no/such/fio.json")

    def test_not_json(self) -> None:
        # fio prints text before the JSON if a job fails to start.
        result = self.run_on_doc(None, raw="fio: failed to open file\n")
        self.assert_clean_failure(result, "JSON")

    def test_no_jobs_list(self) -> None:
        doc = self.sample()
        del doc["jobs"]
        self.assert_clean_failure(self.run_on_doc(doc), "jobs")

    def test_more_than_one_job(self) -> None:
        doc = self.sample()
        doc["jobs"].append(doc["jobs"][0])
        self.assert_clean_failure(self.run_on_doc(doc), "exactly one job")

    def test_fio_reported_job_error(self) -> None:
        doc = self.sample()
        doc["jobs"][0]["error"] = 5
        self.assert_clean_failure(self.run_on_doc(doc), "error", "5")

    def test_missing_job_option(self) -> None:
        doc = self.sample()
        del doc["jobs"][0]["job options"]["bs"]
        self.assert_clean_failure(self.run_on_doc(doc), "bs")

    def test_missing_direction_section(self) -> None:
        doc = self.sample()
        del doc["jobs"][0]["write"]
        self.assert_clean_failure(self.run_on_doc(doc), "write")

    def test_missing_metric_field(self) -> None:
        doc = self.sample()
        del doc["jobs"][0]["read"]["iops"]
        self.assert_clean_failure(self.run_on_doc(doc), "read", "iops")

    def test_missing_p99_when_io_happened(self) -> None:
        doc = self.sample()
        del doc["jobs"][0]["read"]["clat_ns"]["percentile"]["99.000000"]
        self.assert_clean_failure(self.run_on_doc(doc), "p99")

    def test_job_that_did_no_io(self) -> None:
        doc = self.sample()
        doc["jobs"][0]["read"]["total_ios"] = 0
        self.assert_clean_failure(self.run_on_doc(doc), "no I/O")

    def test_non_numeric_metric(self) -> None:
        doc = self.sample()
        doc["jobs"][0]["read"]["bw"] = "fast"
        self.assert_clean_failure(self.run_on_doc(doc), "bw")


class CommandLineTest(unittest.TestCase):
    def test_missing_arguments_is_a_usage_error(self) -> None:
        result = run_script(str(READ_SAMPLE))
        self.assertEqual(result.returncode, 2)
        self.assertIn("--target", result.stderr)

    def test_run_must_be_a_positive_integer(self) -> None:
        result = run_script("--target", "file", "--run", "zero", str(READ_SAMPLE))
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Traceback", result.stderr)

    def test_target_must_be_known(self) -> None:
        result = run_script("--target", "disk", "--run", "1", str(READ_SAMPLE))
        self.assertEqual(result.returncode, 2)
        self.assertIn("logblock", result.stderr)


if __name__ == "__main__":
    unittest.main()
