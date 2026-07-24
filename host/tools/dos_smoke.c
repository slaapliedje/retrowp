/* DOSBox-based smoke test for platform/dos's non-UI platform.h seams.
   Phase 0 (T14.6's go/no-go gate) proved the Open Watcom toolchain and
   the DOSBox CI mechanism work before any real platform/dos/ backend
   code existed; Phase 2 (T14.2) extends this same binary to also prove
   the real engine + metrics/draw seam via wp_dos_layout_selfcheck, the
   same report()-to-a-log-file convention host/tools/amiga_smoke.c
   already uses (a real host directory mounted as C: is a live
   passthrough, not a copied disk image, so this program's own file
   write lands on the real host filesystem in real time -- confirmed
   during Phase 0 research). DOSBox's own `--exit` flag then terminates
   the emulator cleanly once this program returns -- no fixed sleep, no
   Native-Features-style exit trap, and no bespoke shared-folder setup
   needed at all, unlike Hatari's/amiberry's/BasiliskII's own Phase 0
   automation work.

   Real bug found while bringing this up: wp_paragraph_relayout's own
   512-entry token_t stack array (engine/src/linebreak.c) overflowed
   Open Watcom's default DOS stack, silently crashing DOSBox's guest
   partway through the very first relayout call with no error message
   -- fixed by linking with a real, larger stack (see the Makefile's
   own `-k32768` on both DOS .EXE targets), not by shrinking the
   engine's own token buffer. This is exactly the kind of host-only
   assumption (ample stack, an implicit `ulimit -s` most modern
   systems set generously) that this project's own cross-compile
   discipline exists to catch. */

#include <stdio.h>
#include "types.h"
#include "app_shell_internal.h"

static void report(const char *line)
{
    FILE *f = fopen("C:\\DOS_CI.OUT", "a");

    if (f == NULL) return;
    fprintf(f, "%s", line);
    fclose(f);
}

int main(void)
{
    wp_bool ok;
    wp_bool all_ok = WP_TRUE;

    remove("C:\\DOS_CI.OUT");

    ok = wp_dos_layout_selfcheck();
    report(ok ? "layout: PASS\r\n" : "layout: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_file_clipboard_selfcheck();
    report(ok ? "file_clipboard: PASS\r\n" : "file_clipboard: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_transport_selfcheck();
    report(ok ? "transport: PASS\r\n" : "transport: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_key_translate_selfcheck();
    report(ok ? "key_translate: PASS\r\n" : "key_translate: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_config_selfcheck();
    report(ok ? "config: PASS\r\n" : "config: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_help_selfcheck();
    report(ok ? "help: PASS\r\n" : "help: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    ok = wp_dos_image_selfcheck();
    report(ok ? "image: PASS\r\n" : "image: FAIL\r\n");
    if (!ok) all_ok = WP_FALSE;

    printf("RETROWP DOS SMOKE OK\r\n");
    return all_ok ? 0 : 1;
}
