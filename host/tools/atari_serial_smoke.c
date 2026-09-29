/* SPDX-License-Identifier: GPL-2.0-or-later */
/* T6.3: proves offload_init()'s HELLO/CAPS handshake, and one full
   request/response round trip, actually work over real RS-232 --
   platform/atari/transport_atari.c's Bconin/Bconout/Rsconf
   implementation, exercised through the SAME engine/offload_client.c
   and offload/transport/wire.c code the TCP-backed host tests already
   cover, talking to offload/gateway/mock_gateway.c's --serial-rx/
   --serial-tx mode (bridged to this real emulated serial port via two
   FIFOs and Hatari's --rs232-in/--rs232-out -- see
   host/tools/ci_atari_serial.sh and docs/atari-platform.md).

   Unlike host/tools/atari_smoke.c's self-contained checks, this
   requires a live gateway process running concurrently with Hatari --
   see ci_atari_serial.sh for how that's coordinated. Pass/fail via the
   same NF_STDERR/NF_EXIT trap atari_smoke.c uses. */

#include <stddef.h>     /* NULL, for the plat_transport handles below */
#include "offload.h"
#include "wire.h"
#include "platform.h"

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

static wp_bool str_eq(const u8 *a, const char *b, u32 n)
{
    u32 i;
    for (i = 0; i < n && b[i] != '\0'; i++) {
        if (a[i] != (u8)b[i]) return WP_FALSE;
    }
    return WP_TRUE;
}

int main(void)
{
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    static const u8 word[] = "hello";

    g_nf_stderr_id = nf_get_id("NF_STDERR");
    g_nf_exit_id = nf_get_id("NF_EXIT");

    nf_log("ATARI SERIAL SMOKE: starting\r\n");

    check(!offload_available(), "offload_available() false before offload_init");

    /* Endpoint parsing, including the optional 4th ":<flow>" field and the
       backward-compatibility rules around it. Deliberately BEFORE the
       handshake: offload_init()'s own open() re-runs Rsconf and so resets
       whatever configuration these leave behind.

       Note what is and is not covered here. This proves the endpoint
       PARSES and that Rsconf accepts the result -- it cannot prove
       hardware handshaking works, because Hatari's emulated RS-232 has no
       CTS line to assert. FLOW_HARD on a real machine with a real
       RTS/CTS cable remains untested by CI, by necessity. */
    {
        plat_transport *tp = NULL;

        check(plat_transport_open("serial:aux:9600", 1000, &tp) == WP_OK,
              "endpoint with no flow field opens (defaults to none)");
        if (tp) { plat_transport_close(tp); tp = NULL; }

        check(plat_transport_open("serial:aux:9600:hard", 1000, &tp) == WP_OK,
              "endpoint with :hard parses and opens");
        if (tp) { plat_transport_close(tp); tp = NULL; }

        check(plat_transport_open("serial:aux:19200:hard", 1000, &tp) == WP_OK,
              "19200 (the Rsconf ceiling) with :hard parses and opens");
        if (tp) { plat_transport_close(tp); tp = NULL; }

        /* XON/XOFF must be refused, not quietly accepted: it is in-band
           CTRL-S/CTRL-Q, and this protocol's length/seq/CRC fields carry
           arbitrary binary that would be eaten as flow characters. A
           refused endpoint is the correct outcome; a corrupt frame is
           not. Both spellings that would enable it are checked. */
        check(plat_transport_open("serial:aux:9600:soft", 1000, &tp) != WP_OK,
              ":soft (XON/XOFF) is refused -- in-band bytes corrupt binary frames");
        check(plat_transport_open("serial:aux:9600:both", 1000, &tp) != WP_OK,
              ":both is refused for the same reason as :soft");

        check(plat_transport_open("serial:a:b:9600", 1000, &tp) == WP_OK,
              "device containing a colon still parses as before");
        if (tp) { plat_transport_close(tp); tp = NULL; }

        check(plat_transport_open("serial:aux:9600:bogus", 1000, &tp) != WP_OK,
              "unrecognised flow keyword is rejected");
        check(plat_transport_open("serial:aux:38400", 1000, &tp) != WP_OK,
              "baud above the Rsconf ceiling is rejected");
        check(plat_transport_open("serial:aux:99999999999999999999", 1000, &tp) != WP_OK,
              "absurdly long baud digit run is rejected without overflow");
        check(plat_transport_open("serial::9600", 1000, &tp) != WP_OK,
              "empty device is rejected");
        check(plat_transport_open("tcp:1.2.3.4:9000", 1000, &tp) == WP_UNSUPPORTED,
              "tcp: endpoint is WP_UNSUPPORTED, not silently accepted");
    }

    check(offload_init("serial:aux:9600") == WP_OK,
          "offload_init(serial:aux:9600) handshake succeeded");
    check(offload_available(), "offload_available() true after handshake");

    if (offload_query(&caps) == WP_OK) {
        check(caps.protocol_version == WIRE_VERSION, "negotiated protocol_version matches WIRE_VERSION");
        check(str_eq(caps.gateway_name, "mock-gateway", sizeof(caps.gateway_name)),
              "negotiated gateway_name is \"mock-gateway\"");
    } else {
        check(WP_FALSE, "offload_query() succeeded");
        check(WP_FALSE, "negotiated gateway_name is \"mock-gateway\"");
    }

    req.op = OFFLOAD_THESAURUS;
    req.payload = word;
    req.payload_len = 5;
    req.timeout_ms = 5000;
    if (offload_submit(&req, &resp) == WP_OK) {
        check(resp.status == OFFLOAD_OK, "OFFLOAD_THESAURUS request over serial returned OFFLOAD_OK");
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "OFFLOAD_THESAURUS request over serial returned OFFLOAD_OK");
    }

    offload_shutdown();
    check(!offload_available(), "offload_available() false after offload_shutdown");

    nf_log("ATARI SERIAL SMOKE: done\r\n");
    if (g_fail == 0) {
        nf_log("ATARI SERIAL SMOKE: ALL PASSED\r\n");
        nf_exit_now(0);
    } else {
        nf_log("ATARI SERIAL SMOKE: FAILURES PRESENT\r\n");
        nf_exit_now(1);
    }

    return 0;
}
