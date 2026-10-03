# M0 本机 CTest 原始日志

- Base revision: `dac83f800a813e1f97c2ffed45bd43fb124297d5` (working tree dirty; M0 edits are uncommitted).
- Host: Darwin 24.6.0 x86_64, AppleClang 17.0.0, CMake 3.29.2, Python 3.8.8, GTest 1.14.0.
- Each preserved `LastTest.log` contains four passing fast tests, including separate static and shared external CMake consumers.
- These sanitizer runs compile only the M0 build-info/consumer smoke code; they do not test concurrent thread-pool behavior.

Commands:

```text
cmake --preset release
cmake --build --preset release --verbose
python3 scripts/run_ctest.py --build-dir build/release --mode fast

cmake --preset tsan
cmake --build --preset tsan --parallel 2
python3 scripts/run_ctest.py --build-dir build/tsan --mode fast

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan --parallel 2
python3 scripts/run_ctest.py --build-dir build/asan-ubsan --mode fast
```

The three CTest logs are preserved byte-for-byte from their build directories:

| File | SHA-256 |
|---|---|
| `release-fast.LastTest.log` | `1dee7f73cbc89dc66a94f8b1adbda8887c9baef090495e98ab975b4529bcbc67` |
| `tsan-fast.LastTest.log` | `ad835c537dd6fa1062400281682685dd73bb06c0d09098bdfe5a8bbc53677886` |
| `asan-ubsan-fast.LastTest.log` | `c0d623ab405cb7b6e61836ddac96c2779d3dc32b92dcb7015b41a477b85b113a` |
