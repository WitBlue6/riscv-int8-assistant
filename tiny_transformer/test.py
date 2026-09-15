"""Real RTL end-to-end regression: Python reference, checked accelerator,
unchecked accelerator for timing, CPU-only inference. No model calls in server.
"""
import hashlib
import json
import subprocess
import time
import numpy as np
from model import ROOT,D,F,T,generate,load,rms

model=load(); repo=ROOT.parent; sim=repo/'build/obj/Vsoc'
def run(prompt,kind='transformer'):
    begin=time.perf_counter()
    p=subprocess.run([str(sim),'+firmware='+str(ROOT/f'build/{kind}.hex'),'--text',prompt],
                     capture_output=True,text=True,timeout=30)
    assert p.returncode==0,(prompt,kind,p.returncode,p.stdout,p.stderr)
    d=json.loads(p.stdout);d['host_ms']=round((time.perf_counter()-begin)*1000,2)
    return d

def main():
    pairs=json.loads((ROOT/'corpus.json').read_text());cases=[]
    # Exact integer safety contracts for the exported tiny architecture.
    assert len(model['vocab'])==len(set(model['vocab']))
    assert 32<len(model['vocab'])<=64, 'exercise multi-tile output projection'
    for name,shape in [('q',(D,D)),('k',(D,D)),('v',(D,D)),('o',(D,D)),
                       ('up',(F,D)),('down',(D,F)),('head',(len(model['vocab']),D))]:
        a=np.array(model[name]);assert a.shape==shape and a.min()>=-128 and a.max()<=127
    for name in ['emb','pos']:assert np.abs(model[name]).max()<256
    assert np.array_equal(rms([0]*D),[0]*D)
    assert model['exp_lut'][0]==32768 and all(a>=b for a,b in zip(model['exp_lut'],model['exp_lut'][1:]))
    prompts=[p for p,_ in pairs]+['你好','你','我','：','你好：你','谢谢：不','你'*30,'不介晚你不不',
        '早上好','早上好 ','早上好\n','早上好,','天气怎么样','随便说说','🙂','hello',
        '着绍什好手晚介早晚。绍你再小会。么谢项型好谁！好再绍目你着绍']
    expected_text=dict(pairs)
    for prompt in prompts:
        expected=generate(prompt,model)
        variants={kind:run(prompt,kind) for kind in ['transformer','fast','software']}
        for kind,d in variants.items():
            for key,value in expected.items():assert d[key]==value,(prompt,kind,key,d[key],value)
            if kind!='software':
                assert d['completed']==d['forward_tokens']*8
                assert d['accelerator_cycles']==d['forward_tokens']*(16*16*4+16*32*2+16*len(model['vocab']))//4
            else:assert d['completed']==d['accelerator_cycles']==d['hardware_linear_cycles']==0
        checked=variants['transformer']
        assert checked['match'] and checked['mac_checks']==checked['forward_tokens']*(16*4+32+16+len(model['vocab']))
        assert variants['fast']['match'] is None and variants['fast']['mac_checks']==0
        if prompt in expected_text:assert checked['text']==expected_text[prompt]
        cases.append({'prompt':prompt,'text':checked['text'],'tokens':checked['tokens'],
                      'stop_reason':checked['stop_reason'],'forward_tokens':checked['forward_tokens'],
                      'checked_mac_results':checked['mac_checks'],
                      'fast_cycles':variants['fast']['total_cycles'],'cpu_cycles':variants['software']['total_cycles'],
                      'full_inference_speedup':variants['software']['total_cycles']/variants['fast']['total_cycles'],
                      'accelerator_cycles':checked['accelerator_cycles']})
        print('PASS',prompt,repr(checked['text']),cases[-1]['fast_cycles'],flush=True)
    rejected=[]
    for prompt in ['', '你'*31]:
        try:generate(prompt,model)
        except ValueError:pass
        else:raise AssertionError(('reference accepted',prompt))
        p=subprocess.run([str(sim),'+firmware='+str(ROOT/'build/transformer.hex'),'--text',prompt],capture_output=True,text=True,timeout=30)
        assert p.returncode==2 and 'error' in json.loads(p.stdout),(prompt,p.stdout,p.stderr)
        rejected.append(prompt)
    # Pass raw overlong UTF-8 bytes directly; firmware must not accept them as a known character.
    malformed=b'\xf0\x86\x9d\x8e' # noncanonical encoding of a BMP value
    p=subprocess.run([bytes(str(sim),'utf8'),bytes('+firmware='+str(ROOT/'build/transformer.hex'),'utf8'),b'--text',malformed],capture_output=True,timeout=30)
    assert p.returncode==2 and b'error' in p.stdout
    from server import prepare_prompt
    assert prepare_prompt(' 早上好\n')==('早上好',[])
    assert prepare_prompt('天气怎么样')==('天气怎么样',['天','怎','样'])
    # Causality: a suffix cannot change states/projections of a common prefix.
    a=generate('你好：',model);b=generate('你好：你',model)
    prefix=len('你好：')+1
    assert a['state_hashes'][:prefix]==b['state_hashes'][:prefix]
    assert a['projection_hashes'][:prefix*7]==b['projection_hashes'][:prefix*7]
    assert {'eos','context'}<={c['stop_reason'] for c in cases}
    limited=run('你好：','limit');limited_ref=generate('你好：',model,max_new=1)
    for key,value in limited_ref.items():assert limited[key]==value,(key,limited[key],value)
    assert limited['stop_reason']=='max_new' and limited['match']
    report={'all_passed':True,'rtl_generation_cases':len(cases),'firmware_variants':3,
            'invalid_prompt_cases':len(rejected)+1,'max_new_limit_firmware_passed':True,
            'training_reconstruction':f'{len(pairs)}/{len(pairs)}',
            'comparison':'Every generated logit/token, state and projection hashes vs independent Python; every accelerator raw output also checked by RV32 C in checked firmware.',
            'timing_scope':'CPU rdcycle windows cover prompt prefill and autoregressive decode, including memory copies, attention, norms, linear layers and diagnostics; excludes prompt UTF8 parsing and JSON output. Fast variant disables duplicate CPU MAC verification. Zero-wait simulated RAM; no silicon Fmax claim.',
            'weights_sha256':hashlib.sha256((ROOT/'weights.json').read_bytes()).hexdigest(),
            'limitations':'Training corpus reconstruction is not held-out accuracy or general chat quality.',
            'cases':cases}
    (ROOT/'verification_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='cases'},ensure_ascii=False))

if __name__=='__main__':main()
