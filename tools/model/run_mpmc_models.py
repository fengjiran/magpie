#!/usr/bin/env python3
"""Check the bounded RC11 protocol projection, including an exact negative race."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'tools/model/mpmc/queue.c'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--genmc', required=True)
    p.add_argument('--output-dir', required=True, type=Path)
    args = p.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    version = subprocess.run([args.genmc, '--version'], text=True, capture_output=True, check=True).stdout
    if 'GenMC v0.17.0 ' not in version or 'LLVM 19.1.7' not in version:
        raise RuntimeError('expected the already-validated GenMC 0.17.0 / LLVM 19.1.7 toolchain')
    metadata = {'result':'running','version':version,'memory_model':'RC11',
        'scope':'capacity 2; two producers, one/two consumers, three tickets; C11 projection, not C++ wrapper or liveness proof',
        'source_sha256':{str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
            (SOURCE, ROOT/'include/magpie/mpmc_queue.hpp', Path(__file__).resolve())}, 'runs':[]}
    try:
        for name, flags in [('positive',[]), ('two-consumers',['-DTWO_CONSUMERS']), ('broken-publication',['-DBROKEN_PUBLICATION'])]:
            graph = args.output_dir / (name+'.dot')
            cmd = [args.genmc,'-rc11','-disable-estimation','-print-error-trace','-dump-error-graph='+str(graph),'--'] + flags + [str(SOURCE)]
            result = subprocess.run(cmd, text=True, capture_output=True, timeout=120, check=False)
            (args.output_dir/(name+'.stdout.txt')).write_text(result.stdout)
            (args.output_dir/(name+'.stderr.txt')).write_text(result.stderr)
            (args.output_dir/(name+'.command.json')).write_text(json.dumps({'command':cmd,'exit_code':result.returncode},indent=2))
            output = result.stdout+result.stderr
            if name != 'broken-publication':
                import re
                complete = re.search(r'Number of complete executions explored:\s*([1-9][0-9]*)',output)
                blocked = re.search(r'Number of blocked executions seen:\s*([0-9]+)',output)
                if result.returncode or not complete or 'No errors were detected.' not in output or graph.exists():
                    raise RuntimeError('positive model did not exhaust its admitted executions successfully')
                metadata['runs'].append({'name':name,'complete_executions':int(complete[1]),'blocked_executions':int(blocked[1]) if blocked else None})
            else:
                lines=SOURCE.read_text().splitlines()
                marker=next(i for i,line in enumerate(lines) if 'MPMC_EXPECTED_RACE' in line)
                race_line=next(i+1 for i in range(marker+1,len(lines)) if 'slot->data = value;' in lines[i])
                if (result.returncode == 0 or 'Error: Non-atomic race!' not in output or
                    'queue.c:'+str(race_line) not in output or '.data' not in output or not graph.exists() or graph.stat().st_size == 0):
                    raise RuntimeError('negative model missed the specified early-publication payload race')
                metadata['runs'].append({'name':name,'expected_race_line':race_line,'graph_sha256':hashlib.sha256(graph.read_bytes()).hexdigest()})
            print(name+' validated',flush=True)
        metadata['result']='passed'
    except BaseException as e:
        metadata['result']='failed';metadata['error']=str(e);raise
    finally:
        (args.output_dir/'run.json').write_text(json.dumps(metadata,indent=2))

if __name__ == '__main__':
    main()
