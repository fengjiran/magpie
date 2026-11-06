"""The auxiliary checker must fail the process when no compiler can run."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


class DesignValidationCliTests(unittest.TestCase):
    def test_missing_compiler_is_not_success(self):
        script = Path(__file__).resolve().parents[2] / "design/validation/check_design.py"
        with tempfile.TemporaryDirectory() as directory:
            missing = str(Path(directory) / "missing compiler")
            for compiler in (shlex.quote(missing), ""):
                with self.subTest(CXX=compiler):
                    env = os.environ.copy()
                    env["CXX"] = compiler
                    result = subprocess.run(
                        [sys.executable, str(script)], env=env, capture_output=True,
                        text=True, timeout=10, check=False)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn("not found", result.stderr)
                    self.assertNotIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
