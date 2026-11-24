"""Tests for scripts/check_create_guard.sh.

Run from anywhere with:  python3 -I tests/scripts/test_create_guard.py

The script is run as a child process on small temporary source trees, the way
scripts/check.sh runs it. The last test runs it on the real src/ and include/.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SCRIPT = ROOT / "scripts" / "check_create_guard.sh"


def run_guard(*dirs, cwd=None):
    return subprocess.run(
        ["bash", str(SCRIPT), *map(str, dirs)],
        capture_output=True,
        text=True,
        cwd=cwd,
    )


class CreateGuardTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write(self, name, text):
        path = self.dir / name
        path.write_text(text)
        return path

    def test_create_with_excl_passes(self):
        self.write("a.cc", "int fd = ::open(p, O_RDWR | O_CREAT | O_EXCL, 0600);\n")
        result = run_guard(self.dir)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_create_without_excl_fails_and_names_file_and_line(self):
        path = self.write("b.cc", "int x;\nint fd = ::open(p, O_RDWR | O_CREAT, 0600);\n")
        result = run_guard(self.dir)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{path}:2", result.stdout + result.stderr)

    def test_wrapped_flag_list_fails_closed(self):
        self.write("w.cc", "int fd = ::open(p, O_RDWR | O_CREAT |\n    O_EXCL, 0600);\n")
        self.assertNotEqual(run_guard(self.dir).returncode, 0)

    def test_o_creat_only_in_a_comment_is_ignored(self):
        self.write("c.cc", "// No O_CREAT here, on purpose\nint x;  // never O_CREAT\n")
        result = run_guard(self.dir)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_code_before_a_comment_is_still_checked(self):
        self.write("d.cc", "int fd = ::open(p, O_CREAT, 0600);  // O_EXCL later\n")
        self.assertNotEqual(run_guard(self.dir).returncode, 0)

    def test_open_log_call_fails(self):
        self.write("e.cc", "auto r = io.open_log(path);\n")
        self.assertNotEqual(run_guard(self.dir).returncode, 0)

    def test_exists_call_fails(self):
        self.write("f.cc", "if (io->exists(path)) {}\n")
        self.assertNotEqual(run_guard(self.dir).returncode, 0)

    def test_real_src_and_include_pass(self):
        result = run_guard("src", "include", cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
