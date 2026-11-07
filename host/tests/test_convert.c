/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200112L   /* setenv, for the gateway's test-only part size */
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "offload.h"
#include "format_rtf.h"
#include "format_txt.h"
#include "platform.h"
#include "gateway_harness.h"

/* Drives offload/gateway/odt_gateway.py (M7 T7.2) through the exact same
   offload_client.c API test_offload.c already proves against
   mock_gateway.c -- this file is fully deterministic (no LLM involved,
   unlike ollama_gateway.c), so it belongs in the automated suite. */

#define GATEWAY_BINARY "offload/gateway/odt_gateway.py"
#define FORMAT_ODT 1
#define FORMAT_PDF 2
#define FIXTURE_CAP 32768

static wp_status read_fixture(const char *path, u8 *buf, u32 cap, u32 *out_len)
{
    plat_file *f;
    wp_status st;

    st = plat_file_open(path, PLAT_FILE_READ, &f);
    if (st != WP_OK) return st;
    st = plat_file_read(f, buf, cap, out_len);
    plat_file_close(f);
    return st;
}

void test_convert_odt_import_via_gateway(void)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    u8 odt_bytes[FIXTURE_CAP];
    u32 odt_len;
    static u8 req_payload[FIXTURE_CAP + 1];
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    const wp_para_style *ps;
    const wp_char_style *cs;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_available());

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_EQ_STR((const char *)caps.gateway_name, "odt-gateway");
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_CONVERT_IMPORT)) != 0);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_CONVERT_EXPORT)) != 0);

    ASSERT_TRUE(read_fixture("host/tests/fixtures/sample.odt", odt_bytes, FIXTURE_CAP, &odt_len) == WP_OK);
    req_payload[0] = FORMAT_ODT;
    memcpy(req_payload + 1, odt_bytes, odt_len);

    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = req_payload;
    req.payload_len = odt_len + 1;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len > 0);

    ASSERT_TRUE(format_rtf_import(resp.result, resp.result_len, &doc) == WP_OK);
    offload_response_free(&resp);

    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    para = wp_section_paragraph_get(sec, 0);
    ps = wp_document_pstyle_get(&doc, para->style);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_CENTER);
    ASSERT_EQ_U32(para->run_count, 1);
    run = wp_paragraph_run_get(para, 0);
    ASSERT_EQ_U32(run->text.length, 22);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "Fixture Document Title", 22);

    para = wp_section_paragraph_get(sec, 1);
    ps = wp_document_pstyle_get(&doc, para->style);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_JUSTIFY);
    ASSERT_EQ_U32(ps->indent_left, 360);
    ASSERT_EQ_U32(para->run_count, 7);

    run = wp_paragraph_run_get(para, 1); /* "bold" */
    cs = wp_document_cstyle_get(&doc, run->style);
    ASSERT_TRUE((cs->flags & WP_STYLE_BOLD) != 0);
    ASSERT_EQ_U32(run->text.length, 4);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "bold", 4);

    run = wp_paragraph_run_get(para, 3); /* "italic" */
    cs = wp_document_cstyle_get(&doc, run->style);
    ASSERT_TRUE((cs->flags & WP_STYLE_ITALIC) != 0);

    run = wp_paragraph_run_get(para, 5); /* "colored" */
    cs = wp_document_cstyle_get(&doc, run->style);
    ASSERT_EQ_U32(cs->color_r, 0);
    ASSERT_EQ_U32(cs->color_g, 102);
    ASSERT_EQ_U32(cs->color_b, 204);
    ASSERT_EQ_U32(run->text.length, 7);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "colored", 7);

    wp_document_free(&doc);
    offload_shutdown();
    gateway_stop(&gw);
}

/* Builds a small document directly (not via RTF import, so this test is
   independent of test_convert_odt_import_via_gateway), exports it to
   RTF, ships that to the gateway as CONVERT_EXPORT (format=ODT), checks
   the result is a real ZIP container, then feeds those SAME ODT bytes
   back through CONVERT_IMPORT on the same connection (the gateway's
   request loop handles multiple sequential requests -- proven already
   for ollama_gateway.c, exercised here for odt_gateway.py too) and
   asserts every attribute survives the full model -> RTF -> ODT -> RTF
   -> model round trip. No external tool (LibreOffice) needed for this
   automated check -- that cross-validation was done manually during
   development (docs/formats/odt.md has the trace); this closed loop
   only needs this project's own two ends to agree with each other. */
void test_convert_odt_export_roundtrip_via_gateway(void)
{
    gateway_handle gw;
    wp_document doc, doc2;
    wp_section *sec;
    wp_paragraph *para;
    wp_pstyle pstyle;
    wp_cstyle cstyle;
    wp_para_style ps;
    wp_char_style cs;
    wp_span span;
    u16 run_idx;
    u32 sec_idx, para_idx;
    u8 rtf_buf[2048];
    u32 rtf_len;
    static u8 req_payload[FIXTURE_CAP + 1];
    offload_request req;
    offload_response resp;
    u8 odt_bytes[FIXTURE_CAP];
    u32 odt_len;
    wp_run *run;
    const wp_para_style *ps2;
    const wp_char_style *cs2;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_RIGHT;
    ps.indent_left = 480;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps, &pstyle) == WP_OK);
    wp_paragraph_pstyle_set(para, pstyle);

    memset(&cs, 0, sizeof(cs));
    cs.flags = (u16)(WP_STYLE_ITALIC | WP_STYLE_STRIKE);
    cs.size_twips = 280;
    cs.color_r = 10; cs.color_g = 200; cs.color_b = 30;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs, &cstyle) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"round trip me", 13, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(format_rtf_export(&doc, rtf_buf, sizeof(rtf_buf), &rtf_len) == WP_OK);
    wp_document_free(&doc);

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req_payload[0] = FORMAT_ODT;
    memcpy(req_payload + 1, rtf_buf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len > 4);
    /* real ZIP local-file-header magic -- a light structural check that
       this is genuinely a zip container, not a full content.xml parse
       (this project has no XML/zip library on the engine side, nor
       should it -- see docs/formats/odt.md). */
    ASSERT_MEM_EQ(resp.result, "PK\x03\x04", 4);
    ASSERT_TRUE(resp.result_len <= FIXTURE_CAP);
    odt_len = resp.result_len;
    memcpy(odt_bytes, resp.result, odt_len);
    offload_response_free(&resp);

    req_payload[0] = FORMAT_ODT;
    memcpy(req_payload + 1, odt_bytes, odt_len);   /* odt_len <= FIXTURE_CAP, checked above */
    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = req_payload;
    req.payload_len = odt_len + 1;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    ASSERT_TRUE(format_rtf_import(resp.result, resp.result_len, &doc2) == WP_OK);
    offload_response_free(&resp);

    sec = wp_document_section_get(&doc2, 0);
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    ps2 = wp_document_pstyle_get(&doc2, para->style);
    ASSERT_EQ_U32(ps2->align, WP_ALIGN_RIGHT);
    ASSERT_EQ_U32(ps2->indent_left, 480);
    ASSERT_EQ_U32(para->run_count, 1);

    run = wp_paragraph_run_get(para, 0);
    cs2 = wp_document_cstyle_get(&doc2, run->style);
    ASSERT_TRUE((cs2->flags & WP_STYLE_ITALIC) != 0);
    ASSERT_TRUE((cs2->flags & WP_STYLE_STRIKE) != 0);
    ASSERT_EQ_U32(cs2->size_twips, 280);
    ASSERT_EQ_U32(cs2->color_r, 10);
    ASSERT_EQ_U32(cs2->color_g, 200);
    ASSERT_EQ_U32(cs2->color_b, 30);
    ASSERT_EQ_U32(run->text.length, 13);
    ASSERT_MEM_EQ(wp_strpool_get(&doc2.strings, run->text), "round trip me", 13);

    wp_document_free(&doc2);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_convert_unsupported_format_id_reports_unsupported(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    u8 payload[2];

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    payload[0] = 99; /* neither FORMAT_ODT nor FORMAT_PDF */
    payload[1] = 'x';
    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = payload;
    req.payload_len = 2;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    req.op = OFFLOAD_CONVERT_EXPORT; /* same bogus format_id, CONVERT_EXPORT this time */
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

/* PDF is export-only (no format_id for "import a PDF" -- see
   docs/formats/offload-protocol.md), and unlike ODT this gateway does
   its own real text layout for it (word-wrap, pagination, per-line
   positioning -- docs/formats/pdf.md). Builds a small document, exports
   to RTF, ships it through CONVERT_EXPORT with format_id=FORMAT_PDF, and
   checks the result is a genuine PDF (magic bytes, a real "/Type /Page"
   object present -- this project has no PDF-parsing library on the
   engine side, nor should it, so this is a structural check, not a full
   parse). Deliberately kept small: engine/src/offload_client.c caps a
   single response at OFFLOAD_MAX_RESPONSE_PAYLOAD (4096 bytes) --
   a real constraint of the current transport, not specific to PDF, that
   a bigger fixture here actually hit during development (offload_submit
   returned WP_RANGE, not a gateway bug). Multi-page pagination (the
   PDF-specific behavior a tiny fixture like this can't exercise without
   pushing past that cap) was verified manually during development --
   see docs/formats/pdf.md for that trace (40 paragraphs, 3 real pages,
   checked with real pdftotext/pdftoppm). */
void test_convert_pdf_export_via_gateway(void)
{
    gateway_handle gw;
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_pstyle pstyle;
    wp_cstyle cstyle;
    wp_para_style ps;
    wp_char_style cs;
    wp_span span;
    u16 run_idx;
    u32 sec_idx, para_idx;
    u8 rtf_buf[1024];
    u32 rtf_len;
    u8 req_payload[1024 + 1];
    offload_request req;
    offload_response resp;
    static const char *paragraph_text =
        "This paragraph forces real word-wrapping across a couple of lines "
        "within the page column width.";
    wp_bool found_page_object = WP_FALSE;
    u32 i;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_JUSTIFY;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps, &pstyle) == WP_OK);
    wp_paragraph_pstyle_set(para, pstyle);
    memset(&cs, 0, sizeof(cs));
    cs.flags = WP_STYLE_BOLD;
    cs.size_twips = 240;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs, &cstyle) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)paragraph_text,
                                   (u32)strlen(paragraph_text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(format_rtf_export(&doc, rtf_buf, sizeof(rtf_buf), &rtf_len) == WP_OK);
    wp_document_free(&doc);

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req_payload[0] = FORMAT_PDF;
    memcpy(req_payload + 1, rtf_buf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len > 4);
    ASSERT_MEM_EQ(resp.result, "%PDF", 4);

    for (i = 0; i + 12 <= resp.result_len; i++) {
        if (memcmp(resp.result + i, "/Type /Page", 11) == 0 && resp.result[i + 11] != 's') {
            found_page_object = WP_TRUE;
            break;
        }
    }
    ASSERT_TRUE(found_page_object);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_convert_malformed_payload_is_error(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = (const u8 *)"";
    req.payload_len = 0; /* no format_id byte at all */
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_ERR);
    offload_response_free(&resp);

    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = (const u8 *)"\x01not really an odt file";
    req.payload_len = 23;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_ERR); /* zipfile.BadZipFile, caught -> ERR, not a crash */
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

/* The document's own page reaches the PDF: an A4 section with narrow
   margins comes out as an A4 MediaBox, and the page text is compressed
   (what lets a real document fit the link's one-frame response). */
void test_convert_pdf_export_uses_the_document_page(void)
{
    gateway_handle gw;
    wp_document doc;
    u8 rtf_buf[2048];
    u32 rtf_len;
    u8 req_payload[2048 + 1];
    offload_request req;
    offload_response resp;
    static const char text[] = "Name\tAmount\nAlice\t12.50\n";
    wp_bool media = WP_FALSE, flate = WP_FALSE;
    u32 i;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    doc.sections[0].page_width = 11906;     /* A4 */
    doc.sections[0].page_height = 16838;
    doc.sections[0].margin_left = 1134;
    ASSERT_TRUE(format_rtf_export(&doc, rtf_buf, sizeof(rtf_buf), &rtf_len) == WP_OK);
    wp_document_free(&doc);

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    req_payload[0] = FORMAT_PDF;
    memcpy(req_payload + 1, rtf_buf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    for (i = 0; i + 28 <= resp.result_len; i++) {
        if (memcmp(resp.result + i, "/MediaBox [0 0 595.3 841.9]", 27) == 0) media = WP_TRUE;
        if (memcmp(resp.result + i, "/FlateDecode", 12) == 0) flate = WP_TRUE;
    }
    ASSERT_TRUE(media);
    ASSERT_TRUE(flate);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* A page break before a paragraph starts a new PDF page. */
void test_convert_pdf_export_honours_page_breaks(void)
{
    gateway_handle gw;
    static const char rtf[] = "{\\rtf1\\ansi one\\par\\pard\\pagebb two\\par}";
    u8 req_payload[128];
    offload_request req;
    offload_response resp;
    u32 i, pages = 0;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    req_payload[0] = FORMAT_PDF;
    memcpy(req_payload + 1, rtf, sizeof(rtf) - 1);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(rtf);
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    for (i = 0; i + 12 <= resp.result_len; i++) {
        if (memcmp(resp.result + i, "/Type /Page ", 12) == 0) pages++;
    }
    ASSERT_EQ_U32(pages, 2);
    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* A run's face picks the PDF's: a serif (\froman, or a name like
   "Times New Roman") sets in Times, a monospace in Courier -- the
   built-in Type 1 faces, or Liberation Serif and Mono embedded where
   the gateway host has them; either tier is the right answer. */
void test_convert_pdf_export_picks_faces_by_family(void)
{
    gateway_handle gw;
    static const char rtf[] =
        "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\froman Times New Roman;}"
        "{\\f2\\fnil Courier New;}}{\\*\\generator RetroWP;}"
        "\\pard{\\fs24 plain }{\\f1\\fs24 serif }{\\f2\\fs24 mono}\\par}";
    u8 req_payload[512];
    offload_request req;
    offload_response resp;
    wp_bool serif = WP_FALSE, mono = WP_FALSE;
    u32 i;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    req_payload[0] = FORMAT_PDF;
    memcpy(req_payload + 1, rtf, sizeof(rtf) - 1);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(rtf);
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    for (i = 0; i + 26 <= resp.result_len; i++) {
        if (memcmp(resp.result + i, "/BaseFont /Times-Roman", 22) == 0 ||
            memcmp(resp.result + i, "/BaseFont /LiberationSerif", 26) == 0) serif = WP_TRUE;
        if (memcmp(resp.result + i, "/BaseFont /Courier", 18) == 0 ||
            memcmp(resp.result + i, "/BaseFont /LiberationMono", 25) == 0) mono = WP_TRUE;
    }
    ASSERT_TRUE(serif);
    ASSERT_TRUE(mono);
    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* The header, the footer and the page cross to ODT and back: styles.xml
   carries the page layout and a master page whose header and footer
   hold the parts apart by tabs and '#' as a page-number field, and the
   way back hands the engine its own \header, \footer and \paperw... */
void test_convert_odt_carries_headers_footers_and_the_page(void)
{
    gateway_handle gw;
    wp_document doc;
    static const char rtf[] =
        "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\*\\generator RetroWP;}"
        "\\paperw11906\\paperh16838\\margl1134\\margr1134\\margt1440\\margb1440"
        "{\\header\\pard\\plain My Report\\tab Page \\chpgn }{\\footer\\pard\\plain \\chpgn }"
        "\\pard{\\fs24 Body}\\par}";
    static u8 req_payload[FIXTURE_CAP + 1];
    static u8 odt_bytes[FIXTURE_CAP];
    u32 odt_len;
    offload_request req;
    offload_response resp;
    wp_section *sec;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    req_payload[0] = FORMAT_ODT;
    memcpy(req_payload + 1, rtf, sizeof(rtf) - 1);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(rtf);
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len <= FIXTURE_CAP);
    odt_len = resp.result_len;
    memcpy(odt_bytes, resp.result, odt_len);
    offload_response_free(&resp);

    req_payload[0] = FORMAT_ODT;
    memcpy(req_payload + 1, odt_bytes, odt_len);
    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload_len = odt_len + 1;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(format_rtf_import(resp.result, resp.result_len, &doc) == WP_OK);
    offload_response_free(&resp);

    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->header.length, 16);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, sec->header), "My Report|Page #", 16);
    ASSERT_EQ_U32(sec->footer.length, 1);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, sec->footer), "#", 1);
    ASSERT_EQ_U32((u32)sec->page_width, 11906);
    ASSERT_EQ_U32((u32)sec->margin_left, 1134);
    wp_document_free(&doc);
    offload_shutdown();
    gateway_stop(&gw);
}

/* A reply bigger than one frame comes in parts (WIRE_MSG_RESPONSE_PART)
   and reaches the caller whole: the gateway is made to split anything
   over 2000 bytes into 1000-byte parts, and the PDF that comes back that
   way must be the PDF that comes back in one frame, byte for byte. */
static void export_pdf(const char *rtf, u32 rtf_len, u8 **out, u32 *out_len)
{
    gateway_handle gw;
    static u8 payload[4096];
    offload_request req;
    offload_response resp;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    payload[0] = FORMAT_PDF;
    memcpy(payload + 1, rtf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = payload;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    *out = resp.result;          /* kept: the caller frees it */
    *out_len = resp.result_len;
    offload_shutdown();
    gateway_stop(&gw);
}

void test_convert_a_reply_in_parts_arrives_whole(void)
{
    static const char rtf[] =
        "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\*\\generator RetroWP;}"
        "\\pard{\\fs24 One paragraph, and another, enough to make a PDF of a few kilobytes.}\\par"
        "\\pard\\pagebb{\\fs24 A second page.}\\par}";
    u8 *whole, *parts;
    u32 whole_len, parts_len;

    export_pdf(rtf, (u32)sizeof(rtf) - 1, &whole, &whole_len);
    setenv("RETROWP_GATEWAY_PART_TEST", "1", 1);
    export_pdf(rtf, (u32)sizeof(rtf) - 1, &parts, &parts_len);
    unsetenv("RETROWP_GATEWAY_PART_TEST");

    ASSERT_TRUE(whole_len > 2000);                 /* big enough to have been split */
    ASSERT_EQ_U32(parts_len, whole_len);
    ASSERT_TRUE(memcmp(parts, whole, whole_len) == 0);
    mem_free(whole);
    mem_free(parts);
}

/* Sections reach the PDF and the ODT: each starts its own page, at its
   own size, under its own header -- and the ODT carries them back. */
static const char g_sections_rtf[] =
    "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\*\\generator RetroWP;}"
    "\\paperw12240\\paperh15840\\margl1440\\margr1440\\margt1440\\margb1440"
    "{\\header\\pard\\plain Upright}\\pard{\\fs24 Portrait page.}\\par"
    "\\sect\\sectd\\pgwsxn15840\\pghsxn12240\\marglsxn1440\\margrsxn1440\\margtsxn1440\\margbsxn1440"
    "{\\header\\pard\\plain Sideways\\tab \\chpgn }\\pard{\\fs24 Landscape page.}\\par}";

static void gateway_convert(u8 op, u8 format, const u8 *body, u32 len, u8 **out, u32 *out_len)
{
    gateway_handle gw;
    static u8 payload[FIXTURE_CAP + 1];
    offload_request req;
    offload_response resp;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    payload[0] = format;
    memcpy(payload + 1, body, len);
    req.op = (offload_op)op;
    req.payload = payload;
    req.payload_len = len + 1;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    *out = resp.result;
    *out_len = resp.result_len;
    offload_shutdown();
    gateway_stop(&gw);
}

static wp_bool has_bytes(const u8 *hay, u32 n, const char *needle)
{
    u32 k = (u32)strlen(needle), i;
    for (i = 0; i + k <= n; i++) if (memcmp(hay + i, needle, k) == 0) return WP_TRUE;
    return WP_FALSE;
}

void test_convert_sections_reach_the_pdf_and_odt(void)
{
    u8 *pdf, *odt, *rtf;
    u32 pdf_len, odt_len, rtf_len;
    wp_document doc;

    gateway_convert(OFFLOAD_CONVERT_EXPORT, FORMAT_PDF, (const u8 *)g_sections_rtf,
                    (u32)sizeof(g_sections_rtf) - 1, &pdf, &pdf_len);
    ASSERT_TRUE(has_bytes(pdf, pdf_len, "/MediaBox [0 0 612 792]"));    /* section one, upright */
    ASSERT_TRUE(has_bytes(pdf, pdf_len, "/MediaBox [0 0 792 612]"));    /* section two, on its side */
    mem_free(pdf);

    gateway_convert(OFFLOAD_CONVERT_EXPORT, FORMAT_ODT, (const u8 *)g_sections_rtf,
                    (u32)sizeof(g_sections_rtf) - 1, &odt, &odt_len);
    ASSERT_TRUE(odt_len <= FIXTURE_CAP);
    gateway_convert(OFFLOAD_CONVERT_IMPORT, FORMAT_ODT, odt, odt_len, &rtf, &rtf_len);
    mem_free(odt);
    ASSERT_TRUE(format_rtf_import(rtf, rtf_len, &doc) == WP_OK);
    mem_free(rtf);
    ASSERT_EQ_U32(doc.section_count, 2);
    ASSERT_EQ_U32((u32)doc.sections[0].page_width, 12240);
    ASSERT_EQ_U32((u32)doc.sections[1].page_width, 15840);
    ASSERT_EQ_U32(doc.sections[1].header.length, 10);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, doc.sections[1].header), "Sideways|#", 10);
    ASSERT_EQ_U32(doc.sections[0].header.length, 7);
    wp_document_free(&doc);
}

/* A request bigger than one frame goes in parts to a gateway that takes
   them: a document whose RTF is well past 64 KB goes out as ODT and
   comes back with every paragraph. */
void test_convert_a_large_document_goes_in_parts(void)
{
    gateway_handle gw;
    static u8 payload[200000];
    static u8 odt[FIXTURE_CAP];
    u32 len, odt_len, i;
    offload_request req;
    offload_response resp;
    wp_document doc;
    const char *head = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\*\\generator RetroWP;}";
    const char *para = "\\pard{\\fs24 A paragraph of a long document, long enough to fill some lines of a page.}\\par\r\n";

    payload[0] = FORMAT_ODT;
    len = 1;
    memcpy(payload + len, head, strlen(head)); len += (u32)strlen(head);
    for (i = 0; i < 1500; i++) { memcpy(payload + len, para, strlen(para)); len += (u32)strlen(para); }
    payload[len++] = '}';
    ASSERT_TRUE(len > 100000);                      /* well past one frame */

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_takes_large_requests());
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = payload;
    req.payload_len = len;
    req.timeout_ms = 20000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len <= FIXTURE_CAP);
    odt_len = resp.result_len;
    memcpy(odt, resp.result, odt_len);
    offload_response_free(&resp);

    payload[0] = FORMAT_ODT;
    memcpy(payload + 1, odt, odt_len);
    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload_len = odt_len + 1;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(format_rtf_import(resp.result, resp.result_len, &doc) == WP_OK);
    offload_response_free(&resp);
    ASSERT_EQ_U32(doc.sections[0].para_count, 1500);
    wp_document_free(&doc);
    offload_shutdown();
    gateway_stop(&gw);
}

/* ---- OFFLOAD_PRINT ---------------------------------------------------- */

#include <sys/stat.h>

#define PRINT_DIR "build/test_print"

/* The folder empty of printouts, which a test elsewhere leaves there. */
static void print_dir_fresh(void)
{
    char name[40] = PRINT_DIR "/print-000N.pdf";
    char *digit = name + sizeof(PRINT_DIR "/print-000") - 1;

    for (*digit = '1'; *digit <= '9'; (*digit)++) (void)remove(name);
    (void)remove(PRINT_DIR "/lp.args");
    (void)remove(PRINT_DIR "/lp.pdf");
    (void)mkdir("build", 0755);
    (void)mkdir(PRINT_DIR, 0755);
}

/* "Alpha", a one-page document, as the action sends it: a copy count,
   then its RTF. */
static u32 print_payload(u8 *buf, u32 cap, u8 copies)
{
    static const char rtf[] = "{\\rtf1\\ansi{\\fonttbl{\\f0 Helvetica;}}\\f0\\fs24 Alpha\\par}";
    u32 n = (u32)sizeof(rtf) - 1;

    if (n + 1 > cap) return 0;
    buf[0] = copies;
    memcpy(buf + 1, rtf, n);
    return n + 1;
}

static void submit_print(const char *const *args, offload_status want_status, const char *want_reply)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    u8 payload[256];
    u32 n = (u32)strlen(want_reply);

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_PRINT)) != 0);

    req.op = OFFLOAD_PRINT;
    req.payload = payload;
    req.payload_len = print_payload(payload, sizeof(payload), 2);
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_EQ_U32(resp.status, want_status);
    ASSERT_EQ_U32(resp.result_len, n);
    if (resp.result_len == n) ASSERT_MEM_EQ(resp.result, want_reply, n);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

/* No printer configured: the CAPS bit is clear, so a program never
   offers it, and a request anyway is unsupported. */
void test_convert_print_needs_a_printer(void)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    u8 payload[256];

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_PRINT)) == 0);

    req.op = OFFLOAD_PRINT;
    req.payload = payload;
    req.payload_len = print_payload(payload, sizeof(payload), 1);
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_EQ_U32(resp.status, OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

/* --print-dir: the printout is the PDF, in the folder. */
void test_convert_print_to_a_folder(void)
{
    static const char *const args[] = { "--print-dir", PRINT_DIR, NULL };
    char head[8];
    plat_file *f;
    u32 got = 0;

    print_dir_fresh();
    submit_print(args, OFFLOAD_OK, "Printed 1 page to print-0001.pdf");
    ASSERT_TRUE(plat_file_open(PRINT_DIR "/print-0001.pdf", PLAT_FILE_READ, &f) == WP_OK);
    ASSERT_TRUE(plat_file_read(f, (u8 *)head, 4, &got) == WP_OK);
    plat_file_close(f);
    ASSERT_EQ_U32(got, 4);
    ASSERT_MEM_EQ(head, "%PDF", 4);
}

/* --printer: the PDF goes to the print command's stdin with lp's flags
   -- a title, the copies, the printer -- and a refusal comes back as
   STATUS_ERR with a line the program can show. */
void test_convert_print_through_lp(void)
{
    static const char *const args[] = { "--printer", "Office",
                                        "--print-command", "host/tests/fake_lp.sh", NULL };
    static const char *const refuses[] = { "--printer", "", "--print-command", "false", NULL };
    char got[64];
    plat_file *f;
    u32 n = 0;

    print_dir_fresh();
    submit_print(args, OFFLOAD_OK, "Printed 1 page on Office");
    ASSERT_TRUE(plat_file_open(PRINT_DIR "/lp.args", PLAT_FILE_READ, &f) == WP_OK);
    ASSERT_TRUE(plat_file_read(f, (u8 *)got, sizeof(got) - 1, &n) == WP_OK);
    plat_file_close(f);
    got[n] = '\0';
    ASSERT_EQ_STR(got, "-t\nRetroWP\n-n\n2\n-d\nOffice\n");
    ASSERT_TRUE(plat_file_open(PRINT_DIR "/lp.pdf", PLAT_FILE_READ, &f) == WP_OK);
    ASSERT_TRUE(plat_file_read(f, (u8 *)got, 4, &n) == WP_OK);
    plat_file_close(f);
    ASSERT_MEM_EQ(got, "%PDF", 4);

    submit_print(refuses, OFFLOAD_ERR, "The printer refused the document");
}


/* ---- A HELLO mid-session --------------------------------------------- */

#include "wire.h"

static wp_status raw_read(void *ctx, u8 *out, u32 cap, u32 timeout_ms, u32 *out_len)
{
    return plat_transport_read((plat_transport *)ctx, out, cap, timeout_ms, out_len);
}

/* A machine reset, or the program run again, says HELLO on a line the
   gateway still thinks is mid-session -- there is no connection boundary
   on a serial port. The answer must be CAPS, a fresh handshake, not a
   RESPONSE to a "request" for op 0: a Mac Plus on a MiSTer was told
   "unsupported" that way on its second launch and gave up. */
void test_convert_hello_mid_session_is_a_new_handshake(void)
{
    gateway_handle gw;
    plat_transport *t;
    wire_header h, got;
    u8 frame[WIRE_HEADER_SIZE + WIRE_CRC_SIZE];
    u8 payload[64];
    u32 n, len = 0;
    int round;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(plat_transport_open(gw.endpoint, 2000, &t) == WP_OK);
    memset(&h, 0, sizeof(h));
    h.type = WIRE_MSG_HELLO;
    n = wire_encode_frame(&h, NULL, frame, sizeof(frame));
    ASSERT_TRUE(n == sizeof(frame));
    for (round = 0; round < 2; round++) {
        ASSERT_TRUE(plat_transport_write(t, frame, n, 2000) == WP_OK);
        ASSERT_TRUE(wire_read_frame(raw_read, t, 5000, &got, payload, sizeof(payload), &len) == WP_OK);
        ASSERT_EQ_U32(got.type, WIRE_MSG_CAPS);
        ASSERT_EQ_U32(len, 42);
    }
    plat_transport_close(t);
    gateway_stop(&gw);
}

/* Line noise before a frame, and a frame with a bad CRC, are skipped,
   not the end of the session: a MiSTer ST core sent one 0xFF as its port
   opened, a second before its HELLO, and a gateway that gave up on it
   lost the HELLO behind it. */
void test_convert_gateway_resyncs_past_noise(void)
{
    gateway_handle gw;
    plat_transport *t;
    wire_header h, got;
    u8 frame[WIRE_HEADER_SIZE + WIRE_CRC_SIZE];
    u8 noise[3] = { 0xFF, 'W', 'x' };
    u8 bad[WIRE_HEADER_SIZE + WIRE_CRC_SIZE];
    u8 payload[64];
    u32 n, len = 0;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(plat_transport_open(gw.endpoint, 2000, &t) == WP_OK);
    memset(&h, 0, sizeof(h));
    h.type = WIRE_MSG_HELLO;
    n = wire_encode_frame(&h, NULL, frame, sizeof(frame));
    memcpy(bad, frame, n);
    bad[n - 1] ^= 0x5A;                       /* a HELLO with a broken CRC */
    ASSERT_TRUE(plat_transport_write(t, noise, sizeof(noise), 2000) == WP_OK);
    ASSERT_TRUE(plat_transport_write(t, bad, n, 2000) == WP_OK);
    ASSERT_TRUE(plat_transport_write(t, frame, n, 2000) == WP_OK);
    ASSERT_TRUE(wire_read_frame(raw_read, t, 5000, &got, payload, sizeof(payload), &len) == WP_OK);
    ASSERT_EQ_U32(got.type, WIRE_MSG_CAPS);
    ASSERT_EQ_U32(len, 42);
    plat_transport_close(t);
    gateway_stop(&gw);
}

/* A request where a HELLO was expected: served by a lenient gateway (what
   serial mode always is), refused by a strict one. A writer who took two
   minutes over the Print dialog on a MiSTer ST sent the request after the
   gateway's idle timeout had ended its session; refusing it left the ST
   waiting out its own two-minute timeout. */
static wp_status request_without_hello(const char *const *args, wire_header *got,
                                       u8 *reply, u32 cap, u32 *len)
{
    gateway_handle gw;
    plat_transport *t;
    wire_header h;
    static u8 frame[512];
    u8 payload[256];
    u32 n, plen;
    wp_status st;

    if (gateway_start_bin(GATEWAY_BINARY, args, &gw) != WP_OK) return WP_ERR;
    if (plat_transport_open(gw.endpoint, 2000, &t) != WP_OK) { gateway_stop(&gw); return WP_ERR; }
    plen = print_payload(payload, sizeof(payload), 1);
    memset(&h, 0, sizeof(h));
    h.type = WIRE_MSG_REQUEST;
    h.op = OFFLOAD_PRINT;
    h.seq = 7;
    h.length = plen;
    n = wire_encode_frame(&h, payload, frame, sizeof(frame));
    st = plat_transport_write(t, frame, n, 2000);
    if (st == WP_OK) st = wire_read_frame(raw_read, t, 5000, got, reply, cap, len);
    plat_transport_close(t);
    gateway_stop(&gw);
    return st;
}

void test_convert_request_without_hello(void)
{
    static const char *const lenient[] = { "--print-dir", PRINT_DIR, "--lenient-handshake", NULL };
    static const char *const strict[] = { "--print-dir", PRINT_DIR, NULL };
    wire_header got;
    u8 reply[128];
    u32 len = 0;

    print_dir_fresh();
    ASSERT_TRUE(request_without_hello(lenient, &got, reply, sizeof(reply), &len) == WP_OK);
    ASSERT_EQ_U32(got.type, WIRE_MSG_RESPONSE);
    ASSERT_EQ_U32(got.seq, 7);
    ASSERT_EQ_U32(got.status, OFFLOAD_OK);
    ASSERT_TRUE(len > 7 && memcmp(reply, "Printed", 7) == 0);

    ASSERT_TRUE(request_without_hello(strict, &got, reply, sizeof(reply), &len) != WP_OK);
}

/* ---- OFFLOAD_PRINT_OPTIONS -------------------------------------------- */

static const char *const options_gateway[] = {
    "--printer", "Office", "--print-command", "host/tests/fake_lp.sh",
    "--print-options-command", "host/tests/fake_lpoptions.sh", NULL };

/* The gateway lists its printer's options, mapped from the queue's own
   (the M477fdn's, by fake_lpoptions.sh): copies, two-sided from its
   Duplex, colour from HPPJLColorAsGray, paper feed from InputSlot, collate
   -- and nothing printer-specific like EconoMode. */
void test_convert_print_options_listed(void)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    char text[1024];

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, options_gateway, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_PRINT_OPTIONS)) != 0);
    req.op = OFFLOAD_PRINT_OPTIONS;
    req.payload = NULL;
    req.payload_len = 0;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_EQ_U32(resp.status, OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len < sizeof(text));
    memcpy(text, resp.result, resp.result_len);
    text[resp.result_len] = '\0';
    offload_response_free(&resp);
    ASSERT_EQ_STR(text,
        "copies\tCopies\t1\t1=1\t2=2\t3=3\t4=4\t5=5\t10=10\n"
        "sides\tTwo-sided\tone-sided\tone-sided=Off\ttwo-sided-long-edge=Long edge\t"
            "two-sided-short-edge=Short edge\n"
        "color\tColour\tcolor\tcolor=Colour\tgray=Gray\n"
        "tray\tPaper feed\tAuto\tAuto=Automatic\tTray1=Tray 1\tTray2=Tray 2\tTray3=Tray 3\t"
            "ManualFeed=Manual feed\n"
        "collate\tCollate\toff\toff=Off\ton=On\n");
    offload_shutdown();
    gateway_stop(&gw);
}

/* A choice the printer does not offer is refused, with the reason. */
void test_convert_print_option_refused(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    static const char rtf[] = "{\\rtf1\\ansi{\\fonttbl{\\f0 Helvetica;}}\\f0\\fs24 Alpha\\par}";
    static const char opts[] = "tray=Tray9\n";
    u8 payload[256];
    u32 n = 0;
    const char *want = "Paper feed cannot be Tray9 on this printer";

    payload[n++] = 0;
    payload[n++] = 1;
    payload[n++] = 0;
    payload[n++] = (u8)(sizeof(opts) - 1);
    memcpy(payload + n, opts, sizeof(opts) - 1);
    n += (u32)sizeof(opts) - 1;
    memcpy(payload + n, rtf, sizeof(rtf) - 1);
    n += (u32)sizeof(rtf) - 1;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, options_gateway, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    req.op = OFFLOAD_PRINT;
    req.payload = payload;
    req.payload_len = n;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_EQ_U32(resp.status, OFFLOAD_ERR);
    ASSERT_EQ_U32(resp.result_len, (u32)strlen(want));
    if (resp.result_len == strlen(want)) ASSERT_MEM_EQ(resp.result, want, resp.result_len);
    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}
