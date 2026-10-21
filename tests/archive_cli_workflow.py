"""Verify public archive CLI against a fresh, real Vivado-created fixture."""
import argparse, hashlib, json, os, queue, subprocess, threading, time
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--cli',required=True,type=Path)
p.add_argument('--fixture',required=True,type=Path)
p.add_argument('--vivado',required=True,type=Path)
p.add_argument('--work',required=True,type=Path)
a=p.parse_args(); cli=str(a.cli.resolve()); fixture=a.fixture.resolve(); work=a.work.resolve(); work.mkdir(parents=True,exist_ok=False)
source=fixture/'source'; shared=fixture/'共享 输入'; project=source/'demo.xpr'; records=[]
assert project.is_file() and shared.is_dir()
def hashes(root):
 return {str(f.relative_to(root)): {'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'mtimeNs':f.stat().st_mtime_ns} for f in sorted(root.rglob('*')) if f.is_file()}
before={'project':hashes(source),'shared':hashes(shared)}
def call(arguments,code=0):
 result=subprocess.run([cli,*map(str,arguments)],capture_output=True,encoding='utf-8',errors='replace',timeout=180)
 value=json.loads(result.stdout)
 record={'arguments':list(map(str,arguments)),'exitCode':result.returncode,'response':value,'stderr':result.stderr};records.append(record)
 assert result.returncode==code,record
 return value

def request(value,dry=False):
 f=work/f'request-{len(records):03}.json';f.write_text(json.dumps(value,ensure_ascii=False),encoding='utf-8')
 result=call(['--request',f,*(['--dry-run'] if dry else [])])
 if dry:
  plan=work/f'preview-{len(records):03}.json';plan.write_text(json.dumps(result,ensure_ascii=False),encoding='utf-8');return plan,result['data']['plan']
 return result['data']
try:
 inspected=request({'action':'archive-inspect','project':str(project)})
 assert inspected['version']=='2022.2',inspected
 scanned=request({'action':'archive-scan','root':str(source)})
 assert any(Path(x['project'])==project for x in scanned['projects'])
 output=work/'归档 输出';output.mkdir()
 operation={'action':'archive-run','project':str(project),'output':str(output),'name':'verified','vivado':str(a.vivado.resolve()),'sharedRoots':[str(shared)]}
 plan,detail=request(operation,True)
 assert len(detail['preview']['sharedRoots'])==1
 result=call(['--apply',plan])['data']
 archive=Path(result['directory']);assert archive.is_dir() and Path(result['archive']).is_file()
 copied=archive/next(iter(detail['preview']['sharedRoots'].values()))
 assert (copied/'rtl/top.v').read_bytes()==(shared/'rtl/top.v').read_bytes()
 assert (copied/'include/defs.vh').read_bytes()==(shared/'include/defs.vh').read_bytes()
 assert {'project':hashes(source),'shared':hashes(shared)}==before
 # Changed shared headers invalidate the already reviewed archive plan.
 stale,_=request({**operation,'name':'stale'},True)
 header=shared/'include/defs.vh';original=header.read_bytes();stamp=header.stat()
 header.write_bytes(original+b'// changed after preview\n')
 assert call(['--apply',stale],3)['error']['code']=='preview_conflict'
 header.write_bytes(original);os.utime(header,ns=(stamp.st_atime_ns,stamp.st_mtime_ns))
 assert not (output/'stale').exists() and not (output/'stale.7z').exists()
 # Cancel a real Vivado archive job after entering the batch stage.
 cancel_plan,_=request({**operation,'name':'cancelled'},True)
 cancel=work/'cancel.flag'
 child=subprocess.Popen([cli,'--apply',str(cancel_plan),'--cancel-file',str(cancel)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',errors='replace')
 lines=queue.Queue()
 def pump():
  for line in child.stderr: lines.put(line)
 threading.Thread(target=pump,daemon=True).start()
 stderr=[];requested=False;deadline=time.monotonic()+120
 while child.poll() is None and time.monotonic()<deadline:
  try: line=lines.get(timeout=.1)
  except queue.Empty: continue
  stderr.append(line)
  try: event=json.loads(line)
  except ValueError: continue
  if event.get('event')=='log' and 'Vivado v2022.2' in event.get('detail',''):
   cancel.write_text('cancel',encoding='utf-8');requested=True
 if child.poll() is None: child.kill();child.wait();raise AssertionError('Archive cancellation did not complete')
 while not lines.empty():stderr.append(lines.get())
 response=json.loads(child.stdout.read())
 records.append({'check':'cancel during real Vivado batch','requested':requested,'exitCode':child.returncode,'response':response,'stderr':''.join(stderr)})
 assert requested and child.returncode==5 and response['error']['code']=='cancelled',records[-1]
 assert not (output/'cancelled').exists() and not (output/'cancelled.7z').exists()
 assert {'project':hashes(source),'shared':hashes(shared)}==before
 report={'status':'passed','tool':'real Vivado 2022.2 and bundled 7-Zip','cliSha256':hashlib.sha256(Path(cli).read_bytes()).hexdigest(),'before':before,'after':{'project':hashes(source),'shared':hashes(shared)},'records':records}
 (work/'RESULTS.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 print(json.dumps({'status':'passed','report':str(work/'RESULTS.json'),'archive':str(archive)},ensure_ascii=False))
except BaseException as error:
 (work/'RESULTS.json').write_text(json.dumps({'status':'failed','error':str(error),'records':records},ensure_ascii=False,indent=2),encoding='utf-8')
 raise
