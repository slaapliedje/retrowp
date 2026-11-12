import sys, os, re, difflib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import walk, body_text
W = os.path.expanduser('~/Documents/Atari/atariworks-specimens/Documents/Atari/patton/word')
b = sys.argv[1]
d = open(W + '/STW/%s.STW' % b, 'rb').read()
r = walk(d); t, e = body_text(d, r['body'], r['total'], r['fields'])
a = re.findall(rb'\S+', t); c = re.findall(rb'\S+', open(W + '/Text/%s.TXT' % b, 'rb').read())
for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, c, autojunk=False).get_opcodes():
    if op != 'equal': print(op, a[i1:i2][:12], '->', c[j1:j2][:12])
