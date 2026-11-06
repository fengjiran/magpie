#!/usr/bin/env python3
"""Independent-process benchmark runs, immutable snapshots and verified replay.

No command modifies the calling repository's HEAD, index or tracked source.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {
    "program": "bench_empty_task", "mode": "pool", "payload": "empty",
    "rejection": "abort", "workers": 1, "producers": 1, "capacity": 4096,
    "warmup": 2.0, "duration": 10.0, "arrival-rate": 0.0, "seed": 20260929,
    "work": 64, "sample-stride": 1024, "sample-capacity": 65536,
    "depth": 4, "fanout": 2, "task-count": 1000, "sleep-ms": 10.0,
}


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def config_hash(config):
    return hashlib.sha256(canonical(config).encode()).hexdigest()


def execute(command, output=None, cwd=None, timeout=120, env=None):
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                                timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        # Never erase a failed run or automatically retry it.
        if output:
            Path(str(output) + ".stdout.txt").write_text(error.stdout.decode() if isinstance(error.stdout, bytes) else error.stdout or "")
            Path(str(output) + ".stderr.txt").write_text(error.stderr.decode() if isinstance(error.stderr, bytes) else error.stderr or "")
            Path(str(output) + ".command.json").write_text(json.dumps({"command": command, "timeout": timeout, "result": "timeout"}, indent=2))
        raise RuntimeError("command timed out: " + str(command)) from error
    if output:
        Path(str(output) + ".stdout.txt").write_text(result.stdout)
        Path(str(output) + ".stderr.txt").write_text(result.stderr)
        Path(str(output) + ".command.json").write_text(json.dumps({
            "command": command, "cwd": str(cwd) if cwd else None,
            "exit_code": result.returncode, "elapsed_seconds": time.monotonic() - started,
        }, indent=2))
    if result.returncode:
        raise RuntimeError("command failed (%d): %s\n%s" % (result.returncode, command, result.stderr[-3000:]))
    return result


def optional(command):
    try:
        r = subprocess.run(command, text=True, capture_output=True, timeout=15, check=False)
        return {"command": command, "exit_code": r.returncode, "stdout": r.stdout, "stderr": r.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": command, "unavailable": str(error)}


def environment():
    info = {
        "captured_local": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": platform.platform(), "system": platform.system(),
        "machine": platform.machine(), "python": sys.version,
        "logical_cpu_count": os.cpu_count(),
        "allowed_cpus": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "physical_linux": "not-attested", "clock": "steady_clock;POSIX process/thread CPU clocks",
    }
    if platform.system() == "Linux":
        info["cpu_topology"] = optional(["lscpu", "--json"])
        info["standard_library"] = optional(["ldd", "--version"])
        # Resolve the process's actual cgroup, rather than assuming cgroup root.
        memberships = Path("/proc/self/cgroup").read_text()
        info["cgroup_membership"] = memberships
        v2 = next((line.split(":", 2)[2] for line in memberships.splitlines() if line.startswith("0::")), "")
        quota = Path("/sys/fs/cgroup") / v2.lstrip("/") / "cpu.max"
        info["cpu_quota"] = quota.read_text().strip() if quota.exists() else "unavailable (including cgroup v1)"
        policies = Path("/sys/devices/system/cpu/cpufreq")
        info["frequency_policies"] = {str(p): p.read_text().strip() for p in policies.glob("policy*/scaling_governor")}
        info["virtualization"] = optional(["systemd-detect-virt"])
    elif platform.system() == "Darwin":
        info["cpu_topology"] = optional(["sysctl", "hw.model", "machdep.cpu.brand_string", "hw.physicalcpu", "hw.logicalcpu"])
        info["frequency_policy"] = "not-controlled;macOS development data only"
        info["cpu_quota"] = "not-applicable"
    return info


def load_config(path):
    values = json.loads(Path(path).read_text()) if path else {}
    if not isinstance(values, dict) or any(k not in DEFAULTS and k not in {"worker-cpus", "producer-cpus", "local-capacity", "spawn-pattern", "shutdown-pattern"} for k in values):
        raise ValueError("configuration must contain only known benchmark keys")
    if any(isinstance(value, (list, dict, bool)) or value is None for value in values.values()):
        raise ValueError("configuration values must be scalar strings or numbers")
    return dict(DEFAULTS, **values)


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = math.floor(position)
    return values[low] + (values[math.ceil(position)] - values[low]) * (position - low)


def summarize(rows):
    result = {}
    for key in rows[0]:
        try:
            values = [float(row[key]) for row in rows]
        except ValueError:
            continue
        if any(not math.isfinite(value) for value in values):
            result[key] = {"nonfinite": True}
            continue
        result[key] = {"median": statistics.median(values), "q1": percentile(values, .25),
                       "q3": percentile(values, .75), "iqr": percentile(values, .75) - percentile(values, .25)}
    return result


def compare_p99(reference_rows, candidate_rows):
    """Compare empirical quantiles; legacy histogram bounds cannot fill this gap."""
    groups = []
    for rows in (reference_rows, candidate_rows):
        if not rows:
            return {"status": "insufficient-evidence", "reason": "missing exact sample p99"}
        quantiles = []
        for row in rows:
            if row.get("quantile_definition") != "nearest-rank-sample-v1":
                return {"status": "insufficient-evidence", "reason": "missing exact sample p99"}
            count = row.get("samples")
            if isinstance(count, str):
                # Counts are decimal integers in producer TSV, not floats.
                if not count.isascii() or not count.isdecimal():
                    return {"status": "insufficient-evidence", "reason": "invalid sample count"}
                try:
                    count = int(count)
                except ValueError:
                    return {"status": "insufficient-evidence", "reason": "invalid sample count"}
            if type(count) is not int or count <= 0:
                return {"status": "insufficient-evidence", "reason": "invalid sample count"}
            try:
                p99 = float(row["p99_us"])
            except (KeyError, TypeError, ValueError, OverflowError):
                return {"status": "insufficient-evidence", "reason": "invalid sample p99"}
            if not math.isfinite(p99) or p99 < 0:
                return {"status": "insufficient-evidence", "reason": "invalid sample p99"}
            quantiles.append(p99)
        groups.append(statistics.median(quantiles))
    reference, candidate = groups
    if reference == 0:
        return {"status": "insufficient-evidence", "reason": "zero reference p99"}
    ratio = candidate / reference
    return {"status": "passed" if ratio <= 1.05 else "failed", "median_ratio": ratio,
            "scope": "sample p99 only; distribution peaks and sampling uncertainty require review"}


def run_series(binary, config, output, repeat, smoke, platform_attested="off", profile=False):
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"result": "running", "config": config, "config_hash": config_hash(config),
                "binary": str(binary), "binary_sha256": sha256(binary), "repeat": repeat,
                "environment": environment(), "smoke": smoke, "profile": profile, "runs": []}
    protocol = repeat >= 5 and config["warmup"] >= 2 and config["duration"] >= 10
    if not smoke and not protocol:
        raise ValueError("measurement requires repeat >= 5, warmup >= 2 and duration >= 10; use --smoke for diagnostics")
    if platform_attested not in ("off", "physical", "wsl2"):
        raise ValueError("platform-attested must be off, physical or wsl2")
    if platform_attested != "off":
        if platform.system() != "Linux" or not config.get("worker-cpus") or not config.get("producer-cpus"):
            raise ValueError("platform-attested runs require Linux and explicit worker/producer CPU sets")
        if platform_attested == "wsl2":
            # 操作员声明须与机器虚拟化检测一致，防止把物理机/其他容器数据误标为 WSL2。
            detected = manifest["environment"].get("virtualization")
            stdout = (detected or {}).get("stdout", "").lower()
            if "unavailable" in detected or "wsl" not in stdout:
                raise ValueError("platform-attested=wsl2 requires systemd-detect-virt to report WSL; got: %r" % (detected,))
    manifest["environment"]["platform_attested"] = platform_attested
    manifest["measurement_protocol_met"] = protocol
    manifest["formal_platform_eligible"] = bool(platform_attested in ("physical", "wsl2")
                                                and protocol and not smoke and not profile)
    manifest["timing_scope"] = "fixed window plus quiescent tail; shutdown uses finite inventory"
    manifest["attribution"] = "controlled queue/wrapping contrasts; no additive decomposition or isolated gate/epoch timing"
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    rows = []
    try:
        for index in range(repeat):
            command = [str(binary)]
            for key, value in sorted(config.items()):
                command.extend(["--" + key, str(value)])
            command.extend(["--config-id", manifest["config_hash"]])
            if profile:
                if platform.system() != "Linux":
                    raise ValueError("perf profiling requires Linux")
                command = ["perf", "stat", "-x", ",", "-e", "cycles,instructions,cache-misses,context-switches,cpu-migrations", "--"] + command
            timeout = max(120, config["warmup"] + config["duration"] + 60)
            if config["program"] == "bench_shutdown_drain":
                timeout += 2 * config["task-count"] * config["sleep-ms"] / 1000 / config["workers"]
            sample_path = output.resolve() / ("run-%02d.samples.tsv" % index)
            child_env = os.environ.copy()
            child_env.pop("MAGPIE_BENCH_SAMPLES_OUT", None)
            if config["program"] == "bench_latency":
                child_env["MAGPIE_BENCH_SAMPLES_OUT"] = str(sample_path)
            r = execute(command, output / ("run-%02d" % index), timeout=timeout, env=child_env)
            parsed = list(csv.DictReader(io.StringIO(r.stdout), delimiter="\t"))
            if len(parsed) != 1 or parsed[0].get("config_hash") != manifest["config_hash"]:
                raise RuntimeError("malformed benchmark TSV")
            row = parsed[0]
            if row["sample_dropped"] != "0":
                raise RuntimeError("latency sample storage exhausted; increase stride/capacity and rerun in a new directory")
            if platform_attested != "off" and r.stderr and (not profile or "magpie:" in r.stderr):
                raise RuntimeError("formal run emitted warnings, including possible affinity fallback")
            if sha256(binary) != manifest["binary_sha256"]:
                raise RuntimeError("benchmark binary changed during measurement")
            rows.append(row)
            run = {"index": index, "result": "passed", "stdout_sha256": sha256(output / ("run-%02d.stdout.txt" % index))}
            if sample_path.exists():
                run["samples_sha256"] = sha256(sample_path)
            manifest["runs"].append(run)
            manifest_path.write_text(json.dumps(manifest, indent=2))
            print("%s %s %d/%d: %.0f total ops/s" % (config["program"], config["mode"], index + 1, repeat, float(row["total_ops_s"])), flush=True)
        with (output / "raw.tsv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        (output / "summary.json").write_text(json.dumps(summarize(rows), indent=2))
        manifest["result"] = "passed"
        manifest["raw_sha256"] = sha256(output / "raw.tsv")
        if config["program"] == "bench_latency":
            manifest["p99_evidence"] = {
                "exact_samples_archived": all("samples_sha256" in run for run in manifest["runs"]),
                "sample_quantiles_available": compare_p99(rows, rows)["status"] == "passed",
                "distribution_gate": "requires raw sample review; platform eligibility is not gate acceptance",
            }
    except BaseException as error:
        manifest["result"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        manifest_path.write_text(json.dumps(manifest, indent=2))
    return rows


def source_files():
    r = execute(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT)
    paths = sorted(set(r.stdout.rstrip("\0").split("\0")))
    return [p for p in paths if p and not p.startswith("results/") and (ROOT / p).is_file()]


def freeze(args, config):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    files = source_files()
    hashes = {p: sha256(ROOT / p) for p in files}
    original = execute(["git", "rev-parse", "HEAD"], cwd=ROOT).stdout.strip()
    status = execute(["git", "status", "--porcelain"], cwd=ROOT).stdout
    metadata = {"result": "building", "origin_head": original, "origin_status": status,
                "source_sha256": hashes, "snapshot_kind": "independent git commit; calling HEAD/index untouched",
                "config": config, "config_hash": config_hash(config)}
    manifest = output / "baseline.json"
    manifest.write_text(json.dumps(metadata, indent=2))
    try:
        with tempfile.TemporaryDirectory(prefix="magpie-m2-snapshot-") as temp:
            snapshot = Path(temp) / "source"
            execute(["git", "clone", "--quiet", "--no-hardlinks", str(ROOT), str(snapshot)], output / "clone", timeout=120)
            # Preserve deletions as well as additions in the source snapshot.
            old = execute(["git", "ls-files", "-z"], cwd=snapshot).stdout.rstrip("\0").split("\0")
            for p in old:
                if p and not p.startswith("results/") and p not in hashes:
                    (snapshot / p).unlink()
            for p in files:
                if (ROOT / p).is_symlink():
                    raise ValueError("source snapshot does not accept symlinks: " + p)
                target = snapshot / p
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / p, target)
            execute(["git", "add", "-A"], cwd=snapshot)
            execute(["git", "-c", "user.name=magpie baseline", "-c", "user.email=baseline@localhost",
                     "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "--no-gpg-sign", "--allow-empty",
                     "-m", "M2 mutex baseline source snapshot"], cwd=snapshot)
            metadata["baseline_commit"] = execute(["git", "rev-parse", "HEAD"], cwd=snapshot).stdout.strip()
            execute(["git", "bundle", "create", str(output / "source.bundle"), "HEAD"], output / "bundle", cwd=snapshot)
            build = Path(temp) / "build"
            command = ["cmake", "-S", str(snapshot), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                       "-DBUILD_TESTING=ON", "-DMAGPIE_BUILD_BENCHMARKS=ON", "-DMAGPIE_ENABLE_TEST_HOOKS=ON",
                       "-DMAGPIE_SANITIZER=none", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
            if args.compiler:
                command.append("-DCMAKE_CXX_COMPILER=" + args.compiler)
            execute(command, output / "configure", timeout=300)
            execute(["cmake", "--build", str(build), "--parallel", "2"], output / "build", timeout=300)
            for label in ("fast", "stress"):
                execute([sys.executable, str(snapshot / "scripts/run_ctest.py"), "--build-dir", str(build), "--mode", label],
                        output / ("correctness-" + label), timeout=300)
            binary = output / "magpie_bench"
            shutil.copy2(build / "bench/magpie_bench", binary)
            shutil.copy2(build / "compile_commands.json", output / "compile_commands.json")
            commands = json.loads((build / "compile_commands.json").read_text())
            production = [entry for entry in commands if "CMakeFiles/magpie.dir/" in entry["command"]]
            if not production or any("MAGPIE_ENABLE_TEST_HOOKS" in entry["command"] for entry in production):
                raise RuntimeError("production compile commands contain hooks or are missing")
            optimization_levels = {"-O0", "-O1", "-O2", "-O3", "-Os", "-Ofast"}
            for entry in production:
                levels = [word for word in entry["command"].split() if word in optimization_levels]
                if not levels or levels[-1] != "-O2":
                    raise RuntimeError("production Release optimization is not final -O2")
            symbols = execute(["nm", "-gU" if platform.system() == "Darwin" else "-g", str(binary)], output / "symbols")
            if "install_pool_test_hooks" in symbols.stdout:
                raise RuntimeError("benchmark binary exports production test hooks")
            execute(["git", "bundle", "verify", str(output / "source.bundle")], output / "bundle-verify", cwd=snapshot)
            metadata["production_flags"] = "final -O2; no test hooks; production target verified"
            metadata["binary_sha256"] = sha256(binary)
            metadata["bundle_sha256"] = sha256(output / "source.bundle")
            metadata["dependencies"] = optional(["otool", "-L", str(binary)] if platform.system() == "Darwin" else ["ldd", str(binary)])
            metadata["correctness"] = "fresh snapshot Release fast/stress passed; production runtime has hooks disabled"
            if hashes != {p: sha256(ROOT / p) for p in files} or files != source_files():
                raise RuntimeError("live source changed while freezing; snapshot not accepted")
            metadata["result"] = "frozen"
            manifest.write_text(json.dumps(metadata, indent=2))
            run_series(binary, config, output / "initial", args.repeat, args.smoke, args.platform_attested, args.profile)
    except BaseException as error:
        metadata["result"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        manifest.write_text(json.dumps(metadata, indent=2))


def replay(args):
    baseline = args.baseline.resolve()
    metadata = json.loads((baseline / "baseline.json").read_text())
    if metadata["result"] != "frozen":
        raise ValueError("baseline has not passed freeze validation")
    for name, key in (("magpie_bench", "binary_sha256"), ("source.bundle", "bundle_sha256")):
        if sha256(baseline / name) != metadata[key]:
            raise ValueError("frozen artifact checksum mismatch: " + name)
    initial = json.loads((baseline / "initial/manifest.json").read_text())
    if initial["result"] != "passed" or sha256(baseline / "initial/raw.tsv") != initial["raw_sha256"]:
        raise ValueError("initial baseline data missing or corrupt")
    # Legacy archives may have only aggregate TSV. Once a raw sample hash is
    # registered, its distribution evidence must survive intact before replay.
    for run in initial.get("runs", []):
        if "samples_sha256" not in run:
            continue
        index = run.get("index")
        if type(index) is not int or index < 0:
            raise ValueError("invalid sample archive index")
        samples = baseline / "initial" / ("run-%02d.samples.tsv" % index)
        if not samples.is_file() or sha256(samples) != run["samples_sha256"]:
            raise ValueError("frozen sample archive missing or corrupt: " + samples.name)
    config = metadata["config"]
    if config_hash(config) != metadata["config_hash"] or metadata["config_hash"] != initial["config_hash"]:
        raise ValueError("frozen configuration identity mismatch")
    rows = run_series(baseline / "magpie_bench", config, args.output.resolve(), args.repeat, args.smoke,
                      args.platform_attested, args.profile)
    current = json.loads((args.output / "manifest.json").read_text())
    # Environment equivalence is a prerequisite for A/B, not a guarantee of noise-free results.
    keys = ("platform", "machine", "logical_cpu_count", "allowed_cpus", "cpu_quota", "cpu_topology", "frequency_policies", "frequency_policy")
    same_environment = all(current["environment"].get(k) == initial["environment"].get(k) for k in keys)
    same_day = current["environment"]["captured_local"][:10] == initial["environment"]["captured_local"][:10]
    # Summaries are derived data. Recompute from the verified raw history
    # rather than trusting an editable summary file for the comparison.
    with (baseline / "initial/raw.tsv").open(newline="") as handle:
        initial_rows = list(csv.DictReader(handle, delimiter="\t"))
        initial_summary = summarize(initial_rows)
    median = statistics.median(float(row["total_ops_s"]) for row in rows)
    reference = initial_summary["total_ops_s"]["median"]
    comparison = {"baseline_commit": metadata["baseline_commit"], "same_environment": same_environment,
                  "same_day": same_day, "same_config": current["config_hash"] == initial["config_hash"],
                  "baseline_binary_sha256": metadata["binary_sha256"], "median_ratio": median / reference if reference else None,
                  "formal_ab_eligible": bool(same_environment and same_day and current["formal_platform_eligible"] and initial["formal_platform_eligible"] and not initial.get("smoke", False)),
                  "interpretation": "baseline repeatability only; no new mechanism compared"}
    if config["program"] == "bench_latency":
        comparison["p99_sample_comparison"] = compare_p99(initial_rows, rows)
    (args.output / "comparison.json").write_text(json.dumps(comparison, indent=2))


def suite_configs():
    cases = [("empty", {}), ("submit-pool", {"program": "bench_submit_path"}),
             ("submit-queue", {"program": "bench_submit_path", "mode": "queue", "workers": 1}),
             ("submit-wrap", {"program": "bench_submit_path", "mode": "wrap", "workers": 1})]
    for producers in (1, 2, 4, 8):
        cases.append(("producers-%d" % producers, {"program": "bench_producer_scaling", "workers": 4, "producers": producers}))
    cases.append(("caller-runs", {"program": "bench_producer_scaling", "workers": 4, "producers": 4, "rejection": "caller-runs"}))
    for payload in ("cpu", "memory", "atomic", "sleep"):
        cases.append(("fine-" + payload, {"program": "bench_fine_grain", "workers": 4, "payload": payload, "sleep-ms": .1}))
    for depth in (2, 4, 8, 16):
        cases.append(("burst-%d" % depth, {"program": "bench_burst", "workers": 4, "depth": depth, "payload": "cpu"}))
    cases.append(("idle", {"program": "bench_idle_cpu", "workers": 4}))
    for mode in ("pool", "async", "thread"):
        cases.append(("baseline-" + mode, {"program": "bench_baselines", "workers": 4, "mode": mode, "payload": "cpu"}))
    for mode in ("same", "cross"):
        cases.append(("alloc-" + mode, {"program": "bench_alloc_cost", "mode": mode, "workers": 1}))
    for count in (1000, 10000):
        for sleep in (10, 100):
            cases.append(("shutdown-%d-%d" % (count, sleep), {"program": "bench_shutdown_drain", "workers": 4,
                         "capacity": 16384, "task-count": count, "sleep-ms": sleep}))
    return cases


def suite(args, base):
    if (base["program"], base["mode"], base["payload"], base["rejection"]) != ("bench_empty_task", "pool", "empty", "abort"):
        raise ValueError("suite config must use empty-task/pool/empty/abort as its calibration base")
    args.output.mkdir(parents=True, exist_ok=False)
    inventory = {"result": "running", "cases": [],
                 "not_applicable": {"bench_worker_scaling": "no local deque/spawn/steal path in M1",
                                    "bench_skew": "no local victim distribution in M1",
                                    "bench_notify_scan": "no EventCount/WaitSlot in M1"}}
    cases = suite_configs()
    help_text = execute([str(args.binary.resolve()), "--help"]).stdout
    if "--local-capacity" in help_text:
        inventory["not_applicable"].pop("bench_worker_scaling")
        inventory["not_applicable"].pop("bench_skew")
        for workers in (1, 2, 4, 8):
            cases.append(("workers-%d" % workers, {"program": "bench_worker_scaling", "workers": workers, "task-count": 4096, "local-capacity": 4096, "payload": "cpu", "work": 256}))
        for pattern in ("hotspot", "uniform"):
            cases.append(("skew-" + pattern, {"program": "bench_skew", "workers": 4, "task-count": 4096, "local-capacity": 4096, "payload": "cpu", "work": 256, "spawn-pattern": pattern}))
        inventory["local_path"] = "inspect TSV backend; global build provides global worker-spawn control"
    if "bench_notify_scan" in help_text:
        for workers in (1, 4, 8):
            cases.append(("notify-%d" % workers, {"program": "bench_notify_scan", "workers": workers, "arrival-rate": 1000, "sample-stride": 1}))
    if "--shutdown-pattern" in help_text:
        for pattern, count in (("single-tail", 1), ("mixed", 16), ("caller-runs", 16)):
            cases.append(("shutdown-" + pattern, {"program": "bench_shutdown_drain", "workers": 4,
                         "capacity": 16, "task-count": count, "sleep-ms": 10,
                         "shutdown-pattern": pattern,
                         "rejection": "caller-runs" if pattern == "caller-runs" else "abort"}))
    manifest = args.output / "suite.json"
    try:
        for name, changes in cases:
            config = dict(base, **changes)
            if args.smoke:
                config.update({"warmup": .01, "duration": .03, "sample-stride": 16})
                if name.startswith("shutdown-"):
                    config["sleep-ms"] = .1
                    if config.get("shutdown-pattern", "uniform") == "uniform":
                        config["task-count"] = 32
            rows = run_series(args.binary.resolve(), config, args.output / name, args.repeat, args.smoke,
                              args.platform_attested, args.profile)
            inventory["cases"].append({"name": name, "result": "passed", "config_hash": config_hash(config)})
            if name == "empty":
                if "eventcount" in rows[0]["backend"]:
                    inventory["not_applicable"].pop("bench_notify_scan", None)
                saturation = statistics.median(float(row["worker_ops_s"]) for row in rows)
                inventory["latency_calibration"] = {"same_binary_config": config, "worker_saturation_ops_s": saturation}
        for fraction in (.1, .5, .9):
            name = "latency-%d" % (fraction * 100)
            config = dict(base, **{"program": "bench_latency", "arrival-rate": saturation * fraction, "sample-stride": 128})
            if args.smoke:
                config.update({"warmup": .01, "duration": .03, "sample-stride": 4})
            run_series(args.binary.resolve(), config, args.output / name, args.repeat, args.smoke,
                       args.platform_attested, args.profile)
            inventory["cases"].append({"name": name, "result": "passed", "config_hash": config_hash(config)})
        inventory["result"] = "passed"
    except BaseException as error:
        inventory["result"] = "failed"
        inventory["error"] = str(error)
        raise
    finally:
        manifest.write_text(json.dumps(inventory, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("run", "suite", "freeze", "replay"))
    parser.add_argument("--binary", type=Path, default=ROOT / "build/bench/bench/magpie_bench")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--smoke", action="store_true", help="short diagnostics; never performance evidence")
    parser.add_argument("--platform-attested", choices=("off", "physical", "wsl2"), default="off",
                        help="operator attests a fixed Linux platform (physical or wsl2) and controlled CPU sets")
    parser.add_argument("--profile", action="store_true", help="perf stat diagnostics, separate from unprofiled comparisons")
    parser.add_argument("--compiler")
    args = parser.parse_args()
    if args.repeat < 1 or args.repeat > 100:
        parser.error("repeat must be 1..100")
    if args.action == "replay":
        if not args.baseline or args.config:
            parser.error("replay requires --baseline and uses its frozen config")
        replay(args)
        return
    config = load_config(args.config)
    if args.smoke and args.action != "suite":
        config.update({"warmup": .01, "duration": .03})
    if args.action == "freeze":
        freeze(args, config)
    elif args.action == "suite":
        suite(args, config)
    else:
        run_series(args.binary.resolve(), config, args.output.resolve(), args.repeat, args.smoke,
                   args.platform_attested, args.profile)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError, KeyError) as error:
        print("benchmark runner failed: " + str(error), file=sys.stderr)
        sys.exit(1)
