# SPDX-License-Identifier: GPL-2.0-or-later
"""The gateway's paragraph model -> a Word (.docx) package, standard library only.

CONVERT_EXPORT with format_id 3: what docx_reader.py reads, written back
for Word 2007 and later (Word 2021 opens it in its own mode, not
Compatibility Mode, because settings.xml says compatibilityMode 15).
docs/formats/docx.md has the mapping; in short:

- A paragraph's alignment, indents, spacing before and after, line
  spacing (a multiple, or "at least"), tab stops and page break before.
- A run's bold, italic, underline, strikethrough, super/subscript,
  colour, size and face. A run with Greek in it is marked Greek
  (w:lang el-GR) so Word checks its spelling as Greek.
- Each section's page size, orientation and margins, and its header and
  footer: '|' parts left, centre and right on a centre and a right tab
  stop, '#' a PAGE field.
- A face named Helvetica, Times or Courier -- the PostScript names an
  ST, an Amiga or a Mac writes -- is set as Arial, Times New Roman or
  Courier New, which Windows has, Greek included; no face is Arial.
"""

import io
import zipfile
from xml.sax.saxutils import escape as _esc

_W = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
_R = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
_PKG_REL = 'http://schemas.openxmlformats.org/package/2006/relationships'
_REL_DOC = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument'
_REL_STYLES = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles'
_REL_SETTINGS = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings'
_REL_HEADER = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/header'
_REL_FOOTER = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer'
_CT_MAIN = 'application/vnd.openxmlformats-officedocument.wordprocessingml.'

DEFAULT_FACE = 'Arial'
_FACE_FOR = {'helvetica': 'Arial', 'times': 'Times New Roman', 'times roman': 'Times New Roman',
             'times-roman': 'Times New Roman', 'courier': 'Courier New'}
_FACE_FOR_FAMILY = {'roman': 'Times New Roman', 'modern': 'Courier New'}

_DEFAULT_PAGE = (612.0, 792.0, 72.0, 72.0, 72.0, 72.0)   # US Letter, one-inch margins, points


def _a(s):
    """An attribute value."""
    return _esc(s, {'"': '&quot;'})


def _clean(text):
    """Text XML 1.0 can hold: its control characters dropped (a tab and a
    line break are written as elements, not characters)."""
    return ''.join(ch for ch in text if ch >= ' ' or ch in '\t\n')


def _twips(points):
    return int(round(points * 20))


def _is_greek(text):
    return any(0x0370 <= ord(ch) <= 0x03FF or 0x1F00 <= ord(ch) <= 0x1FFF for ch in text)


def _face(run):
    name = run.get('font')
    if name:
        return _FACE_FOR.get(name.strip().lower(), name)
    return _FACE_FOR_FAMILY.get(run.get('family'))


def _run_xml(run, text):
    props = []
    face = _face(run)
    if face:
        props.append('<w:rFonts w:ascii="%s" w:hAnsi="%s" w:cs="%s" w:eastAsia="%s"/>' % ((_a(face),) * 4))
    if run.get('b'):
        props.append('<w:b/><w:bCs/>')
    if run.get('i'):
        props.append('<w:i/><w:iCs/>')
    if run.get('strike'):
        props.append('<w:strike/>')
    if run.get('color'):
        props.append('<w:color w:val="%02X%02X%02X"/>' % tuple(run['color']))
    half = int(round(run.get('size_twips', 240) / 10.0))
    if half != 24:
        props.append('<w:sz w:val="%d"/><w:szCs w:val="%d"/>' % (half, half))
    if run.get('ul'):
        props.append('<w:u w:val="single"/>')
    if run.get('super'):
        props.append('<w:vertAlign w:val="superscript"/>')
    elif run.get('sub'):
        props.append('<w:vertAlign w:val="subscript"/>')
    if _is_greek(text):
        props.append('<w:lang w:val="el-GR"/>')
    body = []
    for k, line in enumerate(text.split('\n')):
        if k:
            body.append('<w:br/>')
        for j, piece in enumerate(line.split('\t')):
            if j:
                body.append('<w:tab/>')
            if piece:
                body.append('<w:t xml:space="preserve">%s</w:t>' % _esc(piece))
    rpr = '<w:rPr>%s</w:rPr>' % ''.join(props) if props else ''
    return '<w:r>%s%s</w:r>' % (rpr, ''.join(body))


def _ppr_xml(p, sect_xml=''):
    out = []
    if p.get('pagebb'):
        out.append('<w:pageBreakBefore/>')
    tabs = p.get('tabs') or []
    if tabs:
        out.append('<w:tabs>%s</w:tabs>' % ''.join(
            '<w:tab w:val="%s" w:pos="%d"/>' % (kind if kind in ('left', 'right', 'center', 'decimal') else 'left',
                                               int(pos))
            for pos, kind in tabs))
    sb, sa, sl, slmult = p.get('sb', 0), p.get('sa', 0), p.get('sl', 0), p.get('slmult', 0)
    spacing = ['w:before="%d"' % sb, 'w:after="%d"' % sa]
    if sl > 0 and slmult:
        spacing.append('w:line="%d" w:lineRule="auto"' % sl)          # a multiple, in 240ths: Word's own unit
    elif sl:
        spacing.append('w:line="%d" w:lineRule="atLeast"' % abs(sl))
    out.append('<w:spacing %s/>' % ' '.join(spacing))
    li, ri, fi = p.get('li', 0), p.get('ri', 0), p.get('fi', 0)
    if li or ri or fi:
        ind = ['w:left="%d"' % li, 'w:right="%d"' % ri]
        ind.append('w:firstLine="%d"' % fi if fi >= 0 else 'w:hanging="%d"' % -fi)
        out.append('<w:ind %s/>' % ' '.join(ind))
    jc = {'center': 'center', 'right': 'right', 'justify': 'both'}.get(p.get('align', 'left'))
    if jc:
        out.append('<w:jc w:val="%s"/>' % jc)
    return '<w:pPr>%s%s</w:pPr>' % (''.join(out), sect_xml)


def _margin_xml(text, column_twips, margin_parts):
    """A header or footer part: one paragraph, its parts on a centre tab
    mid-column and a right tab at the column's edge, '#' a PAGE field."""
    def part(t):
        pieces = _clean(t).split('#')
        out = []
        for k, piece in enumerate(pieces):
            if k:
                out.append('<w:fldSimple w:instr=" PAGE "><w:r><w:rPr><w:sz w:val="20"/></w:rPr>'
                           '<w:t>1</w:t></w:r></w:fldSimple>')
            if piece:
                out.append('<w:r><w:rPr><w:sz w:val="20"/>%s</w:rPr><w:t xml:space="preserve">%s</w:t></w:r>' %
                           ('<w:lang w:val="el-GR"/>' if _is_greek(piece) else '', _esc(piece)))
        return ''.join(out)
    tab = '<w:r><w:tab/></w:r>'
    parts = margin_parts(text) if text else ['', '', '']
    return ('<w:p><w:pPr><w:tabs><w:tab w:val="center" w:pos="%d"/><w:tab w:val="right" w:pos="%d"/></w:tabs>'
            '<w:spacing w:before="0" w:after="0"/></w:pPr>%s</w:p>' %
            (column_twips // 2, column_twips, tab.join(part(t) for t in parts)))


def _part(root_tag, inner):
    return ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            '<w:%s xmlns:w="%s" xmlns:r="%s">%s</w:%s>' % (root_tag, _W, _R, inner, root_tag)).encode('utf-8')


def paragraphs_to_docx_bytes(paragraphs, sections, margin_parts):
    """sections: (page, header, footer) per section as odt_gateway's
    rtf_sections gives them (page in points, None for US Letter); each
    paragraph's 'section' indexes it. margin_parts: odt_gateway's, the
    left/centre/right split every format shares."""
    sections = sections or [(None, '', '')]
    sections = [(pg or _DEFAULT_PAGE, hd or '', ft or '') for pg, hd, ft in sections]

    # Header and footer parts, one per distinct text. A section names
    # its own even when empty, since Word gives a section that names none
    # the one before it -- and the engine has already carried a header
    # on from section to section where the document meant it to.
    rels, extra_parts = [], []
    margin_ids = {}
    any_header = any(h for _, h, _ in sections)
    any_footer = any(f for _, _, f in sections)

    def margin_ref(kind, text, column):
        key = (kind, text, column)
        if key not in margin_ids:
            n = len(margin_ids) + 1
            rid = 'rIdM%d' % n
            name = '%s%d.xml' % (kind, n)
            margin_ids[key] = rid
            rels.append((rid, _REL_HEADER if kind == 'header' else _REL_FOOTER, name))
            extra_parts.append((name, kind, _part('hdr' if kind == 'header' else 'ftr',
                                                  _margin_xml(text, column, margin_parts))))
        return margin_ids[key]

    def sect_pr(si):
        (w, h, ml, mr, mt, mb), header, footer = sections[si]
        column = max(1440, _twips(w - ml - mr))
        refs = ''
        if any_header:
            refs += '<w:headerReference w:type="default" r:id="%s"/>' % margin_ref('header', header, column)
        if any_footer:
            refs += '<w:footerReference w:type="default" r:id="%s"/>' % margin_ref('footer', footer, column)
        orient = ' w:orient="landscape"' if w > h else ''
        return ('<w:sectPr>%s<w:pgSz w:w="%d" w:h="%d"%s/>'
                '<w:pgMar w:top="%d" w:right="%d" w:bottom="%d" w:left="%d" w:header="%d" w:footer="%d" w:gutter="0"/>'
                '</w:sectPr>' % (refs, _twips(w), _twips(h), orient, _twips(mt), _twips(mr), _twips(mb), _twips(ml),
                                 _twips(mt / 2.0), _twips(mb / 2.0)))

    body = []
    secs = [min(max(0, p.get('section', 0)), len(sections) - 1) for p in paragraphs]
    for k, p in enumerate(paragraphs):
        # A section's properties ride on its last paragraph; the last
        # section's are the body's own, after everything.
        ends_section = k + 1 < len(paragraphs) and secs[k + 1] != secs[k]
        runs = ''.join(_run_xml(r, _clean(r.get('text', ''))) for r in p.get('runs', []) if r.get('text'))
        body.append('<w:p>%s%s</w:p>' % (_ppr_xml(p, sect_pr(secs[k]) if ends_section else ''), runs))
    body.append(sect_pr(secs[-1] if secs else 0))
    document = _part('document', '<w:body>%s</w:body>' % ''.join(body))

    styles = _part('styles',
                   '<w:docDefaults><w:rPrDefault><w:rPr>'
                   '<w:rFonts w:ascii="%s" w:hAnsi="%s" w:cs="%s" w:eastAsia="%s"/>'
                   '<w:sz w:val="24"/><w:szCs w:val="24"/><w:lang w:val="en-US"/></w:rPr></w:rPrDefault>'
                   '<w:pPrDefault><w:pPr><w:spacing w:before="0" w:after="0"/></w:pPr></w:pPrDefault>'
                   '</w:docDefaults>'
                   '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/>'
                   '<w:qFormat/></w:style>' % ((DEFAULT_FACE,) * 4))
    settings = _part('settings',
                     '<w:defaultTabStop w:val="720"/><w:compat>'
                     '<w:compatSetting w:name="compatibilityMode" w:uri="http://schemas.microsoft.com/office/word"'
                     ' w:val="15"/></w:compat>')

    doc_rels = [('rIdStyles', _REL_STYLES, 'styles.xml'), ('rIdSettings', _REL_SETTINGS, 'settings.xml')] + rels
    rels_xml = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="%s">%s</Relationships>' %
                (_PKG_REL, ''.join('<Relationship Id="%s" Type="%s" Target="%s"/>' % r for r in doc_rels)))
    overrides = ['<Override PartName="/word/document.xml" ContentType="%sdocument.main+xml"/>' % _CT_MAIN,
                 '<Override PartName="/word/styles.xml" ContentType="%sstyles+xml"/>' % _CT_MAIN,
                 '<Override PartName="/word/settings.xml" ContentType="%ssettings+xml"/>' % _CT_MAIN]
    for name, kind, _ in extra_parts:
        overrides.append('<Override PartName="/word/%s" ContentType="%s%s+xml"/>' % (name, _CT_MAIN, kind))
    content_types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                     '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                     '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                     '<Default Extension="xml" ContentType="application/xml"/>%s</Types>' % ''.join(overrides))
    package_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="%s">'
                    '<Relationship Id="rId1" Type="%s" Target="word/document.xml"/></Relationships>' %
                    (_PKG_REL, _REL_DOC))

    buf = io.BytesIO()
    # A fixed date on every entry: the same document makes the same bytes.
    stamp = (1980, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(buf, 'w') as zf:
        def put(name, data):
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data)
        put('[Content_Types].xml', content_types.encode('utf-8'))
        put('_rels/.rels', package_rels.encode('utf-8'))
        put('word/document.xml', document)
        put('word/styles.xml', styles)
        put('word/settings.xml', settings)
        put('word/_rels/document.xml.rels', rels_xml.encode('utf-8'))
        for name, _, data in extra_parts:
            put('word/' + name, data)
    return buf.getvalue()
