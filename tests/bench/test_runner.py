#!/usr/bin/env python3
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

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

    def test_replay_smoke_never_qualifies_as_formal(self):
        # Exercise the replay path with the unchanged long-window frozen config.
        # Mock only the machine/subprocess, not qualification or archive checks.
        config = dict(runner.DEFAULTS, **{"worker-cpus": "0", "producer-cpus": "1"})

        def execute(command, output=None, **kwargs):
            identity = command[command.index("--config-id") + 1]
            data = "config_hash\tsample_dropped\ttotal_ops_s\n%s\t0\t100\n" % identity
            Path(str(output) + ".stdout.txt").write_text(data)
            return subprocess.CompletedProcess(command, 0, data, "")

        with tempfile.TemporaryDirectory() as temp, \
                patch.object(runner.platform, "system", return_value="Linux"), \
                patch.object(runner, "environment", side_effect=lambda: {
                    "captured_local": "2026-10-04T12:00:00+0800", "system": "Linux"}), \
                patch.object(runner, "execute", side_effect=execute), \
                contextlib.redirect_stdout(io.StringIO()):
            baseline = Path(temp)
            binary = baseline / "magpie_bench"
            bundle = baseline / "source.bundle"
            binary.write_bytes(b"mock binary")
            bundle.write_bytes(b"mock source")
            runner.run_series(binary, config, baseline / "initial", 5, False, True)
            metadata = {"result": "frozen", "baseline_commit": "test-revision",
                        "binary_sha256": runner.sha256(binary),
                        "bundle_sha256": runner.sha256(bundle),
                        "config": config, "config_hash": runner.config_hash(config)}
            (baseline / "baseline.json").write_text(json.dumps(metadata))
            initial_path = baseline / "initial/manifest.json"
            initial = json.loads(initial_path.read_text())
            self.assertTrue(initial["formal_platform_eligible"])
            for smoke in (False, True):
                with self.subTest(smoke=smoke):
                    args = argparse.Namespace(baseline=baseline,
                        output=baseline / ("smoke" if smoke else "formal"),
                        repeat=5, smoke=smoke, physical_linux_attested=True, profile=False)
                    runner.replay(args)
                    current = json.loads((args.output / "manifest.json").read_text())
                    comparison = json.loads((args.output / "comparison.json").read_text())
                    self.assertTrue(current["measurement_protocol_met"])
                    self.assertEqual(current["config"], config)
                    self.assertEqual(current["formal_platform_eligible"], not smoke)
                    self.assertEqual(comparison["formal_ab_eligible"], not smoke)

            # Older runner versions could archive smoke=true with eligibility=true.
            initial["smoke"] = True
            initial_path.write_text(json.dumps(initial))
            args.output = baseline / "legacy-smoke"
            args.smoke = False
            runner.replay(args)
            comparison = json.loads((args.output / "comparison.json").read_text())
            self.assertFalse(comparison["formal_ab_eligible"])


if __name__ == "__main__":
    unittest.main()
