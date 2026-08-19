/* SERMINT.PRG -- ask MiNT what the serial port's handshake lines are
 * actually doing, from the Atari's own side.
 *
 * SERRAW.PRG established the shape of the fault on a real Falcon: aux:
 * is Bconmap device 7, RX over that device is perfect (255 bytes of the
 * far end's pattern, decoded cleanly), and TX on the SAME device never
 * moves a single byte -- Bcostat() reports not-ready tens of thousands
 * of times per pass. Three different Rsconf() configurations, including
 * one that touched nothing but baud and flow, came out within 1% of
 * each other. Receive works; transmit alone is gated.
 *
 * That last detail is what points here. Under MiNT, Bconout/Bcostat on
 * aux: are NOT the raw BIOS: MiNT intercepts them and routes through
 * its own serial driver, which keeps its own flow-control state per
 * device. Rsconf() does not reach that state, which is why passes A, B
 * and C were indistinguishable -- all three were no-ops against the
 * thing actually holding the line. A driver that believes CTS is not
 * satisfied refuses to transmit forever while receiving perfectly
 * happily, because receive never consults CTS. That is precisely the
 * observed behaviour.
 *
 * So stop inferring the handshake and read it. MiNT exposes TIOCMGET
 * (mint/dcntl.h), which reports CTS/DCD/RI and our own RTS/DTR as the
 * DRIVER sees them. With the far end holding its RTS asserted (verified
 * independently at the far end's own GPIO, not inferred), this settles
 * the one question a multimeter would otherwise require opening the
 * machine to answer:
 *
 *   CTS reads LOW  -> the far end's RTS is not arriving. The fault is
 *                     in the wiring between the two connectors, and no
 *                     amount of software will fix it.
 *   CTS reads HIGH -> the wiring is fine and the driver is gating for
 *                     its own reasons. A software fix exists.
 *
 * Either answer eliminates half the remaining search space, which no
 * test run so far has managed.
 *
 * Two secondary probes ride along, because both are nearly free once a
 * handle is open and both have a real chance of being the whole story:
 *
 *   TIOCSTART -- restarts output stopped by a received XOFF. If a stray
 *                0x13 arrived during any earlier experiment (plausible:
 *                this port has had assorted junk thrown at it), MiNT
 *                would have stopped output permanently and would report
 *                exactly the not-ready symptom seen. Costs one ioctl.
 *
 *   Fwrite()  -- writing to the device node directly is a DIFFERENT
 *                path through MiNT than Bconout on aux:. If Fwrite
 *                moves bytes where Bconout cannot, the gate is in the
 *                BIOS-emulation layer specifically, and the transport
 *                has a straightforward way around it on MiNT systems.
 *
 * No Native Features, for the same reason as atari_serial_hw.c and
 * atari_serial_raw.c: 0x7300 is an illegal instruction on a real 68030.
 */

#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <mint/dcntl.h>
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

static long read_hz200(void) { return *(volatile long *)0x4BAL; }
static long ticks(void) { return Supexec(read_hz200); }

/* The candidate device nodes, most-likely first. The Falcon's single
 * DB9 is SCC channel B, which MiNT's own serial driver publishes as
 * modem2 -- but naming has never been uniform across MiNT versions and
 * kernel serial drivers, so probe rather than assume. Getting this
 * wrong silently is exactly the class of mistake that cost this
 * investigation a whole evening once already (a device-6 port that does
 * not exist on this machine at all). */
static const char *const dev_paths[] = {
    "/dev/modem2", "u:/dev/modem2",
    "/dev/modem1", "u:/dev/modem1",
    "/dev/serial2", "/dev/serial1",
    "/dev/aux",    "u:/dev/aux",
    (const char *)0
};

static void report_lines(long bits)
{
    say("      RTS="); say_num((bits & TIOCM_RTS) ? 1 : 0);
    say("  DTR=");     say_num((bits & TIOCM_DTR) ? 1 : 0);
    say("   |   CTS=");
    say_num((bits & TIOCM_CTS) ? 1 : 0);
    say("  DCD=");     say_num((bits & TIOCM_CAR) ? 1 : 0);
    say("  RI=");      say_num((bits & TIOCM_RNG) ? 1 : 0);
    say("\r\n");
}

int main(void)
{
    static const char pattern[] = "RETROWP-SERMINT-0123456789\r\n";
    long bits, rc, t_end, wrote, accepted;
    short fd = -1;
    const char *chosen = (const char *)0;
    int i;

    say("\033E");
    say("RetroWP MiNT serial probe (SERMINT.PRG)\r\n");
    say("Reads the handshake lines from THIS side, via MiNT's own driver.\r\n\r\n");

    say("aux: is Bconmap device "); say_num(Bconmap(-1)); say("\r\n\r\n");

    say("--- probing MiNT device nodes ---\r\n");
    for (i = 0; dev_paths[i] != (const char *)0; i++) {
        long h = Fopen(dev_paths[i], 2);
        say("  "); say(dev_paths[i]); say(" -> ");
        if (h < 0) {
            say("open failed ("); say_num(h); say(")\r\n");
            continue;
        }
        say("open ok\r\n");
        bits = 0;
        rc = Fcntl((short)h, (long)&bits, TIOCMGET);
        if (rc < 0) {
            say("      TIOCMGET unsupported here ("); say_num(rc); say(")\r\n");
        } else {
            report_lines(bits);
        }
        if (chosen == (const char *)0 && rc >= 0) {
            chosen = dev_paths[i];
            fd = (short)h;             /* keep this one open for the write test */
        } else {
            Fclose((short)h);
        }
    }

    if (chosen == (const char *)0) {
        say("\r\nNo device node answered TIOCMGET. Either this is not MiNT,\r\n");
        say("or its serial driver publishes different names. The CTS\r\n");
        say("question stays open -- nothing below would mean anything.\r\n");
        say("\r\nPress any key.\r\n");
        Cconin();
        return 1;
    }

    say("\r\nUsing "); say(chosen); say(" for the write test.\r\n\r\n");

    /* Clear any output-stopped-by-XOFF state before drawing conclusions
     * from a failed write. If this is what was wrong, the write below
     * simply succeeds and the whole mystery was one stray 0x13. */
    rc = Fcntl(fd, 0L, TIOCSTART);
    say("--- TIOCSTART (clear any stuck XOFF) -> "); say_num(rc); say("\r\n");

    say("--- Foutstat before writing            -> ");
    say_num(Foutstat(fd)); say("\r\n\r\n");

    say("--- Fwrite test: 6 seconds via the device node, NOT Bconout\r\n");
    accepted = 0;
    t_end = ticks() + 6L * 200L;
    while (ticks() < t_end) {
        wrote = Fwrite(fd, (long)(sizeof(pattern) - 1), pattern);
        if (wrote > 0) accepted += wrote;
        if (wrote < 0) {
            say("    Fwrite returned error "); say_num(wrote); say("\r\n");
            break;
        }
    }
    say("    bytes accepted by Fwrite : "); say_num(accepted); say("\r\n");
    say("    Foutstat after           : "); say_num(Foutstat(fd)); say("\r\n\r\n");

    bits = 0;
    rc = Fcntl(fd, (long)&bits, TIOCMGET);
    say("--- handshake lines after the write attempt\r\n");
    if (rc >= 0) report_lines(bits);

    say("\r\n=== READ THIS ===\r\n");
    if ((bits & TIOCM_CTS) == 0) {
        say("CTS is LOW. The far end IS asserting its RTS, so it is not\r\n");
        say("arriving. This is a wiring fault between the two DB9s --\r\n");
        say("no software change will help.\r\n");
    } else {
        say("CTS is HIGH -- the wiring is good and the handshake is\r\n");
        say("satisfied. If Bconout still cannot send, the gate is inside\r\n");
        say("the driver, and it is fixable in software.\r\n");
    }
    if (accepted > 0) {
        say("Fwrite MOVED BYTES where Bconout could not: use the device\r\n");
        say("node on MiNT systems, not the BIOS aux: path.\r\n");
    }

    Fclose(fd);
    say("\r\nPress any key.\r\n");
    Cconin();
    return 0;
}
