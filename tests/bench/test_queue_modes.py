import csv
import io
import subprocess
import sys
for mode in ('queue','mpmc'):
    result=subprocess.run([sys.argv[1], '--program','bench_submit_path','--mode',mode,
        '--workers','1','--producers','4','--capacity','16','--warmup','.01','--duration','.03'],
        text=True,capture_output=True,timeout=10,check=False)
    assert result.returncode == 0,(mode,result.returncode,result.stderr)
    rows=list(csv.DictReader(io.StringIO(result.stdout),delimiter='\t'))
    assert len(rows)==1,rows
    row=rows[0]
    assert int(row['accepted'])>0,row
    assert int(row['accepted'])+int(row['refused'])==int(row['attempts']),row
    assert row['stats_scope']=='not-a-pool',row
    assert row['backend']==mode,row
print('mutex/MPMC benchmark routes and conservation passed')
