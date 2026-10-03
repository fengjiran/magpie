#!/usr/bin/env python3
"""Check that the pinned GenMC release understands atomics and fences."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import platform
import re
import shutil
import subprocess
import sys
from typing import Dict, List, Optional
from datetime import datetime, timezone


EXPECTED_VERSION = "0.17.0"
PROBE_DIR = pathlib.Path(__file__).resolve().parent / "probes"
REPO_DIR = PROBE_DIR.parents[2]
VERSION_PATTERN = re.compile(r"(?<![0-9.])0\.17\.0(?![0-9.])")
EXECUTION_COUNT = re.compile(r"Number of complete executions explored:\s*([1-9][0-9]*)")
VERIFICATION_SUCCESS = re.compile(
    r"\*\*\* Verification complete\.\s*No errors were detected\."
)
SAFETY_ERROR = re.compile(
    r"^(?:Error detected:|Error:)\s*Safety violation!$",
    re.IGNORECASE | re.MULTILINE,
)
ASSERTION_MARKER = "GENMC_EXPECTED_ASSERTION"


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def create_output_dir(requested: Optional[str]) -> pathlib.Path:
    if requested:
        output_dir = pathlib.Path(requested).expanduser().absolute()
    else:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        output_dir = REPO_DIR / "results" / "model" / f"genmc-{stamp}"
    output_dir.mkdir(parents=True, exist_ok=False)
    return output_dir


def save_process_result(
    output_dir: pathlib.Path,
    name: str,
    command: List[str],
    result: subprocess.CompletedProcess[str],
) -> None:
    write_json(output_dir / f"{name}.command.json", command)
    (output_dir / f"{name}.stdout.txt").write_text(result.stdout, encoding="utf-8")
    (output_dir / f"{name}.stderr.txt").write_text(result.stderr, encoding="utf-8")
    (output_dir / f"{name}.exit-code.txt").write_text(f"{result.returncode}\n", encoding="utf-8")


def run(
    executable: str,
    source: pathlib.Path,
    output_dir: pathlib.Path,
    name: str,
) -> subprocess.CompletedProcess[str]:
    graph = output_dir / f"{name}.error.dot"
    command = [
        executable, "-rc11", "-print-error-trace", f"-dump-error-graph={graph}", str(source)
    ]
    print("$ " + " ".join(command), flush=True)
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=120)
    except subprocess.TimeoutExpired as error:
        partial_stdout = error.stdout or ""
        partial_stderr = error.stderr or ""
        if isinstance(partial_stdout, bytes):
            partial_stdout = partial_stdout.decode("utf-8", errors="replace")
        if isinstance(partial_stderr, bytes):
            partial_stderr = partial_stderr.decode("utf-8", errors="replace")
        partial = subprocess.CompletedProcess(command, 124, partial_stdout, partial_stderr)
        save_process_result(output_dir, name, command, partial)
        raise RuntimeError(f"GenMC exceeded the 120 second probe timeout for {source.name}")
    save_process_result(output_dir, name, command, result)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result


def finish_status(output_dir: pathlib.Path, status: Dict[str, object]) -> None:
    status["finished_utc"] = datetime.now(timezone.utc).isoformat()
    write_json(output_dir / "run.json", status)


def expected_assertion(source: pathlib.Path) -> tuple[int, str]:
    lines = source.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if ASSERTION_MARKER not in line:
            continue
        for assertion_index in range(index + 1, len(lines)):
            match = re.search(r"\bassert\s*\((.*)\)\s*;", lines[assertion_index])
            if match:
                return assertion_index + 1, match.group(1).strip()
            if lines[assertion_index].strip() and not lines[assertion_index].lstrip().startswith("//"):
                break
        break
    raise RuntimeError(f"No assertion immediately follows {ASSERTION_MARKER} in {source}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--genmc", help="Path to GenMC 0.17.0; defaults to PATH lookup")
    parser.add_argument("--llvm-config", help="Path to the llvm-config used by GenMC")
    parser.add_argument("--source-dir", help="GenMC v0.17.0 source checkout, if available")
    parser.add_argument(
        "--output-dir",
        help="New, non-existing evidence directory; defaults to results/model/genmc-<UTC timestamp>",
    )
    args = parser.parse_args()

    executable = args.genmc or shutil.which("genmc")
    if not executable:
        print("GenMC is unavailable: install the pinned 0.17.0 release and retry.", file=sys.stderr)
        return 2

    try:
        output_dir = create_output_dir(args.output_dir)
    except OSError as error:
        print(f"Could not create a new evidence directory: {error}", file=sys.stderr)
        return 2
    print(f"Evidence directory: {output_dir}")

    source_hashes = {
        source.name: hashlib.sha256(source.read_bytes()).hexdigest()
        for source in sorted(PROBE_DIR.glob("*.c"))
    }
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=REPO_DIR, capture_output=True, text=True, check=False
    )
    status = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "tool": "GenMC",
        "required_version": EXPECTED_VERSION,
        "executable": str(pathlib.Path(executable).expanduser().absolute()),
        "platform": platform.platform(),
        "python": sys.version,
        "source_revision": revision.stdout.strip() if revision.returncode == 0 else None,
        "working_tree_dirty": bool(subprocess.run(
            ["git", "status", "--porcelain"], cwd=REPO_DIR,
            capture_output=True, text=True, check=False
        ).stdout.strip()),
        "probe_sha256": source_hashes,
        "memory_model": "RC11 (explicit -rc11)",
        "result": "running",
    }
    if args.source_dir:
        source_dir = pathlib.Path(args.source_dir).expanduser().absolute()
        tag_command = ["git", "describe", "--tags", "--exact-match"]
        commit_command = ["git", "rev-parse", "HEAD"]
        source_tag = subprocess.run(
            tag_command, cwd=source_dir,
            capture_output=True, text=True, check=False,
        )
        source_commit = subprocess.run(
            commit_command, cwd=source_dir,
            capture_output=True, text=True, check=False,
        )
        save_process_result(output_dir, "genmc-source-tag", tag_command, source_tag)
        save_process_result(output_dir, "genmc-source-commit", commit_command, source_commit)
        status["genmc_source_dir"] = str(source_dir)
        status["genmc_source_tag"] = source_tag.stdout.strip() if source_tag.returncode == 0 else None
        status["genmc_source_commit"] = source_commit.stdout.strip() if source_commit.returncode == 0 else None
    write_json(output_dir / "run.json", status)

    try:
        version_verified = False
        for candidate in ("--version", "-version"):
            command = [executable, candidate]
            try:
                result = subprocess.run(
                    command, capture_output=True, text=True, check=False, timeout=15
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                status["result"] = "tool-version-query-failed"
                status["detail"] = str(error)
                failed = subprocess.CompletedProcess(command, 127, "", str(error))
                save_process_result(
                    output_dir, f"version-{candidate.replace('-', 'dash')}", command, failed
                )
                finish_status(output_dir, status)
                print(f"Could not query GenMC with {candidate}: {error}", file=sys.stderr)
                return 2
            save_process_result(output_dir, f"version-{candidate.replace('-', 'dash')}", command, result)
            candidate_text = result.stdout + result.stderr
            print("$ " + " ".join(command))
            print(candidate_text, end="")
            if result.returncode == 0 and VERSION_PATTERN.search(candidate_text):
                status["version_output"] = candidate_text
                version_verified = True
                break
        if not version_verified:
            status["result"] = "wrong-or-unreported-version"
            finish_status(output_dir, status)
            print(f"Expected GenMC {EXPECTED_VERSION}; refusing to record an unpinned run.", file=sys.stderr)
            return 2
        if args.source_dir and status.get("genmc_source_tag") != f"v{EXPECTED_VERSION}":
            status["result"] = "source-tag-mismatch"
            finish_status(output_dir, status)
            print(f"Expected source tag v{EXPECTED_VERSION}.", file=sys.stderr)
            return 2
        if args.llvm_config:
            llvm_command = [args.llvm_config, "--version"]
            llvm_result = subprocess.run(
                llvm_command, capture_output=True, text=True, check=False, timeout=15
            )
            save_process_result(output_dir, "llvm-config-version", llvm_command, llvm_result)
            if llvm_result.returncode != 0:
                status["result"] = "llvm-config-version-query-failed"
                status["llvm_config_stderr"] = llvm_result.stderr
                finish_status(output_dir, status)
                print("Could not obtain the associated LLVM version.", file=sys.stderr)
                return 2
            status["llvm_config_path"] = str(pathlib.Path(args.llvm_config).expanduser().absolute())
            status["llvm_config_version"] = llvm_result.stdout.strip()
            write_json(output_dir / "run.json", status)

        positive = run(
            executable, PROBE_DIR / "fence_message_passing.c", output_dir, "fence-positive"
        )
        positive_output = positive.stdout + positive.stderr
        if (positive.returncode != 0 or (output_dir / "fence-positive.error.dot").exists() or
                not EXECUTION_COUNT.search(positive_output) or
                not VERIFICATION_SUCCESS.search(positive_output)):
            status["result"] = "fence-positive-control-failed"
            finish_status(output_dir, status)
            print("The release/acquire fence positive control did not complete cleanly.", file=sys.stderr)
            return 1
        print("PASS: RC11 explored at least one complete execution of the release/acquire fence case.")

        negative = run(
            executable, PROBE_DIR / "relaxed_message_passing_negative.c", output_dir,
            "relaxed-negative",
        )
        negative_output = negative.stdout + negative.stderr
        assertion_line, assertion_expression = expected_assertion(
            PROBE_DIR / "relaxed_message_passing_negative.c"
        )
        error_graph = output_dir / "relaxed-negative.error.dot"
        graph_is_valid = error_graph.is_file() and error_graph.stat().st_size > 0
        source_location = f"relaxed_message_passing_negative.c:{assertion_line}"
        assertion_trace = re.compile(
            rf"\bassert\s*\(\s*{re.escape(assertion_expression)}\s*\)\s*;"
        )
        if (negative.returncode == 0 or not graph_is_valid or
                not SAFETY_ERROR.search(negative_output) or
                not assertion_trace.search(negative_output) or
                source_location not in negative_output):
            status["result"] = "relaxed-negative-control-failed"
            status["expected_assertion"] = assertion_expression
            status["expected_source_location"] = source_location
            finish_status(output_dir, status)
            print(
                "The relaxed negative control did not produce GenMC's assertion-error diagnostic "
                "and a non-empty error graph.",
                file=sys.stderr,
            )
            return 1
        print("PASS: RC11 found the expected assertion violation for relaxed message passing.")
        status["result"] = "passed"
    except (OSError, RuntimeError) as error:
        status["result"] = "probe-execution-failed"
        status["detail"] = str(error)
        finish_status(output_dir, status)
        print(f"GenMC probe failed: {error}", file=sys.stderr)
        return 1
    finish_status(output_dir, status)
    print(f"Evidence saved in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
