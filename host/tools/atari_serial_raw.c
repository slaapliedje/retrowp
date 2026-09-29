/* SPDX-License-Identifier: GPL-2.0-or-later */
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
    int pass;
    short key_pressed = 0;

    say("\033E");
    say("RetroWP raw serial test (SERRAW.PRG)\r\n");
    say("No protocol, no framing -- just bytes, each direction alone.\r\n\r\n");

    say("  aux: is Bconmap device ");
    say_num(Bconmap(-1));
    say("\r\n");

    /* ---- Phase 1: transmit, A/B on the TSR transmitter-enable bit ----
     *
     * This is the experiment the whole program exists for.
     *
     * plat_transport_open passes tsr = RS_INQUIRE (-1), which Rsconf
     * documents as "read and leave unmodified" (Compendium p.4.90). Bit
     * 0x01 of tsr is the TRANSMITTER enable, so the transport enables
     * the receiver and never touches the transmitter. On an ST that is
     * harmless -- TOS has already enabled the MFP's transmitter by the
     * time any program runs -- but the Falcon's port is an SCC reached
     * through the same BIOS call, and nothing guarantees the same
     * starting state there.
     *
     * If pass B sends bytes and pass A does not, that is the bug, it is
     * ours, and it explains a Falcon that times out every write while
     * the identical code works on every ST. Run both rather than
     * reasoning about it: the two differ by one register bit. */
    for (pass = 0; pass < 3; pass++) {
        switch (pass) {
        case 0:
            say("--- TX pass A: tsr untouched (what the transport does today)\r\n");
            Rsconf(BAUD_9600, FLOW_NONE,
                   (short)(RS_8BITS | RS_1STOP | RS_CLK16), RS_RECVENABLE,
                   (short)RS_INQUIRE, (short)RS_INQUIRE);
            break;
        case 1:
            say("--- TX pass B: tsr bit0 set (transmitter explicitly enabled)\r\n");
            Rsconf(BAUD_9600, FLOW_NONE,
                   (short)(RS_8BITS | RS_1STOP | RS_CLK16), RS_RECVENABLE,
                   (short)RS_RECVENABLE, (short)RS_INQUIRE);
            break;
        default:
            /* Pass C: touch NOTHING but baud and flow.
             *
             * ucr/rsr/tsr are MFP 68901 register bit patterns. On a
             * Falcon aux: is the SCC (Z85C30), an entirely different
             * chip, so those values are at best meaningless there and at
             * worst set something harmful -- the SCC's Auto Enables bit
             * gates the transmitter on CTS, which is exactly the shape
             * of a Bcostat that never reports ready.
             *
             * RS_INQUIRE for all three means "read and leave unmodified"
             * (Compendium p.4.90), so TOS's own boot-time configuration
             * stands and we set only the two things we actually have an
             * opinion about. If this pass transmits while A and B do
             * not, the transport has been mis-initialising the port on
             * every SCC-based machine since it was written. */
            say("--- TX pass C: baud+flow ONLY, registers untouched (RS_INQUIRE)\r\n");
            Rsconf(BAUD_9600, FLOW_NONE,
                   (short)RS_INQUIRE, (short)RS_INQUIRE,
                   (short)RS_INQUIRE, (short)RS_INQUIRE);
            break;
        }

        sent = 0;
        notready = 0;
        start = ticks();
        t_end = start + 6L * 200L;      /* _hz_200 ticks at 200Hz */
        while (ticks() < t_end) {
            for (i = 0; pattern[i] != '\0'; i++) {
                if (Bcostat(AUX_DEV) == 0) {
                    notready++;
                    continue;
                }
                Bconout(AUX_DEV, (short)(unsigned char)pattern[i]);
                sent++;
            }
        }
        say("    bytes sent via Bconout : "); say_num(sent);     say("\r\n");
        say("    Bcostat said NOT ready : "); say_num(notready); say("\r\n");
    }
    say("\r\n");
    if (sent == 0) {
        say("  *** pass B sent nothing either -- the transmitter never came\r\n");
        say("      ready even when explicitly enabled. Not the TSR bit.\r\n");
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
    if (got == 0 && sent > 0) {
        say("  *** we transmitted but received nothing.\r\n");
        say("      Now check the FAR END's receive count too, because the\r\n");
        say("      two cases look identical from here and need opposite\r\n");
        say("      fixes:\r\n");
        say("        far end DID receive us -> only the return path is\r\n");
        say("            broken: their TX, or our RX pin.\r\n");
        say("        far end received NOTHING either -> both TX lines are\r\n");
        say("            transmitting into the other's TX. The number of\r\n");
        say("            crossovers between the two machines is wrong --\r\n");
        say("            two DTEs need exactly ONE. A null-modem cable AND\r\n");
        say("            a swap-jumpered adapter is two, which cancels to a\r\n");
        say("            straight-through and is indistinguishable from a\r\n");
        say("            dead cable. So is zero crossovers.\r\n");
    } else if (got == 0) {
        say("  *** nothing sent and nothing arrived -- the port itself is\r\n");
        say("      not reachable.\r\n");
    }

    if (!key_pressed) {
        say("\r\nPress any key.\r\n");
        Cconin();
    }
    return 0;
}
