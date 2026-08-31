#include <stddef.h>
#include "config.h"
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
