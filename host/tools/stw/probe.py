#!/usr/bin/env python3
"""Atari Works .STW structure probe (research; the C importer follows)."""
import struct, sys, os, re

def walk(d):
    info = struct.unpack('>14H', d[0x13e:0x13e+28])
    nrec, lines, pics = info[4], info[5], info[6]
    p = 0x170 + 16 * lines
    nt = nf = 0; recs = []; fields = []
    for r in range(nrec):
        idx = struct.unpack('>H', d[p:p+2])[0]
        for k in range(nf):   # merge fields: 8 bytes, database (14), field (32)
            f = p + 2 + 2 * nt + 54 * k
            fields.append((d[f+8:f+22].split(b'\0')[0], d[f+22:f+54].split(b'\0')[0]))
        q = p + 2 + 2 * nt + 54 * nf
        w = struct.unpack('>15H', d[q:q+30])
        recs.append((idx, d[p+2:p+2+2*nt], w))
        nt, nf = w[3], w[11]; p = q + 30
    total = struct.unpack('>H', d[p:p+2])[0]
    for k in range(nf):
        f = p + 2 + 2 * nt + 54 * k
        fields.append((d[f+8:f+22].split(b'\0')[0], d[f+22:f+54].split(b'\0')[0]))
    p += 2 + 2 * nt + 54 * nf
    objs = []
    for k in range(pics):
        hdr = d[p:p+176]
        kind = d[p+2]
        if kind & 0x7f in (5, 6):   # a GEM metafile (5) or IMG (6), a 32-bit length first
            n = struct.unpack('>I', d[p+176:p+180])[0]
            objs.append((kind, hdr, d[p+180:p+180+n])); p += 180 + n
        else:   # a box, rule or frame: the header is all
            objs.append((kind, hdr, b'')); p += 176
    return dict(info=info, recs=recs, total=total, objs=objs, body=p, fields=fields)

def body_text(d, p, paras, fields=()):
    """The body from p: text bytes, 0x0a ends a paragraph, 0x00 starts a
    7-byte style record; stop after `paras` paragraphs."""
    out = bytearray(); n = 0; n_f = [0]
    while p < len(d):
        c = d[p]
        if c == 0:
            # the end: a lone 00, then the font table's first entry --
            # 00, an id, the face's name (a style record has a size there)
            if p + 4 < len(d) and d[p+1] == 0 and 0x20 < d[p+4] < 0x7f:
                break
            p += 8; continue
        if c == 1 and n_f[0] < len(fields):   # a merge field, in order
            db, name = fields[n_f[0]]; n_f[0] += 1
            out += db + b':' + name; p += 1; continue
        out.append(c)
        if c == 0x0a: n += 1
        p += 1
    return bytes(out), p

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    r = walk(d)
    print(r['info'], 'records', len(r['recs']), 'total', r['total'], 'pics', len(r['objs']), 'body at %x' % r['body'])
    t, end = body_text(d, r['body'], r['total'])
    print(t[:400])
    print('ends at %x of %x' % (end, len(d)), d[end:end+24].hex(' '))
