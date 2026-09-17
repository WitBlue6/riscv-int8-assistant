"""Full RV32/RTL differential tests, including external memory latency sweeps."""
import hashlib
import json
import subprocess
import time
from pathlib import Path
from model import ROOT,load,generate,D,F,T

MODEL=load()
def run(prompt,variant='fast',latency=3):
    start=time.perf_counter()
    p=subprocess.run([str(ROOT/'build/obj/Vsoc'),'+firmware='+str(ROOT/f'build/{variant}.hex'),
        '+weights='+str(ROOT/'external.hex'),f'+ext_latency={latency}','--text',prompt],
        capture_output=True,text=True,timeout=120)
    assert p.returncode==0,(variant,prompt,p.stdout[:500],p.stderr)
    result=json.loads(p.stdout);ref=generate(prompt,MODEL)
    for key,value in ref.items():assert result[key]==value,(prompt,variant,key)
    tokens=result['forward_tokens']
    if variant!='cpu':
        assert result['completed']==tokens*90,result
        assert result['accelerator_cycles']==tokens*70224,result
    else:assert result['completed']==result['accelerator_cycles']==0
    if variant in ['fast','checked']:
        assert result['kv_tokens']==tokens and result['attention_jobs']==3*tokens
        expected=sum(16+16*t+64*((t+3)//4) for t in range(1,tokens+1))
        assert result['attention_cycles']==expected,(result,expected)
    else:assert result['kv_tokens']==result['attention_jobs']==result['attention_cycles']==0
    if variant=='checked':
        assert result['match'] and result['mac_checks']==tokens*(4*64+2048+64+37)
        assert result['attention_checks']==tokens*64+tokens*(tokens+1)//2
    assert result['external_reads']>0
    result['host_seconds']=round(time.perf_counter()-start,3)
    return result

def compact(r):return {k:v for k,v in r.items() if k not in ['scores','state_hashes','projection_hashes']}
def main():
    units={}
    for name in ['attention','fc']:
        binary=ROOT/('build/attention/Vattention' if name=='attention' else 'build/fc/Vtiled_fc')
        units[name]=json.loads(subprocess.check_output([str(binary)],text=True))
    pairs=json.loads((ROOT/'corpus.json').read_text());cases=[]
    for prompt,expected in pairs:
        r=run(prompt);assert r['text']==expected
        cases.append({'prompt':prompt,**compact(r)});print('PASS generation',prompt,repr(r['text']),r['total_cycles'],flush=True)
    for prompt in ['早上好','天气怎么样','你好：你']:
        r=run(prompt);cases.append({'prompt':prompt,**compact(r)});print('PASS extra',prompt,repr(r['text']),flush=True)
    compared={}
    for kind in ['fast','cpu_attention','cpu','checked']:
        r=run('你好：',kind);compared[kind]=compact(r)
        print('PASS variant',kind,r['total_cycles'],r['host_seconds'],flush=True)
    latency={}
    for wait in [0,3,7]:
        r=run('你好：','fast',wait);latency[str(wait)]=compact(r);print('PASS latency',wait,r['total_cycles'],flush=True)
    assert latency['0']['external_reads']==latency['7']['external_reads']
    assert latency['7']['total_cycles']-latency['0']['total_cycles']==7*latency['0']['external_reads']
    layout=json.loads((ROOT/'layout.json').read_text());image=(ROOT/'external.bin').read_bytes()
    assert len(image)>256*1024, 'actually exceed old total RAM capacity'
    assert all(v['offset']%4==0 and v['offset']+v['bytes']<=len(image) for v in layout.values())
    assert (ROOT/'build/fast.elf').stat().st_size<128*1024, 'weights must not inflate firmware'
    report={'passed':True,'unit':units,'generation_cases':cases,'variants':compared,'external_latency':latency,
        'parameters':json.loads((ROOT/'training_report.json').read_text())['parameters'],
        'external_weight_bytes':len(image),'external_sha256':hashlib.sha256(image).hexdigest(),
        'full_cpu_over_accelerated':compared['cpu']['total_cycles']/compared['fast']['total_cycles'],
        'attention_offload_full_speedup':compared['cpu_attention']['total_cycles']/compared['fast']['total_cycles'],
        'scope':'All generated logits/tokens and intermediate hashes vs Python; checked firmware also compares FC and attention raw outputs. Toy training-set reconstruction, not language generalization. External memory latency is a configurable transaction model, not DDR.',
        'timing_scope':'rdcycle prefill+decode; excludes UTF8 parse/JSON output, includes transfers and normalization/softmax. fast disables duplicate software verification.'}
    (ROOT/'verification_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print('SUMMARY',json.dumps({k:v for k,v in report.items() if k not in ['generation_cases','variants','external_latency']},ensure_ascii=False))
if __name__=='__main__':main()
