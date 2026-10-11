#!/usr/bin/env python3
"""Bounded RC11 EventCount protocol projections, with precise R5/R6 negatives."""
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
    sources=[ROOT/'tools/model/event_count/core.c',ROOT/'tools/model/event_count/separate_words.c',ROOT/'src/event_count.hpp',ROOT/'src/event_count.cpp',ROOT/'src/event_count_generic.cpp',ROOT/'src/event_count_linux.cpp',ROOT/'src/thread_pool.cpp',Path(__file__).resolve()]
    for source in sources:
        (args.output_dir/(source.name+".source")).write_bytes(source.read_bytes())
    report={'version':version,'memory_model':'RC11','scope':'C11 SC EventCount plus abstract atomic compare-park/wake; bounded safety, not native syscall or full liveness proof','source_sha256':{str(s.relative_to(ROOT)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},'runs':[],'result':'running'}
    cases=[('futex-two-workers',sources[0],['-DSINGLE_NOTIFIER'],False),
           ('generic-two-workers',sources[0],['-DSINGLE_NOTIFIER','-DGENERIC'],False),
           ('reregister-concurrent-notifiers',sources[0],['-DWORKERS=1','-DROUNDS=2'],False),
           ('generic-reregister',sources[0],['-DWORKERS=1','-DROUNDS=2','-DGENERIC'],False),
           ('two-worker-reregister',sources[0],['-DROUNDS=2','-DSINGLE_NOTIFIER'],False),
           ('three-workers',sources[0],['-DWORKERS=3','-DSINGLE_NOTIFIER'],False),
           ('missing-stop-recheck',sources[0],['-DWORKERS=1','-DOMIT_STOP_RECHECK'],True),
           ('separate-words',sources[1],[],False),
           ('shared-generation',sources[1],['-DSHARED_GENERATION'],True)]
    try:
        for name,source,flags,negative in cases:
            graph=args.output_dir/(name+'.dot')
            cmd=[args.genmc,'-rc11','-disable-estimation','-print-error-trace','-dump-error-graph='+str(graph),'--']+flags+[str(source)]
            command_file=args.output_dir/(name+'.command.json')
            command_file.write_text(json.dumps({'command':cmd,'result':'running'},indent=2))
            try:
                r=subprocess.run(cmd,capture_output=True,text=True,timeout=120,check=False)
            except subprocess.TimeoutExpired as error:
                for stream in ('stdout','stderr'):
                    value=getattr(error,stream) or ''
                    if isinstance(value,bytes): value=value.decode(errors='replace')
                    (args.output_dir/(name+'.'+stream+'.txt')).write_text(value)
                command_file.write_text(json.dumps({'command':cmd,'result':'timeout','timeout':120},indent=2))
                raise
            (args.output_dir/(name+'.stdout.txt')).write_text(r.stdout);(args.output_dir/(name+'.stderr.txt')).write_text(r.stderr)
            (args.output_dir/(name+'.command.json')).write_text(json.dumps({'command':cmd,'exit_code':r.returncode},indent=2))
            text=r.stdout+r.stderr
            if negative:
                lines=source.read_text().splitlines();n=next(i for i,l in enumerate(lines) if 'EVENTCOUNT_EXPECTED_ASSERTION' in l)+2
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
