"""Tests for the fixed fio job files in bench/jobs/.

Run from anywhere with:  python3 -I tests/scripts/test_bench_jobs.py

The benchmark design fixes pattern, block size, size and
iodepth for each job, so a typo in one of the eight files would silently skew
a benchmark. These tests pin every setting.
"""

import configparser
import shutil
import subprocess
import unittest
from pathlib import Path

JOBS_DIR = Path(__file__).resolve().parent.parent.parent / "bench" / "jobs"

# file name -> (rw, bs, size, iodepth), straight from the contract table.
EXPECTED = {
    "randread-4k-qd1.fio": ("randread", "4k", "256M", "1"),
    "randread-4k-qd32.fio": ("randread", "4k", "256M", "32"),
    "randwrite-4k-qd1.fio": ("randwrite", "4k", "256M", "1"),
    "randwrite-4k-qd32.fio": ("randwrite", "4k", "256M", "32"),
    "read-128k-qd1.fio": ("read", "128k", "1G", "1"),
    "read-128k-qd32.fio": ("read", "128k", "1G", "32"),
    "write-128k-qd1.fio": ("write", "128k", "1G", "1"),
    "write-128k-qd32.fio": ("write", "128k", "1G", "32"),
}


def load_job(path: Path) -> configparser.ConfigParser:
    # fio job files are INI files; ';' starts a comment line.
    parser = configparser.ConfigParser(comment_prefixes=(";", "#"))
    parser.optionxform = str  # keep option names exactly as written
    parser.read(path)
    return parser


class JobFilesTest(unittest.TestCase):
    def test_exactly_the_eight_expected_files_exist(self) -> None:
        found = sorted(p.name for p in JOBS_DIR.glob("*.fio"))
        self.assertEqual(found, sorted(EXPECTED))

    def test_each_file_has_one_job_section_named_after_the_file(self) -> None:
        for name in EXPECTED:
            with self.subTest(job=name):
                parser = load_job(JOBS_DIR / name)
                self.assertEqual(parser.sections(), [name.removesuffix(".fio")])

    def test_fixed_settings_common_to_all_jobs(self) -> None:
        for name in EXPECTED:
            with self.subTest(job=name):
                section = load_job(JOBS_DIR / name)[name.removesuffix(".fio")]
                self.assertEqual(section["direct"], "1")
                self.assertEqual(section["ioengine"], "libaio")
                self.assertEqual(section["filename"], "/dev/nbd0")

    def test_pattern_block_size_size_and_iodepth(self) -> None:
        for name, (rw, bs, size, iodepth) in EXPECTED.items():
            with self.subTest(job=name):
                section = load_job(JOBS_DIR / name)[name.removesuffix(".fio")]
                self.assertEqual(section["rw"], rw)
                self.assertEqual(section["bs"], bs)
                self.assertEqual(section["size"], size)
                self.assertEqual(section["iodepth"], iodepth)

    def test_fixed_sizes_not_fixed_times(self) -> None:
        for name in EXPECTED:
            with self.subTest(job=name):
                section = load_job(JOBS_DIR / name)[name.removesuffix(".fio")]
                self.assertNotIn("runtime", section)
                self.assertNotIn("time_based", section)

    def test_each_file_starts_with_a_comment(self) -> None:
        for name in EXPECTED:
            with self.subTest(job=name):
                first_line = (JOBS_DIR / name).read_text().splitlines()[0]
                self.assertTrue(first_line.startswith(";"), first_line)

    @unittest.skipUnless(shutil.which("fio"), "fio is not installed")
    def test_fio_accepts_every_file(self) -> None:
        # --parse-only checks syntax and option values without opening the
        # target, so it never touches /dev/nbd0.
        for name in EXPECTED:
            with self.subTest(job=name):
                result = subprocess.run(
                    ["fio", "--parse-only", str(JOBS_DIR / name)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
