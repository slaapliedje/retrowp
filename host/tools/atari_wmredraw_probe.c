/* A second AES process whose only job is to cover and uncover RetroWP's
   window, so that RetroWP is forced to service a real, partial
   WM_REDRAW from a genuinely independent process.

   WHY THIS EXISTS
   ---------------
   host/tools/atari_smoke.c proves the backend's seams in isolation, and
   ci_atari_matrix.sh proves it across six real TOS versions -- but all
   of those are SINGLE-TASKING, so nothing there can ever cover RetroWP's
   window with another program's window. Under plain TOS, omitting
   wind_update(BEG_UPDATE/END_UPDATE) or graf_mouse(M_OFF/M_ON) around
   drawing is merely cosmetic; under XaAES the same omission is a
   correctness bug (see host/tools/afros_ci_lib.sh's header). This probe
   is what actually exercises that path: it opens a window over RetroWP,
   holds it there, then closes it, leaving RetroWP an exposed rectangle
   it must repaint from its own document model.

   WHY NOT JUST CLICK THE TASKBAR
   ------------------------------
   ARAnyM under Xvfb does not track host pointer motion at all (neither
   an absolute XWarpPointer nor a relative delta reaches the emulated
   mouse, with or without X input focus), and the AES's own Alt+Arrow
   keyboard mouse emulation -- which does work -- moves in coarse,
   non-uniform steps that make small targets like a taskbar button
   unreliable to hit. Driving the cover/uncover cycle from inside the
   emulated machine sidesteps the host input layer entirely and, unlike
   a hand-aimed click, is exactly reproducible run to run.

   HOW THE HOST SYNCHRONISES WITH IT
   ---------------------------------
   Every phase transition is announced on the host's stderr through the
   NF_STDERR Native Features call, the same channel atari_smoke.c already
   uses. Those markers are a starting gun, not a timestamp: the host
   (host/tools/afros_redraw_check.py) uses each one to begin waiting for
   the screen state it names, and never assumes the screen is already in
   that state when the line arrives. See that file's own header for what
   happened when an earlier version did assume it.

   This program deliberately observes the very discipline it is testing
   for (wind_update + graf_mouse around its own drawing): a prober that
   modelled the bad behaviour could corrupt the screen itself and be
   mistaken for the defect it is meant to detect. */

#include <gem.h>
#include "types.h"

/* Same trap shape, and the same __attribute__((noipa)) requirement, as
   host/tools/atari_smoke.c -- see its own comment for why that attribute
   is load-bearing at -O2 rather than decorative. */
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

static void nf_log(const char *msg)
{
    if (g_nf_stderr_id) nf_call1(g_nf_stderr_id, (long)msg);
}

/* Same no-stdio decimal formatter atari_smoke.c carries, trimmed to the
   non-negative case (every value logged here is a screen coordinate). */
static void nf_log_short(short v)
{
    char buf[8];
    int i = 7;
    unsigned short uv = (unsigned short)(v < 0 ? 0 : v);

    buf[i--] = '\0';
    if (uv == 0) {
        buf[i--] = '0';
    } else {
        while (uv > 0 && i >= 0) {
            buf[i--] = (char)('0' + (uv % 10));
            uv = (unsigned short)(uv / 10);
        }
    }
    nf_log(&buf[i + 1]);
}

/* Announces the rectangle this probe is about to cover, so the host-side
   checker compares exactly that region rather than inferring it from a
   screenshot difference. Inferring it worked, but it conflated the cover
   window with every other pixel that happened to change (notably XaAES's
   own active/inactive window chrome on OTHER applications' windows,
   which legitimately changes when the top window changes) -- and a test
   whose compared region grows to include unrelated legitimate changes
   reports failures that are not defects. */
static void nf_log_rect(const GRECT *r)
{
    nf_log("WMREDRAW: rect ");
    nf_log_short(r->g_x); nf_log(" ");
    nf_log_short(r->g_y); nf_log(" ");
    nf_log_short(r->g_w); nf_log(" ");
    nf_log_short(r->g_h); nf_log("\r\n");
}

/* How long to wait after appl_init before the first cover. RetroWP is
   launched from the same xaaes.cnf run list as this probe, so it may not
   have opened its window yet when we start; this is slack for the whole
   desktop to settle, not a measured requirement. The host script's own
   timeout is generous enough to absorb it. */
#define SETTLE_MS   20000L

/* How long the covering window stays up, and how long RetroWP is given
   to repaint after it goes away. Neither is a correctness threshold --
   they only have to exceed one host screenshot round trip.

   QUIET_MS is the same idea for the other direction: the host captures
   on a marker line, and if the probe changed the screen again straight
   after emitting one, the capture would race it and photograph the wrong
   state. So every marker the host screenshots on is followed by a quiet
   period at least as long as a capture takes. */
/* Deliberately far longer than the ~0.1-0.5s it actually takes the host
   to see a marker and confirm the screen changed. The host waits for a
   screen predicate rather than trusting a marker's timing, but each
   phase still has to outlast the whole marker-to-observation round trip,
   and that round trip is the part most likely to get slower on a loaded
   or slower machine. Cheap insurance: the gate's total runtime is
   dominated by booting an operating system, not by these waits. */
#define COVER_MS    15000L
#define RESTORE_MS  15000L
#define QUIET_MS     5000L

#define CYCLES       3

/* The rectangle to cover, in screen pixels. Chosen to sit across
   RetroWP's own text band: RetroWP opens full-desktop, and its sample
   document's two rendered lines sit just below the window title, so a
   band starting a little under the menu bar overlaps real glyphs rather
   than blank page. Covering blank white would make "it came back
   identical" trivially true and prove nothing. Width is clamped to the
   real screen below, so this works at 640x480 as well as 1024x768. */
#define COVER_X        0
#define COVER_Y       36
#define COVER_H      140

/* Services this probe's OWN window redraw. Kept correct rather than
   minimal on purpose -- see the file header. */
static void probe_redraw(short handle, VdiHdl vdi, short *msg, short colour)
{
    GRECT clip;
    GRECT work;
    short pxy[4];

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, (const MFORM *)0);

    wind_get_grect(handle, WF_FIRSTXYWH, &clip);
    work.g_x = msg[4];
    work.g_y = msg[5];
    work.g_w = msg[6];
    work.g_h = msg[7];

    while (clip.g_w > 0 && clip.g_h > 0) {
        GRECT r = clip;
        if (rc_intersect(&work, &r)) {
            pxy[0] = r.g_x;
            pxy[1] = r.g_y;
            pxy[2] = (short)(r.g_x + r.g_w - 1);
            pxy[3] = (short)(r.g_y + r.g_h - 1);
            vs_clip(vdi, 1, pxy);
            vsf_interior(vdi, FIS_SOLID);
            vsf_color(vdi, colour);
            v_bar(vdi, pxy);
        }
        wind_get_grect(handle, WF_NEXTXYWH, &clip);
    }

    vs_clip(vdi, 0, pxy);
    graf_mouse(M_ON, (const MFORM *)0);
    wind_update(END_UPDATE);
}

/* Paints the whole work area once, right after wind_open -- the AES does
   not send a WM_REDRAW for a window's initial appearance. */
static void probe_paint_all(short handle, VdiHdl vdi, short colour)
{
    GRECT work;
    short msg[8];

    wind_get_grect(handle, WF_WORKXYWH, &work);
    msg[4] = work.g_x;
    msg[5] = work.g_y;
    msg[6] = work.g_w;
    msg[7] = work.g_h;
    probe_redraw(handle, vdi, msg, colour);
}

/* evnt_timer with the message queue still serviced: a probe that slept
   without answering WM_REDRAW would leave its own window unpainted and
   muddy the screenshots. Returns when the timer expires. */
static void wait_servicing(short handle, VdiHdl vdi, short colour, long ms)
{
    short msg[8];
    short mx, my, mbtn, mkstate, mkey, mcount;
    short events;
    long remaining = ms;
    /* Each message restarts the timer, so this waits for `ms` of QUIET
       rather than `ms` of wall clock. That is the behaviour we want (the
       screen should be settled when the next marker goes out), but it is
       unbounded if messages never stop arriving -- hence the cap, which
       is a livelock guard, not a tuning knob. */
    int restarts = 0;

    while (remaining > 0) {
        events = evnt_multi(MU_MESAG | MU_TIMER, 1, 1, 1,
                            0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0,
                            msg, (short)(remaining & 0x7fff),
                            &mx, &my, &mbtn, &mkstate, &mkey, &mcount);
        if (events & MU_MESAG) {
            if (msg[0] == WM_REDRAW && handle >= 0 && msg[3] == handle) {
                probe_redraw(handle, vdi, msg, colour);
            }
            if (++restarts < 64) continue; /* message consumed the wait */
        }
        remaining = 0;
    }
}

int main(void)
{
    short work_in[11];
    short work_out[57];
    short dummy;
    short wcell, hcell;
    VdiHdl vdi;
    short handle;
    GRECT cover;
    short screen_w;
    int cycle;

    g_nf_stderr_id = nf_get_id("NF_STDERR");

    if (appl_init() < 0) {
        nf_log("WMREDRAW: FAIL appl_init\r\n");
        return 1;
    }

    vdi = graf_handle(&wcell, &hcell, &dummy, &dummy);
    for (dummy = 0; dummy < 10; dummy++) work_in[dummy] = 1;
    work_in[10] = 2; /* raster coordinates */
    v_opnvwk(work_in, &vdi, work_out);
    if (vdi == 0) {
        nf_log("WMREDRAW: FAIL v_opnvwk\r\n");
        appl_exit();
        return 1;
    }

    screen_w = (short)(work_out[0] + 1);

    /* Report what the VDI actually says the screen is. Two reasons this
       is worth a line of output on every run:

       (1) It proves the gate really ran at the resolution/depth intended.
           fvdi.sys silently falls back to its first branch if `setkey`
           was not patched (see docs/atari-platform.md), and a gate that
           quietly re-ran at 640x480x8 while claiming to test true colour
           would be worse than no gate.
       (2) work_out[13] is the exact value atari_screen_max_colors()
           returns, and draw_atari.c's plat_draw_image derives its plane
           count from it. What a true-colour fVDI reports there decides
           whether image drawing degrades honestly (a low value trips the
           `nplanes < 2` refusal and the caller falls back to a solid
           rect) or builds a planar MFDB for a chunky screen. Printing it
           settles that by measurement instead of by assumption. */
    nf_log("WMREDRAW: screen ");
    nf_log_short(screen_w); nf_log("x");
    nf_log_short((short)(work_out[1] + 1)); nf_log(" colors=");
    nf_log_short(work_out[13]); nf_log("\r\n");

    cover.g_x = COVER_X;
    cover.g_y = COVER_Y;
    cover.g_w = screen_w;
    cover.g_h = COVER_H;

    nf_log("WMREDRAW: settling\r\n");
    nf_log_rect(&cover);
    wait_servicing(-1, vdi, G_RED, SETTLE_MS);

    /* One unmeasured warm-up cycle BEFORE the baseline screenshot.
       Opening any window makes it the top window, and XaAES redraws the
       previously-top window's frame in its inactive colours when that
       happens -- a real, correct state change that never reverts when
       the new window closes. Measured against a baseline taken before
       any of that, every cycle would "fail" on another application's
       title bar while RetroWP itself was pixel-perfect (observed
       exactly that: 33k differing pixels, all of them TosWin2's window
       chrome, none of them RetroWP's content). Taking the baseline
       AFTER one cycle makes the comparison steady-state-to-steady-state,
       with no geometric assumptions about who owns which pixels. */
    for (cycle = 0; cycle < CYCLES + 1; cycle++) {
        if (cycle == 1) {
            nf_log("WMREDRAW: settled\r\n");
            wait_servicing(-1, vdi, G_RED, QUIET_MS);
        }

        handle = wind_create_grect(NAME, &cover);
        if (handle < 0) {
            nf_log("WMREDRAW: FAIL wind_create\r\n");
            break;
        }
        wind_set_str(handle, WF_NAME, "Cover");
        wind_open_grect(handle, &cover);
        probe_paint_all(handle, vdi, G_RED);

        /* Emitted AFTER the window is up and painted, so a host that
           screenshots on this line photographs the covered state rather
           than racing the AES's own window opening. */
        if (cycle > 0) nf_log("WMREDRAW: covered\r\n");
        wait_servicing(handle, vdi, G_RED, COVER_MS);

        wind_close(handle);
        wind_delete(handle);

        /* And this one AFTER the close, so the screenshot it triggers
           catches RetroWP's repaint rather than the covering window. */
        if (cycle > 0) nf_log("WMREDRAW: uncovered\r\n");
        wait_servicing(-1, vdi, G_RED, RESTORE_MS);
        if (cycle > 0) {
            nf_log("WMREDRAW: restored\r\n");
            wait_servicing(-1, vdi, G_RED, QUIET_MS);
        }
    }

    nf_log("WMREDRAW: done\r\n");

    v_clsvwk(vdi);
    appl_exit();
    return 0;
}
