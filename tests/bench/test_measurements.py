"""Integration oracles for exact samples, archive ownership and shutdown inventory."""
import csv
import io
from pathlib import Path
import subprocess
import sys
import tempfile

binary = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    archive = Path(directory) / "samples.tsv"
    command = [binary, "--program", "bench_latency", "--workers", "1", "--producers", "1",
               "--arrival-rate", "1000", "--warmup", ".01", "--duration", ".03",
               "--sample-stride", "1", "--samples-output", str(archive)]
    result = subprocess.run(command, text=True, capture_output=True, timeout=10, check=True)
    row, = csv.DictReader(io.StringIO(result.stdout), delimiter="\t")
    data = list(csv.DictReader(archive.open(), delimiter="\t"))
    values = sorted(int(sample["latency_ns"]) for sample in data)
    assert values and len(values) == int(row["samples"]), row
    assert row["quantile_definition"] == "nearest-rank-sample-v1", row
    assert float(row["p99_us"]) == values[len(values) - len(values) // 100 - 1] / 1000, row
    original = archive.read_bytes()
    repeated = subprocess.run(command, text=True, capture_output=True, timeout=10, check=False)
    assert repeated.returncode != 0 and archive.read_bytes() == original

for pattern, count, policy in (("single-tail", 1, "abort"), ("mixed", 8, "abort"),
                               ("caller-runs", 16, "caller-runs")):
    command = [binary, "--program", "bench_shutdown_drain", "--workers", "2", "--producers", "1",
               "--capacity", "16", "--task-count", str(count), "--sleep-ms", "1",
               "--shutdown-pattern", pattern, "--rejection", policy, "--warmup", "0",
               "--duration", ".01"]
    result = subprocess.run(command, text=True, capture_output=True, timeout=10, check=True)
    row, = csv.DictReader(io.StringIO(result.stdout), delimiter="\t")
    expected = count + (3 if pattern == "caller-runs" else 0)
    assert int(row["submitted"]) == int(row["completed"]) == expected, row
    assert int(row["pending"]) == 0 and int(row["samples"]) == expected, row
    assert int(row["inline_completed"]) == (1 if pattern == "caller-runs" else 0), row

print("exact sample archive and shutdown inventory oracles passed")
