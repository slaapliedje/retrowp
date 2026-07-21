/* amiberry-based smoke test for platform/amiga's non-UI platform.h seams
   (T9.1a: memory/file/clipboard/timer; T9.2: layout/metrics; T9.3:
   transport; Phase 6: help system). Cross-compiled with m68k-amigaos-gcc
   into its own standalone binary and run under amiberry against the
   docs/amiga-platform.md fixture.

   amiberry has no equivalent to Hatari's NF_STDERR/NF_EXIT Native
   Features trap -- so unlike atari_smoke.c, this can't make the
   emulator process itself exit with a pass/fail status. Instead it
   writes a plain PASS/FAIL line to a file on the boot volume, host-
   readable after the run regardless of what the (currently icon-less,
   see docs/amiga-platform.md) Workbench screen shows -- the same
   technique proven for the Phase 0 muitest probe, and (Phase 5) now the
   basis of a fully headless CI run: host/tools/ci_amiga.sh's own
   Startup-Sequence hook runs this binary automatically and polls for
   its completion, no interactive step at all. */

#include <string.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include "types.h"
#include "app_shell_internal.h"

static void report(const char *line)
{
    BPTR fh = Open((CONST_STRPTR)"SYS:amiga_smoke.out", MODE_READWRITE);

    if (fh == 0) fh = Open((CONST_STRPTR)"SYS:amiga_smoke.out", MODE_NEWFILE);
    if (fh == 0) return;
    Seek(fh, 0, OFFSET_END);
    Write(fh, (APTR)line, (LONG)strlen(line));
    Close(fh);
}

int main(void)
{
    wp_bool ok;
    wp_bool all_ok = WP_TRUE;

    ok = wp_amiga_file_clipboard_selfcheck();
    report(ok ? "file_clipboard: PASS\n" : "file_clipboard: FAIL\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_amiga_layout_selfcheck();
    report(ok ? "layout: PASS\n" : "layout: FAIL\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_amiga_transport_selfcheck();
    report(ok ? "transport: PASS\n" : "transport: FAIL\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_amiga_help_selfcheck();
    report(ok ? "help: PASS\n" : "help: FAIL\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_amiga_key_translate_selfcheck();
    report(ok ? "key_translate: PASS\n" : "key_translate: FAIL\n");
    if (!ok) all_ok = WP_FALSE;

    return all_ok ? 0 : 20;
}
