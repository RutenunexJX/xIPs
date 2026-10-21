"""Supplementary public CLI checks; fixtures are copied, never edited in place."""
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cli',type=Path,required=True);p.add_argument('--work',type=Path,required=True);p.add_argument('--legacy',type=Path)
a=p.parse_args();work=a.work.resolve();work.mkdir(parents=True,exist_ok=False);lib=work/'library';lib.mkdir();cli=str(a.cli.resolve());records=[]
def call(args,code=0):
 r=subprocess.run([cli,*map(str,args)],capture_output=True,encoding='utf-8',errors='replace',timeout=60)
 o=json.loads(r.stdout);records.append({'arguments':list(map(str,args)),'exitCode':r.returncode,'response':o,'stderr':r.stderr});assert r.returncode==code,records[-1];return o

def op(action,write=False,**fields):
 f=work/f'request-{len(records):03}.json';f.write_text(json.dumps({'action':action,'library':str(lib),**fields}),encoding='utf-8')
 r=call(['--request',f,*(['--dry-run'] if write else [])])
 if not write:return r['data']
 plan=work/f'preview-{len(records):03}.json';plan.write_text(json.dumps(r),encoding='utf-8');return call(['--apply',plan])['data']
try:
 capabilities=call(['--action','capabilities'])['data']
 original=lib/'original';original.mkdir();(original/'top.v').write_text('module top; endmodule\n');(original/'defs.vh').write_text('`define WIDTH 8\n')
 asset=op('create',True,name='linked',category='module',source=str(original))['asset'];aid=asset['id']
 revision=op('save',True,asset=aid)['snapshot']['id']
 destination=lib/'moved';assert original.resolve().is_relative_to(work) and destination.resolve().is_relative_to(work);original.rename(destination)
 op('relocate',True,asset=aid,source=str(destination))
 assert Path(op('working',asset=aid)['asset']['workingRoot'])==destination
 exported=work/'directory export';op('export',True,asset=aid,revision=revision,destination=str(exported));assert exported.is_dir()
 receipt=work/'receipt.json';op('receipt-create',True,asset=aid,revision=revision,payload=str(exported),receipt=str(receipt));op('receipt-verify',receipt=str(receipt))
 before=len(op('history',asset=aid)['versions']);unchanged=op('save',True,asset=aid);assert unchanged['unchanged'] and len(op('history',asset=aid)['versions'])==before
 if a.legacy:
  sample=a.legacy.resolve();before_sample={str(f.relative_to(sample)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sample.rglob('*') if f.is_file()}
  shutil.copytree(sample,lib/'legacy sample')
  legacy=next(x for x in op('list')['assets'] if x['legacy'])
  migrated=op('migrate',True,asset=legacy['id'])
  assert not migrated['asset']['legacy'] and op('history',asset=legacy['id'])['versions']
  assert before_sample=={str(f.relative_to(sample)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sample.rglob('*') if f.is_file()}
 if not capabilities['sessionTransport']:
  request=work/'offline-plan-request.json';request.write_text(json.dumps({'action':'group-create','library':str(lib),'name':'blocked online'}))
  preview=call(['--request',request,'--dry-run']);plan=work/'offline-plan.json';plan.write_text(json.dumps(preview))
  assert call(['--apply',plan,'--session','00000000-0000-4000-8000-000000000001'],4)['error']['code']=='sessions_unavailable'
 report={'status':'passed','operations':len(records),'sessionTransport':capabilities['sessionTransport'],'cliSha256':hashlib.sha256(Path(cli).read_bytes()).hexdigest(),'records':records}
 (work/'RESULTS.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps({'status':'passed','operations':len(records),'report':str(work/'RESULTS.json')}))
except BaseException as e:
 (work/'RESULTS.json').write_text(json.dumps({'status':'failed','error':str(e),'records':records},ensure_ascii=False,indent=2),encoding='utf-8');raise
