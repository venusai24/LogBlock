"""Tests for scripts/bench_recovery.py.

Run from anywhere with:  python3 -I tests/scripts/test_bench_recovery.py

The helper parses the plugin's stderr lines for scripts/bench.sh:
  - the "[info] recovery ..." line  -> one recovery.csv row
  - the "[info] counters ..." line  -> the single checkpoint's duration
and drops a file's page cache without root. The command line is run as a child
process, exactly as bench.sh runs it; the pure functions are called directly.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "scripts" / "bench_recovery.py"
DATA = HERE / "data"

sys.path.insert(0, str(SCRIPT.parent))
import bench_recovery  # noqa: E402

HEADER = (
    "log_bytes,mode,run,recovery_us,log_bytes_scanned,records_replayed,cache_state"
)


def run_cli(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


class HeaderTest(unittest.TestCase):
    def test_header_matches_the_contract(self) -> None:
        result = run_cli("header")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), HEADER)


class RecoveryRowTest(unittest.TestCase):
    def row(self, log: str, mode: str = "checkpoint", **kw: str) -> subprocess.CompletedProcess:
        return run_cli(
            "recovery-row",
            "--log-bytes", kw.get("log_bytes", "104853000"),
            "--mode", mode,
            "--run", kw.get("run", "2"),
            "--cache-state", kw.get("cache_state", "dropped"),
            str(DATA / log),
        )  # fmt: skip

    def test_row_columns_come_from_the_recovery_line(self) -> None:
        result = self.row("server_recovery_checkpoint.log")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "104853000,checkpoint,2,48211,13009720,3145,dropped",
        )

    def test_source_must_match_the_mode(self) -> None:
        # A full_scan run that found a checkpoint would be a wrong measurement.
        result = self.row("server_recovery_checkpoint.log", mode="full_scan")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("source=checkpoint", result.stderr)

    def test_missing_recovery_line_fails(self) -> None:
        result = self.row("server_counters_one.log", mode="full_scan")
        # that file does have a recovery line, source=none: still the wrong mode
        self.assertEqual(result.returncode, 1)
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "server.log"
            empty.write_text("nothing here\n")
            result = run_cli(
                "recovery-row", "--log-bytes", "1", "--mode", "checkpoint",
                "--run", "1", "--cache-state", "dropped", str(empty),
            )  # fmt: skip
            self.assertEqual(result.returncode, 1)
            self.assertIn("no recovery line", result.stderr)

    def test_two_recovery_lines_fail(self) -> None:
        text = (DATA / "server_recovery_checkpoint.log").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            doubled = Path(tmp) / "server.log"
            doubled.write_text(text + text)
            result = run_cli(
                "recovery-row", "--log-bytes", "1", "--mode", "checkpoint",
                "--run", "1", "--cache-state", "dropped", str(doubled),
            )  # fmt: skip
            self.assertEqual(result.returncode, 1)
            self.assertIn("more than one", result.stderr)

    def test_parse_fields_ignores_other_lines(self) -> None:
        text = (DATA / "server_recovery_checkpoint.log").read_text()
        fields = bench_recovery.parse_recovery(text)
        self.assertEqual(fields["source"], "checkpoint")
        self.assertEqual(fields["time_us"], 48211)
        self.assertEqual(fields["records_replayed"], 3145)

    def test_checkpoint_run_that_fell_back_to_a_scan_fails(self) -> None:
        text = (DATA / "server_recovery_checkpoint.log").read_text()
        text = text.replace("source=checkpoint", "source=full_scan")
        with self.assertRaises(bench_recovery.BenchRecoveryError):
            bench_recovery.recovery_row(text, 1, "checkpoint", 1, "dropped")


class CheckpointDurationTest(unittest.TestCase):
    def test_prints_the_single_checkpoint_duration(self) -> None:
        result = run_cli("checkpoint-duration", str(DATA / "server_counters_one.log"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "7534")

    def test_count_other_than_one_fails(self) -> None:
        result = run_cli("checkpoint-duration", str(DATA / "server_counters_two.log"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("checkpoint_duration_count=2", result.stderr)

    def test_missing_counters_line_fails(self) -> None:
        result = run_cli(
            "checkpoint-duration", str(DATA / "server_recovery_checkpoint.log")
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("no counters line", result.stderr)

    def test_unreadable_file_fails_cleanly(self) -> None:
        result = run_cli("checkpoint-duration", str(DATA / "does_not_exist.log"))
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)


class WriteCountTest(unittest.TestCase):
    def test_last_write_under_target(self) -> None:
        # n writes add n * 4136 bytes; the largest n with n * 4136 < target.
        self.assertEqual(bench_recovery.writes_under(4136 * 10 + 1), 10)
        self.assertEqual(bench_recovery.writes_under(4136 * 10), 9)  # strictly under
        self.assertEqual(bench_recovery.writes_under(100 << 20), 25352)
        self.assertEqual(bench_recovery.writes_under(1 << 30), 259608)
        self.assertLess(25352 * 4136, 100 << 20)
        self.assertGreaterEqual(25353 * 4136, 100 << 20)

    def test_cli(self) -> None:
        result = run_cli("writes-under", str(100 << 20))
        self.assertEqual(result.stdout.strip(), "25352")

    def test_rejects_non_positive_target(self) -> None:
        self.assertEqual(run_cli("writes-under", "0").returncode, 2)


class DropCacheTest(unittest.TestCase):
    def test_drops_without_error_and_keeps_contents(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / "f.bin"
            f.write_bytes(b"x" * 10000)
            result = run_cli("drop-cache", str(f))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(f.read_bytes(), b"x" * 10000)

    def test_missing_file_fails_cleanly(self) -> None:
        result = run_cli("drop-cache", "/nonexistent/file")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
