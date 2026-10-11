#!/usr/bin/env python3
"""Bounded RC11 deque protocol projections, with precise R1/R2 negatives."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--genmc',required=True);parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args();args.output_dir.mkdir(parents=True,exist_ok=False)
    version=subprocess.run([args.genmc,'--version'],capture_output=True,text=True,check=True).stdout
    if 'GenMC v0.17.0 ' not in version or 'LLVM 19.1.7' not in version:
        raise RuntimeError('requires pinned GenMC0.17.0/LLVM19.1.7: '+version)
    sources=[ROOT/'tools/model/deque/core.c',ROOT/'tools/model/deque/owner_pops.c',ROOT/'include/magpie/chase_lev_deque.hpp',Path(__file__).resolve()]
    report={'version':version,'memory_model':'RC11','scope':'C11 signed-index/atomic-slot projection; bounded safety, not C++ wrapper/liveness proof','source_sha256':{str(s.relative_to(ROOT)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},'runs':[],'result':'running'}
    cases=[('core',sources[0],[],False),('two-thieves',sources[0],['-DTWO_THIEVES'],False),('last-item',sources[0],['-DSINGLE'],False),('bad-restore',sources[0],['-DSINGLE','-DBAD_RESTORE'],True),('owner-pops',sources[1],[],False),('old-batch',sources[1],['-DOLD_BATCH'],True)]
    try:
        for name,source,flags,negative in cases:
            graph=args.output_dir/(name+'.dot')
            cmd=[args.genmc,'-rc11','-disable-estimation','-print-error-trace','-dump-error-graph='+str(graph),'--']+flags+[str(source)]
            r=subprocess.run(cmd,capture_output=True,text=True,timeout=120,check=False)
            (args.output_dir/(name+'.stdout.txt')).write_text(r.stdout);(args.output_dir/(name+'.stderr.txt')).write_text(r.stderr)
            (args.output_dir/(name+'.command.json')).write_text(json.dumps({'command':cmd,'exit_code':r.returncode},indent=2))
            text=r.stdout+r.stderr
            if negative:
                lines=source.read_text().splitlines();n=next(i for i,l in enumerate(lines) if 'DEQUE_EXPECTED_ASSERTION' in l)+2
                expression=lines[n-1].strip()
                trace=re.sub(r'\s+', '', text)
                if r.returncode==0 or 'Error: Safety violation!' not in text or source.name+':'+str(n) not in text or re.sub(r'\s+', '', expression) not in trace or not graph.exists() or graph.stat().st_size==0:
                    raise RuntimeError(name+' did not find its specified assertion and nonempty graph')
                report['runs'].append({'name':name,'expected_line':n,'graph_sha256':hashlib.sha256(graph.read_bytes()).hexdigest()})
            else:
                m=re.search(r'Number of complete executions explored:\s*([1-9][0-9]*)',text)
                b=re.search(r'Number of blocked executions seen:\s*([0-9]+)',text)
                if r.returncode or 'No errors were detected.' not in text or not m or graph.exists(): raise RuntimeError(name+' positive failed')
                report['runs'].append({'name':name,'complete':int(m[1]),'blocked':int(b[1]) if b else 0})
            print(name+' validated',flush=True)
        report['result']='passed'
    except BaseException as e: report['result']='failed';report['error']=str(e);raise
    finally: (args.output_dir/'run.json').write_text(json.dumps(report,indent=2))
if __name__=='__main__':main()
