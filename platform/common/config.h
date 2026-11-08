/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_CONFIG_H
#define WP_CONFIG_H

#include "types.h"
#include "keymap.h"

/* M10: a tiny startup config file, read once and fixed for the process
   lifetime (no live reload) -- currently the sole purpose is picking
   which keybinding layout to use (see keymap.h). See docs/keymaps.md.

   Build discipline: platform/common/ rule as ever -- may call platform.h
   functions, must never #include a native header. */

/* printer_driver's one value past wp_prn_driver's: the machine's own
   page printing (wp_app_ctx.print_pages, the ST's GDOS printer), saved
   as "printer=gdos". */
#define WP_PRINTER_PAGES 5

typedef struct {
    u8 keymap_id;      /* wp_keymap_id; always a valid, already-clamped value */
    u8 printer_driver; /* wp_prn_driver for the machine's own printer, or
                          WP_PRINTER_PAGES: "printer=plain|epson|pcl|
                          postscript|gdos" (plain) */
    u8 printer_sides;  /* wp_prn_sides: "duplex=off|long|short" (off) */
} wp_config;

/* Line-based `key=value` text, `#`/`;` comment lines, blank lines
   skipped, case-insensitive key/value matching, CRLF or LF line endings.
   Lenient by design (matches help_doc.c's own leniency precedent): an
   unrecognized key, an unrecognized value, or a malformed line is
   silently skipped rather than rejected outright -- this is hand-edited
   content on a vintage machine's text editor, not a document format with
   round-trip requirements. *out is always left fully populated with a
   valid default (WP_KEYMAP_VI) before parsing begins, so a caller never
   needs to check a return value beyond WP_OK ("bytes was NULL" is the
   only realistic failure). Pure -- no mem_alloc, no platform.h calls --
   so it's fully host-testable against hand-fed byte arrays
   (host/tests/test_config.c), the same split help_doc.c's own
   wp_help_doc_parse establishes between parsing and file I/O. */
wp_status wp_config_parse(const u8 *bytes, u32 len, wp_config *out);

/* Thin platform wrapper: plat_file_exists -> open/size/read into a
   small fixed stack buffer (config content is a handful of short lines,
   never large by design; a file larger than the buffer is read
   truncated -- still safe, since wp_config_parse only ever looks for
   whole lines it recognizes) -> wp_config_parse -> close. A missing
   file, an unreadable file, and a parse failure all yield the exact
   same result: *out populated with the documented default
   (keymap_id = WP_KEYMAP_VI) -- an existing zero-config install must
   keep behaving exactly as it always has, not fail to start. Always
   returns WP_OK (never fails the caller's startup path); the return
   value exists only for symmetry with this codebase's other wp_status
   functions. native_path is an opaque native filesystem path, same
   convention as plat_file_open's own. */
wp_status wp_config_load(const char *native_path, wp_config *out);

/* Writes the printer settings into the config file at native_path: its
   printer= and duplex= lines replaced (or added), every other line kept
   as it was -- the writer's own keymap and comments with it. The file is
   made if there is none. What the Printer choice in File > Print saves,
   so it stays chosen. WP_ERR if the file cannot be written. */
wp_status wp_config_save_printer(const char *native_path, u8 driver, u8 sides);

#endif /* WP_CONFIG_H */
