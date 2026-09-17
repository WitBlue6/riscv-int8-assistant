"""Small causal decoder, independent integer reference for RV32 firmware.

One head, one pre-RMSNorm block, learned positions, clipped ReLU FFN.
Activations Q5 (scale 32), weights scale 64, no floating point at inference.
"""
import json
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parent
D, F, T, S, W = 64, 2048, 32, 32, 64

def truncdiv(x, n):
    a=np.asarray(x,dtype=np.int64)
    return np.where(a<0,-((-a)//n),a//n).astype(np.int32)

def rms(x):
    from math import isqrt
    # sqrt(mean(x_Q5^2)) is itself Q5; floor and epsilon match firmware.
    den=max(1,isqrt(int(np.sum(np.asarray(x,dtype=np.int64)**2))//D))
    return np.clip(truncdiv(np.asarray(x)*S,den),-127,127)

def hash_ints(values, h=2166136261):
    for v in np.asarray(values).flat:
        h=((h^(int(v)&0xffffffff))*16777619)&0xffffffff
    return h

def load():
    return json.loads((ROOT/'weights.json').read_text())

def encode(prompt,m):
    if not prompt: raise ValueError('请输入非空提示词')
    lookup={c:i for i,c in enumerate(m['vocab']) if i>=3}
    if '\0' in prompt: raise ValueError('提示词不能包含 NUL')
    ids=[1]+[lookup.get(c,2) for c in prompt]
    if len(ids)>=T: raise ValueError('提示词过长，至少留一个生成位置')
    return ids

def generate(prompt,m,max_new=12):
    if not 1<=max_new<=16: raise ValueError('max_new must be 1..16')
    ids=encode(prompt,m); keys=[];values=[];state_hashes=[];proj_hashes=[]
    lut=np.asarray(m['exp_lut'],dtype=np.int64)
    def linear(x,name):
        raw=np.asarray(m[name],dtype=np.int32)@np.asarray(x,dtype=np.int32)
        proj_hashes.append(hash_ints(raw))
        return truncdiv(raw,W)
    def forward(token,pos):
        x=np.asarray(m['emb'][token],dtype=np.int32)+m['pos'][pos]
        norm=rms(x)
        q=np.clip(linear(norm,'q'),-127,127)
        k=np.clip(linear(norm,'k'),-127,127)
        v=np.clip(linear(norm,'v'),-127,127)
        keys.append(k);values.append(v)
        dots=np.asarray(keys)@q
        buckets=np.minimum((int(dots.max())-dots)//128,1024)
        a=lut[buckets]; ctx=truncdiv(a@np.asarray(values),int(a.sum()))
        x=x+linear(ctx,'o')
        h=np.clip(linear(rms(x),'up'),0,127)
        x=x+linear(h,'down')
        logits=linear(rms(x),'head')
        state_hashes.append(hash_ints(list(x)+list(q)+list(k)+list(v)+list(ctx)))
        return logits
    for p,t in enumerate(ids): logits=forward(t,p)
    generated=[];score_steps=[];reason='max_new'
    for _ in range(max_new):
        score_steps.append(logits.tolist())
        # BOS is an input-only token. EOS remains eligible.
        selectable=logits.copy();selectable[1:3]=-2147483647
        token=int(np.argmax(selectable));generated.append(token)
        if token==0: reason='eos';break
        if len(ids)>=T: reason='context';break
        if len(generated)==max_new:break
        ids.append(token);logits=forward(token,len(ids)-1)
    return {'text':''.join(m['vocab'][i] for i in generated if i>=3),
            'tokens':generated,'scores':score_steps,'state_hashes':state_hashes,
            'projection_hashes':proj_hashes,'stop_reason':reason,'forward_tokens':len(keys)}
