#include <string.h>

#include "platform.h"
#include "app_shell_internal.h"

/* M12 Phase 1: non-UI platform.h seams only (memory/file/clipboard/
   timer) -- no Toolbox window/menu/event loop here yet. Grows into the
   real Toolbox shell in Phase 2, the same way platform/amiga/app_shell.c
   grew from a bare selfcheck (M9 T9.1a) into its full MUI shell
   (M9 T9.1b). "Unix:" is the fixture's ExtFS share (docs/mac-platform.md)
   -- the one location proven reliably read/writable throughout M12
   Phase 0, so the round-trip fixture lives there rather than on the
   boot volume itself. */

wp_bool wp_mac_file_clipboard_selfcheck(void)
{
    static const char fixture[] = "mac file+clipboard round-trip fixture";
    static const char scratch_path[] = "Unix:mac_wptest.txt";
    u32 fixture_len = (u32)strlen(fixture);
    u8 readback[128];
    u32 readback_len;
    plat_file *f;
    wp_bool ok = WP_TRUE;

    if (plat_file_open(scratch_path, PLAT_FILE_WRITE, &f) != WP_OK) return WP_FALSE;
    if (plat_file_write(f, (const u8 *)fixture, fixture_len) != WP_OK) ok = WP_FALSE;
    plat_file_close(f);
    if (!ok) return WP_FALSE;

    if (plat_file_open(scratch_path, PLAT_FILE_READ, &f) != WP_OK) return WP_FALSE;
    if (plat_file_read(f, readback, (u32)sizeof(readback), &readback_len) != WP_OK) ok = WP_FALSE;
    plat_file_close(f);
    plat_file_delete(scratch_path);

    if (!ok || readback_len != fixture_len) return WP_FALSE;
    if (memcmp(readback, fixture, fixture_len) != 0) return WP_FALSE;

    if (plat_clipboard_set_text((const u8 *)fixture, fixture_len) != WP_OK) return WP_FALSE;
    if (plat_clipboard_get_text(readback, (u32)sizeof(readback), &readback_len) != WP_OK) return WP_FALSE;
    if (readback_len != fixture_len) return WP_FALSE;
    if (memcmp(readback, fixture, fixture_len) != 0) return WP_FALSE;
    if (!plat_clipboard_has_text()) return WP_FALSE;

    return WP_TRUE;
}
