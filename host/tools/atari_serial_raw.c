/* SERRAW.PRG -- raw, protocol-free serial test for real hardware.
 *
 * SERHW.PRG proved the machine, the aux: mapping and that nothing
 * crashes, but its only verdict is "handshake or no handshake", and a
 * handshake needs BOTH directions plus correct framing. When it reports
 * nothing at all -- as it did on the Falcon, with the far end seeing
 * zero bytes -- that single verdict cannot say which half is broken.
 *
 * This tests the two directions SEPARATELY, with no protocol involved
 * at all: no framing, no CRC, no HELLO. Just bytes.
 *
 *   Phase 1 (TX): send a recognisable pattern for a fixed period and
 *                 report how many bytes the BIOS accepted, plus how
 *                 often Bcostat() said the transmitter was NOT ready.
 *                 A high not-ready count means something is holding the
 *                 line off -- flow control waiting on a CTS that never
 *                 comes, or another driver owning the port.
 *
 *   Phase 2 (RX): display anything that arrives, as hex and ASCII, so
 *                 the far end can send a known string and the operator
 *                 can see whether it lands, and whether it lands intact
 *                 or garbled (garbled = wrong baud; nothing = wiring).
 *
 * Splitting them matters because the two failures need opposite fixes:
 * TX dead with RX alive is a transmit-path or crossover problem at this
 * end, both dead points at the port being owned by something else or
 * the cable not being connected to the port aux: actually names.
 *
 * No Native Features, for the same reason as atari_serial_hw.c: 0x7300
 * is an illegal instruction on real hardware.
 */

#include <gem.h>
#include <mint/osbind.h>
#include "types.h"

#define AUX_DEV 1

static void say(const char *s) { Cconws(s); }

static void say_num(long v)
{
    char buf[16];
    int i = 15;
    long uv = (v < 0) ? -v : v;

    buf[i--] = '\0';
    if (uv == 0) buf[i--] = '0';
    while (uv > 0 && i >= 0) { buf[i--] = (char)('0' + (int)(uv % 10)); uv /= 10; }
    if (v < 0 && i >= 0) buf[i--] = '-';
    say(&buf[i + 1]);
}

static void say_hex2(unsigned char v)
{
    static const char d[] = "0123456789ABCDEF";
    char b[3];
    b[0] = d[(v >> 4) & 0xF];
    b[1] = d[v & 0xF];
    b[2] = '\0';
    say(b);
}

static long read_hz200(void) { return *(volatile long *)0x4BAL; }
static long ticks(void) { return Supexec(read_hz200); }

int main(void)
{
    static const char pattern[] = "RETROWP-SERRAW-0123456789\r\n";
    long t_end, sent = 0, notready = 0, got = 0;
    long start;
    int i;
    short key_pressed = 0;

    say("\033E");
    say("RetroWP raw serial test (SERRAW.PRG)\r\n");
    say("No protocol, no framing -- just bytes, each direction alone.\r\n\r\n");

    say("  aux: is Bconmap device ");
    say_num(Bconmap(-1));
    say("\r\n");

    /* 9600, no flow control, 8N1. Lowest common denominator on purpose:
       if anything works at all it should work here, and FLOW_NONE means
       a missing CTS cannot be the reason nothing leaves. */
    Rsconf(BAUD_9600, FLOW_NONE,
           (short)(RS_8BITS | RS_1STOP | RS_CLK16), RS_RECVENABLE,
           (short)RS_INQUIRE, (short)RS_INQUIRE);
    say("  configured 9600 8N1, no flow control\r\n\r\n");

    /* ---- Phase 1: transmit ---- */
    say("--- TX: sending for 10 seconds ---\r\n");
    say("  Watch the far end for \"RETROWP-SERRAW\".\r\n");
    start = ticks();
    t_end = start + 10L * 200L;     /* _hz_200 ticks at 200Hz */
    while (ticks() < t_end) {
        for (i = 0; pattern[i] != '\0'; i++) {
            if (Bcostat(AUX_DEV) == 0) {
                notready++;
                continue;           /* transmitter busy; skip, keep counting */
            }
            Bconout(AUX_DEV, (short)(unsigned char)pattern[i]);
            sent++;
        }
    }
    say("  bytes handed to Bconout : "); say_num(sent);    say("\r\n");
    say("  Bcostat said NOT ready  : "); say_num(notready); say("\r\n");
    if (sent == 0) {
        say("  *** nothing could be sent at all -- the BIOS never reported\r\n");
        say("      the transmitter ready. Something else owns this port,\r\n");
        say("      or aux: is not the connector the cable is on.\r\n");
    }
    say("\r\n");

    /* ---- Phase 2: receive ---- */
    say("--- RX: listening for 20 seconds ---\r\n");
    say("  Send something from the far end NOW.\r\n  ");
    t_end = ticks() + 20L * 200L;
    while (ticks() < t_end) {
        if (Bconstat(AUX_DEV) != 0) {
            long c = Bconin(AUX_DEV) & 0xFF;
            got++;
            say_hex2((unsigned char)c);
            say(" ");
            if ((got % 16) == 0) say("\r\n  ");
        }
        /* Let the operator abort early once they have seen enough. */
        if (Cconis() != 0) { Cconin(); key_pressed = 1; break; }
    }
    say("\r\n  bytes received: "); say_num(got); say("\r\n");
    if (got == 0) {
        say("  *** nothing arrived. Combined with the TX count above:\r\n");
        say("      TX>0 and RX=0  -> our transmit works, their->our path\r\n");
        say("                        is broken (RX wiring, or crossover).\r\n");
        say("      TX=0 and RX=0  -> the port itself is not reachable.\r\n");
    }

    if (!key_pressed) {
        say("\r\nPress any key.\r\n");
        Cconin();
    }
    return 0;
}
