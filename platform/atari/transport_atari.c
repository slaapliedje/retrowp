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

/* endpoint format: "serial:<device>:<baud>" (platform.h's own doc
   comment already establishes this convention). <device> is currently
   unused/unvalidated beyond "non-empty" -- the ST has exactly one
   serial port (AUX), so there is nothing to select, but the field is
   kept in the endpoint string for forward compatibility with the
   pipe:/tcp: endpoints' shape. */
static wp_bool parse_baud(const char *s, short *out_idx)
{
    long value = 0;
    wp_bool any_digit = WP_FALSE;

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (long)(*s - '0');
        s++;
        any_digit = WP_TRUE;
    }
    if (!any_digit || *s != '\0') return WP_FALSE;

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
    short baud_idx;

    WP_UNUSED(connect_timeout_ms); /* Rsconf takes effect immediately --
                                       there is no connection handshake
                                       at this layer, unlike a TCP dial */

    if (strncmp(endpoint, "serial:", 7) != 0) return WP_UNSUPPORTED;
    rest = endpoint + 7;

    colon = strrchr(rest, ':');
    if (colon == NULL || colon == rest || colon[1] == '\0') return WP_ERR;
    if (!parse_baud(colon + 1, &baud_idx)) return WP_ERR;

    t = (atari_transport *)mem_alloc((u32)sizeof(atari_transport));
    if (t == NULL) return WP_NOMEM;
    t->opened = WP_TRUE;

    /* 8N1, async /16 clock, receiver enabled, no flow control.
       RS_INQUIRE (-1) for tsr/scr means "leave unchanged" -- the same
       -1-means-inquire/no-change idiom mint/ostruct.h uses consistently
       elsewhere (SERIAL_NOCHANGE, DISK_NOCHANGE, PRT_INQUIRE, ...);
       there is no documented transmit-side bit list on this system to
       set explicitly (docs/atari-platform.md). */
    Rsconf(baud_idx, FLOW_NONE,
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
