/* BasiliskII smoke test for the M12 Phase 0 toolchain/fixture bring-up
   gate. Cross-compiled with m68k-apple-macos-gcc (Retro68) into its
   own standalone binary, copied onto the fixture's ExtFS "Unix:" share
   as CI_TARGET.APPL by host/tools/mac_ci_lib.sh, and launched by the
   permanent Startup Items stub (launcher.c) -- no Finder interaction,
   no per-run disk-image surgery.

   There is no platform/mac/ yet (that's Phase 1+), so unlike
   atari_smoke.c/amiga_smoke.c this doesn't exercise any real
   platform.h seam yet -- it only proves the pipeline itself works
   (Retro68 toolchain -> BasiliskII boot -> Startup Items auto-launch
   -> LaunchApplication hand-off -> ExtFS file write, all headless).
   Phases 1-6 grow this into the same shape amiga_smoke.c/atari_smoke.c
   already have: one line per real wp_mac_*_selfcheck(). */
#include <stdio.h>
#include <Quickdraw.h>

static void report(const char *line)
{
    FILE *f = fopen("Unix:mac_smoke.out", "w");
    if (f) {
        fwrite(line, 1, 5, f);
        fclose(f);
    }
}

int main(void)
{
    report("PASS\n");
    ExitToShell();
    return 0;
}
