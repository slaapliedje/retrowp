#include <stddef.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
#include "platform.h"

/* Real RS-232 transport (T6.3) over the ST's AUX BIOS device (DEV_AUX=1,
   the classic Bconin/Bconout/Bconstat/Bcostat convention -- verified
   against mint/ostruct.h, not guessed). No MiNTnet variant: FreeMiNT's
   BSD-socket GEMDOS extensions (Fsocket et al, function numbers
   0x160+) simply don't exist under plain TOS/EmuTOS, which is this
   project's target -- see docs/atari-platform.md for how that was
   confirmed. `endpoint` prefixes other than "serial:" (including
   "tcp:") are WP_UNSUPPORTED here, honestly, not silently accepted. */

/* endpoint format: "serial:<device>:<baud>[:<flow>]" (platform.h's own
   doc comment already establishes the first three fields). <device> is
   currently unused/unvalidated beyond "non-empty" -- the ST has exactly
   one serial port (AUX), so there is nothing to select, but the field is
   kept in the endpoint string for forward compatibility with the
   pipe:/tcp: endpoints' shape.

   BAUD CEILING. 19200 is not a limit this file invented: it is the top
   of Rsconf()'s speed table (BAUD_19200 == 0 in mint/ostruct.h, and the
   XBIOS #15 documentation lists exactly 0=19200 .. 15=50). On the ST
   that ceiling is the MFP 68901's timer-D baud generator. Machines with
   an SCC (Z85C30 -- Mega STE, TT, Falcon) can drive the line far faster,
   but not through this BIOS entry point: reaching those rates needs
   either HSMODEM (which extends the BIOS with additional speed indices)
   or direct SCC register programming (WR12/WR13 time constants), both of
   which are machine-specific and outside the plain-TOS baseline this
   backend targets. So the table below is complete and correct for the
   interface actually being used; a faster link is a separate piece of
   work, not a missing case here. */
static wp_bool parse_baud(const char *s, const char *end, short *out_idx)
{
    long value = 0;
    wp_bool any_digit = WP_FALSE;

    while (s < end && *s >= '0' && *s <= '9') {
        value = value * 10 + (long)(*s - '0');
        /* Bail the moment we exceed the fastest supported rate. Besides
           rejecting junk early, this bounds `value` so a long digit run
           cannot overflow a 32-bit long (signed overflow is undefined,
           and the previous unbounded loop could reach it). */
        if (value > 19200L) return WP_FALSE;
        s++;
        any_digit = WP_TRUE;
    }
    if (!any_digit || s != end) return WP_FALSE;

    switch (value) {
    case 19200: *out_idx = BAUD_19200; return WP_TRUE;
    case 9600:  *out_idx = BAUD_9600;  return WP_TRUE;
    case 4800:  *out_idx = BAUD_4800;  return WP_TRUE;
    case 3600:  *out_idx = BAUD_3600;  return WP_TRUE;
    case 2400:  *out_idx = BAUD_2400;  return WP_TRUE;
    case 2000:  *out_idx = BAUD_2000;  return WP_TRUE;
    case 1800:  *out_idx = BAUD_1800;  return WP_TRUE;
    case 1200:  *out_idx = BAUD_1200;  return WP_TRUE;
    case 600:   *out_idx = BAUD_600;   return WP_TRUE;
    case 300:   *out_idx = BAUD_300;   return WP_TRUE;
    case 200:   *out_idx = BAUD_200;   return WP_TRUE;
    case 150:   *out_idx = BAUD_150;   return WP_TRUE;
    case 134:   *out_idx = BAUD_134;   return WP_TRUE;
    case 110:   *out_idx = BAUD_110;   return WP_TRUE;
    case 75:    *out_idx = BAUD_75;    return WP_TRUE;
    case 50:    *out_idx = BAUD_50;    return WP_TRUE;
    default:    return WP_FALSE;
    }
}

/* Optional 4th endpoint field. OPT-IN ON PURPOSE, and the default stays
   "none" (i.e. exactly the previous behaviour): a great many retro
   serial rigs are wired with a 3-wire cable (TX/RX/GND only). On such a
   cable CTS is never asserted, so with FLOW_HARD selected Bcostat()
   never reports ready and every plat_transport_write would time out --
   a silent default of "hard" would turn a working link into a dead one.
   The caller has to say it wants handshaking, and is thereby asserting
   the cable actually carries RTS/CTS.

   Worth it where the cable does: FLOW_NONE with a polled BIOS AUX device
   has no way to push back on a fast sender, which is survivable for this
   protocol's short CRC-framed request/response traffic (a dropped byte
   costs one frame, and wire.c resyncs) but not for sustained bulk. */
static wp_bool parse_flow(const char *s, short *out_flow)
{
    if (strcmp(s, "none") == 0) { *out_flow = FLOW_NONE; return WP_TRUE; }
    if (strcmp(s, "soft") == 0) { *out_flow = FLOW_SOFT; return WP_TRUE; }
    if (strcmp(s, "hard") == 0) { *out_flow = FLOW_HARD; return WP_TRUE; }
    if (strcmp(s, "both") == 0) { *out_flow = FLOW_BOTH; return WP_TRUE; }
    return WP_FALSE;
}

/* strrchr() bounded by an explicit end, since the fields of the endpoint
   are not individually NUL-terminated and C89 has no strndup to lean on. */
static const char *last_colon_before(const char *s, const char *end)
{
    const char *found = NULL;
    const char *p;

    for (p = s; p < end; p++) {
        if (*p == ':') found = p;
    }
    return found;
}

/* AUX is a single global BIOS device -- no per-open state beyond a
   non-NULL handle is strictly needed, but a real (trivial) allocation
   keeps this backend symmetric with plat_transport_close's mem_free
   and every other backend's opaque-handle convention. */
typedef struct {
    wp_bool opened;
} atari_transport;

wp_status plat_transport_open(const char *endpoint, u32 connect_timeout_ms,
                               plat_transport **out)
{
    atari_transport *t;
    const char *rest;
    const char *colon;
    const char *baud_end;
    short baud_idx;
    short flow;

    WP_UNUSED(connect_timeout_ms); /* Rsconf takes effect immediately --
                                       there is no connection handshake
                                       at this layer, unlike a TCP dial */

    if (strncmp(endpoint, "serial:", 7) != 0) return WP_UNSUPPORTED;
    rest = endpoint + 7;

    /* The trailing field is consumed as <flow> ONLY if it actually spells
       a flow keyword. That keeps every previously valid endpoint parsing
       to the identical result -- including a <device> containing colons,
       which the old strrchr() form accepted ("serial:a:b:9600" still
       means device "a:b", baud 9600, and not a device "a" with a bogus
       flow of "9600"). */
    flow = FLOW_NONE;
    baud_end = rest + strlen(rest);
    colon = strrchr(rest, ':');
    if (colon != NULL && parse_flow(colon + 1, &flow)) {
        baud_end = colon;
    }

    colon = last_colon_before(rest, baud_end);
    if (colon == NULL || colon == rest) return WP_ERR; /* need a non-empty <device> */
    if (!parse_baud(colon + 1, baud_end, &baud_idx)) return WP_ERR;

    t = (atari_transport *)mem_alloc((u32)sizeof(atari_transport));
    if (t == NULL) return WP_NOMEM;
    t->opened = WP_TRUE;

    /* 8N1, async /16 clock, receiver enabled, flow control as requested
       by the endpoint (FLOW_NONE unless asked otherwise).
       RS_INQUIRE (-1) for tsr/scr means "leave unchanged" -- the same
       -1-means-inquire/no-change idiom mint/ostruct.h uses consistently
       elsewhere (SERIAL_NOCHANGE, DISK_NOCHANGE, PRT_INQUIRE, ...);
       there is no documented transmit-side bit list on this system to
       set explicitly (docs/atari-platform.md). */
    Rsconf(baud_idx, flow,
           (short)(RS_8BITS | RS_1STOP | RS_CLK16),
           RS_RECVENABLE,
           (short)RS_INQUIRE, (short)RS_INQUIRE);

    *out = (plat_transport *)t;
    return WP_OK;
}

/* Bconstat(DEV_AUX) is a non-blocking "is a byte ready" poll; Bconin
   only blocks if called without checking that first. Waits up to
   timeout_ms for the FIRST byte (matching platform.h's documented
   contract), then drains whatever else is immediately available (no
   further waiting) up to out_cap -- Bconin only ever returns one byte
   per call, unlike a socket read(), so draining here means callers
   assembling a multi-byte frame (offload/transport/wire.c) need fewer
   plat_transport_read round trips than a strict one-byte-per-call
   implementation would cost them. */
wp_status plat_transport_read(plat_transport *t, u8 *out, u32 out_cap,
                               u32 timeout_ms, u32 *out_len)
{
    u32 deadline;
    u32 n = 0;

    WP_UNUSED(t);
    if (out_len) *out_len = 0;
    if (out_cap == 0) return WP_OK;

    deadline = plat_ticks_ms() + timeout_ms;
    while (Bconstat(DEV_AUX) == 0) {
        if (plat_ticks_ms() >= deadline) return WP_TIMEOUT;
    }

    while (n < out_cap && Bconstat(DEV_AUX) != 0) {
        out[n++] = (u8)(Bconin(DEV_AUX) & 0xFF);
    }

    if (out_len) *out_len = n;
    return WP_OK;
}

/* Bcostat(DEV_AUX) is the transmit-side non-blocking "ok to send"
   poll, mirroring Bconstat. Each byte gets its own fresh timeout_ms
   budget, matching platform/host/transport_host.c's per-poll-call
   convention for plat_transport_write. */
wp_status plat_transport_write(plat_transport *t, const u8 *data, u32 len,
                                u32 timeout_ms)
{
    u32 i;

    WP_UNUSED(t);
    for (i = 0; i < len; i++) {
        u32 deadline = plat_ticks_ms() + timeout_ms;

        while (Bcostat(DEV_AUX) == 0) {
            if (plat_ticks_ms() >= deadline) return WP_TIMEOUT;
        }
        Bconout(DEV_AUX, data[i]);
    }
    return WP_OK;
}

void plat_transport_close(plat_transport *t)
{
    mem_free(t);
}
