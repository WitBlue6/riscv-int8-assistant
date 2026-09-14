"""Shared feature / bit-exact integer model specification (firmware mirrors this)."""
import json
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]

def codepoints(text):
    for ch in text:
        c = ord(ch)
        if 65 <= c <= 90:
            c += 32
        if 48 <= c <= 57:
            yield 35
        elif 97 <= c <= 122 or 0x4e00 <= c <= 0x9fff:
            yield c

def features(text):
    x = np.zeros(256, dtype=np.int8)
    prev = 0
    for c in codepoints(text):
        x[((c * 2654435761) & 0xffffffff) >> 24] = 1
        if prev:
            h = (((prev * 16777619) & 0xffffffff) ^ c)
            x[((h * 2654435761) & 0xffffffff) >> 24] = 1
        prev = c
    return x

def samples(dataset, split):
    return [(text, label) for label, group in enumerate(dataset[split]) for text in group]

def infer(text, model):
    x = features(text).astype(np.int32)
    h = np.clip(np.array(model['w1'], dtype=np.int32) @ x + model['b1'], 0, 127)
    scores = np.array(model['w2'], dtype=np.int32) @ h + model['b2']
    order = np.argsort(scores, kind='stable')
    pred = int(np.argmax(scores))
    margin = int(scores[order[-1]] - scores[order[-2]])
    cp = list(codepoints(text))
    known = set(model['known_codepoints'])
    known_count = sum(c in known for c in cp)
    accepted = bool(cp) and known_count * 100 >= len(cp) * model['known_percent'] and margin >= model['margin_threshold']
    return {'features': x.tolist(), 'hidden': h.tolist(), 'scores': scores.tolist(),
            'intent': pred, 'margin': margin, 'accepted': accepted}

def load_model():
    return json.loads((ROOT / 'model/weights.json').read_text())
