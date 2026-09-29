#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate printable keyboard reference cards, one per keybinding scheme.

The kind of card that used to sit on the strip above the function keys.
Five schemes ship (vi, WordStar, Emacs, WordPerfect, Word), and nobody
remembers five sets of bindings.

WHY IT IS GENERATED. A card that is typed out by hand is wrong the first
time someone adds a binding, and being wrong is worse than not existing:
the whole point of a card is that you trust it without checking. So both
halves are derived from the files the application itself is built from:

  - the universal application shortcuts, from platform/common/actions.c's
    g_wp_actions[] table -- the same rows the menus and the keyboard
    dispatch are driven by;
  - the per-scheme bindings, from docs/keymaps.md's own reference tables.

That mirrors gen_help_guide.c, which exports AmigaGuide by reusing the
REAL help parser rather than a second hand-rolled one, for the same
reason: an export that reads a different source than the app does will
drift, and will drift silently.

wp_menu_model_selfcheck already proves no two actions share a shortcut
byte, so the universal half of every card is unambiguous by construction.

Python rather than C, unlike the two existing generators. Those are C
specifically so they can link engine parsers (help_doc.c); there is no
engine parser for Markdown, so the reason does not apply, and hand-
rolling one in C89 would buy nothing.

Usage: gen_keycard.py <repo-root> <output-dir>
Writes <scheme>.html and <scheme>.txt per scheme, plus index.html.
"""
import os
import re
import sys

# Section heading in docs/keymaps.md -> (scheme id, display name). The id
# matches the keymap= value config.c accepts, so a card can be named
# after the thing you would actually put in the config file.
SCHEMES = [
    ('vi',          'vi',                          'vi'),
    ('wordstar',    'WordStar',                    'WordStar'),
    ('emacs',       'Emacs',                       'Emacs'),
    ('wordperfect', 'WordPerfect',                 'WordPerfect'),
    ('msword',      'Microsoft Word, classic',     'Microsoft Word'),
]


def strip_md(s):
    return s.replace('`', '')


def parse_table_under(lines, heading_prefix, header_prefix):
    """Rows of the first `header_prefix` table under `## heading_prefix`."""
    start = None
    for i, ln in enumerate(lines):
        if ln.startswith('## ') and ln[3:].startswith(heading_prefix):
            start = i
            break
    if start is None:
        return None
    rows = []
    in_table = False
    for ln in lines[start + 1:]:
        if ln.startswith('## '):
            break
        if ln.startswith(header_prefix):
            in_table = True
            continue
        if in_table and ln.startswith('|---'):
            continue
        if in_table:
            if not ln.startswith('|'):
                in_table = False
                continue
            cells = [c.strip() for c in ln.strip().strip('|').split('|')]
            # Separator rows vary in this repo: some tables write
            # |---|---| and some | --- | --- |. Recognise both by
            # content rather than by prefix.
            if all(set(c) <= set('-: ') and c for c in cells):
                continue
            rows.append(cells)
    return rows


def parse_hardware_keys(md_path):
    """The machine-specific HELP/UNDO keys.

    These belong on EVERY card, not on one scheme's: an ST has a HELP key
    whichever keybinding scheme you drive it with. Optional -- an older
    keymaps.md without the section simply yields nothing rather than
    failing the whole run.
    """
    with open(md_path, encoding='utf-8') as fh:
        lines = fh.read().split('\n')
    rows = parse_table_under(lines, 'Hardware keys', '| Key ')
    if not rows:
        return []
    out = []
    for cells in rows:
        if len(cells) >= 4:
            out.append((strip_md(cells[0]), strip_md(cells[3]),
                        strip_md(cells[1])))
    return out


def parse_keymap_tables(md_path):
    """Pull each scheme's `| Command | Key(s) |` table out of keymaps.md.

    Deliberately strict about what it accepts: a heading whose table is
    missing or empty is an error, not an empty card. A silently blank
    card is exactly the failure this tool exists to prevent.
    """
    with open(md_path, encoding='utf-8') as fh:
        lines = fh.read().split('\n')

    out = {}
    for scheme_id, heading_prefix, _display in SCHEMES:
        start = None
        for i, ln in enumerate(lines):
            if ln.startswith('## ') and ln[3:].startswith(heading_prefix):
                start = i
                break
        if start is None:
            raise SystemExit('gen_keycard: no "## %s" section in %s'
                             % (heading_prefix, md_path))

        rows = []
        in_table = False
        for ln in lines[start + 1:]:
            if ln.startswith('## '):
                break
            if ln.startswith('| Command'):
                in_table = True
                continue
            if in_table and ln.startswith('|---'):
                continue
            if in_table:
                if not ln.startswith('|'):
                    # Blank line or prose ends this table, but a later
                    # table under the same heading still counts.
                    in_table = False
                    continue
                cells = [c.strip() for c in ln.strip().strip('|').split('|')]
                if len(cells) >= 2:
                    rows.append((cells[0], cells[1]))
        if not rows:
            raise SystemExit('gen_keycard: "## %s" has no table rows'
                             % heading_prefix)
        out[scheme_id] = rows
    return out


# One row of g_wp_actions[]. Matches the real formatting in actions.c:
#   { WP_ACT_FILE_NEW, WP_MENU_FILE, "New", "Ctrl+N", 0x0E, act_file_new, NULL },
ACTION_ROW = re.compile(
    r'\{\s*(WP_ACT_[A-Z_]+)\s*,\s*(WP_MENU_[A-Z]+)\s*,\s*'
    r'"([^"]*)"\s*,\s*(?:"([^"]*)"|0)\s*,\s*(0x[0-9A-Fa-f]+|0)\s*,')


def parse_actions(actions_c):
    """The universal application shortcuts, from the registry itself.

    Only rows that actually have a displayed key hint make it onto a
    card: an action reachable solely by menu has nothing to tell someone
    looking at their keyboard.
    """
    with open(actions_c, encoding='utf-8') as fh:
        text = fh.read()

    # Only the table body, so a doc-comment example cannot be mistaken
    # for a real row.
    start = text.index('g_wp_actions[WP_ACT_COUNT] =')
    body = text[start:]

    rows = []
    for m in ACTION_ROW.finditer(body):
        _act_id, group, label, hint, _byte = m.groups()
        if not hint:
            continue
        rows.append((label, hint, group.replace('WP_MENU_', '').title()))
    if not rows:
        raise SystemExit('gen_keycard: parsed no rows from %s' % actions_c)
    return rows


CSS = """
:root { --ink:#111; --rule:#c8c8c8; --band:#f2f2f2; --accent:#7a1f1f; }
* { box-sizing: border-box; }
body { margin:0; padding:1.5rem; background:#fff; color:var(--ink);
       font-family: "DejaVu Sans", Verdana, Geneva, sans-serif; font-size:11px; }
.card { max-width: 27cm; margin:0 auto 1.5rem; border:1.5px solid var(--ink);
        padding:0.6rem 0.8rem; }
.card > header { display:flex; align-items:baseline; gap:0.6rem;
                 border-bottom:1.5px solid var(--ink); padding-bottom:0.35rem;
                 margin-bottom:0.5rem; }
h1 { margin:0; font-size:15px; letter-spacing:0.02em; }
.sub { color:#555; font-size:10px; }
.cfg { margin-left:auto; font-family:"DejaVu Sans Mono",monospace; font-size:10px;
       background:var(--band); padding:0.1rem 0.4rem; border:1px solid var(--rule); }
.cols { column-count:3; column-gap:1rem; column-rule:1px solid var(--rule); }
@media (max-width: 900px) { .cols { column-count:2; } }
@media (max-width: 620px) { .cols { column-count:1; } }
h2 { font-size:10px; text-transform:uppercase; letter-spacing:0.08em;
     color:var(--accent); margin:0 0 0.25rem; break-after:avoid; }
.grp { break-inside:avoid-column; margin-bottom:0.6rem; }
dl { margin:0; }
.row { display:flex; gap:0.4rem; align-items:baseline; padding:0.5px 0;
       break-inside:avoid; }
.cmd { flex:1 1 auto; min-width:0; }
.key { flex:0 0 auto; font-family:"DejaVu Sans Mono",monospace; font-size:10px;
       white-space:nowrap; }
.key code { background:var(--band); border:1px solid var(--rule);
            border-radius:2px; padding:0 0.2rem; }
footer { margin-top:0.4rem; padding-top:0.3rem; border-top:1px solid var(--rule);
         color:#666; font-size:9px; display:flex; gap:0.6rem; }
footer .gen { margin-left:auto; font-style:italic; }
a { color:var(--accent); }
@page { size: A4 landscape; margin: 8mm; }
@media print { body { padding:0; } .card { border-width:1px; page-break-after:always; } }
"""


def esc(s):
    return (s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;'))


def md_inline(s):
    """`code` -> <code>, which is the only markup these tables use."""
    return re.sub(r'`([^`]+)`', lambda m: '<code>%s</code>' % esc(m.group(1)), esc(s))


def render_html(scheme_id, display, rows, universal, hardware=()):
    groups = {}
    order = []
    for label, hint, group in universal:
        if group not in groups:
            groups[group] = []
            order.append(group)
        groups[group].append((label, hint))

    parts = []
    parts.append('<!-- generated by host/tools/gen_keycard.py -- do not edit -->')
    parts.append('<title>RetroWP %s keyboard card</title>' % esc(display))
    parts.append('<style>%s</style>' % CSS)
    parts.append('<div class="card">')
    parts.append('<header><h1>RetroWP &mdash; %s</h1>'
                 '<span class="sub">keyboard reference</span>'
                 '<span class="cfg">keymap=%s</span></header>'
                 % (esc(display), esc(scheme_id)))
    parts.append('<div class="cols">')

    parts.append('<div class="grp"><h2>%s editing</h2><dl>' % esc(display))
    for cmd, key in rows:
        parts.append('<div class="row"><span class="cmd">%s</span>'
                     '<span class="key">%s</span></div>'
                     % (md_inline(cmd), md_inline(key)))
    parts.append('</dl></div>')

    for group in order:
        parts.append('<div class="grp"><h2>%s</h2><dl>' % esc(group))
        for label, hint in groups[group]:
            parts.append('<div class="row"><span class="cmd">%s</span>'
                         '<span class="key"><code>%s</code></span></div>'
                         % (esc(label), esc(hint)))
        parts.append('</dl></div>')

    if hardware:
        parts.append('<div class="grp"><h2>Hardware keys</h2><dl>')
        for key, action, machines in hardware:
            parts.append('<div class="row"><span class="cmd">%s '
                         '<span style="color:#777">(%s)</span></span>'
                         '<span class="key"><code>%s</code></span></div>'
                         % (esc(action), esc(machines), esc(key)))
        parts.append('</dl></div>')

    parts.append('</div>')
    parts.append('<footer><span>Set <code>keymap=%s</code> in the config file.</span>'
                 '<span>F1 opens the built-in manual.</span>'
                 '<span class="gen">generated from docs/keymaps.md and '
                 'platform/common/actions.c</span></footer>' % esc(scheme_id))
    parts.append('</div>')
    return '\n'.join(parts) + '\n'


def render_text(scheme_id, display, rows, universal, hardware=()):
    def block(title, pairs):
        width = max((len(a) for a, _ in pairs), default=0)
        out = [title, '-' * len(title)]
        for a, b in pairs:
            out.append('  %-*s  %s' % (width, a, b))
        out.append('')
        return out

    lines = []
    head = 'RetroWP - %s keyboard reference   (keymap=%s)' % (display, scheme_id)
    lines.append(head)
    lines.append('=' * len(head))
    lines.append('')
    lines += block('%s editing' % display,
                   [(strip_md(c), strip_md(k)) for c, k in rows])

    groups = {}
    order = []
    for label, hint, group in universal:
        if group not in groups:
            groups[group] = []
            order.append(group)
        groups[group].append((label, hint))
    for group in order:
        lines += block(group, groups[group])

    if hardware:
        lines += block('Hardware keys',
                       [('%s (%s)' % (a, m), k) for k, a, m in hardware])

    lines.append('F1 opens the built-in manual, unless the scheme binds it.')
    lines.append('Generated from docs/keymaps.md and platform/common/actions.c '
                 '-- do not edit.')
    return '\n'.join(lines) + '\n'


def render_index(entries):
    parts = ['<!-- generated by host/tools/gen_keycard.py -- do not edit -->',
             '<title>RetroWP keyboard cards</title>',
             '<style>%s</style>' % CSS,
             '<div class="card"><header><h1>RetroWP keyboard cards</h1>'
             '<span class="sub">one per keybinding scheme</span></header><dl>']
    for scheme_id, display, count in entries:
        parts.append('<div class="row"><span class="cmd">'
                     '<a href="%s.html">%s</a> &mdash; %d bindings</span>'
                     '<span class="key"><code>keymap=%s</code> '
                     '&middot; <a href="%s.txt">text</a></span></div>'
                     % (scheme_id, esc(display), count, esc(scheme_id), scheme_id))
    parts.append('</dl><footer><span>Print landscape; each card is one page.'
                 '</span></footer></div>')
    return '\n'.join(parts) + '\n'


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: gen_keycard.py <repo-root> <output-dir>')
    root, outdir = sys.argv[1], sys.argv[2]

    tables = parse_keymap_tables(os.path.join(root, 'docs', 'keymaps.md'))
    universal = parse_actions(os.path.join(root, 'platform', 'common', 'actions.c'))
    hardware = parse_hardware_keys(os.path.join(root, 'docs', 'keymaps.md'))

    if not os.path.isdir(outdir):
        os.makedirs(outdir)

    entries = []
    for scheme_id, _heading, display in SCHEMES:
        rows = tables[scheme_id]
        with open(os.path.join(outdir, scheme_id + '.html'), 'w',
                  encoding='utf-8') as fh:
            fh.write(render_html(scheme_id, display, rows, universal, hardware))
        with open(os.path.join(outdir, scheme_id + '.txt'), 'w',
                  encoding='utf-8') as fh:
            fh.write(render_text(scheme_id, display, rows, universal, hardware))
        entries.append((scheme_id, display, len(rows)))

    with open(os.path.join(outdir, 'index.html'), 'w', encoding='utf-8') as fh:
        fh.write(render_index(entries))

    print('gen_keycard: %d cards, %d universal shortcuts, %d hardware keys -> %s'
          % (len(entries), len(universal), len(hardware), outdir))
    for scheme_id, display, count in entries:
        print('  %-12s %3d scheme bindings' % (scheme_id, count))


if __name__ == '__main__':
    main()
