/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_ws.h"
#include "format_txt.h"

/* Fixtures are built here as byte arrays rather than checked in as
   files, because the interesting parts of WordStar document mode are
   precisely the bytes an editor would mangle: 0x8D soft returns, the
   high bit set on wrapped words, and raw 0x02/0x13/0x19 style toggles.
   A .ws fixture in the repo would survive exactly until someone's editor
   normalised the line endings.
 *
 * HONEST LIMITATION, worth stating where it will be read: these encode
 * the format as DOCUMENTED, so they test the parser against the model,
 * not the model against reality. Only a genuine WordStar file validates
 * the model itself -- see docs/formats/wordstar.md. */

static void dump_para(wp_document *doc, u32 index, char *out)
{
    wp_section *sec = wp_document_section_get(doc, 0);
    wp_paragraph *para = wp_section_paragraph_get(sec, index);
    u16 i;
    u32 pos = 0;

    for (i = 0; i < para->run_count; i++) {
        const u8 *b = wp_strpool_get(&doc->strings, para->runs[i].text);
        memcpy(out + pos, b, para->runs[i].text.length);
        pos += para->runs[i].text.length;
    }
    out[pos] = '\0';
}

/* THE central behaviour. A soft return is where WordStar wrapped a line
   on a screen that no longer exists; a hard return is where the writer
   pressed Return. Treat them alike and a novel imports as thousands of
   one-line paragraphs broken to a 1980s margin, which can never be
   re-wrapped correctly because the real breaks are no longer
   distinguishable. */
void test_ws_soft_return_joins_hard_return_splits(void)
{
    static const u8 ws[] = {
        'T','h','e',' ','q','u','i','c','k',
        0x8D, 0x0A,                     /* soft: wrapped, same paragraph */
        'b','r','o','w','n',' ','f','o','x',
        0x0D, 0x0A,                     /* hard: the writer's own break */
        'N','e','x','t',' ','o','n','e',
        0x0D, 0x0A
    };
    wp_document doc;
    wp_section *sec;
    char buf[128];

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    dump_para(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "The quick brown fox");
    dump_para(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "Next one");

    wp_document_free(&doc);
}

/* WordStar consumed the space at a wrap point, so the reader puts one
   back -- but exactly one. Doubling it would put a visible extra space
   into every re-wrapped line of an imported manuscript. */
void test_ws_soft_return_does_not_double_space(void)
{
    static const u8 ws[] = {
        'a','l','p','h','a',' ',        /* space already present */
        0x8D, 0x0A,
        'b','e','t','a',
        0x0D, 0x0A
    };
    wp_document doc;
    char buf[64];

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    dump_para(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "alpha beta");
    wp_document_free(&doc);
}

/* Bit 7 is a flag, not part of the character. */
void test_ws_high_bit_is_stripped(void)
{
    static const u8 ws[] = {
        'w','o','r','d' | 0x80u,        /* flagged last character */
        (u8)(' ' | 0x80u),              /* soft space */
        'h','e','r','e',
        0x0D, 0x0A
    };
    wp_document doc;
    char buf[64];

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    dump_para(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "word here");
    wp_document_free(&doc);
}

/* Style toggles must produce real runs carrying real style flags, not
   just be stripped -- importing a manuscript and losing every emphasis
   in it is a silent, unrecoverable edit to someone's work. */
void test_ws_style_toggles_produce_styled_runs(void)
{
    static const u8 ws[] = {
        'p','l','a','i','n',' ',
        0x02, 'b','o','l','d', 0x02,
        ' ','p','l','a','i','n',
        0x0D, 0x0A
    };
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    const wp_char_style *cs;
    char buf[64];
    wp_bool saw_bold = WP_FALSE;
    u16 i;

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    dump_para(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "plain bold plain");

    sec = wp_document_section_get(&doc, 0);
    para = wp_section_paragraph_get(sec, 0);
    ASSERT_TRUE(para->run_count >= 3);   /* plain | bold | plain */

    for (i = 0; i < para->run_count; i++) {
        cs = wp_document_cstyle_get(&doc, para->runs[i].style);
        if (cs != NULL && (cs->flags & WP_STYLE_BOLD) != 0) {
            const u8 *b = wp_strpool_get(&doc.strings, para->runs[i].text);
            ASSERT_EQ_U32(para->runs[i].text.length, 4u);
            ASSERT_TRUE(memcmp(b, "bold", 4) == 0);
            saw_bold = WP_TRUE;
        }
    }
    ASSERT_TRUE(saw_bold);
    wp_document_free(&doc);
}

/* Dot commands are directives, consumed and dropped -- but ONLY real
   ones. Prose beginning with an ellipsis or a decimal is text, and
   eating it would delete a line of someone's writing. */
void test_ws_dot_commands_dropped_but_prose_kept(void)
{
    static const u8 ws[] = {
        '.','p','a', 0x0D, 0x0A,                    /* real command */
        'B','o','d','y', 0x0D, 0x0A,
        '.','.','.','a','n','d', 0x0D, 0x0A,        /* ellipsis: text */
        '.','2','2',' ','c','a','l', 0x0D, 0x0A     /* decimal: text */
    };
    wp_document doc;
    wp_section *sec;
    char buf[64];

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 4);   /* the dropped command leaves its own break */

    dump_para(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "Body");
    dump_para(&doc, 2, buf);
    ASSERT_EQ_STR(buf, "...and");
    dump_para(&doc, 3, buf);
    ASSERT_EQ_STR(buf, ".22 cal");

    wp_document_free(&doc);
}

/* Blank lines are how a manuscript is spaced. Swallowing them would
   reformat someone's work on the way in. */
void test_ws_blank_lines_survive(void)
{
    static const u8 ws[] = {
        'o','n','e', 0x0D, 0x0A,
        0x0D, 0x0A,
        't','w','o', 0x0D, 0x0A
    };
    wp_document doc;
    wp_section *sec;
    char buf[64];

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 3);
    dump_para(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "");
    wp_document_free(&doc);
}

/* Sniffing must never claim a plain ASCII file. WordStar's own
   non-document mode produced exactly that, and the plain-text importer
   already handles it correctly -- a false positive would route real
   plain text through a parser that strips its control bytes. */
void test_ws_sniff_rejects_plain_text(void)
{
    static const char plain[] = "Just an ordinary file.\r\nSecond line.\r\n";
    static const char ellipsis[] = "...trailing thought\r\nmore text\r\n";
    static const u8 soft[] = { 'a', 0x8D, 0x0A, 'b' };
    static const char dotcmd[] = ".pa\r\nBody text\r\n";

    ASSERT_TRUE(format_ws_looks_like((const u8 *)plain, (u32)(sizeof(plain) - 1)) == WP_FALSE);
    ASSERT_TRUE(format_ws_looks_like((const u8 *)ellipsis, (u32)(sizeof(ellipsis) - 1)) == WP_FALSE);
    ASSERT_TRUE(format_ws_looks_like(soft, (u32)sizeof(soft)) == WP_TRUE);
    ASSERT_TRUE(format_ws_looks_like((const u8 *)dotcmd, (u32)(sizeof(dotcmd) - 1)) == WP_TRUE);
    ASSERT_TRUE(format_ws_looks_like((const u8 *)"", 0) == WP_FALSE);
}

/* Degrade, never refuse. A manuscript that opens imperfectly is worth
   immeasurably more than one that will not open, so even bytes that make
   no sense as WordStar must still yield a document. */
void test_ws_garbage_still_imports(void)
{
    static const u8 junk[] = { 0x01, 0x8D, 0xFF, 0x00, 0x7F, 0x02, 0x1B, 0x0D, 0x0A };
    wp_document doc;

    ASSERT_TRUE(format_ws_import(junk, (u32)sizeof(junk), &doc) == WP_OK);
    wp_document_free(&doc);

    ASSERT_TRUE(format_ws_import((const u8 *)"", 0, &doc) == WP_OK);
    wp_document_free(&doc);
}

/* An imported document is not "modified" -- it is exactly the file it
   came from. This is why importers use the raw structural functions and
   never the journaled wp_edit_* family (see undo.c's commit_edit).
   Someone opening a manuscript must not be told they have unsaved
   changes before they have typed anything. */
void test_ws_import_is_not_modified(void)
{
    static const u8 ws[] = { 'h','i', 0x0D, 0x0A };
    wp_document doc;

    ASSERT_TRUE(format_ws_import(ws, (u32)sizeof(ws), &doc) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);
    wp_document_free(&doc);
}
