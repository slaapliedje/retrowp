#!/usr/bin/env python3
"""Score the probe's body text against each .TXT, word for word."""
import os, re, sys, difflib, collections
sys.path.insert(0, os.path.dirname(__file__))
from probe import walk, body_text
W = os.path.expanduser('~/Documents/Atari/atariworks-specimens/Documents/Atari/patton/word')
def words(b): return re.findall(rb'\S+', b)
res = collections.Counter(); worst = []
for fn in sorted(os.listdir(W + '/STW')):
    base = os.path.splitext(fn)[0]
    tp = W + '/Text/' + base + '.TXT'
    if not os.path.exists(tp): res['no txt'] += 1; continue
    d = open(W + '/STW/' + fn, 'rb').read()
    try:
        r = walk(d)
        t, end = body_text(d, r['body'], r['total'], r['fields'])
    except Exception as e:
        res['crash'] += 1; worst.append((0, base, str(e))); continue
    a, b = words(t), words(open(tp, 'rb').read())
    ratio = difflib.SequenceMatcher(None, a, b, autojunk=False).ratio() if (a or b) else 1.0
    res['exact' if a == b else '>=99%' if ratio >= .99 else '>=90%' if ratio >= .9 else 'worse'] += 1
    if a != b: worst.append((ratio, base, len(a), len(b)))
print(dict(res))
for w in sorted(worst)[:int(sys.argv[1]) if len(sys.argv) > 1 else 12]: print(w)
