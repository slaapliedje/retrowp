/* SPDX-License-Identifier: GPL-2.0-or-later */
/* M7 T7.4c: proves OFFLOAD_CONVERT_IMPORT/CONVERT_EXPORT actually work
   over real RS-232 -- platform/atari/transport_atari.c's Bconin/Bconout/
   Rsconf implementation, exercised through the SAME engine/
   offload_client.c and offload/transport/wire.c code the TCP-backed
   host tests already cover (host/tests/test_convert.c), talking to
   offload/gateway/odt_gateway.py's --serial-rx/--serial-tx mode
   (bridged to this real emulated serial port via two FIFOs and
   Hatari's --rs232-in/--rs232-out -- see
   host/tools/ci_atari_convert_serial.sh and docs/formats/odt.md).

   This is genuinely new coverage, not a repeat of T6.3's
   atari_serial_smoke.c: that test proves THESAURUS (a tiny single-word
   payload) works over serial; this one proves CONVERT_IMPORT/EXPORT
   (whole-document, multi-hundred-byte payloads, a real ZIP container
   for ODT, a real PDF object stream) survive the same real link.

   Payload sizes here are deliberately small (a short paragraph, not a
   realistic multi-KB document) -- real RS-232 at 9600 baud moves
   roughly 960 bytes/sec (docs/atari-platform.md /
   platform/atari/app_shell.c's WP_ATARI_CONVERT_TIMEOUT_MS comment has
   the same math), and Hatari's cycle-exact serial emulation runs at
   roughly that same real-world rate, so keeping this fixture small
   keeps the CI run's wall-clock cost small too -- the transport's
   correctness doesn't depend on payload size, so a small fixture proves
   the same thing a large one would, faster. */

#include <string.h>
#include "types.h"
#include "docmodel.h"
#include "format_rtf.h"
#include "offload.h"
#include "wire.h"

__attribute__((noipa))
static long nf_get_id(const char *name)
{
    long id;
    __asm__ volatile(".word 0x7300\n\t" : "=d"(id) : "g"(name) : "cc", "memory");
    return id;
}

__attribute__((noipa))
static long nf_call1(long id, long arg1)
{
    long ret;
    __asm__ volatile(".word 0x7301\n\t" : "=d"(ret) : "g"(id), "g"(arg1) : "cc", "memory");
    return ret;
}

static long g_nf_stderr_id;
static long g_nf_exit_id;

static void nf_log(const char *msg)
{
    if (g_nf_stderr_id) nf_call1(g_nf_stderr_id, (long)msg);
}

static void nf_exit_now(long code)
{
    if (g_nf_exit_id) nf_call1(g_nf_exit_id, code);
}

static int g_pass = 0;
static int g_fail = 0;

static void check(wp_bool cond, const char *what)
{
    if (cond) {
        g_pass++;
        nf_log("PASS: ");
    } else {
        g_fail++;
        nf_log("FAIL: ");
    }
    nf_log(what);
    nf_log("\r\n");
}

#define FORMAT_ODT 1u
#define FORMAT_PDF 2u

static wp_status build_fixture_doc(wp_document *doc)
{
    wp_section *sec;
    wp_paragraph *para;
    wp_pstyle pstyle;
    wp_cstyle cstyle;
    wp_para_style ps;
    wp_char_style cs;
    wp_span span;
    u16 run_idx;
    u32 sec_idx, para_idx;
    static const char text[] = "ODT and PDF over real RS-232.";

    if (wp_document_init(doc) != WP_OK) return WP_ERR;
    if (wp_document_section_add(doc, &sec_idx) != WP_OK) return WP_ERR;
    sec = wp_document_section_get(doc, sec_idx);

    if (wp_section_paragraph_add(sec, &para_idx) != WP_OK) return WP_ERR;
    para = wp_section_paragraph_get(sec, para_idx);

    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_CENTER;
    if (wp_document_pstyle_add(doc, &ps, &pstyle) != WP_OK) return WP_ERR;
    wp_paragraph_pstyle_set(para, pstyle);

    memset(&cs, 0, sizeof(cs));
    cs.flags = WP_STYLE_BOLD;
    cs.size_twips = 240;
    if (wp_document_cstyle_add(doc, &cs, &cstyle) != WP_OK) return WP_ERR;
    if (wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) != WP_OK) return WP_ERR;
    if (wp_paragraph_run_add(para, span, cstyle, &run_idx) != WP_OK) return WP_ERR;

    return WP_OK;
}

int main(void)
{
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    wp_document doc;
    u8 rtf_buf[512];
    u32 rtf_len;
    u8 req_buf[1 + 512];
    u8 odt_bytes[4096];
    u32 odt_len = 0;

    g_nf_stderr_id = nf_get_id("NF_STDERR");
    g_nf_exit_id = nf_get_id("NF_EXIT");

    nf_log("ATARI CONVERT SMOKE: starting\r\n");

    check(!offload_available(), "offload_available() false before offload_init");
    check(offload_init("serial:aux:9600") == WP_OK,
          "offload_init(serial:aux:9600) handshake succeeded");
    check(offload_available(), "offload_available() true after handshake");

    if (offload_query(&caps) == WP_OK) {
        check((caps.ops_supported & (1uL << OFFLOAD_CONVERT_IMPORT)) != 0,
              "negotiated capabilities advertise OFFLOAD_CONVERT_IMPORT");
        check((caps.ops_supported & (1uL << OFFLOAD_CONVERT_EXPORT)) != 0,
              "negotiated capabilities advertise OFFLOAD_CONVERT_EXPORT");
    } else {
        check(WP_FALSE, "offload_query() succeeded");
    }

    check(build_fixture_doc(&doc) == WP_OK, "built fixture document");
    check(format_rtf_export(&doc, rtf_buf, sizeof(rtf_buf), &rtf_len) == WP_OK,
          "format_rtf_export() of fixture succeeded");
    wp_document_free(&doc);

    /* --- CONVERT_EXPORT to ODT, over real serial --- */
    req_buf[0] = (u8)FORMAT_ODT;
    memcpy(req_buf + 1, rtf_buf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_buf;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 30000;

    if (offload_submit(&req, &resp) == WP_OK) {
        check(resp.status == OFFLOAD_OK, "CONVERT_EXPORT(ODT) over serial returned OFFLOAD_OK");
        check(resp.result_len > 4 && resp.result_len <= sizeof(odt_bytes) &&
              resp.result[0] == 'P' && resp.result[1] == 'K',
              "CONVERT_EXPORT(ODT) response is a real ZIP container (PK magic)");
        if (resp.result_len <= sizeof(odt_bytes)) {
            odt_len = resp.result_len;
            memcpy(odt_bytes, resp.result, odt_len);
        }
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "CONVERT_EXPORT(ODT) over serial returned OFFLOAD_OK");
        check(WP_FALSE, "CONVERT_EXPORT(ODT) response is a real ZIP container (PK magic)");
    }

    /* --- CONVERT_IMPORT of those SAME ODT bytes, over the same serial
       connection: proves the request/response loop handles more than
       one request (already proven for THESAURUS in T6.3; now proven for
       CONVERT_* too), and closes the round trip without needing any
       external tool on this side. --- */
    if (odt_len > 0) {
        req_buf[0] = (u8)FORMAT_ODT;
        memcpy(req_buf + 1, odt_bytes, odt_len);
        req.op = OFFLOAD_CONVERT_IMPORT;
        req.payload = req_buf;
        req.payload_len = odt_len + 1;
        req.timeout_ms = 30000;

        if (offload_submit(&req, &resp) == WP_OK) {
            check(resp.status == OFFLOAD_OK, "CONVERT_IMPORT(ODT) over serial returned OFFLOAD_OK");
            if (resp.status == OFFLOAD_OK) {
                wp_document doc2;
                if (format_rtf_import(resp.result, resp.result_len, &doc2) == WP_OK) {
                    wp_section *sec2 = wp_document_section_get(&doc2, 0);
                    wp_paragraph *para2 = wp_section_paragraph_get(sec2, 0);
                    const wp_para_style *ps2 = wp_document_pstyle_get(&doc2, para2->style);
                    wp_run *run2 = wp_paragraph_run_get(para2, 0);
                    const wp_char_style *cs2 = wp_document_cstyle_get(&doc2, run2->style);

                    check(ps2->align == WP_ALIGN_CENTER, "round-tripped paragraph kept its alignment");
                    check((cs2->flags & WP_STYLE_BOLD) != 0, "round-tripped run kept its bold flag");
                    check(run2->text.length == 29 &&
                          memcmp(wp_strpool_get(&doc2.strings, run2->text),
                                 "ODT and PDF over real RS-232.", 29) == 0,
                          "round-tripped text matches the original fixture exactly");
                    wp_document_free(&doc2);
                } else {
                    check(WP_FALSE, "format_rtf_import() of the round-tripped RTF succeeded");
                }
            }
            offload_response_free(&resp);
        } else {
            check(WP_FALSE, "CONVERT_IMPORT(ODT) over serial returned OFFLOAD_OK");
        }
    } else {
        check(WP_FALSE, "CONVERT_IMPORT(ODT) over serial returned OFFLOAD_OK");
    }

    /* --- CONVERT_EXPORT to PDF, over real serial --- */
    req_buf[0] = (u8)FORMAT_PDF;
    memcpy(req_buf + 1, rtf_buf, rtf_len);
    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_buf;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = 30000;

    if (offload_submit(&req, &resp) == WP_OK) {
        check(resp.status == OFFLOAD_OK, "CONVERT_EXPORT(PDF) over serial returned OFFLOAD_OK");
        check(resp.result_len > 4 && memcmp(resp.result, "%PDF", 4) == 0,
              "CONVERT_EXPORT(PDF) response starts with the real %PDF magic");
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "CONVERT_EXPORT(PDF) over serial returned OFFLOAD_OK");
        check(WP_FALSE, "CONVERT_EXPORT(PDF) response starts with the real %PDF magic");
    }

    offload_shutdown();
    check(!offload_available(), "offload_available() false after offload_shutdown");

    nf_log("ATARI CONVERT SMOKE: done\r\n");
    if (g_fail == 0) {
        nf_log("ATARI CONVERT SMOKE: ALL PASSED\r\n");
        nf_exit_now(0);
    } else {
        nf_log("ATARI CONVERT SMOKE: FAILURES PRESENT\r\n");
        nf_exit_now(1);
    }

    return 0;
}
