/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SERHW.PRG -- serial diagnostic for REAL Atari hardware.
 *
 * WHY THIS EXISTS SEPARATELY FROM atari_serial_smoke.c
 * ----------------------------------------------------
 * SERIALTST.PRG cannot run on a real machine at all. It reports through
 * the Native Features interface, whose entry point is `.word 0x7300` --
 * and 0x7300 is not a valid MOVEQ (bit 8 is set), so it is an ILLEGAL
 * INSTRUCTION on a real 68000/68030. Emulators trap it; hardware takes
 * the exception. Every existing smoke binary in this project does this
 * unconditionally in its first few instructions, which makes them all
 * emulator-only by construction. Running one on the Falcon would crash
 * before printing anything.
 *
 * So this program uses no NatFeats anywhere. Everything it has to say
 * goes to the console through Cconws(), and it waits for a key at the
 * end so results survive on screen when launched from a desktop.
 *
 * WHAT IT ANSWERS
 * ---------------
 * CI can prove the endpoint parses and that Rsconf accepts it. What CI
 * structurally cannot prove is anything about a real cable: Hatari's
 * emulated RS-232 has no CTS line to assert, so FLOW_HARD has never been
 * exercised for real. This program sweeps the configurations that matter
 * and reports which ones actually complete a HELLO/CAPS handshake with
 * the gateway, which is the one signal that proves cable, wiring, baud,
 * flow control and framing all at once.
 *
 * It also reports what machine it is on and which physical port `aux:`
 * is currently mapped to, because on a Falcon that is not the ST answer
 * and getting it wrong looks exactly like a dead cable.
 */

#include <gem.h>
#include <mint/osbind.h>
#include <mint/cookie.h>
#include <string.h>
#include "types.h"
#include "offload.h"
#include "platform.h"

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

static void say_hex(unsigned long v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) { buf[i] = digits[v & 0xF]; v >>= 4; }
    buf[8] = '\0';
    say(buf);
}

/* Cookie-jar reads and the 200Hz timer both live below the supervisor
   boundary, so they are reached through Supexec rather than poked at
   directly -- on a MiNT/MagiC system in particular, touching low memory
   from user mode is a memory violation, not merely bad manners. */
static long g_cookie_want;
static long g_cookie_got;

static long probe_cookie(void)
{
    long *jar = *(long **)0x5A0L;

    g_cookie_got = 0;
    if (jar == NULL) return 0;
    while (jar[0] != 0) {
        if (jar[0] == g_cookie_want) { g_cookie_got = jar[1]; return 1; }
        jar += 2;
    }
    return 0;
}

static long cookie(long id, long *value_out)
{
    long present;

    g_cookie_want = id;
    present = Supexec(probe_cookie);
    if (value_out) *value_out = g_cookie_got;
    return present;
}

static long read_hz200(void) { return *(volatile long *)0x4BAL; }
static long ticks(void) { return Supexec(read_hz200); }

/* os_version is a WORD at offset 2 of the OS header, whose address sits
   in _sysbase (0x4F2). */
static long read_tos_version(void)
{
    unsigned short *osheader = *(unsigned short **)0x4F2L;
    return (long)osheader[1];
}

static void report_machine(void)
{
    long v = 0;

    say("\r\n--- machine ---\r\n");

    say("  _MCH   : ");
    if (cookie(0x5F4D4348L /* _MCH */, &v)) {
        say_hex((unsigned long)v);
        say("  (");
        switch (v >> 16) {
        case 0:  say("ST");            break;
        case 1:  say("STE");           break;
        case 2:  say("TT030");         break;
        case 3:  say("Falcon030");     break;
        default: say("unknown");       break;
        }
        say(")\r\n");
    } else {
        say("absent (plain ST)\r\n");
    }

    say("  TOS    : ");
    say_hex((unsigned long)Supexec(read_tos_version));
    say("\r\n");

    say("  MiNT   : ");
    if (cookie(0x4D694E54L /* _MiNT */, &v)) { say("yes, version "); say_hex((unsigned long)v); say("\r\n"); }
    else say("no\r\n");

    say("  MagiC  : ");
    if (cookie(0x4D616758L /* MagX */, &v)) { say("yes\r\n"); }
    else say("no\r\n");

    say("  HSMODEM: ");
    if (cookie(0x4853454DL /* HSEM */, &v)) say("present (baud above 19200 may be reachable)\r\n");
    else say("absent (Rsconf ceiling of 19200 applies)\r\n");
}

/* Which physical port `aux:` currently means. This matters more than it
   looks: on a Falcon the ST's MFP RS-232 is not the same device, and
   Bconmap decides which one every Bconin/Bconout/Rsconf call below
   actually talks to. A correct cable on the wrong port is
   indistinguishable from a broken one. */
/* The device NUMBERS are shared across machines but the NAMES are not,
   and printing the wrong machine's name is actively misleading. Straight
   from the Compendium's serial-port-mapping table (p.4.17):

       Device   Mega ST             TT030              Falcon030
         6      Modem 1 (ST MFP)    Modem 1 (ST MFP)   --  (absent)
         7      Modem 2 (SCC B)     Modem 2 (SCC B)    Modem (SCC B)
         8      Serial/LAN (SCC A)  Serial 1 (TT MFP)  LAN (SCC A)
         9      --                  Serial 2/LAN(SCCA) --  (absent)

   A Falcon has NO device 6: there is no ST-MFP serial port on it, and
   its single DB9 is device 7, named just "Modem". Reporting that as
   "Modem 2" -- the Mega ST/TT name -- reads as "you are on the second
   port of a machine that only has one", which sent a real bring-up
   looking for a port that does not exist. Labelled per machine now. */
static void report_bconmap(void)
{
    long dev;
    long mch = 0;
    int is_falcon;

    cookie(0x5F4D4348L /* _MCH */, &mch);
    is_falcon = ((mch >> 16) == 3);

    say("\r\n--- aux: mapping ---\r\n");
    say("  Bconmap(-1) -> ");
    dev = Bconmap(-1);
    say_num(dev);

    if (is_falcon) {
        switch (dev) {
        case 0:  say("  (Bconmap unavailable)");                          break;
        case 6:  say("  (?! device 6 does not exist on a Falcon)");       break;
        case 7:  say("  (Modem, SCC channel B -- the DB9 on the back,");
                 say("\r\n                 and the Falcon's ONLY serial port)"); break;
        case 8:  say("  (LAN, SCC channel A -- NOT the DB9)");            break;
        default: say("  (unexpected on a Falcon)");                       break;
        }
    } else {
        switch (dev) {
        case 0:  say("  (Bconmap unavailable -- ST-style single port)"); break;
        case 6:  say("  (Modem 1, ST MFP)");                             break;
        case 7:  say("  (Modem 2, SCC channel B)");                      break;
        case 8:  say("  (Serial/LAN, SCC channel A)");                   break;
        default: say("  (see XBIOS serial port mapping)");               break;
        }
    }
    say("\r\n");
}

typedef struct {
    const char *endpoint;
    const char *label;
} attempt;

static const attempt g_attempts[] = {
    { "serial:aux:9600",        " 9600 baud, no flow control " },
    { "serial:aux:9600:hard",   " 9600 baud, RTS/CTS          " },
    { "serial:aux:19200",       "19200 baud, no flow control  " },
    { "serial:aux:19200:hard",  "19200 baud, RTS/CTS          " }
};

static int try_one(const attempt *a)
{
    offload_capabilities caps;
    long t0, t1;
    wp_status st;

    say("  ");
    say(a->label);
    say(": ");

    t0 = ticks();
    st = offload_init(a->endpoint);
    t1 = ticks();

    if (st != WP_OK) {
        say("no handshake (");
        say_num((t1 - t0) * 5);
        say(" ms)\r\n");
        offload_shutdown();
        return 0;
    }

    say("HANDSHAKE OK in ");
    say_num((t1 - t0) * 5);
    say(" ms");

    if (offload_query(&caps) == WP_OK) {
        say(", gateway \"");
        {
            char nm[33];
            u32 i;
            for (i = 0; i < 32 && caps.gateway_name[i] != 0; i++) nm[i] = (char)caps.gateway_name[i];
            nm[i] = '\0';
            say(nm);
        }
        say("\", ops=");
        say_hex((unsigned long)caps.ops_supported);
    }
    say("\r\n");

    offload_shutdown();
    return 1;
}

int main(void)
{
    u32 i;
    int worked = 0;

    say("\033E");   /* VT52 clear screen -- TOS console understands this */
    say("RetroWP serial hardware diagnostic (SERHW.PRG)\r\n");
    say("No Native Features calls: safe on real hardware.\r\n");

    report_machine();
    report_bconmap();

    say("\r\n--- handshake sweep ---\r\n");
    say("  Start the gateway on the other end FIRST, matching baud and\r\n");
    say("  flow control (--serial-baud / --serial-flow rtscts).\r\n\r\n");

    for (i = 0; i < sizeof(g_attempts) / sizeof(g_attempts[0]); i++) {
        worked += try_one(&g_attempts[i]);
    }

    say("\r\n--- result ---\r\n  ");
    if (worked == 0) {
        say("NOTHING handshook. Check: gateway running? correct port for\r\n");
        say("  the aux: mapping above? TX/RX not swapped? For the RTS/CTS\r\n");
        say("  rows specifically, pins 4 and 5 must actually be wired --\r\n");
        say("  a 3-wire cable can never pass those two.\r\n");
    } else {
        say_num(worked);
        say(" of 4 configurations handshook.\r\n");
        say("  Build RetroWP with the fastest working one, e.g.\r\n");
        say("  -DWP_ATARI_OFFLOAD_ENDPOINT='\"serial:aux:19200:hard\"'\r\n");
    }

    say("\r\nPress any key.\r\n");
    Cconin();
    return 0;
}
