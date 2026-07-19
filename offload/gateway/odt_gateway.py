#!/usr/bin/env python3
"""Standalone ODT<->RTF conversion gateway (M7 T7.2).

A third offload/gateway executable, alongside mock_gateway.c (fixture-
backed THESAURUS/SPELL_CHECK) and ollama_gateway.c (real-LLM-backed
THESAURUS). This one answers OFFLOAD_CONVERT_IMPORT and
OFFLOAD_CONVERT_EXPORT for format_id=ODT -- everything else, including
CONVERT_EXPORT for PDF if that ever lands, responds OFFLOAD_UNSUPPORTED.

Written in Python, not C, per CLAUDE.md's explicit allowance
("offload/gateway: the Pi-side gateway (modern C/Python)"): ODT is a ZIP
of XML, and Python's stdlib (zipfile, xml.etree) makes that tractable in
a few hundred lines instead of hand-rolling DEFLATE and an XML parser in
C -- the same "don't reinvent a real subsystem" judgment call
docs/services/spell.md already made about not building a Hunspell-
compatible affix engine from scratch.

Design decision (docs/formats/offload-protocol.md has the full writeup):
RTF is the wire-level interchange format, not the internal document
model. The engine always ships/receives RTF across this op, using its
own already-tested format_rtf_import/format_rtf_export -- this gateway
only ever needs to convert ODT <-> RTF, and specifically RTF in the ONE
shape format_rtf_export produces / format_rtf_import accepts (documented
exactly in docs/formats/rtf.md), not arbitrary third-party RTF. That is
what makes a from-scratch RTF reader/writer here tractable: it is a
narrow, fully-specified target, not a general RTF implementation.

Re-implements the wire protocol (magic/version/type/op/status/seq/
length/payload/CRC16-CCITT-FALSE, HELLO/CAPS handshake) independently in
Python rather than sharing code with offload/transport/wire.c (which is
C) -- exactly like mock_gateway.c and ollama_gateway.c don't share code
with each other beyond gw_common.c (also C-only); a Python gateway has
no analogous shared module to link against, so it re-derives the same
documented byte layout from docs/formats/offload-protocol.md instead.
Deliberately does NOT reimplement wire.c's magic-byte resync-on-corrupt-
frame behavior -- this is host-side tooling on a reliable local
transport (TCP loopback, or the FIFO bridge ci_atari_serial.sh-style
tooling uses), and a malformed frame here just tears down the
connection, an honest, narrower scope than wire.c's own resync
contract, not a silent gap.
"""

import argparse
import io
import os
import re
import select
import socket
import struct
import sys
import time
import zipfile
from xml.sax.saxutils import escape as xml_escape
import xml.etree.ElementTree as ET

# ---------------------------------------------------------------------
# Wire protocol (docs/formats/offload-protocol.md)
# ---------------------------------------------------------------------

WIRE_MAGIC0 = 0x57
WIRE_MAGIC1 = 0x50
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 12
WIRE_CRC_SIZE = 2

MSG_HELLO, MSG_CAPS, MSG_REQUEST, MSG_RESPONSE, MSG_ERROR = range(5)

(OP_SPELL_CHECK, OP_GRAMMAR_CHECK, OP_THESAURUS,
 OP_CONVERT_IMPORT, OP_CONVERT_EXPORT, OP_CHAT, OP_IMAGE) = range(7)

STATUS_OK, STATUS_TIMEOUT, STATUS_UNSUPPORTED, STATUS_ERR = range(4)

FORMAT_ODT = 1
FORMAT_PDF = 2

CAPS_PAYLOAD_SIZE = 42  # 2 (version) + 4 (ops_supported) + 4 (max_payload) + 32 (name)


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def encode_frame(msg_type, op, status, seq, payload):
    header_tail = bytes([WIRE_VERSION, msg_type, op, status]) + struct.pack('>H', seq) + struct.pack('>I', len(payload))
    crc = crc16_ccitt_false(header_tail + payload)
    return bytes([WIRE_MAGIC0, WIRE_MAGIC1]) + header_tail + payload + struct.pack('>H', crc)


def encode_caps(ops_supported, name):
    name_bytes = name.encode('utf-8')[:32].ljust(32, b'\x00')
    return struct.pack('>HII', WIRE_VERSION, ops_supported, 65536) + name_bytes


class GatewayError(Exception):
    pass


class GatewayTimeout(GatewayError):
    pass


def read_exact(read_fd, n, deadline):
    buf = b''
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GatewayTimeout()
        r, _, _ = select.select([read_fd], [], [], remaining)
        if not r:
            raise GatewayTimeout()
        chunk = os.read(read_fd, n - len(buf))
        if not chunk:
            raise GatewayError('peer closed')
        buf += chunk
    return buf


def read_frame(read_fd, timeout_s):
    deadline = time.monotonic() + timeout_s
    header = read_exact(read_fd, WIRE_HEADER_SIZE, deadline)
    if header[0] != WIRE_MAGIC0 or header[1] != WIRE_MAGIC1:
        raise GatewayError('bad magic')
    version, msg_type, op, status = header[2], header[3], header[4], header[5]
    seq = struct.unpack('>H', header[6:8])[0]
    length = struct.unpack('>I', header[8:12])[0]
    if version != WIRE_VERSION:
        raise GatewayError('bad version')
    payload = read_exact(read_fd, length, deadline) if length else b''
    crc_bytes = read_exact(read_fd, WIRE_CRC_SIZE, deadline)
    expected = crc16_ccitt_false(header[2:] + payload)
    if struct.unpack('>H', crc_bytes)[0] != expected:
        raise GatewayError('crc mismatch')
    return {'type': msg_type, 'op': op, 'status': status, 'seq': seq}, payload


def open_fifo_rdwr(path):
    """O_RDWR, not O_RDONLY/O_WRONLY -- avoids the classic named-FIFO
    open-blocks-until-peer-connects deadlock (same trick
    gw_common.c's gw_open_fifo_rdwr and platform/host/transport_host.c's
    pipe: backend already rely on)."""
    if not os.path.exists(path):
        os.mkfifo(path, 0o600)
    return os.open(path, os.O_RDWR)


# ---------------------------------------------------------------------
# ODF (ODT) unit conversion
# ---------------------------------------------------------------------

_ODF_LEN_RE = re.compile(r'^(-?[0-9.]+)(in|cm|mm|pt|px)?$')


def twips_from_odf_len(s):
    if not s:
        return 0
    m = _ODF_LEN_RE.match(s.strip())
    if not m:
        return 0
    val = float(m.group(1))
    unit = m.group(2) or 'in'
    inches = {'in': val, 'cm': val / 2.54, 'mm': val / 25.4,
              'pt': val / 72.0, 'px': val / 96.0}[unit]
    return int(round(inches * 1440))


def odf_len_from_twips(twips):
    return '%.4fin' % (twips / 1440.0)


_ODF_NS = {
    'office': 'urn:oasis:names:tc:opendocument:xmlns:office:1.0',
    'text': 'urn:oasis:names:tc:opendocument:xmlns:text:1.0',
    'style': 'urn:oasis:names:tc:opendocument:xmlns:style:1.0',
    'fo': 'urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0',
}


def _q(prefix, local):
    return '{%s}%s' % (_ODF_NS[prefix], local)


def _new_para():
    return {'align': 'left', 'li': 0, 'ri': 0, 'fi': 0, 'sb': 0, 'sa': 0, 'runs': []}


def _new_run():
    return {'b': False, 'i': False, 'ul': False, 'strike': False, 'super': False, 'sub': False,
            'size_twips': 240, 'color': None, 'text': ''}


# ---------------------------------------------------------------------
# ODT bytes -> paragraph model (CONVERT_IMPORT's first half)
# ---------------------------------------------------------------------

def odt_bytes_to_paragraphs(odt_bytes):
    with zipfile.ZipFile(io.BytesIO(odt_bytes)) as zf:
        content = zf.read('content.xml')
    root = ET.fromstring(content)

    para_styles = {}
    text_styles = {}
    autostyles = root.find('.//' + _q('office', 'automatic-styles'))
    if autostyles is not None:
        for st in autostyles.findall(_q('style', 'style')):
            name = st.get(_q('style', 'name'))
            family = st.get(_q('style', 'family'))
            if family == 'paragraph':
                pp = st.find(_q('style', 'paragraph-properties'))
                d = _new_para()
                if pp is not None:
                    align = pp.get(_q('fo', 'text-align'))
                    d['align'] = {'end': 'right', 'start': 'left'}.get(align, align) if align else 'left'
                    if d['align'] not in ('left', 'right', 'center', 'justify'):
                        d['align'] = 'left'
                    d['li'] = twips_from_odf_len(pp.get(_q('fo', 'margin-left')))
                    d['ri'] = twips_from_odf_len(pp.get(_q('fo', 'margin-right')))
                    d['fi'] = twips_from_odf_len(pp.get(_q('fo', 'text-indent')))
                    d['sb'] = twips_from_odf_len(pp.get(_q('fo', 'margin-top')))
                    d['sa'] = twips_from_odf_len(pp.get(_q('fo', 'margin-bottom')))
                para_styles[name] = d
            elif family == 'text':
                tp = st.find(_q('style', 'text-properties'))
                d = _new_run()
                if tp is not None:
                    if tp.get(_q('fo', 'font-weight')) == 'bold':
                        d['b'] = True
                    if tp.get(_q('fo', 'font-style')) == 'italic':
                        d['i'] = True
                    uls = tp.get(_q('style', 'text-underline-style'))
                    if uls and uls != 'none':
                        d['ul'] = True
                    tls = tp.get(_q('style', 'text-line-through-style'))
                    if tls and tls != 'none':
                        d['strike'] = True
                    pos = tp.get(_q('style', 'text-position'))
                    if pos:
                        if pos.startswith('super'):
                            d['super'] = True
                        elif pos.startswith('sub'):
                            d['sub'] = True
                    fsz = tp.get(_q('fo', 'font-size'))
                    if fsz:
                        d['size_twips'] = twips_from_odf_len(fsz) or d['size_twips']
                    color = tp.get(_q('fo', 'color'))
                    if color and color.startswith('#') and len(color) == 7:
                        d['color'] = (int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16))
                text_styles[name] = d

    paragraphs = []
    body_text = root.find('.//' + _q('office', 'text'))
    if body_text is None:
        return paragraphs

    for p_el in body_text.findall(_q('text', 'p')):
        style_name = p_el.get(_q('text', 'style-name'))
        para = dict(para_styles.get(style_name, _new_para()))
        para['runs'] = []

        def add_run(rstyle_name, text):
            if not text:
                return
            rd = dict(text_styles.get(rstyle_name, _new_run()))
            rd['text'] = text
            para['runs'].append(rd)

        add_run(None, p_el.text)
        for child in p_el:
            tag = child.tag
            if tag == _q('text', 'span'):
                add_run(child.get(_q('text', 'style-name')), child.text)
            elif tag == _q('text', 's'):
                add_run(None, ' ' * int(child.get(_q('text', 'c'), '1')))
            elif tag == _q('text', 'tab'):
                add_run(None, '\t')
            add_run(None, child.tail)

        paragraphs.append(para)

    return paragraphs


# ---------------------------------------------------------------------
# paragraph model -> ODT bytes (CONVERT_EXPORT's second half, format=ODT)
# ---------------------------------------------------------------------

_ODT_MIMETYPE = b'application/vnd.oasis.opendocument.text'

_ODT_MANIFEST = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.2">\n'
    ' <manifest:file-entry manifest:full-path="/" manifest:version="1.2" manifest:media-type="application/vnd.oasis.opendocument.text"/>\n'
    ' <manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>\n'
    '</manifest:manifest>\n'
)

_ODT_CONTENT_HEADER = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<office:document-content '
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
    'office:version="1.2">\n<office:automatic-styles>\n'
)


def paragraphs_to_odt_bytes(paragraphs):
    """Minimal but real, spec-valid ODT: mimetype + META-INF/manifest.xml
    + content.xml only -- no styles.xml/meta.xml/settings.xml/Thumbnails,
    all optional per ODF 1.2. Verified openable by real LibreOffice
    (`soffice --headless --convert-to txt`), not just self-consistent
    with this project's own odt_bytes_to_paragraphs (docs/formats/odt.md
    has the cross-validation trace). Deliberately does NOT deduplicate
    styles the way a hand-authored ODT might -- one paragraph style and
    one text-run style per actual paragraph/run instance, the same
    pattern real LibreOffice output itself uses (P1, P2, T1, T2, ...),
    simpler to generate and still fully valid ODF."""
    styles = []
    body = []

    for pi, p in enumerate(paragraphs):
        pstyle_name = 'P%d' % (pi + 1)
        pp_attrs = []
        align = p.get('align', 'left')
        if align != 'left':
            pp_attrs.append('fo:text-align="%s"' % align)
        if p.get('li'):
            pp_attrs.append('fo:margin-left="%s"' % odf_len_from_twips(p['li']))
        if p.get('ri'):
            pp_attrs.append('fo:margin-right="%s"' % odf_len_from_twips(p['ri']))
        if p.get('fi'):
            pp_attrs.append('fo:text-indent="%s"' % odf_len_from_twips(p['fi']))
        if p.get('sb'):
            pp_attrs.append('fo:margin-top="%s"' % odf_len_from_twips(p['sb']))
        if p.get('sa'):
            pp_attrs.append('fo:margin-bottom="%s"' % odf_len_from_twips(p['sa']))
        styles.append(
            '<style:style style:name="%s" style:family="paragraph" style:parent-style-name="Standard">'
            '<style:paragraph-properties %s/></style:style>' % (pstyle_name, ' '.join(pp_attrs)))

        run_xml = []
        runs = p['runs'] or [_new_run()]
        for ri, r in enumerate(runs):
            tp_attrs = []
            if r.get('b'):
                tp_attrs.append('fo:font-weight="bold"')
            if r.get('i'):
                tp_attrs.append('fo:font-style="italic"')
            if r.get('ul'):
                tp_attrs.append('style:text-underline-style="solid" style:text-underline-width="auto" '
                                 'style:text-underline-color="font-color"')
            if r.get('strike'):
                tp_attrs.append('style:text-line-through-style="solid"')
            if r.get('super'):
                tp_attrs.append('style:text-position="super 58%"')
            elif r.get('sub'):
                tp_attrs.append('style:text-position="sub 58%"')
            tp_attrs.append('fo:font-size="%.1fpt"' % (r.get('size_twips', 240) / 20.0))
            if r.get('color'):
                tp_attrs.append('fo:color="#%02x%02x%02x"' % r['color'])

            if tp_attrs:
                tstyle_name = 'P%dT%d' % (pi + 1, ri + 1)
                styles.append('<style:style style:name="%s" style:family="text">'
                               '<style:text-properties %s/></style:style>' % (tstyle_name, ' '.join(tp_attrs)))
                run_xml.append('<text:span text:style-name="%s">%s</text:span>' %
                                (tstyle_name, xml_escape(r.get('text', ''))))
            else:
                run_xml.append(xml_escape(r.get('text', '')))

        body.append('<text:p text:style-name="%s">%s</text:p>' % (pstyle_name, ''.join(run_xml)))

    content = (_ODT_CONTENT_HEADER + '\n'.join(styles) +
               '</office:automatic-styles><office:body><office:text>' +
               ''.join(body) + '</office:text></office:body></office:document-content>')

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as zf:
        zf.writestr('mimetype', _ODT_MIMETYPE, compress_type=zipfile.ZIP_STORED)
        zf.writestr('META-INF/manifest.xml', _ODT_MANIFEST)
        zf.writestr('content.xml', content.encode('utf-8'))
    return buf.getvalue()


# ---------------------------------------------------------------------
# RTF <-> paragraph model. Deliberately narrow: only the ONE shape
# format_rtf_export produces / format_rtf_import accepts -- see the
# module docstring and docs/formats/offload-protocol.md.
# ---------------------------------------------------------------------

_CP1252_TO_UNICODE = {
    0x80: 0x20AC, 0x82: 0x201A, 0x83: 0x0192, 0x84: 0x201E, 0x85: 0x2026,
    0x86: 0x2020, 0x87: 0x2021, 0x88: 0x02C6, 0x89: 0x2030, 0x8A: 0x0160,
    0x8B: 0x2039, 0x8C: 0x0152, 0x8E: 0x017D, 0x91: 0x2018, 0x92: 0x2019,
    0x93: 0x201C, 0x94: 0x201D, 0x95: 0x2022, 0x96: 0x2013, 0x97: 0x2014,
    0x98: 0x02DC, 0x99: 0x2122, 0x9A: 0x0161, 0x9B: 0x203A, 0x9C: 0x0153,
    0x9E: 0x017E, 0x9F: 0x0178,
}
_UNICODE_TO_CP1252 = {v: k for k, v in _CP1252_TO_UNICODE.items()}

_RTF_TOKEN_RE = re.compile(rb"\\'([0-9a-fA-F]{2})|\\([a-zA-Z]+)(-?[0-9]+)?[ ]?|([{}])|([^\\{}\r\n]+)")


def _rtf_escape_text(s):
    out = []
    for ch in s:
        cp = ord(ch)
        if ch in '{}\\':
            out.append('\\' + ch)
        elif cp < 0x80:
            out.append(ch)
        elif 0xA0 <= cp <= 0xFF:
            out.append("\\'%02x" % cp)
        elif cp in _UNICODE_TO_CP1252:
            out.append("\\'%02x" % _UNICODE_TO_CP1252[cp])
        else:
            out.append('?')
    return ''.join(out)


def paragraphs_to_rtf_bytes(paragraphs):
    colors = []

    def color_index(rgb):
        if not rgb or rgb == (0, 0, 0):
            return 0
        if rgb not in colors:
            colors.append(rgb)
        return colors.index(rgb) + 1

    for p in paragraphs:
        for r in p['runs']:
            color_index(r.get('color'))

    parts = ['{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Helvetica;}}']
    if colors:
        parts.append('{\\colortbl;')
        for (r, g, b) in colors:
            parts.append('\\red%d\\green%d\\blue%d;' % (r, g, b))
        parts.append('}')

    for p in paragraphs:
        parts.append('\\pard')
        align = p.get('align', 'left')
        if align == 'right':
            parts.append('\\qr')
        elif align == 'center':
            parts.append('\\qc')
        elif align == 'justify':
            parts.append('\\qj')
        if p.get('li'):
            parts.append('\\li%d' % p['li'])
        if p.get('ri'):
            parts.append('\\ri%d' % p['ri'])
        if p.get('fi'):
            parts.append('\\fi%d' % p['fi'])
        if p.get('sb'):
            parts.append('\\sb%d' % p['sb'])
        if p.get('sa'):
            parts.append('\\sa%d' % p['sa'])

        for r in (p['runs'] or [_new_run()]):
            parts.append('{')
            if r.get('b'):
                parts.append('\\b')
            if r.get('i'):
                parts.append('\\i')
            if r.get('ul'):
                parts.append('\\ul')
            if r.get('strike'):
                parts.append('\\strike')
            if r.get('super'):
                parts.append('\\super')
            if r.get('sub'):
                parts.append('\\sub')
            parts.append('\\fs%d' % (r.get('size_twips', 240) // 10))
            ci = color_index(r.get('color'))
            if ci:
                parts.append('\\cf%d' % ci)
            parts.append(' ')
            parts.append(_rtf_escape_text(r.get('text', '')))
            parts.append('}')
        parts.append('\\par\r\n')

    parts.append('}')
    return ''.join(parts).encode('cp1252', errors='replace')


def rtf_bytes_to_paragraphs(rtf_bytes):
    paragraphs = []
    colors = [(0, 0, 0)]
    cur_para = _new_para()
    cur_run = _new_run()
    dest_stack = ['body']  # top = current (innermost) group's destination

    for m in _RTF_TOKEN_RE.finditer(rtf_bytes):
        hexval, word, num, brace, text = m.groups()
        word = word.decode('ascii') if word is not None else None
        brace = brace.decode('ascii') if brace is not None else None
        dest = dest_stack[-1]

        if brace == '{' or brace == '}':
            # format_rtf_export's character runs are SIBLING brace groups
            # at paragraph level, {run1}{run2}..., never nested inside
            # each other, so flushing/resetting cur_run on every brace
            # (open or close) is correct: a run's text always lands
            # between its own { and }. Flushing an empty cur_run while
            # inside/around a \fonttbl or \colortbl group is a harmless
            # no-op (those never touch cur_run, since dest != 'body' there).
            if cur_run['text']:
                cur_para['runs'].append(cur_run)
            cur_run = _new_run()
            if brace == '{':
                dest_stack.append(dest_stack[-1])
            elif len(dest_stack) > 1:
                dest_stack.pop()
            continue

        if hexval is not None:
            if dest != 'body':
                continue
            b = int(hexval, 16)
            if b < 0x80 or 0xA0 <= b <= 0xFF:
                cur_run['text'] += chr(b)
            else:
                cur_run['text'] += chr(_CP1252_TO_UNICODE.get(b, ord('?')))
            continue

        if text is not None:
            if dest == 'body':
                cur_run['text'] += text.decode('cp1252')
            continue

        if word is None:
            continue
        n = int(num) if num else None

        if word == 'fonttbl':
            dest_stack[-1] = 'fonttbl'
            continue
        if word == 'colortbl':
            dest_stack[-1] = 'colortbl'
            continue

        if dest == 'colortbl':
            if word == 'red':
                colors.append([n, 0, 0])
            elif word == 'green' and colors:
                colors[-1][1] = n
            elif word == 'blue' and colors:
                colors[-1][2] = n
            continue
        if dest != 'body':
            continue

        if word == 'pard':
            cur_para['align'] = 'left'
            cur_para['li'] = cur_para['ri'] = cur_para['fi'] = cur_para['sb'] = cur_para['sa'] = 0
        elif word == 'ql':
            cur_para['align'] = 'left'
        elif word == 'qr':
            cur_para['align'] = 'right'
        elif word == 'qc':
            cur_para['align'] = 'center'
        elif word == 'qj':
            cur_para['align'] = 'justify'
        elif word == 'li':
            cur_para['li'] = n
        elif word == 'ri':
            cur_para['ri'] = n
        elif word == 'fi':
            cur_para['fi'] = n
        elif word == 'sb':
            cur_para['sb'] = n
        elif word == 'sa':
            cur_para['sa'] = n
        elif word == 'b':
            cur_run['b'] = True
        elif word == 'i':
            cur_run['i'] = True
        elif word == 'ul':
            cur_run['ul'] = True
        elif word == 'ulnone':
            cur_run['ul'] = False
        elif word == 'strike':
            cur_run['strike'] = True
        elif word == 'super':
            cur_run['super'] = True
        elif word == 'sub':
            cur_run['sub'] = True
        elif word == 'fs':
            cur_run['size_twips'] = n * 10
        elif word == 'cf':
            if n is not None and 0 <= n < len(colors):
                cur_run['color'] = tuple(colors[n]) if n else None
        elif word == 'par':
            if cur_run['text']:
                cur_para['runs'].append(cur_run)
            paragraphs.append(cur_para)
            cur_para = _new_para()
            cur_run = _new_run()
        # anything else: ignored (forward compatible, matches the C importer)

    return paragraphs


# ---------------------------------------------------------------------
# Request dispatch + server loop
# ---------------------------------------------------------------------

def handle_convert_import(payload):
    if len(payload) < 1:
        return STATUS_ERR, b''
    format_id, body = payload[0], payload[1:]
    if format_id != FORMAT_ODT:
        return STATUS_UNSUPPORTED, b''
    try:
        return STATUS_OK, paragraphs_to_rtf_bytes(odt_bytes_to_paragraphs(body))
    except Exception:
        return STATUS_ERR, b''


def handle_convert_export(payload):
    if len(payload) < 1:
        return STATUS_ERR, b''
    format_id, rtf_bytes = payload[0], payload[1:]
    if format_id not in (FORMAT_ODT, FORMAT_PDF):
        return STATUS_UNSUPPORTED, b''
    try:
        paragraphs = rtf_bytes_to_paragraphs(rtf_bytes)
    except Exception:
        return STATUS_ERR, b''
    try:
        if format_id == FORMAT_ODT:
            return STATUS_OK, paragraphs_to_odt_bytes(paragraphs)
        return STATUS_UNSUPPORTED, b''  # PDF: not implemented by this gateway
    except Exception:
        return STATUS_ERR, b''


def handle_request(op, payload):
    if op == OP_CONVERT_IMPORT:
        return handle_convert_import(payload)
    if op == OP_CONVERT_EXPORT:
        return handle_convert_export(payload)
    return STATUS_UNSUPPORTED, b''


def serve(read_fd, write_fd, hello_timeout_ms):
    try:
        hdr, _ = read_frame(read_fd, hello_timeout_ms / 1000.0)
    except GatewayError:
        return 1
    if hdr['type'] != MSG_HELLO:
        return 1

    caps_payload = encode_caps((1 << OP_CONVERT_IMPORT) | (1 << OP_CONVERT_EXPORT), 'odt-gateway')
    os.write(write_fd, encode_frame(MSG_CAPS, 0, 0, 0, caps_payload))

    while True:
        try:
            hdr, payload = read_frame(read_fd, 120.0)
        except GatewayError:
            break
        status, resp_payload = handle_request(hdr['op'], payload)
        os.write(write_fd, encode_frame(MSG_RESPONSE, hdr['op'], status, hdr['seq'], resp_payload))

    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial-rx')
    parser.add_argument('--serial-tx')
    parser.add_argument('--hello-timeout-ms', type=int, default=5000)
    args = parser.parse_args()

    if args.serial_rx and args.serial_tx:
        rx_fd = open_fifo_rdwr(args.serial_rx)
        tx_fd = open_fifo_rdwr(args.serial_tx)
        print('serial', flush=True)
        return serve(rx_fd, tx_fd, args.hello_timeout_ms)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', 0))
    srv.listen(1)
    print(srv.getsockname()[1], flush=True)
    conn, _ = srv.accept()
    srv.close()
    fd = conn.fileno()
    return serve(fd, fd, args.hello_timeout_ms)


if __name__ == '__main__':
    sys.exit(main() or 0)
