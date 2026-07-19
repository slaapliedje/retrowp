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
