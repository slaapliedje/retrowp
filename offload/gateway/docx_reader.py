# SPDX-License-Identifier: GPL-2.0-or-later
"""Word (.docx) -> the gateway's paragraph model, for CONVERT_IMPORT with
format_id 3 (odt_gateway.py answers the op; this module only reads).

A .docx is Office Open XML (ECMA-376): a ZIP whose word/document.xml
holds the body, word/styles.xml the styles, word/theme/theme1.xml the
theme's fonts, word/numbering.xml the lists and word/header*.xml /
footer*.xml the margins, found through word/_rels/document.xml.rels.
Read with the standard library alone, like the ODT reader beside it:
the Pi has neither python-docx nor LibreOffice, and needs neither.

Elements are matched by their LOCAL name, so the Transitional namespace
Word writes by default and the Strict one it writes on request
(purl.oclc.org) both read. What comes out is exactly the shape
odt_bytes_to_paragraphs gives -- the caller passes in its own
_new_para/_new_run so the two never drift -- and the same (page, header,
footer) sections odt_sections gives, so paragraphs_to_rtf_bytes takes
it unchanged.

Text is Unicode in the file, so Greek (or anything else) arrives as
itself; how a machine DRAWS it is that machine's business. The one
exception is the Symbol font, whose letters are stored as Latin ones --
Word's Insert Symbol writes a w:sym, and older documents type Greek in
Symbol directly -- and those are turned into the Unicode characters they
show (SYMBOL_TO_UNICODE).

Carried: paragraphs (alignment, indents, spacing before/after, line
spacing, tab stops, page break before), runs (bold, italic, underline,
strike, super/subscript, size, colour, face), the style inheritance
behind both (docDefaults, basedOn chains, character styles), lists as
their number or bullet and a tab, tables as one paragraph per row with
the cells apart by tabs (a cell of several paragraphs breaks the row
into them), page and column breaks, section page sizes and margins,
default headers and footers with the page number field, fields' shown
results, and tracked insertions. Left out: deletions, field
instructions, pictures and text boxes (w:drawing, w:pict), footnote
references, comments.
"""

import io
import re
import zipfile
import xml.etree.ElementTree as ET

# Adobe's Symbol encoding, 0x20-0xFE, for the characters whose shape
# differs from the Latin letter they are stored as (digits and most
# punctuation are the same in both and pass unchanged). From the Symbol
# font's own encoding vector as Adobe publishes it (symbol.txt).
SYMBOL_TO_UNICODE = {
    0x22: 0x2200, 0x24: 0x2203, 0x27: 0x220B, 0x2A: 0x2217, 0x2D: 0x2212,
    0x40: 0x2245,
    0x41: 0x0391, 0x42: 0x0392, 0x43: 0x03A7, 0x44: 0x0394, 0x45: 0x0395,
    0x46: 0x03A6, 0x47: 0x0393, 0x48: 0x0397, 0x49: 0x0399, 0x4A: 0x03D1,
    0x4B: 0x039A, 0x4C: 0x039B, 0x4D: 0x039C, 0x4E: 0x039D, 0x4F: 0x039F,
    0x50: 0x03A0, 0x51: 0x0398, 0x52: 0x03A1, 0x53: 0x03A3, 0x54: 0x03A4,
    0x55: 0x03A5, 0x56: 0x03C2, 0x57: 0x03A9, 0x58: 0x039E, 0x59: 0x03A8,
    0x5A: 0x0396, 0x5C: 0x2234, 0x5E: 0x22A5, 0x60: 0xF8E5,
    0x61: 0x03B1, 0x62: 0x03B2, 0x63: 0x03C7, 0x64: 0x03B4, 0x65: 0x03B5,
    0x66: 0x03C6, 0x67: 0x03B3, 0x68: 0x03B7, 0x69: 0x03B9, 0x6A: 0x03D5,
    0x6B: 0x03BA, 0x6C: 0x03BB, 0x6D: 0x03BC, 0x6E: 0x03BD, 0x6F: 0x03BF,
    0x70: 0x03C0, 0x71: 0x03B8, 0x72: 0x03C1, 0x73: 0x03C3, 0x74: 0x03C4,
    0x75: 0x03C5, 0x76: 0x03D6, 0x77: 0x03C9, 0x78: 0x03BE, 0x79: 0x03C8,
    0x7A: 0x03B6, 0x7E: 0x223C,
    0xA1: 0x03D2, 0xA2: 0x2032, 0xA3: 0x2264, 0xA4: 0x2044, 0xA5: 0x221E,
    0xA6: 0x0192, 0xA7: 0x2663, 0xA8: 0x2666, 0xA9: 0x2665, 0xAA: 0x2660,
    0xAB: 0x2194, 0xAC: 0x2190, 0xAD: 0x2191, 0xAE: 0x2192, 0xAF: 0x2193,
    0xB0: 0x00B0, 0xB1: 0x00B1, 0xB2: 0x2033, 0xB3: 0x2265, 0xB4: 0x00D7,
    0xB5: 0x221D, 0xB6: 0x2202, 0xB7: 0x2022, 0xB8: 0x00F7, 0xB9: 0x2260,
    0xBA: 0x2261, 0xBB: 0x2248, 0xBC: 0x2026,
    0xC0: 0x2135, 0xC1: 0x2111, 0xC2: 0x211C, 0xC3: 0x2118, 0xC4: 0x2297,
    0xC5: 0x2295, 0xC6: 0x2205, 0xC7: 0x2229, 0xC8: 0x222A, 0xC9: 0x2283,
    0xCA: 0x2287, 0xCB: 0x2284, 0xCC: 0x2282, 0xCD: 0x2286, 0xCE: 0x2208,
    0xCF: 0x2209, 0xD0: 0x2220, 0xD1: 0x2207, 0xD2: 0x00AE, 0xD3: 0x00A9,
    0xD4: 0x2122, 0xD5: 0x220F, 0xD6: 0x221A, 0xD7: 0x22C5, 0xD8: 0x00AC,
    0xD9: 0x2227, 0xDA: 0x2228, 0xDB: 0x21D4, 0xDC: 0x21D0, 0xDD: 0x21D1,
    0xDE: 0x21D2, 0xDF: 0x21D3, 0xE0: 0x25CA, 0xE1: 0x2329, 0xE5: 0x2211,
    0xF1: 0x232A, 0xF2: 0x222B,
}

# The fonts whose letters are Symbol's, not Latin ones.
_SYMBOL_FACES = ('symbol',)

# Word's w:jc values, both the old spellings and ECMA-376's start/end.
_JC = {'left': 'left', 'start': 'left', 'right': 'right', 'end': 'right',
       'center': 'center', 'both': 'justify', 'distribute': 'justify',
       'lowKashida': 'justify', 'mediumKashida': 'justify', 'highKashida': 'justify',
       'thaiDistribute': 'justify'}

_TAB_KINDS = {'left': 'left', 'start': 'left', 'right': 'right', 'end': 'right',
              'center': 'center', 'decimal': 'decimal'}

# Where a PAGE field stood, until the margin reader turns it into the
# engine's page-number part. NUL cannot occur in XML text.
PAGE_MARK = '\x00PAGE\x00'


def _local(tag):
    return tag.rsplit('}', 1)[-1] if isinstance(tag, str) else ''


def _attr(el, name):
    """An attribute by its local name, whatever namespace it is in."""
    if el is None:
        return None
    for k, v in el.attrib.items():
        if k == name or k.endswith('}' + name):
            return v
    return None


def _child(el, name):
    if el is None:
        return None
    for c in el:
        if _local(c.tag) == name:
            return c
    return None


def _int(v, default=0):
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def _on(el):
    """A toggle property (w:b, w:i, ...): present and not val="0/false/off"."""
    if el is None:
        return None
    v = _attr(el, 'val')
    return v is None or v.lower() not in ('0', 'false', 'off', 'none')


def symbol_text(text):
    """Text stored in the Symbol font, as the characters it shows. Word
    keeps a Symbol character either as its byte (0x20-0xFF) or in the
    private-use copy at U+F020-U+F0FF; both read the same."""
    out = []
    for ch in text:
        cp = ord(ch)
        if 0xF020 <= cp <= 0xF0FF:
            cp -= 0xF000
        out.append(chr(SYMBOL_TO_UNICODE.get(cp, cp)) if cp <= 0xFF else ch)
    return ''.join(out)


class _Docx(object):
    def __init__(self, data, new_para, new_run, family_guess, zip_read):
        self.new_para = new_para
        self.new_run = new_run
        self.family_guess = family_guess
        self.zf = zipfile.ZipFile(io.BytesIO(data))
        self.zip_read = zip_read
        names = self.zf.namelist()
        self.names = set(names)
        self.main = self._main_part()
        if self.main not in self.names:
            raise ValueError('no word/document.xml: not a Word document')
        self.rels = self._rels(self.main)
        self.theme = self._theme_fonts()
        self.styles, self.default_pstyle, self.default_cstyle, self.defaults = self._styles()
        self.numbering = self._numbering()
        self.counters = {}

    # -- parts ---------------------------------------------------------

    def _xml(self, name):
        if name not in self.names:
            return None
        return ET.fromstring(self.zip_read(self.zf, name))

    def _main_part(self):
        """The main document part, from _rels/.rels (Word always calls it
        word/document.xml, but the package says where it is)."""
        root = self._xml('_rels/.rels')
        if root is not None:
            for r in root:
                if (_attr(r, 'Type') or '').endswith('/officeDocument'):
                    return (_attr(r, 'Target') or '').lstrip('/')
        return 'word/document.xml'

    def _rels(self, part):
        """A part's relationships, id -> target part name."""
        d = part.rsplit('/', 1)[0] if '/' in part else ''
        root = self._xml('%s/_rels/%s.rels' % (d, part.rsplit('/', 1)[-1]) if d
                         else '_rels/%s.rels' % part)
        out = {}
        if root is None:
            return out
        for r in root:
            if _attr(r, 'TargetMode') == 'External':
                continue
            tgt = _attr(r, 'Target') or ''
            if tgt.startswith('/'):
                tgt = tgt[1:]
            elif d:
                tgt = d + '/' + tgt
            out[_attr(r, 'Id')] = tgt
        return out

    def _theme_fonts(self):
        """minor/major Latin faces, for rFonts' asciiTheme/hAnsiTheme."""
        out = {}
        tgt = None
        for rid, t in self.rels.items():
            if 'theme/' in t:
                tgt = t
        root = self._xml(tgt or 'word/theme/theme1.xml')
        if root is None:
            return out
        for el in root.iter():
            ln = _local(el.tag)
            if ln in ('minorFont', 'majorFont'):
                latin = _child(el, 'latin')
                face = _attr(latin, 'typeface')
                if face:
                    out['minor' if ln == 'minorFont' else 'major'] = face
        return out

    # -- properties ----------------------------------------------------

    def _font_of(self, rfonts):
        if rfonts is None:
            return None
        for name in ('ascii', 'hAnsi'):
            v = _attr(rfonts, name)
            if v:
                return v
        for name in ('asciiTheme', 'hAnsiTheme'):
            v = _attr(rfonts, name)
            if v:
                return self.theme.get('major' if v.startswith('major') else 'minor')
        return None

    def _rpr(self, rpr, into):
        """w:rPr's properties over `into` (a dict of only what is set)."""
        if rpr is None:
            return into
        for c in rpr:
            ln = _local(c.tag)
            if ln in ('b', 'i', 'strike'):
                v = _on(c)
                into['b' if ln == 'b' else 'i' if ln == 'i' else 'strike'] = v
            elif ln == 'dstrike':
                into['strike'] = _on(c)
            elif ln == 'u':
                v = _attr(c, 'val')
                into['ul'] = v is not None and v != 'none'
            elif ln == 'vertAlign':
                v = _attr(c, 'val')
                into['super'] = v == 'superscript'
                into['sub'] = v == 'subscript'
            elif ln == 'sz':
                hp = _int(_attr(c, 'val'))
                if hp > 0:
                    into['size_twips'] = hp * 10    # half-points
            elif ln == 'color':
                v = _attr(c, 'val') or ''
                if len(v) == 6 and v.lower() != 'auto':
                    try:
                        into['color'] = (int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16))
                    except ValueError:
                        pass
                elif v.lower() == 'auto':
                    into['color'] = None
            elif ln == 'rFonts':
                f = self._font_of(c)
                if f:
                    into['font'] = f
            elif ln == 'rStyle':
                into['_rstyle'] = _attr(c, 'val')
        return into

    def _ppr(self, ppr, into):
        if ppr is None:
            return into
        for c in ppr:
            ln = _local(c.tag)
            if ln == 'jc':
                into['align'] = _JC.get(_attr(c, 'val') or 'left', 'left')
            elif ln == 'ind':
                for key, names in (('li', ('left', 'start')), ('ri', ('right', 'end'))):
                    for n in names:
                        v = _attr(c, n)
                        if v is not None:
                            into[key] = _int(v)
                            break
                if _attr(c, 'hanging') is not None:
                    into['fi'] = -_int(_attr(c, 'hanging'))
                elif _attr(c, 'firstLine') is not None:
                    into['fi'] = _int(_attr(c, 'firstLine'))
            elif ln == 'spacing':
                if _attr(c, 'before') is not None:
                    into['sb'] = _int(_attr(c, 'before'))
                if _attr(c, 'after') is not None:
                    into['sa'] = _int(_attr(c, 'after'))
                line = _attr(c, 'line')
                if line is not None:
                    rule = _attr(c, 'lineRule') or 'auto'
                    n = _int(line)
                    if rule == 'auto':
                        # 240ths of a line: RTF's \\slmult1 spelling.
                        into['sl'], into['slmult'] = (n, 1) if n != 240 else (0, 0)
                    elif rule == 'exact':
                        into['sl'], into['slmult'] = -n, 0
                    else:
                        into['sl'], into['slmult'] = n, 0
            elif ln == 'tabs':
                tabs = list(into.get('tabs', []))
                for t in c:
                    if _local(t.tag) != 'tab':
                        continue
                    kind = _attr(t, 'val') or 'left'
                    pos = _int(_attr(t, 'pos'))
                    if kind == 'clear':
                        tabs = [x for x in tabs if x[0] != pos]
                    elif kind in _TAB_KINDS and pos > 0:
                        tabs = [x for x in tabs if x[0] != pos] + [(pos, _TAB_KINDS[kind])]
                into['tabs'] = sorted(tabs)
            elif ln == 'pageBreakBefore':
                into['pagebb'] = bool(_on(c))
            elif ln == 'pStyle':
                into['_pstyle'] = _attr(c, 'val')
            elif ln == 'numPr':
                into['_num'] = (_attr(_child(c, 'numId'), 'val'), _int(_attr(_child(c, 'ilvl'), 'val')))
        return into

    def _styles(self):
        root = self._xml('word/styles.xml')
        styles, dp, dc = {}, None, None
        defaults = ({}, {})
        if root is None:
            return styles, dp, dc, defaults
        dd = _child(root, 'docDefaults')
        if dd is not None:
            rpr = _child(_child(dd, 'rPrDefault'), 'rPr')
            ppr = _child(_child(dd, 'pPrDefault'), 'pPr')
            defaults = (self._ppr(ppr, {}), self._rpr(rpr, {}))
        for st in root:
            if _local(st.tag) != 'style':
                continue
            sid = _attr(st, 'styleId')
            kind = _attr(st, 'type')
            based = _attr(_child(st, 'basedOn'), 'val')
            p = self._ppr(_child(st, 'pPr'), {})
            r = self._rpr(_child(st, 'rPr'), {})
            p.pop('_pstyle', None)
            r.pop('_rstyle', None)
            styles[(kind, sid)] = (based, p, r)
            if _attr(st, 'default') in ('1', 'true', 'on'):
                if kind == 'paragraph':
                    dp = sid
                elif kind == 'character':
                    dc = sid
        return styles, dp, dc, defaults

    def _chain(self, kind, sid):
        """A style's (pPr, rPr), its basedOn ancestors applied first."""
        seen = []
        while sid is not None and (kind, sid) in self.styles and sid not in seen:
            seen.append(sid)
            sid = self.styles[(kind, sid)][0]
        p, r = {}, {}
        for s in reversed(seen):
            _, sp, sr = self.styles[(kind, s)]
            p.update(sp)
            r.update(sr)
        return p, r

    def _numbering(self):
        root = self._xml('word/numbering.xml')
        abstract, nums = {}, {}
        if root is None:
            return {}
        for el in root:
            ln = _local(el.tag)
            if ln == 'abstractNum':
                levels = {}
                for lvl in el:
                    if _local(lvl.tag) != 'lvl':
                        continue
                    rpr = _child(lvl, 'rPr')
                    levels[_int(_attr(lvl, 'ilvl'))] = {
                        'fmt': _attr(_child(lvl, 'numFmt'), 'val') or 'decimal',
                        'text': _attr(_child(lvl, 'lvlText'), 'val') or '',
                        'start': _int(_attr(_child(lvl, 'start'), 'val'), 1),
                        'suff': _attr(_child(lvl, 'suff'), 'val') or 'tab',
                        'font': self._font_of(_child(rpr, 'rFonts')) if rpr is not None else None,
                        'ppr': self._ppr(_child(lvl, 'pPr'), {}),
                    }
                abstract[_attr(el, 'abstractNumId')] = levels
            elif ln == 'num':
                nums[_attr(el, 'numId')] = _attr(_child(el, 'abstractNumId'), 'val')
        return {nid: abstract.get(aid, {}) for nid, aid in nums.items()}

    # -- lists ---------------------------------------------------------

    @staticmethod
    def _format_number(n, fmt):
        if fmt in ('lowerLetter', 'upperLetter'):
            s = ''
            while n > 0:
                n, rem = divmod(n - 1, 26)
                s = chr(ord('a') + rem) + s
            return s.upper() if fmt == 'upperLetter' else s
        if fmt in ('lowerRoman', 'upperRoman'):
            vals = ((1000, 'm'), (900, 'cm'), (500, 'd'), (400, 'cd'), (100, 'c'), (90, 'xc'),
                    (50, 'l'), (40, 'xl'), (10, 'x'), (9, 'ix'), (5, 'v'), (4, 'iv'), (1, 'i'))
            s = ''
            for v, r in vals:
                while n >= v:
                    s += r
                    n -= v
            return s.upper() if fmt == 'upperRoman' else s
        if fmt == 'none':
            return ''
        return str(n)

    def _list_label(self, num):
        """The label a w:numPr paragraph starts with, or None."""
        nid, ilvl = num
        levels = self.numbering.get(nid)
        if not levels or nid in (None, '0'):
            return None, {}
        lvl = levels.get(ilvl)
        if lvl is None:
            return None, {}
        count = self.counters.setdefault(nid, {})
        for deeper in [k for k in count if k > ilvl]:
            del count[deeper]
        count[ilvl] = count.get(ilvl, lvl['start'] - 1) + 1
        if lvl['fmt'] == 'bullet':
            text = lvl['text']
            if (lvl['font'] or '').lower() in _SYMBOL_FACES:
                text = symbol_text(text)
            elif (lvl['font'] or '').lower() == 'wingdings' or not text or \
                    any(0xF000 <= ord(c) <= 0xF0FF for c in text):
                text = u'•'   # a Wingdings bullet has no Unicode spelling; a bullet is what it means
        else:
            def sub(m):
                k = int(m.group(1)) - 1
                lv = levels.get(k, lvl)
                return self._format_number(count.get(k, lv['start']), lv['fmt'])
            text = re.sub(r'%([1-9])', sub, lvl['text'])
        suff = {'tab': '\t', 'space': ' '}.get(lvl['suff'], '')
        return text + suff, lvl['ppr']

    # -- body ----------------------------------------------------------

    def _make_para(self, ppr_el):
        direct = self._ppr(ppr_el, {})
        sid = direct.pop('_pstyle', None) or self.default_pstyle
        sp, sr = self._chain('paragraph', sid)
        para = self.new_para()
        para.update(self.defaults[0])
        para.update(sp)
        num = direct.pop('_num', None) or sp.pop('_num', None)
        para.pop('_num', None)
        para.pop('_pstyle', None)
        label = None
        if num is not None:
            label, lppr = self._list_label(num)
            if label is not None:
                para.update({k: v for k, v in lppr.items() if not k.startswith('_')})
        para.update(direct)
        para['runs'] = []
        base_run = dict(self.defaults[1])
        base_run.update(sr)
        para['_base_run'] = base_run
        if label:
            self._add_text(para, label, base_run)
        return para

    def _run_props(self, base, rpr_el):
        props = dict(base)
        direct = self._rpr(rpr_el, {})
        rs = direct.pop('_rstyle', None) or self.default_cstyle
        if rs is not None:
            props.update(self._chain('character', rs)[1])
        props.update(direct)
        return props

    def _add_text(self, para, text, props):
        if not text:
            return
        face = props.get('font')
        if face and face.lower() in _SYMBOL_FACES:
            text = symbol_text(text)
            props = dict(props)
            props['font'] = None    # the characters are Unicode now; Symbol would misdraw them
        runs = para['runs']
        key = tuple(sorted((k, v) for k, v in props.items() if not k.startswith('_')))
        if runs and runs[-1].get('_key') == key:
            runs[-1]['text'] += text
            return
        r = self.new_run()
        for k, v in props.items():
            if not k.startswith('_') and k in r:
                r[k] = bool(v) if k in ('b', 'i', 'ul', 'strike', 'super', 'sub') else v
        if r.get('font'):
            r['family'] = self.family_guess(r['font'])
        r['text'] = text
        r['_key'] = key
        runs.append(r)

    def _walk_runs(self, el, para, out, field):
        """The inline content of a paragraph (or of a hyperlink, insertion,
        smart tag, ... inside one). `field` is a one-item list: the depth
        of complex fields whose instructions are being skipped."""
        for c in el:
            ln = _local(c.tag)
            if ln == 'r':
                props = self._run_props(para['_base_run'], _child(c, 'rPr'))
                for x in c:
                    xl = _local(x.tag)
                    if xl == 'fldChar':
                        kind = _attr(x, 'fldCharType')
                        if kind == 'begin':
                            field.append('instr')
                        elif kind == 'separate' and field and field[-1] != 'page':
                            field[-1] = 'result'
                        elif kind == 'end' and field:
                            field.pop()
                        continue
                    if field and field[-1] == 'instr':
                        if xl == 'instrText' and field_is_page(x.text):
                            # Its shown result is skipped; the number
                            # itself is what a header or footer keeps.
                            field[-1] = 'page'
                            self._add_text(para, PAGE_MARK, para['_base_run'])
                        continue
                    if field and field[-1] == 'page':
                        continue
                    if xl == 't':
                        self._add_text(para, x.text or '', props)
                    elif xl == 'tab':
                        self._add_text(para, '\t', props)
                    elif xl in ('br', 'cr'):
                        kind = _attr(x, 'type')
                        if kind in ('page', 'column'):
                            para = self._split(para, out)
                            para['pagebb'] = True
                        else:
                            para = self._split(para, out)
                    elif xl == 'noBreakHyphen':
                        self._add_text(para, u'‑', props)
                    elif xl == 'sym':
                        try:
                            ch = int(_attr(x, 'char') or '0', 16)
                        except ValueError:
                            continue
                        face = (_attr(x, 'font') or '').lower()
                        if face in _SYMBOL_FACES:
                            sp = dict(props)
                            sp['font'] = 'Symbol'
                            self._add_text(para, chr(ch), sp)
                        elif 0xF000 <= ch <= 0xF0FF or face == 'wingdings':
                            self._add_text(para, u'□', props)   # no Unicode form known
                        else:
                            self._add_text(para, chr(ch), props)
                    elif xl == 'ptab':
                        self._add_text(para, '\t', props)
            elif ln in ('hyperlink', 'ins', 'smartTag', 'customXml', 'fldSimple', 'dir', 'bdo',
                        'moveTo'):
                if ln == 'fldSimple' and field_is_page(_attr(c, 'instr')):
                    self._add_text(para, PAGE_MARK, para['_base_run'])
                    continue
                para = self._walk_runs(c, para, out, field)
            elif ln == 'sdt':
                content = _child(c, 'sdtContent')
                if content is not None:
                    para = self._walk_runs(content, para, out, field)
            elif ln == 'AlternateContent':
                choice = _child(c, 'Choice')   # Fallback repeats it
                if choice is not None:
                    para = self._walk_runs(choice, para, out, field)
        return para

    def _split(self, para, out):
        """A line or page break inside a paragraph: the paragraph so far
        ends, and one like it carries on."""
        out.append(para)
        nxt = dict(para)
        nxt['runs'] = []
        nxt['fi'] = 0 if para.get('fi', 0) > 0 else para.get('fi', 0)
        nxt['sb'] = 0
        nxt.pop('pagebb', None)
        return nxt

    def paragraphs_of(self, container, out, field=None):
        """Every paragraph under a body, cell, header or footer, in order."""
        field = field if field is not None else []
        for c in container:
            ln = _local(c.tag)
            if ln == 'p':
                ppr = _child(c, 'pPr')
                para = self._make_para(ppr)
                para = self._walk_runs(c, para, out, field)
                out.append(para)
                sect = _child(ppr, 'sectPr') if ppr is not None else None
                if sect is not None:
                    para['_sect_end'] = sect
            elif ln == 'tbl':
                self._table(c, out, field)
            elif ln == 'sdt':
                content = _child(c, 'sdtContent')
                if content is not None:
                    self.paragraphs_of(content, out, field)
            elif ln in ('customXml', 'ins', 'moveTo'):
                self.paragraphs_of(c, out, field)
            elif ln == 'AlternateContent':
                choice = _child(c, 'Choice')
                if choice is not None:
                    self.paragraphs_of(choice, out, field)

    def _table(self, tbl, out, field):
        for tr in tbl:
            if _local(tr.tag) != 'tr':
                continue
            cells = []
            for tc in tr:
                if _local(tc.tag) == 'tc':
                    paras = []
                    self.paragraphs_of(tc, paras, field)
                    cells.append(paras)
                elif _local(tc.tag) == 'sdt':
                    content = _child(tc, 'sdtContent')
                    for tc2 in (content if content is not None else []):
                        if _local(tc2.tag) == 'tc':
                            paras = []
                            self.paragraphs_of(tc2, paras, field)
                            cells.append(paras)
            if cells and all(len(p) <= 1 for p in cells):
                # A plain row: one paragraph, the cells apart by tabs,
                # laid out as the first cell's paragraph is.
                row = None
                for k, paras in enumerate(cells):
                    p = paras[0] if paras else None
                    if row is None:
                        row = p if p is not None else self._make_para(None)
                        row['align'] = 'left'
                        continue
                    row['runs'].append(self._tab_run(row))
                    if p is not None:
                        row['runs'].extend(p['runs'])
                if row is not None:
                    row.pop('_sect_end', None)
                    out.append(row)
            else:
                for paras in cells:
                    for p in paras:
                        p.pop('_sect_end', None)
                        out.append(p)

    def _tab_run(self, para):
        r = self.new_run()
        r.update({k: v for k, v in para['_base_run'].items() if k in r and not k.startswith('_')})
        r['b'] = r['i'] = r['ul'] = r['strike'] = r['super'] = r['sub'] = False
        if r.get('font'):
            r['family'] = self.family_guess(r['font'])
        r['text'] = '\t'
        return r

    # -- margins and sections -----------------------------------------

    def _margin(self, sect, kind):
        """The default header/footer of a section as the engine's parts
        (odt_masters' shape): [] for one that is empty, None when the
        section names none (and so has the one before it)."""
        for ref in sect:
            if _local(ref.tag) != kind + 'Reference':
                continue
            if (_attr(ref, 'type') or 'default') != 'default':
                continue
            target = self.rels.get(_attr(ref, 'id'))
            root = self._xml(target) if target else None
            if root is None:
                return []
            paras = []
            self.paragraphs_of(root, paras)
            for p in paras:
                t = ''.join(r['text'] for r in p['runs'])
                if t.strip():
                    return _margin_parts_from(t, p.get('align', 'left')) or []
            return []
        return None

    def section_info(self, sect):
        if sect is None:
            return (None, None, None)
        pg = _child(sect, 'pgSz')
        mar = _child(sect, 'pgMar')
        page = None
        if pg is not None:
            w = _int(_attr(pg, 'w'), 12240) / 20.0
            h = _int(_attr(pg, 'h'), 15840) / 20.0
            m = [72.0] * 4
            if mar is not None:
                m = [_int(_attr(mar, k), 1440) / 20.0 for k in ('left', 'right', 'top', 'bottom')]
                m = [abs(x) for x in m]
            page = (w, h, m[0], m[1], m[2], m[3])
        return (page, self._margin(sect, 'header'), self._margin(sect, 'footer'))


def field_is_page(instr):
    return bool(instr) and instr.strip().split(' ')[0].upper() == 'PAGE'


def _margin_parts_from(text, align='left'):
    """A header/footer line as left/centre/right parts the way the engine
    keeps them: tabs part it (Word's default header has a centre tab and
    a right tab), an empty middle dropped, a lone centre kept alone. A
    line with no tab goes where its paragraph is aligned -- to the engine
    one part is centred and two are left and right."""
    raw = text.split('\t')
    parts = []
    for piece in raw:
        toks = []
        bits = piece.split(PAGE_MARK)
        for i, b in enumerate(bits):
            if i:
                toks.append(('page',))
            if b:
                toks.append(('text', b))
        parts.append(toks)
    if len(parts) == 1:
        if align == 'right':
            parts = [[], parts[0]]
        elif align != 'center':
            parts = [parts[0], []]
    if len(parts) > 3:
        parts = parts[:2] + [[t for p in parts[2:] for t in p]]
    if len(parts) == 3 and not parts[1]:
        parts = [parts[0], parts[2]]
    elif len(parts) == 3 and not parts[0] and not parts[2]:
        parts = [parts[1]]
    return parts if any(parts) else None


def docx_bytes_to_paragraphs(data, new_para, new_run, family_guess, zip_read):
    """(paragraphs, sections) from a .docx's bytes, in odt_bytes_to_
    paragraphs' and odt_sections' shapes, each paragraph's 'section'
    numbered."""
    d = _Docx(data, new_para, new_run, family_guess, zip_read)
    root = d._xml(d.main)
    body = None
    for el in root:
        if _local(el.tag) == 'body':
            body = el
    paragraphs = []
    if body is None:
        return paragraphs, [(None, None, None)]
    d.paragraphs_of(body, paragraphs)
    final_sect = _child(body, 'sectPr')

    # Sections: a paragraph carrying w:sectPr ENDS its section; the body's
    # own w:sectPr is the last one's.
    sections = []
    k = 0
    for p in paragraphs:
        p['section'] = k
        sect = p.pop('_sect_end', None)
        if sect is not None:
            sections.append(d.section_info(sect))
            k += 1
    if not paragraphs or paragraphs[-1]['section'] == k:
        # The body's own w:sectPr is the last section's -- unless the last
        # paragraph already ended a section and none follows.
        sections.append(d.section_info(final_sect))
    # A section that names no header (or footer) has the one before it,
    # as Word shows it (ECMA-376 17.10.5): a section's own empty one is
    # how it says it has none. _margin answers None for "names none" and
    # [] for "names an empty one".
    for k in range(1, len(sections)):
        page, header, footer = sections[k]
        sections[k] = (page, sections[k - 1][1] if header is None else header,
                       sections[k - 1][2] if footer is None else footer)
    for p in paragraphs:
        p.pop('_base_run', None)
        for r in p['runs']:
            r.pop('_key', None)
            # A page number in the body is rare; it has no place to go.
            r['text'] = r['text'].replace(PAGE_MARK, '')
        p['runs'] = [r for r in p['runs'] if r['text']]
    if not paragraphs:
        paragraphs = [new_para()]
        paragraphs[0]['section'] = 0
    return paragraphs, sections
