/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_rtf.h"
#include "layout.h"

/* Typefaces: the document's font table, RTF's \fonttbl/\fN/\deff/
   \fcharset in and out, and the seam's family bits and face matcher
   (retroplat's fontpick.c, which every face-listing backend uses). */

static wp_status import(const char *rtf, wp_document *doc)
{
    return format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), doc);
}

static const wp_run *run_at(wp_document *doc, u32 pi, u16 ri)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(doc, 0), pi);
    return (p != NULL && ri < p->run_count) ? &p->runs[ri] : NULL;
}

/* The font name of a run, NUL-terminated; "" for the target default. */
static void run_font(wp_document *doc, u32 pi, u16 ri, char *out, u32 cap, u8 *family)
{
    const wp_run *r = run_at(doc, pi, ri);
    const wp_char_style *cs = r ? wp_document_cstyle_get(doc, r->style) : NULL;
    const wp_font_desc *fd = cs ? wp_document_font_get(doc, cs->font) : NULL;
    u32 n = 0;

    if (fd != NULL) {
        n = fd->name.length < cap - 1 ? fd->name.length : cap - 1;
        memcpy(out, wp_strpool_get(&doc->strings, fd->name), n);
    }
    out[n] = '\0';
    if (family != NULL) *family = fd ? fd->family : 0;
}

static void run_text(wp_document *doc, u32 pi, u16 ri, char *out, u32 cap)
{
    const wp_run *r = run_at(doc, pi, ri);
    u32 n = 0;

    if (r != NULL) {
        n = r->text.length < cap - 1 ? r->text.length : cap - 1;
        memcpy(out, wp_strpool_get(&doc->strings, r->text), n);
    }
    out[n] = '\0';
}

void test_fonts_table_finds_by_name_ignoring_case(void)
{
    wp_document doc;
    wp_font a, b, c, none;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"Times", 5, WP_FAMILY_NIL, &a) == WP_OK);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"TIMES ", 6, WP_FAMILY_ROMAN, &b) == WP_OK);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"Courier", 7, WP_FAMILY_MODERN, &c) == WP_OK);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"", 0, WP_FAMILY_SWISS, &none) == WP_OK);
    ASSERT_EQ_U32(a, 1);
    ASSERT_EQ_U32(b, 1);          /* the same face, its trailing space and case aside */
    ASSERT_EQ_U32(c, 2);
    ASSERT_EQ_U32(none, 0);       /* no name: the target default, nothing added */
    ASSERT_EQ_U32(doc.font_count, 3);
    /* The family a later add knew fills in the one the first did not. */
    ASSERT_EQ_U32(wp_document_font_get(&doc, a)->family, WP_FAMILY_ROMAN);
    ASSERT_TRUE(wp_document_font_get(&doc, 0) == NULL);
    wp_document_free(&doc);
}

void test_fonts_family_guess_from_a_bare_name(void)
{
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"Times New Roman", 15), WP_FAMILY_ROMAN);
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"Courier New", 11), WP_FAMILY_MODERN);
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"Microsoft Sans Serif", 20), WP_FAMILY_SWISS);
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"Geneva", 6), WP_FAMILY_SWISS);
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"Monaco", 6), WP_FAMILY_MODERN);
    ASSERT_EQ_U32(wp_font_family_guess((const u8 *)"", 0), WP_FAMILY_NIL);
}

void test_fonts_rtf_word_font_table_and_charsets(void)
{
    wp_document doc;
    char t[64];
    u8 fam;

    /* Word's shape: entries in groups, a \panose and an \falt inside one,
       \deff naming the body font, and a Central European entry whose
       \'hh bytes are cp1250's, not the document's 1252. */
    ASSERT_TRUE(import("{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl"
                       "{\\f0\\froman\\fcharset0\\fprq2{\\*\\panose 02020603050405020304}Times New Roman{\\*\\falt Times};}"
                       "{\\f1\\fswiss\\fcharset0 Arial;}"
                       "{\\f2\\fmodern\\fcharset238 Courier New CE;}}"
                       "{\\*\\generator Microsoft Word 11;}"
                       "\\pard\\plain\\f0 Hello {\\f1 world}{\\f2 \\'b9}\\par}", &doc) == WP_OK);
    run_font(&doc, 0, 0, t, sizeof(t), &fam);
    ASSERT_EQ_STR(t, "Times New Roman");
    ASSERT_EQ_U32(fam, WP_FAMILY_ROMAN);
    run_font(&doc, 0, 1, t, sizeof(t), &fam);
    ASSERT_EQ_STR(t, "Arial");
    ASSERT_EQ_U32(fam, WP_FAMILY_SWISS);
    run_font(&doc, 0, 2, t, sizeof(t), &fam);
    ASSERT_EQ_STR(t, "Courier New CE");
    ASSERT_EQ_U32(fam, WP_FAMILY_MODERN);
    run_text(&doc, 0, 2, t, sizeof(t));
    ASSERT_EQ_STR(t, "\xc4\x85");          /* U+0105, cp1250's 0xB9; 1252's would be U+00B9 */
    ASSERT_EQ_U32(doc.font_count, 4);       /* three faces and the sentinel */
    wp_document_free(&doc);
}

void test_fonts_rtf_default_font_and_plain(void)
{
    wp_document doc;
    char t[64];

    /* Ungrouped entries, a body with no \f at all (the \deff face), and
       \plain going back to it after a change. */
    ASSERT_TRUE(import("{\\rtf1\\ansi\\deff1{\\fonttbl\\f0\\fswiss Arial;\\f1\\froman Georgia;}"
                       "a{\\f0 b}\\f0 c\\plain d\\par}", &doc) == WP_OK);
    run_font(&doc, 0, 0, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Georgia");
    run_font(&doc, 0, 1, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Arial");
    run_font(&doc, 0, 2, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Arial");
    run_font(&doc, 0, 3, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Georgia");
    wp_document_free(&doc);

    /* A foreign file's Helvetica is Helvetica -- only this program's own
       \f0 placeholder is the target default. */
    ASSERT_TRUE(import("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}x\\par}", &doc) == WP_OK);
    run_font(&doc, 0, 0, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Helvetica");
    wp_document_free(&doc);
}

static void add_run(wp_document *doc, wp_paragraph *para, const char *text, wp_font font)
{
    wp_span span;
    wp_char_style cs;
    wp_cstyle h;
    u16 ri;

    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) == WP_OK);
    memset(&cs, 0, sizeof(cs));
    cs.size_twips = 240;
    cs.font = font;
    ASSERT_TRUE(wp_document_cstyle_find_or_add(doc, &cs, &h) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, h, &ri) == WP_OK);
}

void test_fonts_rtf_round_trip_keeps_faces_and_the_default(void)
{
    wp_document doc, back;
    wp_section *sec;
    wp_paragraph *para;
    u32 si, pi, len = 0;
    wp_font courier, unused;
    static u8 out[4096];
    char t[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &si) == WP_OK);
    sec = wp_document_section_get(&doc, si);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &pi) == WP_OK);
    para = wp_section_paragraph_get(sec, pi);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"Unused", 6, WP_FAMILY_DECOR, &unused) == WP_OK);
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"Courier", 7, WP_FAMILY_MODERN, &courier) == WP_OK);
    add_run(&doc, para, "plain ", 0);
    add_run(&doc, para, "code", courier);

    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *)out, "{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f2\\fmodern Courier;}}") != NULL);
    ASSERT_TRUE(strstr((const char *)out, "Unused") == NULL);     /* only faces a run names */
    ASSERT_TRUE(strstr((const char *)out, "{\\*\\generator RetroWP;}") != NULL);
    ASSERT_TRUE(strstr((const char *)out, "{\\f2\\fs24 code}") != NULL);

    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    run_font(&back, 0, 0, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "");   /* still the default */
    run_font(&back, 0, 1, t, sizeof(t), NULL); ASSERT_EQ_STR(t, "Courier");
    ASSERT_EQ_U32(back.font_count, 2);     /* the placeholder never became a font */
    wp_document_free(&back);
    wp_document_free(&doc);
}

static const char *const g_faces[] = {
    "6x6 system font", "Swiss 721 Bold", "Swiss 721", "Dutch 801", "Courier", "Symbol"
};

/* fVDI's TrueType list under AFROS, in its own order: a Sans Mono ahead
   of the Sans it must not stand in for. */
static const char *const g_fvdi_faces[] = {
    "DejaVu LGC Sans Mono", "DejaVu LGC Serif", "Liberation Sans", "Liberation Mono"
};

static const char *fvdi_at(u32 i, void *user)
{
    (void)user;
    return g_fvdi_faces[i];
}

static const char *face_at(u32 i, void *user)
{
    (void)user;
    return g_faces[i];
}

static i32 pick(const char *want, u16 flags)
{
    return plat_font_pick(face_at, NULL, 6, (const u8 *)want, (u32)strlen(want), flags);
}

void test_fonts_face_pick_by_name_then_family(void)
{
    u16 roman = (u16)(PLAT_FAMILY_ROMAN << PLAT_FAMILY_SHIFT);
    u16 swiss = (u16)(PLAT_FAMILY_SWISS << PLAT_FAMILY_SHIFT);
    u16 modern = (u16)(PLAT_FAMILY_MODERN << PLAT_FAMILY_SHIFT);

    ASSERT_TRUE(pick("courier", 0) == 4);                 /* same name, case aside */
    ASSERT_TRUE(pick("Swiss", 0) == 2);                   /* a prefix; the plain face over Bold */
    ASSERT_TRUE(pick("Swiss", PLAT_STYLE_BOLD) == 1);     /* ...unless bold was asked for */
    ASSERT_TRUE(pick("Courier New", modern) == 4);        /* the face is a prefix of the request */
    ASSERT_TRUE(pick("Times New Roman", roman) == 3);     /* no Times: the serif, Dutch */
    ASSERT_TRUE(pick("Arial", swiss) == 2);
    ASSERT_TRUE(pick("Arial", 0) == -1);                  /* no such face and no family: the default */
    ASSERT_TRUE(pick("Cour", 0) == -1);                   /* a prefix must end on a word */

    ASSERT_TRUE(plat_font_pick(fvdi_at, NULL, 4, (const u8 *)"Arial", 5, swiss) == 2);
    ASSERT_TRUE(plat_font_pick(fvdi_at, NULL, 4, (const u8 *)"Courier New", 11, modern) == 0);
    ASSERT_TRUE(plat_font_pick(fvdi_at, NULL, 4, (const u8 *)"Times New Roman", 15, roman) == 1);
}

/* The family the seam is handed is the document's, bit for bit. */
void test_fonts_family_bits_match_the_seam(void)
{
    ASSERT_EQ_U32(PLAT_FAMILY_ROMAN,  (unsigned)WP_FAMILY_ROMAN);
    ASSERT_EQ_U32(PLAT_FAMILY_SWISS,  (unsigned)WP_FAMILY_SWISS);
    ASSERT_EQ_U32(PLAT_FAMILY_MODERN, (unsigned)WP_FAMILY_MODERN);
    ASSERT_EQ_U32(PLAT_FAMILY_SCRIPT, (unsigned)WP_FAMILY_SCRIPT);
    ASSERT_EQ_U32(PLAT_FAMILY_DECOR,  (unsigned)WP_FAMILY_DECOR);
    /* Clear of every style bit, the document-only SUPER/SUB included. */
    ASSERT_EQ_U32(PLAT_FAMILY_MASK & 0xFFu, 0u);
}
