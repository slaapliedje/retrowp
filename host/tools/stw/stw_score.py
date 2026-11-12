#!/usr/bin/env python3
"""Score RetroWP's Atari Works importer against the specimen corpus.

Runs build/stw2txt (format_stw_import, then the plain-text exporter, in
the Atari character set) on every .STW that has a .TXT, and compares
them word for word -- Atari Works' text export wraps lines at its
screen, so lines would not compare. `make stw-score`."""
import os, re, subprocess, sys, difflib, collections

CORPUS = os.environ.get('STW_CORPUS', os.path.expanduser(
    '~/Documents/Atari/atariworks-specimens/Documents/Atari/patton/word'))
TOOL = sys.argv[1] if len(sys.argv) > 1 else 'build/stw2txt'
STYLE = sys.argv[2] if len(sys.argv) > 2 else None   # build/stw_vs_rtf

def words(b): return re.findall(rb'\S+', b)
# Merge fields: Atari Works' export writes LABELS.STD:Company, RetroWP
# «Company» (in the Atari set, 0xAE and 0xAF) -- the same field.
def ours(b): return words(b.replace(b'\xae', b'').replace(b'\xaf', b''))
def theirs(b): return words(re.sub(rb'[A-Za-z0-9_]+\.STD:', b'', b))

if not os.path.isdir(CORPUS):
    print('stw-score: no corpus at %s (set STW_CORPUS); skipped' % CORPUS)
    sys.exit(0)
res = collections.Counter(); misses = []
for fn in sorted(os.listdir(CORPUS + '/STW')):
    base = os.path.splitext(fn)[0]
    tp = CORPUS + '/Text/' + base + '.TXT'
    if not os.path.exists(tp):
        continue
    run = subprocess.run([TOOL, CORPUS + '/STW/' + fn], capture_output=True)
    if run.returncode != 0:
        res['does not open'] += 1; misses.append((0.0, base)); continue
    txt = open(tp, 'rb').read()
    if re.search(rb'\.STD:', txt):   # a merge document: compare fields by name
        a, b = ours(run.stdout), theirs(txt)
    else:
        a, b = words(run.stdout), words(txt)
    if a == b:
        res['identical'] += 1
    else:
        r = difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()
        res['>=99%' if r >= .99 else '>=90%' if r >= .9 else 'worse'] += 1
        misses.append((r, base))
total = sum(res.values())
print('STW SCORE: %d of %d identical  %s' % (res['identical'], total, dict(res)))
for r, base in sorted(misses):
    print('  %-10s %5.1f%%' % (base, 100 * r))

if STYLE:
    keys = ('align', 'bold', 'italic', 'underline', 'size')
    tot = collections.Counter(); per = []
    for fn in sorted(os.listdir(CORPUS + '/STW')):
        base = os.path.splitext(fn)[0]
        rp = CORPUS + '/RTF/' + base + '.RTF'
        if not os.path.exists(rp):
            continue
        run = subprocess.run([STYLE, CORPUS + '/STW/' + fn, rp], capture_output=True, text=True)
        if run.returncode != 0:
            tot['unread'] += 1; continue
        v = dict(re.findall(r'([a-z]+) (\d+)', run.stdout.split(' ', 3)[3]))
        m = int(v['matched']); tot['matched'] += m
        for k in keys:
            tot[k] += int(v[k])
        if m:
            per.append((min(int(v[k]) / m for k in keys), base, {k: int(v[k]) * 100 // m for k in keys}))
    m = tot['matched'] or 1
    print('STW STYLE vs RTF: %d characters compared; ' % tot['matched'] +
          ', '.join('%s %.1f%%' % (k, 100.0 * tot[k] / m) for k in keys) +
          ('; %d RTFs unread' % tot['unread'] if tot['unread'] else ''))
    for worst, base, pc in sorted(per)[:12]:
        print('  %-10s %s' % (base, pc))
