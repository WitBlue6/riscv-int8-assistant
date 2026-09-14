#!/usr/bin/env python3
"""Three-way differential tests: Python INT8, RV32 C, four-lane RTL accelerator."""
import json
import statistics
import subprocess
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from model_common import ROOT, load_model, infer, samples

model = load_model()
dataset = json.loads((ROOT/'model/dataset.json').read_text())
sim = ROOT/'build/obj/Vsoc'
def run(text):
    proc = subprocess.run([str(sim),'+firmware='+str(ROOT/'build/firmware.hex'),'--text',text],capture_output=True,text=True,timeout=30,check=True)
    result=json.loads(proc.stdout)
    expected=infer(text,model)
    assert result['match'], text
    for key in ['features','hidden','scores','intent','margin','accepted']:
        assert result[key]==expected[key], (text,key,result[key],expected[key])
    assert result['accelerator_cycles']==2096, result
    assert result['completed_before_tool']==2, result
    assert result['accel_status']&5==0, result
    assert result['completed_after_tool']==(0 if result['accepted'] and result['intent']==5 else 2)
    assert result['hardware_cycles']>result['accelerator_cycles'], result
    return result

cases=[]
texts=[t for t,y in samples(dataset,'test')]+dataset['ood_test']
texts += ['你好','你能做什么','帮我算一下12加30','查看运行状态','介绍一下你的模型','清零统计','', ' ', '🙂', '你'*85, 'İ你好', 'HELLO你好']
for text in dict.fromkeys(texts):
    r=run(text)
    cases.append({'text':text,**{k:v for k,v in r.items() if k not in ['features','hidden','scores','reply']}})
    print('PASS',repr(text),r['intent'],r['accepted'],r['software_cycles'],r['hardware_cycles'])
for text, answer in [('计算12加30',42),('计算100减27',73),('计算7乘8',56),('计算81除以9',9),('计算8除以0',None),('计算100000乘100000',None),('计算1加2加3',None),('计算-3加5',None),('计算3加-5',None),('计算- 3减5',None)]:
    r=run(text);assert r['accepted'] and r['intent']==2,(text,r)
    assert r['calculation']==answer,(text,r['calculation']);cases.append({'text':text,'calculation':answer,'tool_test':True})
proc=subprocess.run([str(sim),'--text','你'*86],capture_output=True,text=True)
assert proc.returncode==2, 'oversized input must be rejected'
proc=subprocess.run([str(ROOT/'build/unit/Vint8_accel')],capture_output=True,text=True,check=True)
unit=json.loads(proc.stdout)
timed=[c for c in cases if 'software_cycles' in c]
ratios=[c['software_cycles']/c['hardware_cycles'] for c in timed]
baseline=json.loads(subprocess.run([str(sim),'+firmware='+str(ROOT/'build/baseline.hex'),'--text','你好'],capture_output=True,text=True,check=True,timeout=30).stdout)
optimized=run('你好')
assert baseline['match'] and baseline['scores']==optimized['scores'] and baseline['hidden']==optimized['hidden']
report={'all_passed':True,'system_cases':len(cases),'oversized_input_rejected':True,'unit':unit,
        'cpu':'PicoRV32 RV32IM, ENABLE_FAST_MUL=1, zero-wait-state simulated RAM',
        'timing_scope':'software/hardware cycle windows cover two-layer inference; hardware includes all buffer transfers, polling and output reads. Both exclude text featurization and display.',
        'mac_cycles_two_layers':2096,'speedup_min':min(ratios),'speedup_max':max(ratios),'speedup_median':statistics.median(ratios),
        'software_cycles_median':statistics.median(c['software_cycles'] for c in timed),
        'hardware_cycles_median':statistics.median(c['hardware_cycles'] for c in timed),
        'transfer_optimization':{'baseline_byte_pack_cycles':baseline['hardware_cycles'],'aligned_word_copy_cycles':optimized['hardware_cycles'],
            'baseline_software_cycles':baseline['software_cycles'],'optimized_software_cycles':optimized['software_cycles'],
            'hardware_path_improvement':baseline['hardware_cycles']/optimized['hardware_cycles'],'same_outputs':True},'cases':cases}
(ROOT/'docs/verification_results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print('SUMMARY',json.dumps({k:v for k,v in report.items() if k!='cases'},ensure_ascii=False))
