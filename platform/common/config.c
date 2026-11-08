/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include <stddef.h>
#include "config.h"
#include "format_prn.h"
#include "platform.h"

#define WP_CONFIG_FILE_MAX 4096 /* room for a documented, commented file --
                                   see config.h's own comment on truncated reads being safe */

static u8 ascii_lower(u8 c)
{
    if (c >= 'A' && c <= 'Z') return (u8)(c - 'A' + 'a');
    return c;
}

static wp_bool ascii_ieq(const u8 *a, u32 a_len, const char *b)
{
    u32 i;
    for (i = 0; i < a_len; i++) {
        if (b[i] == '\0') return WP_FALSE;
        if (ascii_lower(a[i]) != ascii_lower((u8)b[i])) return WP_FALSE;
    }
    return b[a_len] == '\0';
}

static wp_bool is_line_space(u8 c)
{
    return (wp_bool)(c == ' ' || c == '\t');
}

static void trim(const u8 *bytes, u32 len, u32 *out_start, u32 *out_len)
{
    u32 start = 0, end = len;
    while (start < end && is_line_space(bytes[start])) start++;
    while (end > start && is_line_space(bytes[end - 1])) end--;
    *out_start = start;
    *out_len = end - start;
}

static void parse_line(const u8 *bytes, u32 len, wp_config *out)
{
    u32 start, trimmed_len, eq, key_start, key_len, val_start, val_len;

    trim(bytes, len, &start, &trimmed_len);
    if (trimmed_len == 0) return;
    if (bytes[start] == '#' || bytes[start] == ';') return;

    for (eq = 0; eq < trimmed_len; eq++) {
        if (bytes[start + eq] == '=') break;
    }
    if (eq >= trimmed_len) return; /* no '=' -- not a recognizable line, skip */

    trim(bytes + start, eq, &key_start, &key_len);
    trim(bytes + start + eq + 1, trimmed_len - eq - 1, &val_start, &val_len);
    key_start += start;
    val_start += start + eq + 1;

    if (ascii_ieq(bytes + key_start, key_len, "printer")) {
        const u8 *v = bytes + val_start;
        if (ascii_ieq(v, val_len, "plain") || ascii_ieq(v, val_len, "text")) out->printer_driver = (u8)WP_PRN_PLAIN;
        else if (ascii_ieq(v, val_len, "epson")) out->printer_driver = (u8)WP_PRN_EPSON;
        else if (ascii_ieq(v, val_len, "pcl")) out->printer_driver = (u8)WP_PRN_PCL;
        else if (ascii_ieq(v, val_len, "postscript") || ascii_ieq(v, val_len, "ps")) out->printer_driver = (u8)WP_PRN_POSTSCRIPT;
        else if (ascii_ieq(v, val_len, "gdos")) out->printer_driver = (u8)WP_PRINTER_PAGES;
        return;
    }
    if (ascii_ieq(bytes + key_start, key_len, "duplex")) {
        const u8 *v = bytes + val_start;
        if (ascii_ieq(v, val_len, "off")) out->printer_sides = (u8)WP_PRN_SIMPLEX;
        else if (ascii_ieq(v, val_len, "long")) out->printer_sides = (u8)WP_PRN_DUPLEX_LONG;
        else if (ascii_ieq(v, val_len, "short")) out->printer_sides = (u8)WP_PRN_DUPLEX_SHORT;
        return;
    }
    if (!ascii_ieq(bytes + key_start, key_len, "keymap")) return;

    if (ascii_ieq(bytes + val_start, val_len, "vim")) {
        out->keymap_id = (u8)WP_KEYMAP_VI;
    } else if (ascii_ieq(bytes + val_start, val_len, "wordstar")) {
        out->keymap_id = (u8)WP_KEYMAP_WORDSTAR;
    } else if (ascii_ieq(bytes + val_start, val_len, "emacs")) {
        out->keymap_id = (u8)WP_KEYMAP_EMACS;
    } else if (ascii_ieq(bytes + val_start, val_len, "wordperfect")) {
        out->keymap_id = (u8)WP_KEYMAP_WORDPERFECT;
    } else if (ascii_ieq(bytes + val_start, val_len, "msword") ||
               ascii_ieq(bytes + val_start, val_len, "word")) {
        out->keymap_id = (u8)WP_KEYMAP_MSWORD;
    }
    /* Any other value (unrecognized, or a scheme not implemented yet)
       is silently left at whatever out->keymap_id already was -- see
       wp_config_parse's own leniency contract. */
}

wp_status wp_config_parse(const u8 *bytes, u32 len, wp_config *out)
{
    u32 i, line_start;

    out->keymap_id = (u8)WP_KEYMAP_VI;
    out->printer_driver = (u8)WP_PRN_PLAIN;
    out->printer_sides = (u8)WP_PRN_SIMPLEX;
    if (bytes == NULL) return WP_OK;

    line_start = 0;
    for (i = 0; i < len; i++) {
        if (bytes[i] == '\n') {
            u32 line_len = i - line_start;
            /* Tolerate a trailing '\r' from CRLF-authored content. */
            if (line_len > 0 && bytes[line_start + line_len - 1] == '\r') line_len--;
            parse_line(bytes + line_start, line_len, out);
            line_start = i + 1;
        }
    }
    if (line_start < len) parse_line(bytes + line_start, len - line_start, out);

    return WP_OK;
}

wp_status wp_config_load(const char *native_path, wp_config *out)
{
    plat_file *f;
    u32 size, read_len;
    u8 *buf;
    wp_bool exists;
    wp_status st;

    out->keymap_id = (u8)WP_KEYMAP_VI;
    out->printer_driver = (u8)WP_PRN_PLAIN;
    out->printer_sides = (u8)WP_PRN_SIMPLEX;

    if (plat_file_exists(native_path, &exists) != WP_OK || !exists) return WP_OK;
    if (plat_file_open(native_path, PLAT_FILE_READ, &f) != WP_OK) return WP_OK;

    if (plat_file_size(f, &size) != WP_OK) {
        plat_file_close(f);
        return WP_OK;
    }
    if (size > (u32)WP_CONFIG_FILE_MAX) size = (u32)WP_CONFIG_FILE_MAX;

    /* Heap, not stack. This was a 512-byte stack buffer, which was two
       problems at once: a config file with enough comments to be worth
       hand-editing silently truncated at 512 bytes -- taking any key
       past that point with it, with no error -- and growing the buffer
       in place would have put kilobytes on the stack of a RUN-launched
       Amiga program, which gets 4096 bytes TOTAL (docs/amiga-platform.md).
       Same reason format_ws_import's scratch buffer is heap. */
    buf = (u8 *)mem_alloc(size > 0 ? size : 1u);
    if (buf == NULL) {
        plat_file_close(f);
        return WP_OK;                   /* documented default stands */
    }

    if (plat_file_read(f, buf, size, &read_len) != WP_OK) {
        mem_free(buf);
        plat_file_close(f);
        return WP_OK;
    }
    plat_file_close(f);

    st = wp_config_parse(buf, read_len, out);
    mem_free(buf);
    return st;
}

/* Whether line[0..len) sets `key` (case ignored, blanks around it). */
static wp_bool line_sets(const u8 *line, u32 len, const char *key)
{
    u32 start, tl, eq, ks, kl;

    trim(line, len, &start, &tl);
    for (eq = 0; eq < tl && line[start + eq] != '='; eq++) { }
    if (eq >= tl) return WP_FALSE;
    trim(line + start, eq, &ks, &kl);
    return ascii_ieq(line + start + ks, kl, key);
}

wp_status wp_config_save_printer(const char *native_path, u8 driver, u8 sides)
{
    static const char *const drivers[] = { "plain", "epson", "pcl", "plain", "postscript", "gdos" };
    static const char *const sides_names[] = { "off", "long", "short" };
    plat_file *f;
    u32 size = 0, read_len = 0, i, line_start, n = 0;
    u8 *old = NULL, *out;
    wp_bool exists = WP_FALSE;
    const char *dv = driver <= WP_PRINTER_PAGES ? drivers[driver] : "plain";
    const char *sv = sides < 3 ? sides_names[sides] : "off";

    if (plat_file_exists(native_path, &exists) == WP_OK && exists &&
        plat_file_open(native_path, PLAT_FILE_READ, &f) == WP_OK) {
        if (plat_file_size(f, &size) != WP_OK) size = 0;
        if (size > (u32)WP_CONFIG_FILE_MAX) size = (u32)WP_CONFIG_FILE_MAX;
        old = (u8 *)mem_alloc(size > 0 ? size : 1u);
        if (old == NULL || plat_file_read(f, old, size, &read_len) != WP_OK) read_len = 0;
        plat_file_close(f);
    }
    out = (u8 *)mem_alloc(read_len + 64u);
    if (out == NULL) {
        if (old != NULL) mem_free(old);
        return WP_NOMEM;
    }
    /* Every line but printer= and duplex=, as it was, each ended with a
       newline (a last line without one gets one). */
    line_start = 0;
    for (i = 0; i <= read_len; i++) {
        if (i == read_len || old[i] == '\n') {
            u32 len = i - line_start;
            wp_bool tail = (wp_bool)(i == read_len && len == 0);   /* after a final newline */
            if (!tail && !line_sets(old + line_start, len, "printer") &&
                !line_sets(old + line_start, len, "duplex")) {
                memcpy(out + n, old + line_start, len);
                n += len;
                out[n++] = '\n';
            }
            line_start = i + 1;
        }
    }
    memcpy(out + n, "printer=", 8); n += 8;
    memcpy(out + n, dv, strlen(dv)); n += (u32)strlen(dv);
    out[n++] = '\n';
    memcpy(out + n, "duplex=", 7); n += 7;
    memcpy(out + n, sv, strlen(sv)); n += (u32)strlen(sv);
    out[n++] = '\n';
    if (old != NULL) mem_free(old);

    if (plat_file_open(native_path, PLAT_FILE_WRITE, &f) != WP_OK) {
        mem_free(out);
        return WP_ERR;
    }
    i = (u32)(plat_file_write(f, out, n) == WP_OK);
    plat_file_close(f);
    mem_free(out);
    return i ? WP_OK : WP_ERR;
}
