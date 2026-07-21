/* BasiliskII smoke test for platform/mac's platform.h seams. Cross-
   compiled with m68k-apple-macos-gcc (Retro68) into its own standalone
   binary, copied onto the fixture's ExtFS "Unix:" share as
   CI_TARGET.APPL by host/tools/mac_ci_lib.sh, and launched by the
   permanent Startup Items stub (launcher.c) -- no Finder interaction,
   no per-run disk-image surgery (M12 Phase 0, docs/mac-platform.md).

   Mirrors atari_smoke.c/amiga_smoke.c's own shape: one report line per
   real wp_mac_*_selfcheck(), grown phase by phase as platform/mac/
   grows. Phase 0 itself had no platform/mac/ yet, so this only proved
   the pipeline; from Phase 1 onward it proves real seams.

   Both lines are written in a SINGLE fopen("w") call rather than one
   fopen("a") per check -- "w" (create/truncate) is proven reliable
   throughout M12 Phase 0's extensive testing, while append semantics
   remain untested here; concatenating in memory first sidesteps that
   risk entirely rather than accepting it for convenience. */
#include <string.h>
#include <stdio.h>
#include <Quickdraw.h>
#include "app_shell_internal.h"

static void report(const char *line)
{
    FILE *f = fopen("Unix:mac_smoke.out", "w");
    if (f) {
        fwrite(line, 1, strlen(line), f);
        fclose(f);
    }
}

int main(void)
{
    char buf[128];
    u32 pos = 0;
    wp_bool ok;
    const char *line;

    /* Phase 3's layout selfcheck calls into real QuickDraw/Font Manager
       calls (metrics_mac.c's OpenPort/TextFont/GetFontInfo/TextWidth),
       which are only safe once QuickDraw and the Font Manager are
       actually initialized -- a real, reproducible SIGSEGV during boot
       (not a hang) confirmed this the hard way: this smoke test's own
       main() never called InitGraf/InitFonts at all before Phase 3
       added a metrics selfcheck that needed them. app_shell_run()
       always calls these before doing anything else; this smoke test
       needs the same minimum, even though it never opens a real window. */
    InitGraf(&qd.thePort);
    InitFonts();

    ok = wp_mac_file_clipboard_selfcheck();
    line = ok ? "file_clipboard: PASS\n" : "file_clipboard: FAIL\n";
    strcpy(buf + pos, line);
    pos += (u32)strlen(line);

    ok = wp_mac_layout_selfcheck();
    line = ok ? "layout: PASS\n" : "layout: FAIL\n";
    strcpy(buf + pos, line);
    pos += (u32)strlen(line);

    ok = wp_mac_transport_selfcheck();
    line = ok ? "transport: PASS\n" : "transport: FAIL\n";
    strcpy(buf + pos, line);
    pos += (u32)strlen(line);

    report(buf);

    ExitToShell();
    return 0;
}
