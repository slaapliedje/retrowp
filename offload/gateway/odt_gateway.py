#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone document-conversion gateway: ODT<->RTF and RTF->PDF (M7
T7.2, T7.3).

A third offload/gateway executable, alongside mock_gateway.c (fixture-
backed THESAURUS/SPELL_CHECK) and ollama_gateway.c (real-LLM-backed
THESAURUS). This one answers OFFLOAD_CONVERT_IMPORT for format_id=ODT,
and OFFLOAD_CONVERT_EXPORT for format_id=ODT or format_id=PDF --
everything else responds OFFLOAD_UNSUPPORTED. There is deliberately no
PDF *import*: PDF has no general, reliable text-reconstruction guarantee
(docs/formats/offload-protocol.md has the reasoning), so it is export-only.

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

import io
import os
import re
import sys
import zipfile
import zlib
from xml.sax.saxutils import escape as xml_escape
import xml.etree.ElementTree as ET

from wire_protocol import (
    STATUS_OK, STATUS_UNSUPPORTED, STATUS_ERR,
    OP_CONVERT_IMPORT, OP_CONVERT_EXPORT,
    run_gateway,
)

FORMAT_ODT = 1
FORMAT_PDF = 2

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
    'svg': 'urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0',
}


def _q(prefix, local):
    return '{%s}%s' % (_ODF_NS[prefix], local)


def _new_para():
    # sl/slmult: RTF line spacing; tabs: [(position_twips, kind)] with kind
    # one of 'left', 'right', 'center', 'decimal' -- the same stops the
    # engine lays out with (docmodel.h), from the column's left edge.
    return {'align': 'left', 'li': 0, 'ri': 0, 'fi': 0, 'sb': 0, 'sa': 0,
            'sl': 0, 'slmult': 0, 'tabs': [], 'runs': []}


def _new_run():
    # font: the face's name, None for the reader's default; family: RTF's
    # generic family word without the 'f' ('roman', 'swiss', 'modern',
    # 'script', 'decor'), None for unknown -- what a PDF picks its
    # built-in face by and an ODT declares as the generic family.
    return {'b': False, 'i': False, 'ul': False, 'strike': False, 'super': False, 'sub': False,
            'size_twips': 240, 'color': None, 'font': None, 'family': None, 'text': ''}


_FAMILY_WORDS = (
    ('modern', ('courier', 'mono', 'consol', 'typewriter', 'fixed', 'terminal', 'menlo', 'monaco')),
    ('swiss', ('sans',)),
    ('roman', ('times', 'roman', 'serif', 'georgia', 'garamond', 'palatino', 'book', 'century',
               'cambria', 'dutch', 'new york', 'baskerville', 'bodoni')),
    ('script', ('script', 'brush', 'chancery', 'comic')),
)


def font_family_guess(name):
    """A family for a bare face name -- the engine's wp_font_family_guess,
    word for word, so both sides of the link agree."""
    if not name:
        return None
    low = name.lower()
    for family, words in _FAMILY_WORDS:
        if any(w in low for w in words):
            return family
    return 'swiss'


# ---------------------------------------------------------------------
# ODT bytes -> paragraph model (CONVERT_IMPORT's first half)
# ---------------------------------------------------------------------

def odt_bytes_to_paragraphs(odt_bytes):
    with zipfile.ZipFile(io.BytesIO(odt_bytes)) as zf:
        content = zf.read('content.xml')
    root = ET.fromstring(content)

    para_styles = {}
    text_styles = {}
    # <style:font-face style:name="X" svg:font-family="'X'"
    # style:font-family-generic="roman"/>: what a style:font-name names.
    faces = {}
    for decls in (root.find('.//' + _q('office', 'font-face-decls')),):
        if decls is None:
            continue
        for ff in decls.findall(_q('style', 'font-face')):
            fam = (ff.get(_q('svg', 'font-family')) or ff.get(_q('style', 'name')) or '').strip("'\"")
            generic = ff.get(_q('style', 'font-family-generic'))
            faces[ff.get(_q('style', 'name'))] = (fam, generic if generic in ('roman', 'swiss', 'modern', 'script', 'decorative') else None)
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
                    d['pagebb'] = (pp.get(_q('fo', 'break-before')) == 'page')
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
                    # A declared face by name, or (LibreOffice's other
                    # spelling) the family itself, inline.
                    fname = tp.get(_q('style', 'font-name'))
                    inline = tp.get(_q('fo', 'font-family'))
                    if fname:
                        fam, generic = faces.get(fname, (fname, None))
                    elif inline:
                        fam, generic = inline.strip("'\""), tp.get(_q('style', 'font-family-generic'))
                    if fname or inline:
                        d['font'] = fam or fname
                        d['family'] = ({'decorative': 'decor'}.get(generic, generic)
                                       if generic in ('roman', 'swiss', 'modern', 'script', 'decorative')
                                       else None) or font_family_guess(d['font'])
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

        def walk(el, rstyle):
            # A span's own children too: LibreOffice (and this module's
            # own writer) put <text:tab/> and <text:s/> inside spans, and
            # reading only a span's leading text dropped everything after
            # its first tab.
            add_run(rstyle, el.text)
            for child in el:
                tag = child.tag
                if tag == _q('text', 'span'):
                    walk(child, child.get(_q('text', 'style-name')))
                elif tag == _q('text', 's'):
                    add_run(rstyle, ' ' * int(child.get(_q('text', 'c'), '1')))
                elif tag == _q('text', 'tab'):
                    add_run(rstyle, '\t')
                add_run(rstyle, child.tail)

        walk(p_el, None)

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
    ' <manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>\n'
    '</manifest:manifest>\n'
)

_ODF_NS_DECLS = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
)


def _margin_parts(text):
    """A header or footer as the engine keeps it ('|' parts, '#' the page
    number) as its left, centre and right parts -- one part is centred,
    two go left and right, three are all three: the PDF's own rule."""
    parts = text.split('|')
    if len(parts) == 1:
        return ['', parts[0], '']
    if len(parts) == 2:
        return [parts[0], '', parts[1]]
    return [parts[0], parts[1], '|'.join(parts[2:])]


def _odt_margin_xml(text):
    """One header or footer paragraph: left<tab>centre<tab>right, '#' a
    live page-number field."""
    def part(t):
        return '<text:page-number text:select-page="current">1</text:page-number>'.join(
            xml_escape(seg) for seg in t.split('#'))
    return '<text:p text:style-name="RWPMargin">%s</text:p>' % '<text:tab/>'.join(
        part(t) for t in _margin_parts(text))


def _odt_styles_xml(page, header, footer):
    """styles.xml: the page (size and margins, in points) as the one
    master page every paragraph is on, with the header and footer in it.
    Their paragraph style has a centre tab stop mid-column and a right one
    at its edge, so the three parts land where the PDF puts them."""
    w, h, ml, mr, mt, mb = page
    col = max(72.0, w - ml - mr)
    master = ''
    if header:
        master += '<style:header>%s</style:header>' % _odt_margin_xml(header)
    if footer:
        master += '<style:footer>%s</style:footer>' % _odt_margin_xml(footer)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document-styles ' + _ODF_NS_DECLS + 'office:version="1.2">'
            '<office:styles>'
            '<style:style style:name="RWPMargin" style:family="paragraph">'
            '<style:paragraph-properties><style:tab-stops>'
            '<style:tab-stop style:position="%.2fpt" style:type="center"/>'
            '<style:tab-stop style:position="%.2fpt" style:type="right"/>'
            '</style:tab-stops></style:paragraph-properties>'
            '<style:text-properties fo:font-size="10pt"/></style:style>'
            '</office:styles>'
            '<office:automatic-styles><style:page-layout style:name="RWPPage">'
            '<style:page-layout-properties fo:page-width="%.2fpt" fo:page-height="%.2fpt" '
            'fo:margin-left="%.2fpt" fo:margin-right="%.2fpt" fo:margin-top="%.2fpt" '
            'fo:margin-bottom="%.2fpt"/></style:page-layout></office:automatic-styles>'
            '<office:master-styles><style:master-page style:name="Standard" '
            'style:page-layout-name="RWPPage">%s</style:master-page></office:master-styles>'
            '</office:document-styles>' % (col / 2.0, col, w, h, ml, mr, mt, mb, master))


def _pt_from_odf_len(s, default):
    t = twips_from_odf_len(s)
    return t / 20.0 if t else default


def odt_page_and_margins(odt_bytes):
    """From styles.xml: the page as (width, height, left, right, top,
    bottom) in points, or None, and the default master page's header and
    footer, each a list of parts (left/centre/right, an empty middle part
    dropped and a lone centre kept alone, the way the engine writes them),
    each part a list of ('text', s) and ('page',) -- or None."""
    with zipfile.ZipFile(io.BytesIO(odt_bytes)) as zf:
        if 'styles.xml' not in zf.namelist():
            return None, None, None
        root = ET.fromstring(zf.read('styles.xml'))
    masters = root.findall('.//' + _q('style', 'master-page'))
    if not masters:
        return None, None, None
    master = next((m for m in masters if m.get(_q('style', 'name')) == 'Standard'), masters[0])

    page = None
    layout_name = master.get(_q('style', 'page-layout-name'))
    for pl in root.findall('.//' + _q('style', 'page-layout')):
        if pl.get(_q('style', 'name')) != layout_name:
            continue
        pp = pl.find(_q('style', 'page-layout-properties'))
        if pp is not None:
            page = (_pt_from_odf_len(pp.get(_q('fo', 'page-width')), 612.0),
                    _pt_from_odf_len(pp.get(_q('fo', 'page-height')), 792.0),
                    _pt_from_odf_len(pp.get(_q('fo', 'margin-left')), 72.0),
                    _pt_from_odf_len(pp.get(_q('fo', 'margin-right')), 72.0),
                    _pt_from_odf_len(pp.get(_q('fo', 'margin-top')), 72.0),
                    _pt_from_odf_len(pp.get(_q('fo', 'margin-bottom')), 72.0))

    def margin(tag):
        el = master.find(_q('style', tag))
        p_el = el.find(_q('text', 'p')) if el is not None else None
        if p_el is None:
            return None
        parts = [[]]

        def add(t):
            if t:
                parts[-1].append(('text', t))

        def walk(e):
            add(e.text)
            for c in e:
                if c.tag == _q('text', 'tab'):
                    parts.append([])
                elif c.tag == _q('text', 'page-number'):
                    parts[-1].append(('page',))
                elif c.tag == _q('text', 's'):
                    add(' ' * int(c.get(_q('text', 'c'), '1')))
                else:
                    walk(c)
                add(c.tail)
        walk(p_el)
        if len(parts) == 3 and not parts[1]:
            parts = [parts[0], parts[2]]
        elif len(parts) == 3 and not parts[0] and not parts[2]:
            parts = [parts[1]]
        return parts if any(parts) else None

    return page, margin('header'), margin('footer')

_ODT_CONTENT_HEADER = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<office:document-content '
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
    'office:version="1.2">\n'
)


def _odt_text(text):
    """Run text as ODF wants it: escaped, and each tab a <text:tab/> --
    ODF collapses a literal tab character into a space like any other
    whitespace."""
    return xml_escape(text).replace('\t', '<text:tab/>')


def paragraphs_to_odt_bytes(paragraphs, page=None, header='', footer=''):
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
    faces = {}     # face name -> family, for <office:font-face-decls>

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
        if p.get('pagebb'):
            pp_attrs.append('fo:break-before="page"')
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
            if r.get('font'):
                if r['font'] not in faces:
                    faces[r['font']] = r.get('family') or font_family_guess(r['font'])
                tp_attrs.append('style:font-name="%s"' % xml_escape(r['font'], {'"': '&quot;'}))

            if tp_attrs:
                tstyle_name = 'P%dT%d' % (pi + 1, ri + 1)
                styles.append('<style:style style:name="%s" style:family="text">'
                               '<style:text-properties %s/></style:style>' % (tstyle_name, ' '.join(tp_attrs)))
                run_xml.append('<text:span text:style-name="%s">%s</text:span>' %
                                (tstyle_name, _odt_text(r.get('text', ''))))
            else:
                run_xml.append(_odt_text(r.get('text', '')))

        body.append('<text:p text:style-name="%s">%s</text:p>' % (pstyle_name, ''.join(run_xml)))

    decls = ''.join('<style:font-face style:name="%s" svg:font-family="%s"%s/>' %
                    (xml_escape(n, {'"': '&quot;'}), xml_escape("'%s'" % n, {'"': '&quot;'}),
                     ' style:font-family-generic="%s"' % ('decorative' if f == 'decor' else f)
                     if f in ('roman', 'swiss', 'modern', 'script', 'decor') else '')
                    for n, f in faces.items())
    content = (_ODT_CONTENT_HEADER +
               ('<office:font-face-decls>%s</office:font-face-decls>\n' % decls if decls else '') +
               '<office:automatic-styles>\n' + '\n'.join(styles) +
               '</office:automatic-styles><office:body><office:text>' +
               ''.join(body) + '</office:text></office:body></office:document-content>')

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as zf:
        zf.writestr('mimetype', _ODT_MIMETYPE, compress_type=zipfile.ZIP_STORED)
        # Deflated: only the mimetype must be stored as is (ODF 1.2), and
        # the rest packs several-fold -- room under the link's one-frame
        # response for a real document.
        zf.writestr('META-INF/manifest.xml', _ODT_MANIFEST, compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr('content.xml', content.encode('utf-8'), compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr('styles.xml', _odt_styles_xml(page or (612.0, 792.0, 72.0, 72.0, 72.0, 72.0),
                                                  header, footer).encode('utf-8'),
                    compress_type=zipfile.ZIP_DEFLATED)
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

_RTF_TOKEN_RE = re.compile(rb"\\'([0-9a-fA-F]{2})|\\([a-zA-Z]+)(-?[0-9]+)?[ ]?|([{}])|([^\\{}\r\n]+)|\\([^a-zA-Z'])")


def _rtf_escape_text(s):
    out = []
    for ch in s:
        cp = ord(ch)
        if ch in '{}\\':
            out.append('\\' + ch)
        elif ch == '\t':
            out.append('\\tab ')       # a raw tab is ignorable whitespace to RTF readers
        elif cp < 0x80:
            out.append(ch)
        elif 0xA0 <= cp <= 0xFF:
            out.append("\\'%02x" % cp)
        elif cp in _UNICODE_TO_CP1252:
            out.append("\\'%02x" % _UNICODE_TO_CP1252[cp])
        elif cp <= 0xFFFF:
            out.append('\\u%d?' % (cp - 65536 if cp >= 0x8000 else cp))   # \\uc1: one fallback
        else:
            out.append('?')
    return ''.join(out)


def _rtf_margin_group(dest, parts):
    """{\\header ...} in the engine's shape: parts apart by \\tab (its
    '|'), the page number \\chpgn (its '#')."""
    out = []
    for i, part in enumerate(parts):
        if i:
            out.append('\\tab ')
        for tok in part:
            out.append('\\chpgn ' if tok[0] == 'page' else _rtf_escape_text(tok[1]))
    return '{\\%s\\pard\\plain %s}' % (dest, ''.join(out))


def paragraphs_to_rtf_bytes(paragraphs, page=None, header=None, footer=None):
    colors = []

    def color_index(rgb):
        if not rgb or rgb == (0, 0, 0):
            return 0
        if rgb not in colors:
            colors.append(rgb)
        return colors.index(rgb) + 1

    fonts = []    # face names, \\f1 onward; \\f0 is the reader's default

    for p in paragraphs:
        for r in p['runs']:
            color_index(r.get('color'))
            if r.get('font') and r['font'] not in [f for f, _ in fonts]:
                fonts.append((r['font'], r.get('family') or font_family_guess(r['font'])))

    # The engine's own shape (format_rtf_export.c): \\f0 a "Helvetica"
    # placeholder for the default face, which the \\*\\generator below
    # tells its reader not to take as Helvetica.
    parts = ['{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}']
    for i, (name, family) in enumerate(fonts):
        parts.append('{\\f%d\\f%s %s;}' % (i + 1, family if family in ('roman', 'swiss', 'modern', 'script', 'decor') else 'nil',
                                         _rtf_escape_text(name)))
    parts.append('}')
    if colors:
        parts.append('{\\colortbl;')
        for (r, g, b) in colors:
            parts.append('\\red%d\\green%d\\blue%d;' % (r, g, b))
        parts.append('}')
    parts.append('{\\*\\generator RetroWP;}')
    if page is not None:
        parts.append('\\paperw%d\\paperh%d\\margl%d\\margr%d\\margt%d\\margb%d' %
                     tuple(int(round(v * 20)) for v in page))
    if header:
        parts.append(_rtf_margin_group('header', header))
    if footer:
        parts.append(_rtf_margin_group('footer', footer))
    parts.append('\r\n')
    font_num = {name: i + 1 for i, (name, _) in enumerate(fonts)}

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
        if p.get('pagebb'):
            parts.append('\\pagebb')

        for r in (p['runs'] or [_new_run()]):
            parts.append('{')
            if r.get('font') in font_num:
                parts.append('\\f%d' % font_num[r['font']])
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
    break_next = False   # a \\page or \\sect: the next paragraph starts a page
    uc = 1          # \\ucN: fallback characters after each \\uN
    skip = 0        # fallback characters still to drop
    tab_kind = 'left'
    fonts = {}      # RTF font number -> (name, family)
    ft_num, ft_family, ft_name = None, None, ''
    deff = 0
    ours = False    # \\*\\generator RetroWP: its \\f0 is the default face
    generator = ''

    def font_commit():
        nonlocal ft_num, ft_family, ft_name
        name = ft_name.strip()
        if ft_num is not None and name:
            fonts[ft_num] = (name, ft_family or font_family_guess(name))
        ft_num, ft_family, ft_name = None, None, ''

    def font_select(run, num):
        if num is None or (ours and num == deff) or num not in fonts:
            run['font'], run['family'] = None, None
        else:
            run['font'], run['family'] = fonts[num]

    for m in _RTF_TOKEN_RE.finditer(rtf_bytes):
        hexval, word, num, brace, text, sym = m.groups()
        if sym is not None:
            # Control symbols: \\* marks a destination to skip (a field
            # instruction, a list table...) -- this reader used to drop
            # the star and keep the text; the escaped braces and
            # backslash are text.
            if sym == b'*':
                dest_stack[-1] = 'star'   # skipped -- unless the next word is \\generator
            elif sym in (b'{', b'}', b'\\') and dest_stack[-1] == 'body':
                cur_run['text'] += sym.decode('ascii')
            elif sym == b'~' and dest_stack[-1] == 'body':
                cur_run['text'] += ' '
            continue
        word = word.decode('ascii') if word is not None else None
        brace = brace.decode('ascii') if brace is not None else None
        dest = dest_stack[-1]

        if brace == '}' and dest == 'fonttbl' and ft_name.strip():
            font_commit()      # an entry in its own group may omit its ';'
        if brace == '}' and dest == 'generator':
            ours = generator.strip().rstrip(';').startswith('RetroWP')
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
            if skip > 0:
                skip -= 1
                continue
            b = int(hexval, 16)
            if b < 0x80 or 0xA0 <= b <= 0xFF:
                cur_run['text'] += chr(b)
            else:
                cur_run['text'] += chr(_CP1252_TO_UNICODE.get(b, ord('?')))
            continue

        if text is not None:
            if dest == 'fonttbl':
                for ch in text.decode('cp1252'):
                    if ch == ';':
                        font_commit()
                    else:
                        ft_name += ch
                continue
            if dest == 'generator':
                generator += text.decode('cp1252')
                continue
            if dest == 'body':
                t = text.decode('cp1252')
                if skip > 0:
                    t, skip = t[skip:], max(0, skip - len(t))
                cur_run['text'] += t
            continue

        if word is None:
            continue
        n = int(num) if num else None

        if dest == 'star':
            dest_stack[-1] = 'generator' if word == 'generator' else 'skip'
            continue
        if word == 'deff':
            deff = n or 0
            continue
        if word == 'fonttbl':
            dest_stack[-1] = 'fonttbl'
            continue
        if dest == 'fonttbl':
            if word == 'f':
                if ft_name.strip():
                    font_commit()
                ft_num, ft_family, ft_name = n, None, ''
            elif word in ('fnil', 'froman', 'fswiss', 'fmodern', 'fscript', 'fdecor', 'ftech', 'fbidi'):
                ft_family = {'fnil': None, 'ftech': 'decor', 'fbidi': 'decor'}.get(word, word[1:])
            continue
        # Headers, footers and the other non-body destinations are not
        # body text (rtf_margin_texts reads the header and footer).
        if word in ('header', 'headerr', 'headerl', 'headerf', 'footer', 'footerr', 'footerl',
                    'footerf', 'info', 'stylesheet', 'pict', 'object', 'footnote', 'pntext',
                    'listtext', 'pn', 'filetbl', 'revtbl'):
            dest_stack[-1] = 'skip'
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

        if word == 'u' and n is not None:
            cur_run['text'] += chr(n + 65536 if n < 0 else n)
            skip = uc
            continue
        if word == 'uc':
            uc = n if n is not None else 1
            continue
        skip = 0
        if word == 'pard':
            cur_para['align'] = 'left'
            cur_para['li'] = cur_para['ri'] = cur_para['fi'] = cur_para['sb'] = cur_para['sa'] = 0
            cur_para['sl'] = cur_para['slmult'] = 0
            cur_para['tabs'] = []
            cur_para['pagebb'] = break_next   # a pending \\page survives Word's "\\page\\pard"
            tab_kind = 'left'
        elif word == 'pagebb':
            cur_para['pagebb'] = (n != 0)
        elif word in ('page', 'sect'):
            # Straight after a \\par there is nothing to end; otherwise
            # this ends the paragraph. Either way the next one starts a page.
            if cur_run['text'] or cur_para['runs']:
                if cur_run['text']:
                    cur_para['runs'].append(cur_run)
                paragraphs.append(cur_para)
                cur_para = _new_para()
                cur_run = _new_run()
            cur_para['pagebb'] = True
            break_next = True
        elif word == 'tab':
            cur_run['text'] += '\t'
        elif word == 'sl':
            cur_para['sl'] = n or 0
        elif word == 'slmult':
            cur_para['slmult'] = n or 0
        elif word in ('tqr', 'tqc', 'tqdec'):
            tab_kind = {'tqr': 'right', 'tqc': 'center', 'tqdec': 'decimal'}[word]
        elif word == 'tx':
            if n:
                cur_para['tabs'] = sorted(cur_para['tabs'] + [(n, tab_kind)])
            tab_kind = 'left'
        elif word == 'plain':
            if cur_run['text']:
                cur_para['runs'].append(cur_run)
            cur_run = _new_run()
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
        elif word == 'f':
            font_select(cur_run, n)
        elif word == 'cf':
            if n is not None and 0 <= n < len(colors):
                cur_run['color'] = tuple(colors[n]) if n else None
        elif word == 'par':
            if cur_run['text']:
                cur_para['runs'].append(cur_run)
            paragraphs.append(cur_para)
            cur_para = _new_para()
            cur_run = _new_run()
            break_next = False
        # anything else: ignored (forward compatible, matches the C importer)

    return paragraphs


# ---------------------------------------------------------------------
# paragraph model -> PDF bytes (CONVERT_EXPORT, format=PDF, M7 T7.3;
# font embedding tier T13.3)
#
# Unlike ODT (which only carries already-tagged structured text), PDF
# needs THIS module to do its own text layout: word-wrap, pagination,
# and explicit per-line/per-run positioning in a content stream. US
# Letter, 1in margins. Two font tiers, chosen independently per style
# slot (regular/bold/italic/bolditalic): Tier 0 is the 4 standard
# Type1 Helvetica variants below (zero dependencies, always available,
# every PDF viewer has them built in); Tier 1 (below _pdf_tier1_slot_
# info) is real embedded TrueType via fonttools + Liberation Sans, used
# whenever both are actually available on this host, falling a given
# slot back to Tier 0 alone if not.
# ---------------------------------------------------------------------

# Real Adobe Core 14 AFM widths (ASCII 32-126, 1000-unit em), extracted
# from matplotlib's bundled pdfcorefonts/Helvetica*.afm during
# development -- not guessed or approximated. Helvetica-Oblique and
# Helvetica-BoldOblique share exactly these same widths as their
# upright counterparts (italic is a shear of the same glyphs, not a
# separate width table) -- verified against the real AFM files. Tier
# 0's fallback whenever Tier 1 isn't available for a given style slot.
def _rtf_group_at(rtf_bytes, start):
    """The bytes of the group opening at rtf_bytes[start] ('{'), braces
    balanced, escaped braces respected."""
    depth, i = 0, start
    while i < len(rtf_bytes):
        c = rtf_bytes[i:i + 1]
        if c == b'\\':
            i += 2
            continue
        if c == b'{':
            depth += 1
        elif c == b'}':
            depth -= 1
            if depth == 0:
                return rtf_bytes[start:i + 1]
        i += 1
    return rtf_bytes[start:]


def _rtf_margin_text(group):
    """One header/footer group as the engine keeps it (docmodel.h): a tab
    is a part break '|', \\chpgn or a PAGE field the page number '#'."""
    out = []
    depth = 0
    skip_depth = None
    page_field = False
    pending_skip_next = False
    for m in _RTF_TOKEN_RE.finditer(group):
        hexval, word, num, brace, text, sym = m.groups()
        if brace == '{' or brace == b'{':
            depth += 1
            continue
        if brace == '}' or brace == b'}':
            if skip_depth is not None and depth == skip_depth:
                skip_depth = None
            depth -= 1
            continue
        if sym == b'*':
            pending_skip_next = True
            continue
        if word is not None:
            w = word.decode('ascii')
            if pending_skip_next:
                pending_skip_next = False
                if w == 'fldinst':
                    rest = group[m.end():m.end() + 40]
                    page_field = b'PAGE' in rest.split(b'}')[0]
                skip_depth = depth
                continue
            if skip_depth is not None:
                continue
            if w == 'fldrslt' and page_field:
                out.append('#')
                page_field = False
                skip_depth = depth
            elif w == 'tab':
                out.append('|')
            elif w == 'chpgn':
                out.append('#')
            elif w in ('par', 'line'):
                out.append(' ')
            elif w == 'u' and num:
                n = int(num)
                out.append(chr(n + 65536 if n < 0 else n))
            continue
        if skip_depth is not None:
            continue
        if hexval is not None:
            b = int(hexval, 16)
            out.append(chr(b) if b < 0x80 or b >= 0xA0 else chr(_CP1252_TO_UNICODE.get(b, ord('?'))))
        elif text is not None:
            out.append(text.decode('cp1252'))
    return ''.join(out).strip()


def rtf_margin_texts(rtf_bytes):
    """(header, footer) the way the engine keeps them, '' for none: the
    first \\header (or \\headerr) and \\footer (or \\footerr) group."""
    found = {}
    for m in re.finditer(rb"\{\\(header|headerr|footer|footerr)(?![a-z])", rtf_bytes):
        kind = 'header' if m.group(1).startswith(b'header') else 'footer'
        if kind not in found:
            found[kind] = _rtf_margin_text(_rtf_group_at(rtf_bytes, m.start())[len(m.group(0)):-1])
    return found.get('header', ''), found.get('footer', '')


_RTF_PAGE_RE = re.compile(rb"\\(paperw|paperh|margl|margr|margt|margb)(-?[0-9]+)")


def rtf_page_setup(rtf_bytes):
    """The page an RTF file asks for, in points: (width, height, left,
    right, top, bottom). The first of each control word wins (the
    document-wide values come before any section's); a file that says
    nothing gets US Letter with one-inch margins, RTF's own defaults."""
    found = {}
    for m in _RTF_PAGE_RE.finditer(rtf_bytes):
        found.setdefault(m.group(1).decode('ascii'), int(m.group(2)))
    tw = lambda k, d: max(0, found.get(k, d)) / 20.0
    w, h = tw('paperw', 12240), tw('paperh', 15840)
    if w < 144 or h < 144:          # nonsense: keep a usable page
        w, h = 612.0, 792.0
    return (w, h, tw('margl', 1440),
            tw('margr', 1440), tw('margt', 1440), tw('margb', 1440))


_HELV_WIDTHS = {32: 278, 33: 278, 34: 355, 35: 556, 36: 556, 37: 889, 38: 667, 39: 222, 40: 333, 41: 333, 42: 389, 43: 584, 44: 278, 45: 333, 46: 278, 47: 278, 48: 556, 49: 556, 50: 556, 51: 556, 52: 556, 53: 556, 54: 556, 55: 556, 56: 556, 57: 556, 58: 278, 59: 278, 60: 584, 61: 584, 62: 584, 63: 556, 64: 1015, 65: 667, 66: 667, 67: 722, 68: 722, 69: 667, 70: 611, 71: 778, 72: 722, 73: 278, 74: 500, 75: 667, 76: 556, 77: 833, 78: 722, 79: 778, 80: 667, 81: 778, 82: 722, 83: 667, 84: 611, 85: 722, 86: 667, 87: 944, 88: 667, 89: 667, 90: 611, 91: 278, 92: 278, 93: 278, 94: 469, 95: 556, 96: 222, 97: 556, 98: 556, 99: 500, 100: 556, 101: 556, 102: 278, 103: 556, 104: 556, 105: 222, 106: 222, 107: 500, 108: 222, 109: 833, 110: 556, 111: 556, 112: 556, 113: 556, 114: 333, 115: 500, 116: 278, 117: 556, 118: 500, 119: 722, 120: 500, 121: 500, 122: 500, 123: 334, 124: 260, 125: 334, 126: 584}
_HELV_BOLD_WIDTHS = {32: 278, 33: 333, 34: 474, 35: 556, 36: 556, 37: 889, 38: 722, 39: 278, 40: 333, 41: 333, 42: 389, 43: 584, 44: 278, 45: 333, 46: 278, 47: 278, 48: 556, 49: 556, 50: 556, 51: 556, 52: 556, 53: 556, 54: 556, 55: 556, 56: 556, 57: 556, 58: 333, 59: 333, 60: 584, 61: 584, 62: 584, 63: 611, 64: 975, 65: 722, 66: 722, 67: 722, 68: 722, 69: 667, 70: 611, 71: 778, 72: 722, 73: 278, 74: 556, 75: 722, 76: 611, 77: 833, 78: 722, 79: 778, 80: 667, 81: 778, 82: 722, 83: 667, 84: 611, 85: 722, 86: 667, 87: 944, 88: 667, 89: 667, 90: 611, 91: 333, 92: 278, 93: 333, 94: 584, 95: 556, 96: 278, 97: 556, 98: 611, 99: 556, 100: 611, 101: 556, 102: 333, 103: 611, 104: 611, 105: 278, 106: 278, 107: 556, 108: 278, 109: 889, 110: 611, 111: 611, 112: 611, 113: 611, 114: 389, 115: 556, 116: 333, 117: 611, 118: 556, 119: 778, 120: 556, 121: 556, 122: 500, 123: 389, 124: 280, 125: 389, 126: 584}
# The Times family's, from the same bundled AFMs (Times-Roman, -Bold,
# -Italic, -BoldItalic): unlike Helvetica's, Times's italics are their
# own designs with their own widths. Courier is 600 for every glyph in
# every style -- it is a monospace -- so it needs no table.
_TIMES_WIDTHS = {32: 250, 33: 333, 34: 408, 35: 500, 36: 500, 37: 833, 38: 778, 39: 333, 40: 333, 41: 333, 42: 500, 43: 564, 44: 250, 45: 333, 46: 250, 47: 278, 48: 500, 49: 500, 50: 500, 51: 500, 52: 500, 53: 500, 54: 500, 55: 500, 56: 500, 57: 500, 58: 278, 59: 278, 60: 564, 61: 564, 62: 564, 63: 444, 64: 921, 65: 722, 66: 667, 67: 667, 68: 722, 69: 611, 70: 556, 71: 722, 72: 722, 73: 333, 74: 389, 75: 722, 76: 611, 77: 889, 78: 722, 79: 722, 80: 556, 81: 722, 82: 667, 83: 556, 84: 611, 85: 722, 86: 722, 87: 944, 88: 722, 89: 722, 90: 611, 91: 333, 92: 278, 93: 333, 94: 469, 95: 500, 96: 333, 97: 444, 98: 500, 99: 444, 100: 500, 101: 444, 102: 333, 103: 500, 104: 500, 105: 278, 106: 278, 107: 500, 108: 278, 109: 778, 110: 500, 111: 500, 112: 500, 113: 500, 114: 333, 115: 389, 116: 278, 117: 500, 118: 500, 119: 722, 120: 500, 121: 500, 122: 444, 123: 480, 124: 200, 125: 480, 126: 541}
_TIMES_BOLD_WIDTHS = {32: 250, 33: 333, 34: 555, 35: 500, 36: 500, 37: 1000, 38: 833, 39: 333, 40: 333, 41: 333, 42: 500, 43: 570, 44: 250, 45: 333, 46: 250, 47: 278, 48: 500, 49: 500, 50: 500, 51: 500, 52: 500, 53: 500, 54: 500, 55: 500, 56: 500, 57: 500, 58: 333, 59: 333, 60: 570, 61: 570, 62: 570, 63: 500, 64: 930, 65: 722, 66: 667, 67: 722, 68: 722, 69: 667, 70: 611, 71: 778, 72: 778, 73: 389, 74: 500, 75: 778, 76: 667, 77: 944, 78: 722, 79: 778, 80: 611, 81: 778, 82: 722, 83: 556, 84: 667, 85: 722, 86: 722, 87: 1000, 88: 722, 89: 722, 90: 667, 91: 333, 92: 278, 93: 333, 94: 581, 95: 500, 96: 333, 97: 500, 98: 556, 99: 444, 100: 556, 101: 444, 102: 333, 103: 500, 104: 556, 105: 278, 106: 333, 107: 556, 108: 278, 109: 833, 110: 556, 111: 500, 112: 556, 113: 556, 114: 444, 115: 389, 116: 333, 117: 556, 118: 500, 119: 722, 120: 500, 121: 500, 122: 444, 123: 394, 124: 220, 125: 394, 126: 520}
_TIMES_ITALIC_WIDTHS = {32: 250, 33: 333, 34: 420, 35: 500, 36: 500, 37: 833, 38: 778, 39: 333, 40: 333, 41: 333, 42: 500, 43: 675, 44: 250, 45: 333, 46: 250, 47: 278, 48: 500, 49: 500, 50: 500, 51: 500, 52: 500, 53: 500, 54: 500, 55: 500, 56: 500, 57: 500, 58: 333, 59: 333, 60: 675, 61: 675, 62: 675, 63: 500, 64: 920, 65: 611, 66: 611, 67: 667, 68: 722, 69: 611, 70: 611, 71: 722, 72: 722, 73: 333, 74: 444, 75: 667, 76: 556, 77: 833, 78: 667, 79: 722, 80: 611, 81: 722, 82: 611, 83: 500, 84: 556, 85: 722, 86: 611, 87: 833, 88: 611, 89: 556, 90: 556, 91: 389, 92: 278, 93: 389, 94: 422, 95: 500, 96: 333, 97: 500, 98: 500, 99: 444, 100: 500, 101: 444, 102: 278, 103: 500, 104: 500, 105: 278, 106: 278, 107: 444, 108: 278, 109: 722, 110: 500, 111: 500, 112: 500, 113: 500, 114: 389, 115: 389, 116: 278, 117: 500, 118: 444, 119: 667, 120: 444, 121: 444, 122: 389, 123: 400, 124: 275, 125: 400, 126: 541}
_TIMES_BOLDITALIC_WIDTHS = {32: 250, 33: 389, 34: 555, 35: 500, 36: 500, 37: 833, 38: 778, 39: 333, 40: 333, 41: 333, 42: 500, 43: 570, 44: 250, 45: 333, 46: 250, 47: 278, 48: 500, 49: 500, 50: 500, 51: 500, 52: 500, 53: 500, 54: 500, 55: 500, 56: 500, 57: 500, 58: 333, 59: 333, 60: 570, 61: 570, 62: 570, 63: 500, 64: 832, 65: 667, 66: 667, 67: 667, 68: 722, 69: 667, 70: 667, 71: 722, 72: 778, 73: 389, 74: 500, 75: 667, 76: 611, 77: 889, 78: 722, 79: 722, 80: 611, 81: 722, 82: 667, 83: 556, 84: 611, 85: 722, 86: 667, 87: 889, 88: 667, 89: 611, 90: 611, 91: 333, 92: 278, 93: 333, 94: 570, 95: 500, 96: 333, 97: 500, 98: 500, 99: 444, 100: 500, 101: 444, 102: 333, 103: 500, 104: 556, 105: 278, 106: 278, 107: 500, 108: 278, 109: 778, 110: 556, 111: 500, 112: 500, 113: 500, 114: 389, 115: 389, 116: 278, 117: 556, 118: 444, 119: 667, 120: 500, 121: 444, 122: 389, 123: 348, 124: 220, 125: 348, 126: 570}
_COURIER_WIDTHS = {c: 600 for c in range(32, 127)}
_PDF_FALLBACK_WIDTH = 556  # any char outside Tier 0's ASCII 32-126 AFM table -- an average-width approximation, never hit for a Tier-1 slot (real widths cover the full 32-255 WinAnsi range there)

_PDF_PAGE_WIDTH = 612.0   # US Letter, points
_PDF_PAGE_HEIGHT = 792.0
_PDF_MARGIN = 72.0        # 1 inch


# ---------------------------------------------------------------------
# Tier 1: real embedded TrueType, gated on fonttools + Liberation Sans
# actually being present on this host. Liberation Sans is metrically
# compatible with Helvetica by design (Red Hat's own stated goal for
# the family) -- verified during development directly against this
# module's own _HELV_WIDTHS ('A' advance: 667 in both). No document-
# level font-family selection exists anywhere in this pipeline yet
# (RTF's \fonttbl is a skipped destination on import, same as the C
# engine's importer; every RTF export always declares a single
# Helvetica font) -- so Tier 1 embeds the same 4 logical style slots
# Tier 0 already has (regular/bold/italic/bolditalic) in ONE real
# family, not a font-family feature.
# ---------------------------------------------------------------------

try:
    from fontTools.ttLib import TTFont as _TTFont
    from fontTools import subset as _fonttools_subset
    _FONTTOOLS_AVAILABLE = True
except ImportError:
    _FONTTOOLS_AVAILABLE = False

# Real install paths across the distros this project's real hosts (dev
# machine, Raspberry Pi gateway) realistically run: Arch, Debian/
# Ubuntu/Raspbian, Fedora. Checked in order; first hit wins.
# Three families, each of the four style slots: Sans (Helvetica, the
# default, and every face a run names that is not one of the other
# two), Serif (Times) and Mono (Courier) -- which is as many as a PDF
# has built in, and the RTF \\froman/\\fswiss/\\fmodern a document's font
# table says, or its face's name suggests (font_family_guess). Liberation
# Sans, Serif and Mono are metric twins of the three.
_PDF_FAMILIES = ('Sans', 'Serif', 'Mono')
_PDF_STYLE_NAMES = ('Regular', 'Bold', 'Italic', 'BoldItalic')


def _liberation_candidates(family, style):
    stem = 'Liberation%s-%s.ttf' % (family, style)
    return ['/usr/share/fonts/liberation/' + stem,
            '/usr/share/fonts/truetype/liberation/' + stem,
            '/usr/share/fonts/liberation-%s/%s' % (family.lower(), stem),
            '/usr/share/fonts/TTF/' + stem]


# Style slots shared by _pdf_run_slot, _pdf_tier1_slot_info, and
# _pdf_assemble: family * 4 + (0=regular, 1=bold, 2=italic, 3=bolditalic).
_PDF_SLOT_COUNT = 12
_PDF_SLOT_TIER0_NAME = {0: 'Helvetica', 1: 'Helvetica-Bold', 2: 'Helvetica-Oblique', 3: 'Helvetica-BoldOblique',
                        4: 'Times-Roman', 5: 'Times-Bold', 6: 'Times-Italic', 7: 'Times-BoldItalic',
                        8: 'Courier', 9: 'Courier-Bold', 10: 'Courier-Oblique', 11: 'Courier-BoldOblique'}
_PDF_SLOT_TIER0_WIDTHS = {0: _HELV_WIDTHS, 1: _HELV_BOLD_WIDTHS, 2: _HELV_WIDTHS, 3: _HELV_BOLD_WIDTHS,
                          4: _TIMES_WIDTHS, 5: _TIMES_BOLD_WIDTHS, 6: _TIMES_ITALIC_WIDTHS,
                          7: _TIMES_BOLDITALIC_WIDTHS,
                          8: _COURIER_WIDTHS, 9: _COURIER_WIDTHS, 10: _COURIER_WIDTHS, 11: _COURIER_WIDTHS}
_PDF_SLOT_RESOURCE = {slot: '/F%d' % (slot + 1) for slot in range(_PDF_SLOT_COUNT)}

# Identity ToUnicode CMap for codes 0x20-0xFF: correct (not an
# approximation) precisely because this module's own character codes
# ARE the Unicode codepoint in that range by construction -- see
# _pdf_escape_str's own "WinAnsiEncoding matches Latin-1 in this range"
# comment. Fixes real-viewer text extraction/copy-paste for embedded
# TrueType fonts (found during development: without this, poppler's
# text-extraction fallback heuristic breaks down partway through a
# subsetted font's remapped glyph IDs even though rendering is
# correct); shared by every Tier-1 slot, since the mapping doesn't
# depend on which face is embedded.
_PDF_TOUNICODE_CMAP = (
    "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
    "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
    "1 begincodespacerange\n<20> <FF>\nendcodespacerange\n"
    "1 beginbfrange\n<20> <FF> <0020>\nendbfrange\nendcmap\n"
    "CMapName currentdict /CMap defineresource pop\nend\nend"
).encode('ascii')


def _liberation_font_path(slot):
    for path in _liberation_candidates(_PDF_FAMILIES[slot // 4], _PDF_STYLE_NAMES[slot % 4]):
        if os.path.isfile(path):
            return path
    return None


def _pdf_tier1_slot_info(slot):
    """Lazily loads and caches, per style slot, the real metrics needed
    for BOTH layout (widths, covering the full 32-255 WinAnsi range --
    not just ASCII, unlike Tier 0's AFM table) and final embedding
    (ascent/descent/bbox/etc) -- none of this depends on which glyphs
    end up subsetted, so it's computed once from the full font
    regardless of what text a given document actually uses. Returns
    None if fonttools isn't importable or this slot's font file isn't
    found on this host -- the caller falls that slot back to Tier 0."""
    if not _FONTTOOLS_AVAILABLE:
        return None
    if slot in _pdf_tier1_slot_info._cache:
        return _pdf_tier1_slot_info._cache[slot]

    info = None
    path = _liberation_font_path(slot)
    if path is not None:
        try:
            font = _TTFont(path)
            upm = font['head'].unitsPerEm
            cmap = font.getBestCmap()
            hmtx = font['hmtx']
            widths = {}
            for cp in range(32, 256):
                gname = cmap.get(cp)
                if gname is not None:
                    widths[cp] = round(hmtx[gname][0] * 1000 / upm)
            info = {
                'path': path,
                'widths': widths,
                'ascent': round(font['hhea'].ascent * 1000 / upm),
                'descent': round(font['hhea'].descent * 1000 / upm),
                'cap_height': round(getattr(font['OS/2'], 'sCapHeight', font['hhea'].ascent) * 1000 / upm),
                'bbox': [round(v * 1000 / upm) for v in
                         (font['head'].xMin, font['head'].yMin, font['head'].xMax, font['head'].yMax)],
                'italic_angle': font['post'].italicAngle,
                # bit 6 (nonsymbolic) always; bit 7 (italic) for the italic
                # slots; bit 1 (fixed pitch) for Mono, bit 2 (serif) for Serif
                'flags': (96 if slot % 4 in (2, 3) else 32) | (1 if slot // 4 == 2 else 0) |
                         (2 if slot // 4 == 1 else 0),
                'font_name': 'Liberation%s' % _PDF_FAMILIES[slot // 4],
            }
        except Exception:
            info = None
    _pdf_tier1_slot_info._cache[slot] = info
    return info


_pdf_tier1_slot_info._cache = {}


def _pdf_subset_embed(slot, used_chars):
    """Subsets this slot's real font down to used_chars plus printable
    ASCII (always included, so a slot with no drawn text -- or an
    unexpectedly empty used_chars -- still embeds a usable font) and
    returns the raw TrueType bytes to embed as /FontFile2."""
    info = _pdf_tier1_slot_info(slot)
    font = _TTFont(info['path'])
    options = _fonttools_subset.Options()
    options.set(layout_features=[])
    options.drop_tables += ['GSUB', 'GPOS', 'GDEF']
    subsetter = _fonttools_subset.Subsetter(options=options)
    text = ''.join(used_chars) + ''.join(chr(c) for c in range(32, 127))
    subsetter.populate(text=text)
    subsetter.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


_PDF_COLUMN_WIDTH = _PDF_PAGE_WIDTH - 2 * _PDF_MARGIN
_PDF_CONTENT_TOP = _PDF_PAGE_HEIGHT - _PDF_MARGIN
_PDF_CONTENT_BOTTOM = _PDF_MARGIN

_PDF_SUPERSUB_SCALE = 0.65  # typographic convention: super/subscript glyphs render smaller


def _pdf_run_slot(run):
    """family * 4 + (0=regular, 1=bold, 2=italic, 3=bolditalic) -- the
    logical slots both font tiers key off of. The family is the run's
    own (RTF's \\froman is Serif, \\fmodern Mono) or its face's guessed
    one; everything else, the default face included, is Sans."""
    b, i = run.get('b'), run.get('i')
    style = 3 if (b and i) else 1 if b else 2 if i else 0
    family = run.get('family') or (font_family_guess(run['font']) if run.get('font') else None)
    return {'roman': 4, 'modern': 8}.get(family, 0) + style


def _pdf_font_for_run(run):
    """Returns (resource_name, widths) for this run -- Tier 1's real
    Liberation Sans widths when that slot is actually available on this
    host, else Tier 0's AFM table. Both tiers use the SAME resource
    name per slot (a document only ever needs one font object per slot,
    whichever tier backs it), so nothing downstream of this needs to
    know which tier is active."""
    slot = _pdf_run_slot(run)
    tier1 = _pdf_tier1_slot_info(slot)
    widths = tier1['widths'] if tier1 is not None else _PDF_SLOT_TIER0_WIDTHS[slot]
    return _PDF_SLOT_RESOURCE[slot], widths


def _pdf_text_width_pt(text, widths, size_pt):
    return sum(widths.get(ord(ch), _PDF_FALLBACK_WIDTH) for ch in text) * size_pt / 1000.0


def _pdf_effective_size_pt(run):
    """The size used for BOTH width measurement and Tf, so wrapping
    decisions and rendering never disagree about how wide super/
    subscript text is."""
    size_pt = run.get('size_twips', 240) / 20.0
    if run.get('super') or run.get('sub'):
        return size_pt * _PDF_SUPERSUB_SCALE
    return size_pt


def _pdf_text_rise_pt(run, size_pt):
    """PDF Ts (text rise), 0 for normal text. Proportional to the
    (already-reduced) super/sub size -- a reasonable typographic
    default, not a claim of matching any font's real, published super/
    subscript metrics (Helvetica has none)."""
    if run.get('super'):
        return size_pt * 0.4
    if run.get('sub'):
        return -size_pt * 0.2
    return 0.0


def _pdf_build_items(para):
    """Splits a paragraph into (is_space, [(text, run), ...]) items,
    merging consecutive same-kind fragments across run boundaries so a
    word split by a style change never breaks mid-word (the same
    property engine/src/linebreak.c's own token merging guarantees on
    the engine side, reimplemented here since this gateway does its own
    independent layout pass)."""
    items = []
    cur_is_space = None
    cur_fragments = []
    for run in para['runs']:
        text = run.get('text', '')
        i, n = 0, len(text)
        while i < n:
            if text[i] == '\t':
                # A tab is an item of its own, never merged: each goes to
                # its own stop. Marked 'tab' (truthy, so code that only
                # asks "is this a gap?" sees it as one).
                if cur_fragments:
                    items.append((cur_is_space, cur_fragments))
                items.append(('tab', [('\t', run)]))
                cur_is_space, cur_fragments = None, []
                i += 1
                continue
            is_space = (text[i] == ' ')
            j = i
            while j < n and text[j] != '\t' and (text[j] == ' ') == is_space:
                j += 1
            frag = (text[i:j], run)
            if cur_is_space is None:
                cur_is_space, cur_fragments = is_space, [frag]
            elif is_space == cur_is_space:
                cur_fragments.append(frag)
            else:
                items.append((cur_is_space, cur_fragments))
                cur_is_space, cur_fragments = is_space, [frag]
            i = j
    if cur_fragments:
        items.append((cur_is_space, cur_fragments))
    return items


def _pdf_next_stop(tabs, x_pt):
    """The stop a tab starting at x_pt (from the column's left edge)
    reaches, and its kind: the first explicit stop past x, else the next
    half inch -- the engine's rule (layout.h, wp_tab_next_stop)."""
    last = 0.0
    for pos_twips, kind in tabs:
        pos = pos_twips / 20.0
        if pos > x_pt + 0.01:
            return pos, kind
        last = pos
    x = max(x_pt, last)
    return (int(x // 36.0) + 1) * 36.0, 'left'


def _pdf_line_tab_widths(line_items, x0_pt, tabs):
    """Every tab's width on one line, now that the text after it is
    known: a right (or decimal) tab ends its segment on the stop, a
    centre tab centres it there, a left tab starts it there."""
    widths = []
    x = x0_pt
    for idx, (kind, frags) in enumerate(line_items):
        if kind == 'tab':
            stop, tkind = _pdf_next_stop(tabs, x)
            seg = 0.0
            for k2, f2 in line_items[idx + 1:]:
                if k2 == 'tab':
                    break
                seg += _pdf_item_width_pt(f2)
            if tkind in ('right', 'decimal'):
                w = stop - x - seg
            elif tkind == 'center':
                w = stop - x - seg / 2.0
            else:
                w = stop - x
            w = max(0.0, w)
            widths.append(w)
            x += w
        else:
            x += _pdf_item_width_pt(frags)
    return widths


def _pdf_item_width_pt(frags):
    return sum(_pdf_text_width_pt(text, _pdf_font_for_run(run)[1], _pdf_effective_size_pt(run))
               for text, run in frags if text != '\t')


def _pdf_wrap_paragraph(para, column_width_pt):
    """Greedy word wrap, breaking BEFORE a word that doesn't fit and
    dropping a would-be trailing space at the break (standard word-wrap
    behavior). An overlong single word wider than the column overflows
    its line rather than breaking mid-word -- the same allowance
    docs/layout.md documents for the engine's own line breaker. Returns
    (lines, li, ri, fi) where each line is (line_items, is_first_line)."""
    items = _pdf_build_items(para)
    li = para.get('li', 0) / 20.0
    ri = para.get('ri', 0) / 20.0
    fi = para.get('fi', 0) / 20.0

    def avail(first_line):
        return max(0.0, column_width_pt - li - ri - (fi if first_line else 0.0))

    lines = []
    cur, cur_width, first_line = [], 0.0, True

    def flush():
        nonlocal cur, cur_width, first_line
        if cur and cur[-1][0] is True:
            cur_width -= _pdf_item_width_pt(cur[-1][1])
            cur = cur[:-1]
        lines.append((cur, first_line))
        first_line = False
        cur, cur_width = [], 0.0

    i = 0
    while i < len(items):
        is_space, frags = items[i]
        w = _pdf_item_width_pt(frags)
        a = avail(first_line)
        if is_space == 'tab':
            # Content, like a word (a tab can start a line), sized as a
            # left tab from where it starts; the render pass corrects
            # right and centre stops once the line is known.
            x = li + (fi if first_line else 0.0) + cur_width
            w = _pdf_next_stop(para.get('tabs', []), x)[0] - x
            if cur and cur_width + w > a:
                flush()
                continue
            cur.append((is_space, frags))
            cur_width += w
            i += 1
            continue
        if is_space:
            if cur and cur_width + w > a:
                flush()
                i += 1
                continue
            cur.append((is_space, frags))
            cur_width += w
            i += 1
        else:
            if cur and cur_width + w > a:
                flush()
                continue  # retry this word on the new line
            cur.append((is_space, frags))
            cur_width += w
            i += 1

    if cur or not lines:
        flush()
    return lines, li, ri, fi


def _pdf_escape_str(text):
    out = []
    for ch in text:
        cp = ord(ch)
        if ch in '()\\':
            out.append('\\' + ch)
        elif cp < 0x80 or 0xA0 <= cp <= 0xFF:
            out.append(ch)  # WinAnsiEncoding matches Latin-1 in this range
        else:
            out.append('?')
    return ''.join(out)


def _pdf_margin_ops(text, page_no, y, m_left, column_w, used_chars):
    """A header or footer line on one page: '|' parts left, centre and
    right (one part centred, two left and right), '#' the page number,
    10 pt regular."""
    if not text:
        return []
    parts = text.replace('#', str(page_no)).split('|')
    if len(parts) == 1:
        slots = [None, parts[0], None]
    elif len(parts) == 2:
        slots = [parts[0], None, parts[1]]
    else:
        slots = [parts[0], parts[1], '|'.join(parts[2:])]
    run = {'size_twips': 200}
    font_name, widths = _pdf_font_for_run(run)
    ops = []
    for idx, part in enumerate(slots):
        if not part:
            continue
        w = _pdf_text_width_pt(part, widths, 10.0)
        x = m_left + (0.0 if idx == 0 else (column_w - w) / 2.0 if idx == 1 else column_w - w)
        ops += ['BT', '%s 10.00 Tf' % font_name, '0 0 0 rg', '%.2f %.2f Td' % (x, y),
                '(%s) Tj' % _pdf_escape_str(part), 'ET']
        used_chars[_pdf_run_slot(run)].update(part)
    return ops


def paragraphs_to_pdf_bytes(paragraphs, page=None, header='', footer=''):
    """page: (width, height, left, right, top, bottom) in points, as
    rtf_page_setup returns it; None for US Letter with one-inch margins.
    header/footer: the running lines (rtf_margin_texts), drawn halfway
    into the top and bottom margins of every page."""
    page_w, page_h, m_left, m_right, m_top, m_bottom = page or (
        _PDF_PAGE_WIDTH, _PDF_PAGE_HEIGHT, _PDF_MARGIN, _PDF_MARGIN, _PDF_MARGIN, _PDF_MARGIN)
    column_w = max(72.0, page_w - m_left - m_right)
    content_top = page_h - m_top
    content_bottom = m_bottom
    pages = []
    cur_ops = []
    cur_y = content_top
    cur_tw = None
    cur_font = cur_size = None
    cur_color = (0, 0, 0)
    used_chars = {slot: set() for slot in range(_PDF_SLOT_COUNT)}  # per style slot, for Tier-1 subsetting

    def new_page():
        nonlocal cur_ops, cur_y, cur_tw, cur_font, cur_size, cur_color
        pages.append(cur_ops)
        cur_ops = []
        cur_y = content_top
        cur_tw = None
        cur_font = cur_size = None
        cur_color = (0, 0, 0)

    def ensure_room(leading):
        if cur_y - leading < content_bottom:
            new_page()

    for pi, para in enumerate(paragraphs):
        lines, li, ri, fi = _pdf_wrap_paragraph(para, column_w)
        tabs = para.get('tabs', [])
        sl, slmult = para.get('sl', 0), para.get('slmult', 0)
        sb = para.get('sb', 0) / 20.0
        sa = para.get('sa', 0) / 20.0
        align = para.get('align', 'left')

        if para.get('pagebb') and cur_ops:
            new_page()   # the paragraph asks to start a page, and this one has text
        if pi > 0:
            ensure_room(sb)
            cur_y -= sb

        for line_idx, (line_items, is_first) in enumerate(lines):
            is_last_line_of_para = (line_idx == len(lines) - 1)
            max_size_pt = 12.0
            for _, frags in line_items:
                for _, run in frags:
                    max_size_pt = max(max_size_pt, run.get('size_twips', 240) / 20.0)
            leading = max_size_pt * 1.2
            # The engine's line spacing (docmodel.h): a multiple in 240ths,
            # or "at least" so many twips.
            if sl > 0 and slmult:
                leading = leading * sl / 240.0
            elif sl:
                leading = max(leading, abs(sl) / 20.0)
            ensure_room(leading)

            indent = li + (fi if is_first else 0.0)
            tab_widths = _pdf_line_tab_widths(line_items, indent, tabs)
            line_width = (sum(_pdf_item_width_pt(frags) for k, frags in line_items if k != 'tab') +
                          sum(tab_widths))
            col = column_w - li - ri - (fi if is_first else 0.0)
            has_tab = bool(tab_widths)

            extra_per_space = 0.0
            x0 = m_left + indent
            if align == 'right':
                x0 += max(0.0, col - line_width)
            elif align == 'center':
                x0 += max(0.0, (col - line_width) / 2.0)
            elif align == 'justify' and not is_last_line_of_para and not has_tab:
                space_count = sum(1 for is_space, _ in line_items if is_space is True)
                if space_count > 0 and col > line_width:
                    extra_per_space = (col - line_width) / space_count

            cur_y -= max_size_pt  # baseline sits max_size_pt below the line's top (ascent-ish approximation)

            cur_ops.append('BT')
            cur_ops.append('%.2f %.2f Td' % (x0, cur_y))
            if extra_per_space != cur_tw:
                cur_ops.append('%.3f Tw' % extra_per_space)
                cur_tw = extra_per_space
            cur_font = cur_size = None
            cur_rise = 0.0
            # Tracked independently (not relying on Tj's own auto-advance)
            # so underline/strikethrough segments land exactly where the
            # SAME AFM widths say the text does -- as long as this uses
            # those same widths, it can't drift from where Tj itself draws.
            running_x = x0
            deco = []  # (x_start, x_end, y) segments to stroke AFTER this ET
            tab_i = 0
            for is_space, frags in line_items:
                if is_space == 'tab':
                    # Never drawn: the text after it starts at its stop,
                    # in a text object of its own.
                    running_x += tab_widths[tab_i]
                    tab_i += 1
                    cur_ops.append('ET')
                    cur_ops.append('BT')
                    cur_ops.append('%.2f %.2f Td' % (running_x, cur_y))
                    continue
                for text, run in frags:
                    font_name, widths = _pdf_font_for_run(run)
                    size_pt = _pdf_effective_size_pt(run)
                    if font_name != cur_font or size_pt != cur_size:
                        cur_ops.append('%s %.2f Tf' % (font_name, size_pt))
                        cur_font, cur_size = font_name, size_pt
                    rise = _pdf_text_rise_pt(run, size_pt)
                    if rise != cur_rise:
                        cur_ops.append('%.2f Ts' % rise)
                        cur_rise = rise
                    color = run.get('color') or (0, 0, 0)
                    if color != cur_color:
                        cur_ops.append('%.3f %.3f %.3f rg' % (color[0] / 255.0, color[1] / 255.0, color[2] / 255.0))
                        cur_color = color
                    cur_ops.append('(%s) Tj' % _pdf_escape_str(text))
                    used_chars[_pdf_run_slot(run)].update(text)

                    w = _pdf_text_width_pt(text, widths, size_pt)
                    if is_space and extra_per_space:
                        w += extra_per_space * text.count(' ')
                    if run.get('ul'):
                        deco.append((running_x, running_x + w, cur_y - size_pt * 0.08, color))
                    if run.get('strike'):
                        deco.append((running_x, running_x + w, cur_y + size_pt * 0.3, color))
                    running_x += w
            cur_ops.append('ET')

            for x_start, x_end, y, color in deco:
                if x_end <= x_start:
                    continue
                cur_ops.append('%.3f %.3f %.3f RG' % (color[0] / 255.0, color[1] / 255.0, color[2] / 255.0))
                cur_ops.append('1 w')
                cur_ops.append('%.2f %.2f m %.2f %.2f l S' % (x_start, y, x_end, y))

            cur_y -= (leading - max_size_pt)

        cur_y -= sa

    pages.append(cur_ops)
    for k, ops in enumerate(pages):
        ops += _pdf_margin_ops(header, k + 1, page_h - m_top / 2.0, m_left, column_w, used_chars)
        ops += _pdf_margin_ops(footer, k + 1, m_bottom / 2.0, m_left, column_w, used_chars)
    return _pdf_assemble(pages, used_chars, (page_w, page_h))


def _pdf_assemble(pages, used_chars, media=(_PDF_PAGE_WIDTH, _PDF_PAGE_HEIGHT)):
    """Builds the final PDF byte stream: Catalog, Pages tree, one Page +
    one content-stream object per page, and one font object per style
    slot -- Tier 0's standard Type1 font (Helvetica/-Bold/-Oblique/-
    BoldOblique, no embedding, no /Widths array needed: every viewer
    already knows the standard 14 fonts' real metrics, which is exactly
    why this module used the SAME real AFM widths for its own layout)
    for whichever slots Tier 1 isn't available for, real embedded
    TrueType (with a full /Widths array and a shared /ToUnicode CMap)
    for whichever slots it is."""
    objects = []

    next_obj = 14
    font_res_parts = []
    tounicode_num = None

    for slot in range(_PDF_SLOT_COUNT):
        resource_name = _PDF_SLOT_RESOURCE[slot][1:]  # strip the leading '/'
        # The Sans slots are always declared (objects 10-13, as they
        # always were); a Serif or Mono slot only when some text uses it.
        if slot >= 4 and not used_chars.get(slot):
            continue
        # Embedding an unused slot would be pure waste (a real font
        # subset costs real KB even for zero glyphs actually drawn) --
        # a slot with nothing in used_chars falls back to Tier 0's
        # free standard font regardless of Tier 1's availability.
        tier1 = _pdf_tier1_slot_info(slot) if used_chars.get(slot) else None

        if tier1 is None:
            base_name = _PDF_SLOT_TIER0_NAME[slot]
            if slot < 4:
                num = 10 + slot
            else:
                num, next_obj = next_obj, next_obj + 1
            objects.append((num, ('<< /Type /Font /Subtype /Type1 /BaseFont /%s '
                                   '/Encoding /WinAnsiEncoding >>' % base_name).encode('ascii')))
            font_res_parts.append('/%s %d 0 R' % (resource_name, num))
            continue

        if tounicode_num is None:
            tounicode_num = next_obj
            next_obj += 1
            objects.append((tounicode_num, b'<< /Length %d >>\nstream\n' % len(_PDF_TOUNICODE_CMAP) +
                             _PDF_TOUNICODE_CMAP + b'\nendstream'))

        fontfile_num, next_obj = next_obj, next_obj + 1
        descr_num, next_obj = next_obj, next_obj + 1
        font_num, next_obj = next_obj, next_obj + 1

        fontdata = _pdf_subset_embed(slot, used_chars.get(slot, set()))
        widths_arr = ' '.join(str(tier1['widths'].get(cp, 0)) for cp in range(32, 256))
        descr = ('<< /Type /FontDescriptor /FontName /%s /Flags %d '
                 '/FontBBox [%d %d %d %d] /ItalicAngle %g /Ascent %d /Descent %d '
                 '/CapHeight %d /StemV 80 /FontFile2 %d 0 R >>' %
                 (tier1['font_name'], tier1['flags'], tier1['bbox'][0], tier1['bbox'][1], tier1['bbox'][2], tier1['bbox'][3],
                  tier1['italic_angle'], tier1['ascent'], tier1['descent'], tier1['cap_height'], fontfile_num))
        packed = zlib.compress(fontdata, 9)
        objects.append((fontfile_num, b'<< /Length %d /Length1 %d /Filter /FlateDecode >>\nstream\n' %
                         (len(packed), len(fontdata)) + packed + b'\nendstream'))
        objects.append((descr_num, descr.encode('ascii')))
        objects.append((font_num, ('<< /Type /Font /Subtype /TrueType /BaseFont /%s '
                                    '/FirstChar 32 /LastChar 255 /Widths [%s] '
                                    '/FontDescriptor %d 0 R /Encoding /WinAnsiEncoding '
                                    '/ToUnicode %d 0 R >>' %
                                    (tier1['font_name'], widths_arr, descr_num, tounicode_num)).encode('ascii')))
        font_res_parts.append('/%s %d 0 R' % (resource_name, font_num))

    font_res = ' '.join(font_res_parts)

    page_obj_nums, content_obj_nums = [], []
    for _ in pages:
        page_obj_nums.append(next_obj)
        next_obj += 1
        content_obj_nums.append(next_obj)
        next_obj += 1

    kids = ' '.join('%d 0 R' % n for n in page_obj_nums)
    objects.append((1, b'<< /Type /Catalog /Pages 2 0 R >>'))
    objects.append((2, ('<< /Type /Pages /Kids [%s] /Count %d >>' % (kids, len(pages))).encode('ascii')))
    for page_num, content_num, ops in zip(page_obj_nums, content_obj_nums, pages):
        objects.append((page_num, ('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %g %g] '
                                    '/Resources << /Font << %s >> >> /Contents %d 0 R >>' %
                                    (media[0], media[1], font_res, content_num)).encode('ascii')))
        # Compressed: page text is the bulk of a PDF and packs four- or
        # five-fold, which is what lets a real document fit the link's
        # 64 KB response at all.
        stream = zlib.compress(('\n'.join(ops) + '\n').encode('latin-1', errors='replace'), 9)
        objects.append((content_num, b'<< /Length %d /Filter /FlateDecode >>\nstream\n' % len(stream) +
                         stream + b'\nendstream'))

    objects.sort(key=lambda o: o[0])
    out = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = {}
    for num, body in objects:
        offsets[num] = len(out)
        out += ('%d 0 obj\n' % num).encode('ascii') + body + b'\nendobj\n'

    xref_start = len(out)
    max_num = max(offsets)
    out += ('xref\n0 %d\n' % (max_num + 1)).encode('ascii')
    out += b'0000000000 65535 f \n'
    for n in range(1, max_num + 1):
        off = offsets.get(n)
        out += (b'0000000000 00000 f \n' if off is None else ('%010d 00000 n \n' % off).encode('ascii'))
    out += ('trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF' %
            (max_num + 1, xref_start)).encode('ascii')
    return bytes(out)


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
        page, header, footer = odt_page_and_margins(body)
        return STATUS_OK, paragraphs_to_rtf_bytes(odt_bytes_to_paragraphs(body), page, header, footer)
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
        header, footer = rtf_margin_texts(rtf_bytes)
        if format_id == FORMAT_ODT:
            return STATUS_OK, paragraphs_to_odt_bytes(paragraphs, rtf_page_setup(rtf_bytes), header, footer)
        return STATUS_OK, paragraphs_to_pdf_bytes(paragraphs, rtf_page_setup(rtf_bytes), header, footer)
    except Exception:
        return STATUS_ERR, b''


def handle_request(op, payload):
    if op == OP_CONVERT_IMPORT:
        return handle_convert_import(payload)
    if op == OP_CONVERT_EXPORT:
        return handle_convert_export(payload)
    return STATUS_UNSUPPORTED, b''


def main():
    ops_supported = (1 << OP_CONVERT_IMPORT) | (1 << OP_CONVERT_EXPORT)
    return run_gateway(handle_request, ops_supported, 'odt-gateway', description=__doc__)


if __name__ == '__main__':
    sys.exit(main() or 0)
