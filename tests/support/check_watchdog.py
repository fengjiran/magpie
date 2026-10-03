import subprocess
import sys
result = subprocess.run([sys.argv[1]], text=True, capture_output=True, timeout=5, check=False)
assert result.returncode == 124, (result.returncode, result.stderr)
assert 'pending=1 gate=1 task-id=17' in result.stderr, result.stderr
print('watchdog independently dumped state and exited 124')
