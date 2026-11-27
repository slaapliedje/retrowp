#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes sample_greek.docx: a small Word document in the shape Word 2021
saves (Transitional namespaces, a theme whose minor font is Calibri,
docDefaults of 11 pt with 8 pt after and 259/240 line spacing, the
Normal/Heading 1/List Paragraph styles), carrying what test_convert.c's
DOCX tests check:

  - a Heading 1 in Greek (style-inherited bold and size)
  - a centred paragraph of modern Greek with tonos, bold and italic runs
  - Greek typed in the Symbol font and one w:sym, both to become Unicode
  - a numbered list (decimal) and a bulleted one (Symbol F0B7)
  - a two-row table
  - a page break, then a second section on A4 landscape
  - a default header with a complex PAGE field, a footer of plain text

Deterministic (fixed timestamps), so the file it writes only changes when
this script does. Run from the repository root:
    python3 host/tests/fixtures/make_greek_docx.py
"""

import zipfile

W = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
R = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
NS = 'xmlns:w="%s" xmlns:r="%s"' % (W, R)
HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'

CONTENT_TYPES = HEAD + (
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="xml" ContentType="application/xml"/>'
    '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
    '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
    '<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>'
    '<Override PartName="/word/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>'
    '<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>'
    '<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>'
    '</Types>')

ROOT_RELS = HEAD + (
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
    '</Relationships>')

DOC_RELS = HEAD + (
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
    '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>'
    '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="theme/theme1.xml"/>'
    '<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>'
    '<Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>'
    '<Relationship Id="rId6" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.org/" TargetMode="External"/>'
    '</Relationships>')

THEME = HEAD + (
    '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme">'
    '<a:themeElements><a:fontScheme name="Office">'
    '<a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/>'
    '<a:font script="Grek" typeface="Calibri Light"/></a:majorFont>'
    '<a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/>'
    '<a:font script="Grek" typeface="Calibri"/></a:minorFont>'
    '</a:fontScheme></a:themeElements></a:theme>')

STYLES = HEAD + (
    '<w:styles %s>'
    '<w:docDefaults><w:rPrDefault><w:rPr>'
    '<w:rFonts w:asciiTheme="minorHAnsi" w:eastAsiaTheme="minorHAnsi" w:hAnsiTheme="minorHAnsi" w:cstheme="minorBidi"/>'
    '<w:kern w:val="2"/><w:sz w:val="22"/><w:szCs w:val="22"/><w:lang w:val="el-GR" w:eastAsia="en-US" w:bidi="ar-SA"/>'
    '</w:rPr></w:rPrDefault>'
    '<w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/></w:pPr></w:pPrDefault>'
    '</w:docDefaults>'
    '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>'
    '<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/>'
    '<w:next w:val="Normal"/><w:pPr><w:keepNext/><w:spacing w:before="240" w:after="0"/><w:outlineLvl w:val="0"/></w:pPr>'
    '<w:rPr><w:rFonts w:asciiTheme="majorHAnsi" w:hAnsiTheme="majorHAnsi"/><w:b/><w:color w:val="2F5496"/>'
    '<w:sz w:val="32"/><w:szCs w:val="32"/></w:rPr></w:style>'
    '<w:style w:type="paragraph" w:styleId="ListParagraph"><w:name w:val="List Paragraph"/><w:basedOn w:val="Normal"/>'
    '<w:pPr><w:ind w:left="720"/><w:contextualSpacing/></w:pPr></w:style>'
    '<w:style w:type="character" w:default="1" w:styleId="DefaultParagraphFont"><w:name w:val="Default Paragraph Font"/></w:style>'
    '<w:style w:type="character" w:styleId="Strong"><w:name w:val="Strong"/><w:basedOn w:val="DefaultParagraphFont"/>'
    '<w:rPr><w:b/><w:bCs/></w:rPr></w:style>'
    '<w:style w:type="table" w:default="1" w:styleId="TableNormal"><w:name w:val="Normal Table"/></w:style>'
    '</w:styles>') % NS

NUMBERING = HEAD + (
    '<w:numbering %s>'
    '<w:abstractNum w:abstractNumId="0"><w:multiLevelType w:val="hybridMultilevel"/>'
    '<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%%1."/>'
    '<w:lvlJc w:val="left"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl>'
    '</w:abstractNum>'
    '<w:abstractNum w:abstractNumId="1"><w:multiLevelType w:val="hybridMultilevel"/>'
    '<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val=""/>'
    '<w:lvlJc w:val="left"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr>'
    '<w:rPr><w:rFonts w:ascii="Symbol" w:hAnsi="Symbol" w:hint="default"/></w:rPr></w:lvl>'
    '</w:abstractNum>'
    '<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>'
    '<w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num>'
    '</w:numbering>') % NS


def para(runs, ppr=''):
    return '<w:p>%s%s</w:p>' % ('<w:pPr>%s</w:pPr>' % ppr if ppr else '', runs)


def run(text, rpr=''):
    return '<w:r>%s<w:t xml:space="preserve">%s</w:t></w:r>' % (
        '<w:rPr>%s</w:rPr>' % rpr if rpr else '', text)


def lst(num, text):
    return para(run(text), '<w:pStyle w:val="ListParagraph"/><w:numPr><w:ilvl w:val="0"/>'
                           '<w:numId w:val="%d"/></w:numPr>' % num)


def cell(text):
    return '<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/></w:tcPr>%s</w:tc>' % para(run(text))


SECT1 = ('<w:sectPr><w:headerReference w:type="default" r:id="rId4"/>'
         '<w:footerReference w:type="default" r:id="rId5"/>'
         '<w:pgSz w:w="11906" w:h="16838"/>'
         '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="708" w:footer="708" w:gutter="0"/>'
         '<w:cols w:space="708"/><w:docGrid w:linePitch="360"/></w:sectPr>')
SECT2 = ('<w:sectPr><w:pgSz w:w="16838" w:h="11906" w:orient="landscape"/>'
         '<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134" w:header="708" w:footer="708" w:gutter="0"/>'
         '</w:sectPr>')

BODY = ''.join([
    para(run('Καλημέρα κόσμε'), '<w:pStyle w:val="Heading1"/>'),
    para(run('Η γρήγορη καφέ αλεπού ') +
         run('πηδάει', '<w:b/>') + run(' πάνω από ') +
         run('τον τεμπέλη σκύλο.', '<w:i/>') +
         run(' Ϊϋΐΰ ', '<w:rStyle w:val="Strong"/>'),
         '<w:jc w:val="center"/>'),
    para(run('Symbol: ') + run('abgdW', '<w:rFonts w:ascii="Symbol" w:hAnsi="Symbol"/>') +
         '<w:r><w:sym w:font="Symbol" w:char="F070"/></w:r>' +
         '<w:hyperlink r:id="rId6"><w:r><w:t xml:space="preserve"> σύνδεσμος</w:t></w:r></w:hyperlink>' +
         '<w:ins w:id="1" w:author="a"><w:r><w:t xml:space="preserve"> νέο</w:t></w:r></w:ins>' +
         '<w:del w:id="2" w:author="a"><w:r><w:delText>παλιό</w:delText></w:r></w:del>'),
    lst(1, 'Πρώτο'), lst(1, 'Δεύτερο'),
    lst(2, 'Ελιά'),
    '<w:tbl><w:tblPr><w:tblW w:w="0" w:type="auto"/></w:tblPr>'
    '<w:tblGrid><w:gridCol w:w="4675"/><w:gridCol w:w="4675"/></w:tblGrid>'
    '<w:tr>%s%s</w:tr><w:tr>%s%s</w:tr></w:tbl>' % (cell('Όνομα'), cell('Τιμή'), cell('ένα'), cell('1')),
    para(run('Τέλος πρώτου τμήματος.') + '<w:r><w:br w:type="page"/></w:r>' + run('Νέα σελίδα.'),
         SECT1),
    para(run('Δεύτερο τμήμα, οριζόντια σελίδα.', '<w:sz w:val="28"/>'),
         '<w:spacing w:line="360" w:lineRule="auto"/><w:tabs><w:tab w:val="right" w:pos="9000"/></w:tabs>'),
])

DOCUMENT = HEAD + '<w:document %s><w:body>%s%s</w:body></w:document>' % (NS, BODY, SECT2)

HEADER = HEAD + (
    '<w:hdr %s><w:p><w:pPr><w:pStyle w:val="Header"/><w:jc w:val="right"/></w:pPr>'
    '<w:r><w:t xml:space="preserve">Σελίδα </w:t></w:r>'
    '<w:r><w:fldChar w:fldCharType="begin"/></w:r>'
    '<w:r><w:instrText xml:space="preserve"> PAGE   \\* MERGEFORMAT </w:instrText></w:r>'
    '<w:r><w:fldChar w:fldCharType="separate"/></w:r>'
    '<w:r><w:t>1</w:t></w:r>'
    '<w:r><w:fldChar w:fldCharType="end"/></w:r>'
    '</w:p></w:hdr>') % NS

FOOTER = HEAD + '<w:ftr %s>%s</w:ftr>' % (NS, para(run('Υποσέλιδο')))


def main():
    with zipfile.ZipFile('host/tests/fixtures/sample_greek.docx', 'w', zipfile.ZIP_DEFLATED) as zf:
        for name, text in (('[Content_Types].xml', CONTENT_TYPES), ('_rels/.rels', ROOT_RELS),
                           ('word/document.xml', DOCUMENT), ('word/_rels/document.xml.rels', DOC_RELS),
                           ('word/styles.xml', STYLES), ('word/numbering.xml', NUMBERING),
                           ('word/theme/theme1.xml', THEME), ('word/header1.xml', HEADER),
                           ('word/footer1.xml', FOOTER)):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 27, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, text.encode('utf-8'))


if __name__ == '__main__':
    main()
