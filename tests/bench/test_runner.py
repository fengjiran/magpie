#!/usr/bin/env python3
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("runner", Path(__file__).resolve().parents[2] / "scripts/run_benchmarks.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_summary_independent_repeats(self):
        summary = runner.summarize([{"ops": str(v), "mode": "pool"} for v in (1, 2, 3, 4, 9)])
        self.assertEqual(summary["ops"], {"median": 3, "q1": 2, "q3": 4, "iqr": 2})
        self.assertNotIn("mode", summary)

    def test_hash_covers_resources_and_semantics(self):
        config = dict(runner.DEFAULTS)
        self.assertEqual(runner.config_hash(config), runner.config_hash(dict(reversed(list(config.items())))))
        for key, value in (("workers", 2), ("rejection", "caller-runs"), ("sample-stride", 1)):
            changed = dict(config, **{key: value})
            self.assertNotEqual(runner.config_hash(config), runner.config_hash(changed))

    def test_corrupt_frozen_binary_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            baseline = Path(temp)
            binary = baseline / "magpie_bench"
            binary.write_bytes(b"original")
            manifest = {"result": "frozen", "binary_sha256": runner.sha256(binary)}
            (baseline / "baseline.json").write_text(json.dumps(manifest))
            binary.write_bytes(b"tampered")
            args = argparse.Namespace(baseline=baseline, output=baseline / "replay")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                runner.replay(args)
            self.assertFalse(args.output.exists())

    def test_result_directory_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            keep = output / "old-failure"
            keep.write_text("preserve")
            with self.assertRaises(FileExistsError):
                runner.run_series(Path("missing"), runner.DEFAULTS, output, 5, False)
            self.assertEqual(keep.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
