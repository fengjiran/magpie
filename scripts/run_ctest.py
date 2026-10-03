#!/usr/bin/env python3
"""Run one CTest label with the plan's outer process timeout."""

import argparse
import json
import subprocess
import sys


TIMEOUT_SECONDS = {"fast": 300, "stress": 3600, "soak": 86400}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--mode", choices=TIMEOUT_SECONDS, required=True)
    args = parser.parse_args()

    inspect = subprocess.run(
        ["ctest", "--test-dir", args.build_dir, "--show-only=json-v1"],
        check=False,
        capture_output=True,
        text=True,
    )
    if inspect.returncode != 0:
        sys.stderr.write(inspect.stdout)
        sys.stderr.write(inspect.stderr)
        return inspect.returncode

    try:
        tests = json.loads(inspect.stdout).get("tests", [])
    except json.JSONDecodeError as error:
        print(f"Could not parse CTest inventory: {error}", file=sys.stderr)
        return 2

    selected = []
    for test in tests:
        properties = test.get("properties", [])
        if isinstance(properties, dict):
            labels = properties.get("LABELS", [])
        else:
            labels = next(
                (prop.get("value", []) for prop in properties if prop.get("name") == "LABELS"),
                [],
            )
        if isinstance(labels, str):
            labels = [labels]
        if args.mode in labels:
            selected.append(test)
    if not selected:
        print(
            f"No CTest tests are registered with label '{args.mode}'; "
            "this mode is not verified by the current milestone.",
            file=sys.stderr,
        )
        return 3

    command = [
        "ctest", "--test-dir", args.build_dir,
        "--output-on-failure", "--label-regex", f"^{args.mode}$",
    ]
    try:
        return subprocess.run(command, timeout=TIMEOUT_SECONDS[args.mode], check=False).returncode
    except subprocess.TimeoutExpired:
        print(
            f"CTest mode '{args.mode}' exceeded its {TIMEOUT_SECONDS[args.mode]} second outer timeout.",
            file=sys.stderr,
        )
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
