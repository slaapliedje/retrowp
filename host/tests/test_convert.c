#include <string.h>
#include "test.h"
#include "offload.h"
#include "format_rtf.h"
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
    u8 req_payload[FIXTURE_CAP + 1];
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
    u8 req_payload[2048 + 1];
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
    memcpy(req_payload + 1, odt_bytes, odt_len > sizeof(req_payload) - 1 ? sizeof(req_payload) - 1 : odt_len);
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

    payload[0] = 99; /* not FORMAT_ODT */
    payload[1] = 'x';
    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = payload;
    req.payload_len = 2;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    payload[0] = FORMAT_PDF; /* CONVERT_EXPORT to PDF: not implemented by this gateway */
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = payload;
    req.payload_len = 2;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
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
