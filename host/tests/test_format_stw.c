/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_stw.h"
#include "strpool.h"
#include "drawing.h"
#include "format_txt.h"
#include "app_ctx.h"
#include "actions.h"

/* Atari Works .STW (docs/formats/atariworks.md), against documents built
   here byte by byte from the map -- the specimen corpus is Atari's and
   stays out of the repository; `make stw-score` checks the importer
   against it where it is installed. */

static u8 g_f[4096];
static u32 g_n;

static void put8(u8 v) { g_f[g_n++] = v; }
static void put16(u16 v) { put8((u8)(v >> 8)); put8((u8)v); }
static void put32(u32 v) { put16((u16)(v >> 16)); put16((u16)v); }
static void putz(u32 n) { while (n-- > 0) put8(0); }
static void puts_(const char *s) { while (*s) put8((u8)*s++); }
static void put_name(const char *s, u32 room) { u32 n = (u32)strlen(s); puts_(s); putz(room - n); }

/* A paragraph record: line index, tabs, fields, then the fixed part. */
static void record(u16 line, const u16 *tabs, u32 nt, const char *field, u16 left, u16 first,
                   u16 right, u16 next_tabs, u16 fmt, u16 font, u16 size, u16 effects,
                   u16 len, u16 next_fields)
{
    u32 k;
    put16(line);
    for (k = 0; k < nt; k++) put16(tabs[k]);
    if (field != NULL) {   /* 8 bytes, the database, the field */
        putz(8);
        put_name("LABELS.STD", 14);
        put_name(field, 32);
    }
    put16(left); put16(first); put16(right); put16(next_tabs); put16(fmt);
    put16(font); put16(size); put16(effects); putz(4);
    put16(len); put16(next_fields); putz(6);
}

/* Version 11: an 8.5 x 11 page, margins 1, 1, 0.75, 0.5 inch; three
   paragraphs (justified with a centre tab; centred, bold by its
   record, with a merge field; left, with an underlined word); a GEM
   picture the metafile reader refuses (too short) and a rule between
   the records and the body; two fonts. */
static void build_v11(void)
{
    static const u8 d8_5[8] = { 0x40, 0x21, 0, 0, 0, 0, 0, 0 };
    static const u8 d11[8] = { 0x40, 0x26, 0, 0, 0, 0, 0, 0 };
    static const u8 d1[8] = { 0x3F, 0xF0, 0, 0, 0, 0, 0, 0 };
    static const u8 d075[8] = { 0x3F, 0xE8, 0, 0, 0, 0, 0, 0 };
    static const u8 d05[8] = { 0x3F, 0xE0, 0, 0, 0, 0, 0, 0 };
    static const u16 tab_c[1] = { 0x8000u | 320u };   /* centre, 320 units = 5040 twips */
    const u8 *reals[6];
    u32 i, k;

    reals[0] = d8_5; reals[1] = d11; reals[2] = d1; reals[3] = d1; reals[4] = d075; reals[5] = d05;
    g_n = 0;
    memset(g_f, 0, sizeof(g_f));
    puts_("11");
    putz(0x22 - 2);
    for (i = 0; i < 6; i++) for (k = 0; k < 8; k++) put8(reals[i][k]);
    put_name("Price List#RPage #P", 0xC8 - 0x52);     /* the header, then its face */
    put_name("#C- #p -#R#D", 0x13E - 0xC8);           /* the footer, then its face */
    put16(26); put16(0x0802); put16(1); put16(1);
    put16(3);    /* paragraphs */
    put16(3);    /* lines */
    put16(2);    /* objects */
    putz(0x170 - g_n);
    for (i = 0; i < 3; i++) {   /* the line table: 19 units a line, a page at the first */
        put16(0); put16(i == 0 ? 1 : 0); putz(6); put16(19); put16(4); put16(0);
    }
    record(0, NULL, 0, NULL, 0, 0, 0, 1, 0x1800, 0x14AA, 12, 0, 20, 0);
    record(1, tab_c, 1, NULL, 0, 0, 0, 0, 0x1400, 0x14AA, 14, 1, 10, 1);
    record(2, NULL, 0, "Company", 32, 64, 549, 0, 0x2100, 0x14AA, 12, 0, 12, 0);
    put16(3);       /* the line count */
    /* a GEM picture: header, 32-bit length, the metafile */
    put16(0); put8(5); put8(0); putz(176 - 4);
    put32(6);
    put16(0xFFFF); put16(0x1800); put16(0xFFFF);
    /* a rule (kind 1, 2 units tall): x 10, y 24 -- 5 units below the
       first paragraph's line -- 100 wide; the header alone */
    put16(0); put8(1); put8(1); put16(10); put16(0); put16(24); put16(100); put16(2);
    putz(176 - 14);
    /* the body */
    puts_("Caf"); put8(0x82); puts_("\tna"); put8(0x8B); puts_("ve x"); put8(0x0A);
    puts_("Dear "); put8(0x01);
    put8(0); put16(0x14AC); put16(14); put16(8); put8(0);   /* runs to the paragraph's end */
    puts_("!"); put8(0x0A);
    puts_("see ");
    put8(0); put16(0x14AA); put16(12); put16(8); put8(0);
    puts_("this");
    put8(0); put16(0x14AA); put16(12); put16(0); put8(0);
    puts_(" now"); put8(0x0A);
    put8(0);   /* the body's end */
    /* the font table */
    put8(0); put16(0x14AA); put_name("Humanist 521", 33);
    put8(0); put16(0x14AC); put_name("Humanist 521 Bold Italic", 33);
}

/* The version-11 document above, for host/tools/fuzz_formats.c to
   start from: its length, or 0 if `cap` is too small. */
u32 test_stw_seed(u8 *out, u32 cap)
{
    build_v11();
    if (g_n > cap) return 0;
    memcpy(out, g_f, g_n);
    return g_n;
}

static const wp_paragraph *para_at(wp_document *doc, u32 i)
{
    return wp_section_paragraph_get(wp_document_section_get(doc, 0), i);
}

static void run_text(wp_document *doc, u32 pi, u16 ri, char *out, u32 cap)
{
    wp_run *run = wp_paragraph_run_get((wp_paragraph *)para_at(doc, pi), ri);
    u32 n = 0;
    if (run != NULL) {
        n = run->text.length < cap - 1 ? run->text.length : cap - 1;
        memcpy(out, wp_strpool_get(&doc->strings, run->text), n);
    }
    out[n] = '\0';
}

static const wp_char_style *run_style(wp_document *doc, u32 pi, u16 ri)
{
    wp_run *run = wp_paragraph_run_get((wp_paragraph *)para_at(doc, pi), ri);
    return run != NULL ? wp_document_cstyle_get(doc, run->style) : NULL;
}

void test_format_stw_reads_text_styles_and_page(void)
{
    wp_document doc;
    const wp_section *sec;
    const wp_para_style *ps;
    const wp_char_style *cs;
    const wp_font_desc *fd;
    char t[64];

    build_v11();
    ASSERT_TRUE(format_stw_looks_like(g_f, g_n));
    ASSERT_TRUE(format_stw_import(g_f, g_n, &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 4);   /* the rule's picture paragraph after the first */
    ASSERT_EQ_U32((u32)sec->page_width, 12240);
    ASSERT_EQ_U32((u32)sec->page_height, 15840);
    ASSERT_EQ_U32((u32)sec->margin_left, 1440);
    ASSERT_EQ_U32((u32)sec->margin_top, 1080);
    ASSERT_EQ_U32((u32)sec->margin_bottom, 720);
    /* Header and footer in RetroWP's own form: parts between '|', the
       page number '#'; the date has nothing to stand for and goes. */
    ASSERT_TRUE(sec->header.length == 17 &&
                memcmp(wp_strpool_get(&doc.strings, sec->header), "Price List|Page #", 17) == 0);
    ASSERT_TRUE(sec->footer.length == 7 &&
                memcmp(wp_strpool_get(&doc.strings, sec->footer), "|- # -|", 7) == 0);

    /* The Atari set's e-acute and i-diaeresis, as UTF-8; justified. */
    run_text(&doc, 0, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Caf\xc3\xa9\tna\xc3\xafve x");
    ps = wp_document_pstyle_get(&doc, para_at(&doc, 0)->style);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_JUSTIFY);
    ASSERT_EQ_U32(ps->tab_count, 0);

    /* The rule: a drawing in a frame after the first paragraph, 5 units
       (79 twips) below it, 10 (158) from the margin, 100 (1575) wide. */
    {
        const wp_paragraph *pic = para_at(&doc, 1);
        const wp_frame_desc *fr = wp_document_frame_get(&doc, pic->frame);
        const wp_image_desc *img = wp_document_image_get(&doc, pic->image);
        ASSERT_EQ_U32(pic->kind, WP_PARA_IMAGE);
        ASSERT_TRUE(fr != NULL && img != NULL);
        if (fr != NULL) {
            ASSERT_EQ_U32(fr->yref, WP_FRAME_Y_PARA);
            ASSERT_EQ_U32((u32)fr->y, 79);
            ASSERT_EQ_U32((u32)fr->x, 158);
            ASSERT_EQ_U32((u32)fr->w, 1575);
        }
        if (img != NULL) {
            ASSERT_TRUE(WP_IMAGE_IS_DRAWING(img));
            ASSERT_EQ_U32((u32)img->display_width_twips, 1575);
        }
    }

    /* Centred, its tab carried from the record before; bold by its
       record's effects, 14 pt; the merge field as Atari Works' text
       export writes it; then "!" in the face whose name says bold
       italic, underlined. */
    run_text(&doc, 2, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Dear \xc2\xab" "Company\xc2\xbb");
    cs = run_style(&doc, 2, 1);
    ASSERT_TRUE(cs != NULL && cs->flags == (WP_STYLE_BOLD | WP_STYLE_ITALIC | WP_STYLE_UNDERLINE));
    ps = wp_document_pstyle_get(&doc, para_at(&doc, 2)->style);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_CENTER);
    ASSERT_EQ_U32(ps->tab_count, 1);
    ASSERT_EQ_U32((u32)ps->tab_pos[0], 5040);
    ASSERT_EQ_U32(ps->tab_kind[0], WP_TAB_CENTER);
    cs = run_style(&doc, 2, 0);
    ASSERT_TRUE(cs != NULL && cs->flags == WP_STYLE_BOLD);
    ASSERT_EQ_U32((u32)cs->size_twips, 280);
    fd = wp_document_font_get(&doc, cs->font);
    ASSERT_TRUE(fd != NULL && fd->name.length == 12);

    /* Left, one and a half spaced, indented; "this" underlined, and the
       paragraph back in its record's style, not the one before. */
    ps = wp_document_pstyle_get(&doc, para_at(&doc, 3)->style);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_LEFT);
    ASSERT_EQ_U32((u32)ps->line_spacing, 360);
    ASSERT_EQ_U32((u32)ps->indent_left, 504);
    ASSERT_EQ_U32((u32)ps->indent_first, 504);
    run_text(&doc, 3, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "see ");
    cs = run_style(&doc, 3, 0);
    ASSERT_TRUE(cs != NULL && cs->flags == 0);
    run_text(&doc, 3, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "this");
    cs = run_style(&doc, 3, 1);
    ASSERT_TRUE(cs != NULL && cs->flags == WP_STYLE_UNDERLINE);
    run_text(&doc, 3, 2, t, sizeof(t));
    ASSERT_EQ_STR(t, " now");
    wp_document_free(&doc);
}

/* Version 10 keeps the page in singles of four times the inches. */
void test_format_stw_reads_version_10_page(void)
{
    static const u8 s34[4] = { 0x42, 0x08, 0, 0 };   /* 34.0 = 8.5 in */
    static const u8 s44[4] = { 0x42, 0x30, 0, 0 };   /* 44.0 = 11 in */
    static const u8 s4[4] = { 0x40, 0x80, 0, 0 };    /* 4.0 = 1 in */
    wp_document doc;
    const wp_section *sec;
    u32 k;

    build_v11();
    g_f[1] = '0';
    for (k = 0; k < 48; k++) g_f[0x22 + k] = 0;
    for (k = 0; k < 4; k++) {
        g_f[0x22 + k] = s34[k];
        g_f[0x2A + k] = s44[k];
        g_f[0x32 + k] = g_f[0x3A + k] = g_f[0x42 + k] = g_f[0x4A + k] = s4[k];
    }
    ASSERT_TRUE(format_stw_import(g_f, g_n, &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32((u32)sec->page_width, 12240);
    ASSERT_EQ_U32((u32)sec->page_height, 15840);
    ASSERT_EQ_U32((u32)sec->margin_right, 1440);
    wp_document_free(&doc);
}

/* Not Atari Works, or cut short: refused, never half-read. */
void test_format_stw_refuses_what_is_not(void)
{
    static const u8 text[] = "11 is a number, and this is a text file that starts with one.";
    wp_document doc;
    u32 cut;

    ASSERT_TRUE(!format_stw_looks_like(text, (u32)sizeof(text) - 1));
    ASSERT_TRUE(format_stw_import(text, (u32)sizeof(text) - 1, &doc) != WP_OK);
    build_v11();
    for (cut = 0x100; cut < g_n - 80; cut += 37) {
        ASSERT_TRUE(format_stw_import(g_f, cut, &doc) != WP_OK);
    }
}

/* The sample above, for tests elsewhere (test_actions.c's Open). */
const u8 *test_stw_sample(u32 *len)
{
    build_v11();
    *len = g_n;
    return g_f;
}

/* Merge fields named in Atari ST characters that take three bytes in
   UTF-8: 64 of them, each 32 characters of U+2261, every one used. The
   scratch the run is built in once allowed 48 bytes a field; each needs
   100 (the guillemets, two bytes each, and 32 x 3). */
void test_format_stw_long_wide_field_names_fit(void)
{
    static u8 f[0x170 + 64 + 64 * 54 + 400];
    wp_document doc;
    u32 p, at, k, total = 0, ri;
    const wp_paragraph *para;

    memset(f, 0, sizeof(f));
    f[0] = '1'; f[1] = '1';
    f[0x13E + 9] = 1;
    p = 0x170; at = p + 2;                         /* record 0 */
    f[at + 23] = 64; f[at + 13] = 12;
    p = at + 30; at = p + 2;
    for (k = 0; k < 64; k++) memset(f + at + 54 * k + 22, 0xF0, 32);
    p = at + 54 * 64;
    for (k = 0; k < 64; k++) f[p + k] = 0x01;
    p += 64;
    memcpy(f + p + 4, "Arial", 5);
    ASSERT_TRUE(format_stw_import(f, p + 40, &doc) == WP_OK);
    para = para_at(&doc, 0);
    ASSERT_TRUE(para != NULL);
    for (ri = 0; para != NULL && ri < para->run_count; ri++) {
        total += wp_paragraph_run_get((wp_paragraph *)para, (u16)ri)->text.length;
    }
    ASSERT_EQ_U32(total, 64u * 100u);
    wp_document_free(&doc);
}

/* An Atari Works file whose path is too long to remember: it opens,
   untitled (the next Save asks), and the .STW -> .RTF rename does not
   reach before doc_path, which it did when doc_path was "". */
void test_format_stw_opens_from_a_path_too_long_to_keep(void)
{
    char path[400];
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    plat_file *f;
    u32 n;

    build_v11();
    strcpy(path, "build/");
    for (n = 0; n < 140; n++) strcat(path, "./");
    strcat(path, "LONG.STW");
    ASSERT_TRUE(strlen(path) >= WP_APP_PATH_MAX);
    ASSERT_TRUE(plat_file_open(path, PLAT_FILE_WRITE, &f) == WP_OK);
    ASSERT_TRUE(plat_file_write(f, g_f, g_n) == WP_OK);
    plat_file_close(f);

    ASSERT_TRUE(format_txt_import((const u8 *)"x\n", 2, &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.doc_loaded = WP_TRUE;
    wp_action_remember_path(&ctx, "A:\\X.STW");
    ASSERT_TRUE(wp_action_load_file(&ctx, path, NULL) == WP_OK);
    ASSERT_EQ_STR(ctx.doc_path, "");
    ASSERT_TRUE(ctx.doc_rtf);
    (void)plat_file_delete(path);
    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(ctx.doc);
}
