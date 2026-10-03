#!/usr/bin/env python3
"""Configure, build, and run an independent CMake consumer of magpie."""

import argparse
import subprocess
import sys
from typing import List


def run(command: List[str]) -> int:
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(command, check=False).returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer-source", required=True)
    parser.add_argument("--magpie-source", required=True)
    parser.add_argument("--binary-dir", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--sanitizer", required=True)
    parser.add_argument("--cache-line", required=True)
    parser.add_argument("--shared", choices=("ON", "OFF"), required=True)
    args = parser.parse_args()

    configure = [
        "cmake", "-S", args.consumer_source, "-B", args.binary_dir,
        "-G", args.generator,
        f"-DMAGPIE_SOURCE_DIR={args.magpie_source}",
        f"-DCMAKE_CXX_COMPILER={args.compiler}",
        f"-DCMAKE_BUILD_TYPE={args.configuration}",
        f"-DMAGPIE_SANITIZER={args.sanitizer}",
        f"-DMAGPIE_CACHE_LINE={args.cache_line}",
        f"-DMAGPIE_BUILD_SHARED={args.shared}",
        f"-DMAGPIE_EXPECT_SHARED={args.shared}",
        "-DBUILD_TESTING=OFF",
    ]
    if run(configure) != 0:
        return 1

    build = ["cmake", "--build", args.binary_dir, "--target", "magpie_external_consumer", "--parallel", "2"]
    if args.configuration:
        build.extend(["--config", args.configuration])
    if run(build) != 0:
        return 1

    ctest = ["ctest", "--test-dir", args.binary_dir, "--output-on-failure"]
    if args.configuration:
        ctest.extend(["-C", args.configuration])
    return run(ctest)


if __name__ == "__main__":
    sys.exit(main())
