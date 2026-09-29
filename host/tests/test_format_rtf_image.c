/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "docmodel.h"
#include "editing.h"
#include "format_rtf.h"
#include "format_txt.h"
#include "mem_host.h"

/* Saving a document that has a picture in it.
 *
 * Before this, an image paragraph carried no runs, so the exporter
 * walked its empty run list and wrote an empty paragraph: placing an
 * image, saving, and reopening lost it with nothing said. The importer
 * discarded \pict to match. M15 built the model, the layout, the
 * painting and the gateway conversion, and no milestone task ever
 * covered persistence.
 *
 * The pixels live in a backend-written cache file that the engine may
 * not read (CLAUDE.md rule 1), so they arrive through a caller-supplied
 * wp_image_source -- which is what these tests stand in for. */

/* A 4x2 image, two bytes per row (4 pixels at 4bpp), so that padding to
   the DIB's 4-byte row boundary is visible in the output, as is the
   bottom-up row order. */
#define IMG_W 4
#define IMG_H 2
#define ROW_BYTES 2

static const u8 g_pixels[IMG_H * ROW_BYTES] = {
    0x01, 0x23,     /* row 0 (top) */
    0x45, 0x67      /* row 1 (bottom) */
};

static wp_status read_pixels(void *user, wp_span blob_path, u32 offset,
                              u8 *out, u32 cap, u32 *out_len)
{
    u32 avail;

    (void)user; (void)blob_path;
    if (offset >= sizeof(g_pixels)) { *out_len = 0; return WP_OK; }
    avail = (u32)sizeof(g_pixels) - offset;
    if (avail > cap) avail = cap;
    memcpy(out, g_pixels + offset, avail);
    *out_len = avail;
    return WP_OK;
}

static wp_status read_fails(void *user, wp_span blob_path, u32 offset,
                            u8 *out, u32 cap, u32 *out_len)
{
    (void)user; (void)blob_path; (void)offset; (void)out; (void)cap;
    *out_len = 0;
    return WP_ERR;   /* the cache file is gone */
}

static void build_image_doc(wp_document *doc)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_image_desc desc;
    wp_image handle;
    wp_span path;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    memset(&desc, 0, sizeof(desc));
    desc.width_px = IMG_W;
    desc.height_px = IMG_H;
    desc.display_width_twips = 960;
    desc.display_height_twips = 480;
    desc.palette_count = 2;
    desc.palette[0][0] = 0x11; desc.palette[0][1] = 0x22; desc.palette[0][2] = 0x33;
    desc.palette[1][0] = 0xAA; desc.palette[1][1] = 0xBB; desc.palette[1][2] = 0xCC;
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"C:\\WPIMG000.TMP", 15,
                                   &path) == WP_OK);
    desc.blob_path = path;
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"cat.png", 7,
                                   &desc.source_name) == WP_OK);

    ASSERT_TRUE(wp_document_image_add(doc, &desc, &handle) == WP_OK);
    para->kind = (u8)WP_PARA_IMAGE;
    para->image = handle;
}

/* void, not u32: the ASSERT_* macros return on failure, so they can only
   be used inside a function returning nothing. */
static void export_with(wp_document *doc, const wp_image_source *src,
                         u8 *buf, u32 cap)
{
    u32 measured = 0, written = 0;

    /* The two-call contract: measure, then write, and the two must agree
       exactly -- an image whose bytes are fetched at write time is the
       first thing that could ever have broken it. */
    ASSERT_EQ_U32(format_rtf_export_images(doc, NULL, 0, &measured, src), WP_RANGE);
    ASSERT_TRUE(measured > 0 && measured < cap);
    ASSERT_EQ_U32(format_rtf_export_images(doc, buf, cap, &written, src), WP_OK);
    ASSERT_EQ_U32(written, measured);
    buf[written] = '\0';
}

void test_rtf_image_exports_a_real_pict(void)
{
    wp_document doc;
    wp_image_source src;
    u8 buf[4096];

    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;

    export_with(&doc, &src, buf, (u32)sizeof(buf));

    /* A standard picture destination, so this is not a RetroWP-only
       document: any reader that knows \dibitmap can show it. */
    ASSERT_TRUE(strstr((char *)buf, "{\\pict\\dibitmap0") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\picw4") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\pich2") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\picwgoal960") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\pichgoal480") != NULL);

    wp_document_free(&doc);
}

void test_rtf_image_dib_header_is_well_formed(void)
{
    wp_document doc;
    wp_image_source src;
    u8 buf[4096];
    const char *hex;

    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;
    export_with(&doc, &src, buf, (u32)sizeof(buf));

    hex = strstr((char *)buf, "\\pichgoal480");
    ASSERT_TRUE(hex != NULL);
    hex = strstr(hex, "\r\n");
    ASSERT_TRUE(hex != NULL);
    hex += 2;

    /* BITMAPINFOHEADER, little-endian throughout -- a DIB is a foreign
       format and does not follow this project's big-endian rule.
       biSize 40, width 4, height 2 (positive: bottom-up), planes 1,
       bitcount 4, biClrUsed 2. */
    ASSERT_TRUE(strncmp(hex, "28000000", 8) == 0);          /* biSize */
    ASSERT_TRUE(strncmp(hex + 8, "04000000", 8) == 0);      /* biWidth */
    ASSERT_TRUE(strncmp(hex + 16, "02000000", 8) == 0);     /* biHeight */
    ASSERT_TRUE(strncmp(hex + 24, "0100", 4) == 0);         /* biPlanes */
    ASSERT_TRUE(strncmp(hex + 28, "0400", 4) == 0);         /* biBitCount */
    ASSERT_TRUE(strncmp(hex + 64, "02000000", 8) == 0);     /* biClrUsed */

    /* The palette follows, as RGBQUADs -- blue, green, red, zero. The
       model stores RGB, so the first entry 11/22/33 must come back
       33/22/11/00. */
    ASSERT_TRUE(strncmp(hex + 80, "33221100", 8) == 0);
    ASSERT_TRUE(strncmp(hex + 88, "ccbbaa00", 8) == 0);

    wp_document_free(&doc);
}

void test_rtf_image_rows_are_bottom_up_and_padded(void)
{
    wp_document doc;
    wp_image_source src;
    u8 buf[4096];
    const char *rows;

    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;
    export_with(&doc, &src, buf, (u32)sizeof(buf));

    rows = strstr((char *)buf, "ccbbaa00");     /* end of the palette */
    ASSERT_TRUE(rows != NULL);
    rows += 8;

    /* A positive biHeight means the LAST row is stored first, and every
       row is padded out to four bytes. So "4567" (the bottom row) then
       two zero bytes, then "0123" (the top row) and two more. */
    ASSERT_TRUE(strncmp(rows, "45670000", 8) == 0);
    ASSERT_TRUE(strncmp(rows + 8, "01230000", 8) == 0);

    wp_document_free(&doc);
}

void test_rtf_image_without_a_source_is_an_honest_placeholder(void)
{
    wp_document doc;
    u8 buf[4096];

    build_image_doc(&doc);
    export_with(&doc, NULL, buf, (u32)sizeof(buf));

    /* No way to reach the pixels, so say so in the document rather than
       writing the empty paragraph that used to lose it silently. */
    ASSERT_TRUE(strstr((char *)buf, "[image: cat.png 4x2]") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\pict") == NULL);

    wp_document_free(&doc);
}

void test_rtf_image_export_is_not_an_empty_paragraph(void)
{
    wp_document doc;
    u8 with_image[4096];
    u8 empty[4096];
    wp_document empty_doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_image_source src;
    u32 n_empty = 0;

    /* The regression this whole file exists for: an image paragraph and
       a genuinely empty one must not export to the same bytes. */
    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;
    export_with(&doc, &src, with_image, (u32)sizeof(with_image));

    ASSERT_TRUE(wp_document_init(&empty_doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&empty_doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&empty_doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    ASSERT_EQ_U32(format_rtf_export(&empty_doc, empty, (u32)sizeof(empty), &n_empty), WP_OK);
    empty[n_empty] = '\0';

    ASSERT_TRUE(strcmp((char *)with_image, (char *)empty) != 0);

    wp_document_free(&empty_doc);
    wp_document_free(&doc);
}

void test_rtf_image_missing_cache_file_still_writes_the_promised_size(void)
{
    wp_document doc;
    wp_image_source src;
    u8 buf[4096];
    u32 measured = 0, written = 0;

    build_image_doc(&doc);
    src.read = read_fails;
    src.user = NULL;

    /* The measure pass cannot know the read will fail -- it never calls
       the reader at all -- so a failing read must not change the size,
       or the buffer the caller allocated would be wrong. The image goes
       out as zeros; the document survives intact. */
    ASSERT_EQ_U32(format_rtf_export_images(&doc, NULL, 0, &measured, &src), WP_RANGE);
    ASSERT_EQ_U32(format_rtf_export_images(&doc, buf, (u32)sizeof(buf), &written, &src), WP_OK);
    ASSERT_EQ_U32(written, measured);
    buf[written] = '\0';
    ASSERT_TRUE(strstr((char *)buf, "{\\pict\\dibitmap0") != NULL);

    wp_document_free(&doc);
}

void test_rtf_image_text_paragraphs_are_unaffected(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    u8 buf[4096];
    u32 n = 0;

    /* An export with no images in it must be byte-identical to what it
       was before any of this: format_rtf_export is still the old
       signature and the old behaviour. */
    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"plain", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    ASSERT_EQ_U32(format_rtf_export(&doc, buf, (u32)sizeof(buf), &n), WP_OK);
    buf[n] = '\0';
    ASSERT_TRUE(strstr((char *)buf, "plain") != NULL);
    ASSERT_TRUE(strstr((char *)buf, "\\pict") == NULL);
    ASSERT_TRUE(strstr((char *)buf, "[image") == NULL);

    wp_document_free(&doc);
}

/* -- Import: the mirror ------------------------------------------------
 *
 * Export alone made the picture readable by other word processors but
 * not by RetroWP itself, which is a strange place to stop. These cover
 * the round trip: the bytes that went out come back, right way up. */

static u8 g_sunk[64];
static u32 g_sunk_len;
static i32 g_sunk_w, g_sunk_h;
static int g_sunk_begins;

static wp_status sink_begin(void *user, i32 w, i32 h, char *path_out, u32 cap, u32 *path_len)
{
    (void)user;
    g_sunk_w = w; g_sunk_h = h; g_sunk_len = 0; g_sunk_begins++;
    if (cap < 12) return WP_ERR;
    memcpy(path_out, "C:\\CACHE.TMP", 12);
    *path_len = 12;
    return WP_OK;
}

static wp_status sink_write(void *user, const u8 *bytes, u32 len)
{
    (void)user;
    if (g_sunk_len + len > sizeof(g_sunk)) return WP_ERR;
    memcpy(g_sunk + g_sunk_len, bytes, len);
    g_sunk_len += len;
    return WP_OK;
}

static wp_status sink_end(void *user) { (void)user; return WP_OK; }

static void reset_sink(wp_image_sink *sink)
{
    g_sunk_len = 0; g_sunk_w = g_sunk_h = 0; g_sunk_begins = 0;
    memset(g_sunk, 0, sizeof(g_sunk));
    sink->begin = sink_begin;
    sink->write = sink_write;
    sink->end = sink_end;
    sink->user = NULL;
}

void test_rtf_image_round_trips_through_export_and_import(void)
{
    wp_document doc, back;
    wp_image_source src;
    wp_image_sink sink;
    u8 buf[4096];
    wp_section *sec;
    wp_paragraph *para;
    const wp_image_desc *desc;
    u32 pi, found = 0;

    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;
    export_with(&doc, &src, buf, (u32)sizeof(buf));

    reset_sink(&sink);
    ASSERT_EQ_U32(format_rtf_import_images(buf, (u32)strlen((char *)buf), &back, &sink), WP_OK);

    /* The geometry survived. */
    ASSERT_EQ_U32(g_sunk_begins, 1);
    ASSERT_EQ_U32(g_sunk_w, IMG_W);
    ASSERT_EQ_U32(g_sunk_h, IMG_H);

    /* And so did the pixels, the right way up: the DIB stored them
       bottom-up and padded, and what comes back out is the model's own
       top-down packed rows -- byte for byte what went in. */
    ASSERT_EQ_U32(g_sunk_len, (u32)sizeof(g_pixels));
    ASSERT_TRUE(memcmp(g_sunk, g_pixels, sizeof(g_pixels)) == 0);

    /* The document has a real image paragraph again. */
    sec = wp_document_section_get(&back, 0);
    for (pi = 0; pi < sec->para_count; pi++) {
        para = wp_section_paragraph_get(sec, pi);
        if (para->kind != (u8)WP_PARA_IMAGE) continue;
        found++;
        desc = wp_document_image_get(&back, para->image);
        ASSERT_TRUE(desc != NULL);
        ASSERT_EQ_U32(desc->width_px, IMG_W);
        ASSERT_EQ_U32(desc->height_px, IMG_H);
        ASSERT_EQ_U32(desc->display_width_twips, 960);
        ASSERT_EQ_U32(desc->display_height_twips, 480);
        ASSERT_EQ_U32(desc->palette_count, 2);
        /* RGB again, not the DIB's BGRA */
        ASSERT_EQ_U32(desc->palette[0][0], 0x11);
        ASSERT_EQ_U32(desc->palette[0][2], 0x33);
        ASSERT_EQ_U32(desc->palette[1][0], 0xAA);
        ASSERT_TRUE(desc->blob_path.length == 12);
    }
    ASSERT_EQ_U32(found, 1);

    wp_document_free(&back);
    wp_document_free(&doc);
}

void test_rtf_image_import_without_a_sink_skips_the_picture(void)
{
    wp_document doc, back;
    wp_image_source src;
    u8 buf[4096];
    wp_section *sec;
    u32 pi, images = 0;

    build_image_doc(&doc);
    src.read = read_pixels;
    src.user = NULL;
    export_with(&doc, &src, buf, (u32)sizeof(buf));

    /* No sink means nowhere to put the pixels, so the picture is skipped
       exactly as it was before import understood \pict at all -- and
       crucially the hex must not leak into the document as text. */
    ASSERT_EQ_U32(format_rtf_import(buf, (u32)strlen((char *)buf), &back), WP_OK);
    sec = wp_document_section_get(&back, 0);
    for (pi = 0; pi < sec->para_count; pi++) {
        if (wp_section_paragraph_get(sec, pi)->kind == (u8)WP_PARA_IMAGE) images++;
    }
    ASSERT_EQ_U32(images, 0);

    {   /* not one hex digit of the DIB became text */
        u8 out[2048];
        u32 n = 0;
        ASSERT_EQ_U32(format_txt_export(&back, out, (u32)sizeof(out), &n), WP_OK);
        out[n] = '\0';
        ASSERT_TRUE(strstr((char *)out, "28000000") == NULL);
    }

    wp_document_free(&back);
    wp_document_free(&doc);
}

void test_rtf_image_import_keeps_the_text_around_a_bad_picture(void)
{
    wp_document back;
    wp_image_sink sink;
    wp_section *sec;
    u32 pi, images = 0;
    u8 out[1024];
    u32 n = 0;
    /* A \pict whose DIB is truncated mid-header. */
    static const char rtf[] =
        "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Helvetica;}}"
        "\\pard{\\f0 before}\\par\r\n"
        "\\pard{\\pict\\dibitmap0\\picw4\\pich2 2800}\\par\r\n"
        "\\pard{\\f0 after}\\par\r\n}";

    reset_sink(&sink);
    ASSERT_EQ_U32(format_rtf_import_images((const u8 *)rtf, (u32)strlen(rtf), &back, &sink), WP_OK);

    sec = wp_document_section_get(&back, 0);
    for (pi = 0; pi < sec->para_count; pi++) {
        if (wp_section_paragraph_get(sec, pi)->kind == (u8)WP_PARA_IMAGE) images++;
    }
    ASSERT_EQ_U32(images, 0);          /* the picture is dropped ... */

    ASSERT_EQ_U32(format_txt_export(&back, out, (u32)sizeof(out), &n), WP_OK);
    out[n] = '\0';
    ASSERT_TRUE(strstr((char *)out, "before") != NULL);   /* ... and the text survives */
    ASSERT_TRUE(strstr((char *)out, "after") != NULL);

    wp_document_free(&back);
}
