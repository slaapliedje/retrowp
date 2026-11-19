/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "editing.h"
#include "format_rtf.h"
#include "mem_host.h"

/* T20.8: every journaled edit, its undo and its redo, walked through
   running out of memory at each allocation in turn (mem_host_fail_after).
   Whatever allocation fails:
   - the document is the one before or the one after -- never a third
     thing -- for an edit that is one step; for a compound one (paste,
     delete across paragraphs, styling a range) the steps that did happen
     all come undone again, back to before;
   - Undo and Redo, failing, leave the document as it was;
   - nothing leaks.
   The document is compared as the RTF it exports, which carries text,
   runs and their styles, paragraph styles, tables and sections. */

static const char k_start[] =
    "{\\rtf1\\ansi{\\fonttbl{\\f0 Times;}}\\f0\\fs24 "
    "Hello {\\b bold} world\\par "
    "Second {\\i para}graph here\\par "
    "\\qc Third, centred\\par "
    "\\ql Last of the first section\\sect "
    "Fourth, in section two\\par "
    "Fifth\\par}";

#define SNAP_CAP 8192u

/* Undo is content-equivalent, not identical (docs/editing.md): an
   insert in the middle of a run splits it, and undoing the insert
   leaves the two halves as two runs of the same style. The RTF writes
   them as {\f1\fs24 He}{\f1\fs24 llo } -- the same document. So two
   adjacent run groups with the same formatting are joined before
   comparing: what is left to differ is text, styles, paragraphs and
   sections. (Run text has its braces escaped, so a bare '}{' is always
   between groups.) */
static void join_same_runs(char *s)
{
    char *p = s;

    while ((p = strstr(p, "}{")) != NULL) {
        char *prev = p, *next = p + 1, *sp_prev, *sp_next;
        size_t lp, ln;

        if (p > s && p[-1] == '\\') { p += 2; continue; }   /* an escaped brace */
        while (prev > s && !(prev[0] == '{' && (prev == s || prev[-1] != '\\'))) prev--;
        sp_prev = strchr(prev, ' ');
        sp_next = strchr(next, ' ');
        if (prev == s || sp_prev == NULL || sp_next == NULL || sp_prev > p) { p += 2; continue; }
        lp = (size_t)(sp_prev - prev);
        ln = (size_t)(sp_next - next);
        if (lp == ln && memcmp(prev, next, lp) == 0) {
            /* drop "}" and the next group's "{\ctrl " */
            memmove(p, sp_next + 1, strlen(sp_next + 1) + 1);
        } else {
            p += 2;
        }
    }
}

static void snap(const wp_document *doc, char *out)
{
    u32 n = 0;
    if (format_rtf_export(doc, (u8 *)out, SNAP_CAP - 1, &n) != WP_OK) n = 0;
    out[n] = '\0';
    join_same_runs(out);
}

static wp_bool load(wp_document *doc)
{
    return (wp_bool)(format_rtf_import((const u8 *)k_start, (u32)strlen(k_start), doc) == WP_OK);
}

static wp_pos at(u32 s, u32 p, u16 r, u32 o)
{
    wp_pos pos;
    pos.section = s; pos.paragraph = p; pos.run = r; pos.offset = o;
    return pos;
}

/* ---- the edits ---------------------------------------------------------- */

typedef wp_status (*edit_fn)(wp_document *doc, wp_undo_journal *j);

static wp_status e_insert(wp_document *d, wp_undo_journal *j)
{ return wp_edit_insert_text(d, j, at(0, 0, 0, 2), (const u8 *)"XYZ", 3, NULL); }

static wp_status e_delete(wp_document *d, wp_undo_journal *j)
{ return wp_edit_delete_range(d, j, 0, 1, 0, 1, 5); }

static wp_status e_split(wp_document *d, wp_undo_journal *j)
{ return wp_edit_split_paragraph(d, j, at(0, 1, 0, 6), NULL); }

static wp_status e_merge(wp_document *d, wp_undo_journal *j)
{ return wp_edit_merge_paragraph(d, j, 0, 0, NULL); }

static wp_status e_split_section(wp_document *d, wp_undo_journal *j)
{ return wp_edit_split_section(d, j, at(0, 2, 0, 0), NULL); }

static wp_status e_merge_section(wp_document *d, wp_undo_journal *j)
{ return wp_edit_merge_section(d, j, 0, NULL); }

static wp_status e_set_section(wp_document *d, wp_undo_journal *j)
{
    wp_section setup = *wp_document_section_get(d, 0);
    setup.margin_left += 720;
    setup.margin_top += 360;
    return wp_edit_set_section(d, j, 0, &setup);
}

static wp_status e_set_pstyle(wp_document *d, wp_undo_journal *j)
{
    /* the centred paragraph's style onto the first */
    wp_pstyle centred = wp_section_paragraph_get(wp_document_section_get(d, 0), 2)->style;
    return wp_edit_set_pstyle(d, j, 0, 0, centred);
}

static wp_status e_set_table(wp_document *d, wp_undo_journal *j)
{ return wp_edit_set_table(d, j, 0, 1, (wp_trow)1, (u8)1); }

static wp_status e_insert_empty(wp_document *d, wp_undo_journal *j)
{
    wp_pstyle ps = wp_section_paragraph_get(wp_document_section_get(d, 0), 0)->style;
    return wp_edit_insert_empty_paragraph(d, j, 0, 1, ps, (wp_trow)0, (u8)0);
}

static wp_status e_remove_empty(wp_document *d, wp_undo_journal *j)
{
    /* an empty paragraph made first, outside the walk's count, by the
       caller's setup -- see walk_edit's `prepare` */
    return wp_edit_remove_empty_paragraph(d, j, 0, 1);
}

static wp_status e_image(wp_document *d, wp_undo_journal *j)
{
    wp_image_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width_px = 8;
    desc.height_px = 8;
    return wp_edit_insert_image(d, j, at(0, 1, 0, 3), &desc);
}

static wp_status e_delete_selection(wp_document *d, wp_undo_journal *j)
{
    wp_selection sel;
    sel.anchor = at(0, 0, 1, 2);
    sel.caret = at(0, 2, 0, 4);
    return wp_edit_delete_selection(d, j, sel, NULL);
}

static wp_status e_paste(wp_document *d, wp_undo_journal *j)
{
    wp_status st;
    wp_undo_group_begin(j);
    st = wp_edit_paste_text(d, j, at(0, 1, 0, 3), (const u8 *)"one\ntwo\nthree", 13, NULL);
    wp_undo_group_end(j);
    return st;
}

static wp_status e_style(wp_document *d, wp_undo_journal *j)
{
    wp_style_delta delta;
    memset(&delta, 0, sizeof(delta));
    delta.flags_set = WP_STYLE_ITALIC;
    delta.size_twips = 280;
    return wp_edit_style_range(d, j, at(0, 0, 0, 3), at(0, 2, 0, 5), &delta);
}

/* ---- the walk ------------------------------------------------------------- */

static char g_before[SNAP_CAP], g_after[SNAP_CAP], g_now[SNAP_CAP];
static wp_bool g_walk_failed;
static i32 g_fail_n;

/* One failure per edit is enough to read: the first is reported, with
   the allocation it failed at, and that edit's walk stops. */
static void test_fail_msg(const char *name, const char *why)
{
    if (g_walk_failed) return;
    g_walk_failed = WP_TRUE;
    printf("  FAIL oom walk, %s at allocation %ld: %s\n", name, (long)g_fail_n, why);
    if (getenv("OOM_WALK_DUMP")) {
        printf("--- before\n%s\n--- after\n%s\n--- now\n%s\n", g_before, g_after, g_now);
    }
    g_test_failures++;
}

/* Undo everything on the journal (without a limit); WP_FALSE if Undo
   refused while the journal still said it could. */
static wp_bool undo_all(wp_document *d, wp_undo_journal *j)
{
    u32 guard = 0;
    while (wp_undo_can_undo(j) && guard++ < 64)
        if (wp_edit_undo(d, j) != WP_OK) return WP_FALSE;
    return WP_TRUE;
}

/* `prepare` (may be NULL) runs before the snapshot, unlimited and
   unjournaled as far as the walk is concerned; `compound` says the edit
   is several steps that may stop part-way. */
static void walk_edit(const char *name, edit_fn edit, edit_fn prepare, wp_bool compound)
{
    i32 n;
    wp_bool done = WP_FALSE;

    g_walk_failed = WP_FALSE;
    g_fail_n = -1;
    fprintf(stderr, "  oom walk: %s ...\n", name);   /* unbuffered: a crash still names it */
    /* The result with memory to spare. */
    {
        wp_document d;
        wp_undo_journal j;
        ASSERT_TRUE(load(&d));
        wp_undo_journal_init(&j);
        if (prepare) ASSERT_TRUE(prepare(&d, &j) == WP_OK);
        wp_undo_journal_free(&j);
        wp_undo_journal_init(&j);
        snap(&d, g_before);
        ASSERT_TRUE(edit(&d, &j) == WP_OK);
        snap(&d, g_after);
        if (strcmp(g_before, g_after) == 0) {
            test_fail_msg(name, "the edit changed nothing; the walk would prove nothing");
        }
        wp_undo_journal_free(&j);
        wp_document_free(&d);
    }

    for (n = 0; n < 400 && !done && !g_walk_failed; n++) {
        wp_document d;
        wp_undo_journal j;
        wp_status st;
        u32 live;

        live = mem_host_live_count();
        ASSERT_TRUE(load(&d));
        wp_undo_journal_init(&j);
        if (prepare) ASSERT_TRUE(prepare(&d, &j) == WP_OK);
        wp_undo_journal_free(&j);
        wp_undo_journal_init(&j);

        /* the edit, failing at allocation n */
        g_fail_n = n;
        mem_host_fail_after(n);
        st = edit(&d, &j);
        mem_host_fail_after(-1);
        snap(&d, g_now);
        if (st == WP_OK) {
            if (strcmp(g_now, g_after) != 0) test_fail_msg(name, "WP_OK, but not the edited document");
            done = WP_TRUE;
        } else if (strcmp(g_now, g_before) != 0) {
            if (!compound) {
                test_fail_msg(name, "failed part-way: neither before nor after");
            } else {
                ASSERT_TRUE(undo_all(&d, &j));
                snap(&d, g_now);
                if (strcmp(g_now, g_before) != 0) test_fail_msg(name, "failed part-way and Undo did not bring it back");
            }
        }

        /* Undo, then Redo, each failing at allocation n */
        if (st == WP_OK) {
            i32 m;
            for (m = 0; m < 400; m++) {
                wp_status us;
                g_fail_n = m;
                mem_host_fail_after(m);
                us = wp_edit_undo(&d, &j);
                mem_host_fail_after(-1);
                snap(&d, g_now);
                if (us == WP_OK) {
                    if (strcmp(g_now, g_before) != 0) test_fail_msg(name, "Undo said WP_OK but did not restore");
                    break;
                }
                if (strcmp(g_now, g_after) != 0) {
                    test_fail_msg(name, "Undo failed part-way");
                    break;
                }
            }
            for (m = 0; m < 400; m++) {
                wp_status rs;
                g_fail_n = m;
                mem_host_fail_after(m);
                rs = wp_edit_redo(&d, &j);
                mem_host_fail_after(-1);
                snap(&d, g_now);
                if (rs == WP_OK) {
                    if (strcmp(g_now, g_after) != 0) test_fail_msg(name, "Redo said WP_OK but did not redo");
                    break;
                }
                if (strcmp(g_now, g_before) != 0) {
                    test_fail_msg(name, "Redo failed part-way");
                    break;
                }
            }
        }

        wp_undo_journal_free(&j);
        wp_document_free(&d);
        if (mem_host_live_count() != live) test_fail_msg(name, "leaked");
    }
    if (!done) test_fail_msg(name, "never succeeded within 400 allocations");
    if (!g_walk_failed) printf("  oom walk: %s, %ld allocations\n", name, (long)(n - 1));
}

static wp_status make_empty_para(wp_document *d, wp_undo_journal *j) { return e_insert_empty(d, j); }

void test_oom_walk_every_edit_undo_and_redo(void)
{
    walk_edit("insert_text", e_insert, NULL, WP_FALSE);
    walk_edit("delete_range", e_delete, NULL, WP_FALSE);
    walk_edit("split_paragraph", e_split, NULL, WP_FALSE);
    walk_edit("merge_paragraph", e_merge, NULL, WP_FALSE);
    walk_edit("split_section", e_split_section, NULL, WP_FALSE);
    walk_edit("merge_section", e_merge_section, NULL, WP_FALSE);
    walk_edit("set_section", e_set_section, NULL, WP_FALSE);
    walk_edit("set_pstyle", e_set_pstyle, NULL, WP_FALSE);
    walk_edit("set_table", e_set_table, NULL, WP_FALSE);
    walk_edit("insert_empty_paragraph", e_insert_empty, NULL, WP_FALSE);
    walk_edit("remove_empty_paragraph", e_remove_empty, make_empty_para, WP_FALSE);
    walk_edit("insert_image", e_image, NULL, WP_FALSE);
    walk_edit("delete_selection", e_delete_selection, NULL, WP_TRUE);
    walk_edit("paste_text", e_paste, NULL, WP_TRUE);
    walk_edit("style_range", e_style, NULL, WP_TRUE);
}
