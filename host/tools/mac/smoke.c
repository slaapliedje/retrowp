/* BasiliskII smoke test for platform/mac's non-UI platform.h seams
   (Phase 1: memory/file/clipboard/timer). Cross-compiled with
   m68k-apple-macos-gcc (Retro68) into its own standalone binary, copied
   onto the fixture's ExtFS "Unix:" share as CI_TARGET.APPL by
   host/tools/mac_ci_lib.sh, and launched by the permanent Startup Items
   stub (launcher.c) -- no Finder interaction, no per-run disk-image
   surgery (M12 Phase 0, docs/mac-platform.md).

   Mirrors atari_smoke.c/amiga_smoke.c's own shape: one report line per
   real wp_mac_*_selfcheck(), grown phase by phase as platform/mac/
   grows. Phase 0 itself had no platform/mac/ yet, so this only proved
   the pipeline; from Phase 1 onward it proves real seams. */
#include <string.h>
#include <stdio.h>
#include <Quickdraw.h>
#include "app_shell_internal.h"

/* "w" (create/truncate), not "a" -- proven reliable throughout M12
   Phase 0's extensive testing; append semantics are untested here and
   not worth the risk for a single report line. Revisit once a second
   selfcheck actually needs its own line. */
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
    wp_bool ok;

    ok = wp_mac_file_clipboard_selfcheck();
    report(ok ? "file_clipboard: PASS\n" : "file_clipboard: FAIL\n");

    ExitToShell();
    return 0;
}
