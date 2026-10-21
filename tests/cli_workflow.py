"""Run real xips-cli workflows using a new, isolated directory. No test-only product API."""
import argparse, json, os, subprocess, sys, time, hashlib, uuid
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--cli', required=True, type=Path)
parser.add_argument('--work', required=True, type=Path)
parser.add_argument('--gui', type=Path)
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=False)
records = []
lib = work / '资产 库'
lib.mkdir()
cli = str(args.cli.resolve())

def run(arguments, expected=0):
    proc = subprocess.run([cli, *map(str, arguments)], capture_output=True, encoding='utf-8', errors='replace', timeout=120)
    try:
        value = json.loads(proc.stdout)
    except Exception as exc:
        raise AssertionError((arguments, proc.returncode, proc.stdout, proc.stderr)) from exc
    record = {'arguments':list(map(str,arguments)), 'exitCode':proc.returncode, 'response':value, 'stderr':proc.stderr}
    records.append(record)
    assert proc.returncode == expected, record
    if expected == 0: assert value['ok'], record
    return value

def request(action, expected=0, **fields):
    name = work / f'request-{len(records):03}.json'
    obj = {'action': action, 'library': str(lib), **fields}
    name.write_text(json.dumps(obj, ensure_ascii=False), encoding='utf-8')
    return run(['--request', name], expected)['data'] if expected == 0 else run(['--request', name], expected)

def preview(action, **fields):
    name = work / f'request-{len(records):03}.json'
    name.write_text(json.dumps({'action': action, 'library': str(lib), **fields}, ensure_ascii=False), encoding='utf-8')
    response = run(['--request', name, '--dry-run'])
    plan = work / f'preview-{len(records):03}.json'
    plan.write_text(json.dumps(response, ensure_ascii=False), encoding='utf-8')
    return plan, response['data']['plan']

def apply(action, **fields):
    plan, _ = preview(action, **fields)
    return run(['--apply', plan])['data']

def legacy(arguments, expected=0):
    proc = subprocess.run([cli, *map(str,arguments)], capture_output=True, encoding='utf-8', errors='replace', timeout=60)
    value = json.loads(proc.stdout or proc.stderr)
    records.append({'arguments':list(map(str,arguments)), 'exitCode':proc.returncode, 'response':value})
    assert proc.returncode == expected, records[-1]
    return value

def main():
    cap = run(['--action', 'capabilities'])['data']
    assert cap['schema'] == 'xips.operations/v1'
    asset = apply('create', name='demo', category='ip')['asset']
    aid = asset['id']; working = Path(asset['workingRoot'])
    source = working / 'top.sv'; source.write_text('module top; localparam VALUE = 1; endmodule\n', encoding='utf-8')
    v1 = apply('save', asset=aid, note='first')['snapshot']
    assert v1['id'] and len(request('history', asset=aid)['versions']) == 1
    assert not request('working', asset=aid)['changed']
    source.write_text('module top; localparam VALUE = 2; endmodule\n', encoding='utf-8')
    assert request('working', asset=aid)['changed']
    v2 = apply('save', asset=aid, note='second')['snapshot']
    assert v2['id'] != v1['id'] and v2['hash'] != v1['hash']
    exported = work/'旧版本 导出'
    apply('export', asset=aid, revision=v1['id'], destination=str(exported))
    assert 'VALUE = 1' in exported.read_text()
    assert 'VALUE = 2' in source.read_text()
    apply('version-rename', asset=aid, revision=v2['id'], name='2.0')
    assert request('history', asset=aid)['versions'][-1]['label'] == '2.0'
    legacy(['--action','resolve','--library',lib,'--asset',aid,'--asset-version',v1['id']])
    assert len(legacy(['--action','list','--library',lib])['data']['assets']) == 1
    apply('metadata', asset=aid, name='renamed', category='project', indexes={'tag':['通信'], 'interface':['AXI4 Lite'], 'category':['bus/uart']})
    working = lib/'renamed'; source = working/'top.sv'
    assert source.exists() and not (lib/'demo').exists()
    assert len(request('list', query='interface:"AXI4 Lite" tag:通信 category:bus', category='project')['assets']) == 1
    group = apply('group-create', name='Buses')['groupId']
    target = apply('group-create', name='Buses/Serial')['groupId']
    apply('group-member', asset=aid, group=group, included=True)
    apply('group-move', asset=aid, group=group, targetGroup=target)
    apply('group-rename', group=target, name='Serial')
    memberships = request('groups')['groups']
    assert aid in next(g for g in memberships if g['id']==target)['members']
    assert aid not in next(g for g in memberships if g['id']==group)['members']
    apply('group-member', asset=aid, group=target, included=False)
    apply('group-delete', group=group)
    source.write_text('module top; localparam VALUE = 3; endmodule\n', encoding='utf-8')
    stale, _ = preview('save', asset=aid)
    source.write_text('module top; localparam VALUE = 4; endmodule\n', encoding='utf-8')
    assert run(['--apply', stale], 3)['error']['code'] == 'preview_conflict'
    assert len(request('history', asset=aid)['versions']) == 2
    fresh, _ = preview('save', asset=aid)
    cancelled = work/'取消'; cancelled.write_text('cancel')
    run(['--apply', fresh, '--cancel-file', cancelled], 5)
    assert len(request('history', asset=aid)['versions']) == 2
    receipt = work/'origin.json'
    apply('receipt-create', asset=aid, revision=v1['id'], payload=str(exported), receipt=str(receipt))
    request('receipt-verify', receipt=str(receipt))
    exported.write_text('changed')
    request('receipt-verify', expected=3, receipt=str(receipt))
    recovered = work/'恢复 导出'
    apply('receipt-recover', receipt=str(receipt), destination=str(recovered))
    assert 'VALUE = 1' in recovered.read_text()
    apply('delete-version', asset=aid, revision=v1['id'])
    assert v1['id'] in request('history', asset=aid)['deletedRevisionIds']
    request('verify', expected=4, asset=aid, revision=v1['id'])
    apply('restore-version', asset=aid, revision=v1['id'])
    assert request('verify', asset=aid, revision=v1['id'])['result']['snapshot']['hash'] == v1['hash']
    refs = work/'项目 引用'; refs.mkdir()
    apply('reference-add', asset=aid, revision=v1['id'], destinationLibrary=str(refs))
    assert request('references', library=str(refs))['assets'][0]['pinnedRevision'] == v1['id']
    request('reference-verify', library=str(refs), asset=aid, ownerLibrary=str(lib))
    apply('reference-update', library=str(refs), asset=aid, ownerLibrary=str(lib), revision=v2['id'])
    apply('reference-remove', library=str(refs), asset=aid)
    assert not request('references', library=str(refs))['assets']
    imported = work/'拖入 文件'; imported.mkdir()
    (imported/'keep.sv').write_text('module keep; endmodule\n')
    (imported/'leave.txt').write_text('leave')
    _, plan = preview('create', name='imported', category='module', sources=[str(imported)], version='1.1')
    selected = [f for f in plan['payload']['files'] if f.endswith('keep.sv')]
    assert len(selected)==1, plan
    result = apply('create', name='imported', category='module', sources=[str(imported)], files=selected, move=True, version='1.1')
    assert result['snapshot']['label']=='1.1' and not (imported/'keep.sv').exists() and (imported/'leave.txt').exists()
    more = work/'更多'; more.mkdir(); (more/'next.sv').write_text('module next; endmodule\n')
    apply('import', asset=result['asset']['id'], sources=[str(more)], version='1.2')
    registered = lib/'existing folder'; registered.mkdir(); (registered/'file.v').write_text('module file; endmodule\n')
    reg = apply('create', name='existing', category='module', source=str(registered))['asset']
    apply('save', asset=reg['id'])
    apply('unregister', asset=reg['id'])
    assert (registered/'file.v').exists()
    apply('create', name='existing', category='module', source=str(registered))
    collected = apply('collect', name='archive_only', category='module', sources=[str(more)])['asset']
    assert request('working', asset=collected['id'])['working'] is None
    (more/'next.sv').write_text('module next; wire revised; endmodule\n')
    apply('save', asset=collected['id'], sources=[str(more)])
    assert len(request('history', asset=collected['id'])['versions'])==2
    request('history', expected=4, asset='missing-id')
    invalid = work/'bad.json'; invalid.write_text('{"action":"create","library":'+json.dumps(str(lib))+',"name":"bad","typo":1}')
    run(['--request', invalid, '--dry-run'], 2)
    # A replay cannot publish a second asset, even when two writers race.
    race,_=preview('create',name='race_once',category='module')
    racers=[subprocess.Popen([cli,'--apply',str(race)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8') for _ in range(2)]
    codes=[]
    for racer in racers:
        stdout,stderr=racer.communicate(timeout=30);codes.append(racer.returncode)
        records.append({'check':'concurrent preview apply','exitCode':racer.returncode,'response':json.loads(stdout),'stderr':stderr})
    assert sorted(codes)==[0,3],codes
    assert len([a for a in request('list')['assets'] if a['name']=='race_once'])==1
    conflict=work/'occupied export';plan,_=preview('export',asset=aid,revision=v1['id'],destination=str(conflict))
    conflict.write_text('unrelated destination')
    assert run(['--apply',plan],3)['error']['code']=='preview_conflict'
    assert conflict.read_text()=='unrelated destination'
    tampered=json.loads(plan.read_text(encoding='utf-8'));tampered['data']['plan']['request']['revision']='missing'
    altered=work/'altered.json';altered.write_text(json.dumps(tampered),encoding='utf-8')
    run(['--apply',altered],2)
    # Locate a pinned reference after moving its owner library to a new root.
    owner=work/'owner original';owner.mkdir()
    owned=apply('create',library=str(owner),name='owner_asset',category='module',sources=[str(more)])
    owner_id=owned['asset']['id'];owner_revision=owned['snapshot']['id']
    apply('reference-add',library=str(owner),asset=owner_id,revision=owner_revision,destinationLibrary=str(refs))
    relocated=work/'owner relocated';owner.rename(relocated)
    apply('reference-update',library=str(refs),asset=owner_id,ownerLibrary=str(relocated),revision=owner_revision)
    request('reference-verify',library=str(refs),asset=owner_id,ownerLibrary=str(relocated))
    # Deleting an archived-only fixture uses the existing OS recycle-bin service.
    apply('delete-asset',asset=collected['id'])
    request('history',expected=4,asset=collected['id'])
    # Public session-discovery fault fixture: a live process with no responsive endpoint.
    registry=work/'unresponsive sessions';registry.mkdir();sid=str(uuid.uuid4())
    registration=registry/(sid+'.json')
    record={'schema':'xips.session/v1','id':sid,'endpoint':'xips.session.'+sid,'pid':os.getpid(),'library':str(lib).replace('\\','/')}
    registration.write_text(json.dumps(record),encoding='utf-8')
    unavailable,_=preview('group-create',name='must_not_publish')
    failure=run(['--apply',unavailable,'--session',sid,'--session-dir',registry],3)
    assert failure['error']['code']=='session_unavailable',failure
    assert all(g['name']!='must_not_publish' for g in request('groups')['groups'])
    record['processStart']='0';registration.write_text(json.dumps(record),encoding='utf-8')
    assert not run(['--action','sessions','--session-dir',registry])['data']['sessions']
    registration.unlink()
    if args.gui:
        gui_check(aid)

def gui_check(aid):
    sessions = work/'sessions'; profiles = work/'profile'
    env = os.environ.copy(); env['QT_QPA_PLATFORM']='offscreen'
    processes=[]
    try:
        for index in range(2):
            processes.append(subprocess.Popen([str(args.gui.resolve()), '--no-runtime', '--library',str(lib), '--profile', str(profiles/str(index)), '--session-dir',str(sessions)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        deadline=time.monotonic()+45
        live=[]
        while time.monotonic()<deadline:
            live=run(['--action','sessions','--session-dir',sessions])['data']['sessions']
            if len(live)==2 and all(not s['busy'] for s in live): break
            time.sleep(.25)
        assert len(live)==2 and all(not s['busy'] for s in live), live
        target=live[0]['id']
        pinned=request('history',asset=aid)['versions'][0]['id']
        plan,_=preview('metadata', asset=aid, description='Adopted while GUI is open')
        assert run(['--apply',plan,'--session-dir',sessions],3)['error']['code']=='session_required'
        result=run(['--apply',plan,'--session',target,'--session-dir',sessions,'--focus-asset',aid,'--focus-revision',pinned])['data']
        assert result['disk']=='committed' and result['gui']['adoption']['state']=='adopted',result
        status=run(['--action','session-status','--session',target,'--session-dir',sessions])['data']
        assert status['selection']['asset']==aid and status['selection']['revision']==pinned,status
        assert status['focus']['state']=='focused',status
        assert status['viewToken']==result['viewTokens'][str(lib).replace('\\','/')],status
        records.append({'check':'actual standalone GUI sessions','processIds':[p.pid for p in processes], 'target':target, 'status':'passed'})
    finally:
        for p in processes:
            if p.poll() is None: p.terminate()
            p.wait(timeout=15)

try:
    main()
    report={'status':'passed','operations':len(records),'cli':cli,'cliSha256':hashlib.sha256(Path(cli).read_bytes()).hexdigest(),'records':records}
    (work/'RESULTS.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'status':'passed','operations':len(records),'report':str(work/'RESULTS.json')},ensure_ascii=False))
except BaseException as exc:
    (work/'RESULTS.json').write_text(json.dumps({'status':'failed','error':str(exc),'records':records},ensure_ascii=False,indent=2),encoding='utf-8')
    raise
