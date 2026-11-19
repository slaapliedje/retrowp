/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "recovery.h"
#include "app_ctx.h"
#include "actions.h"
#include "docmodel.h"
#include "format_rtf.h"

/* The header's three lines after the magic; the body follows the empty
   line. Read back by parse_header, which is all that knows the layout. */
#define K_PATH "path="
#define K_RTF  "rtf="

static wp_bool have_slot(const wp_app_ctx *ctx)
{
    return (wp_bool)(ctx != NULL && ctx->recovery_path != NULL && ctx->recovery_path[0] != '\0');
}

static wp_bool file_there(const char *path)
{
    wp_bool there = WP_FALSE;
    return (wp_bool)(plat_file_exists(path, &there) == WP_OK && there);
}

static wp_status write_all(plat_file *f, const char *s, u32 len)
{
    return plat_file_write(f, (const u8 *)s, len);
}

wp_status wp_recovery_write(wp_app_ctx *ctx, wp_image_source *img_src)
{
    u32 needed = 0;
    u8 *buf;
    plat_file *f;
    wp_status st;

    if (!have_slot(ctx) || !ctx->doc_loaded || ctx->doc == NULL) return WP_ERR;

    /* The exporter's two-call sizing, then the bytes. No wp_action_tell
       anywhere on this path: it runs while the AES waits on us. */
    if (img_src != NULL) format_rtf_export_images(ctx->doc, NULL, 0, &needed, img_src);
    else                 format_rtf_export(ctx->doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed > 0 ? needed : 1);
    if (buf == NULL) return WP_NOMEM;
    st = (img_src != NULL) ? format_rtf_export_images(ctx->doc, buf, needed, &needed, img_src)
                           : format_rtf_export(ctx->doc, buf, needed, &needed);
    if (st == WP_OK) st = plat_file_open(ctx->recovery_path, PLAT_FILE_WRITE, &f);
    if (st == WP_OK) {
        st = write_all(f, WP_RECOVERY_MAGIC, (u32)strlen(WP_RECOVERY_MAGIC));
        if (st == WP_OK) st = write_all(f, K_PATH, (u32)strlen(K_PATH));
        if (st == WP_OK) st = write_all(f, ctx->doc_path, (u32)strlen(ctx->doc_path));
        if (st == WP_OK) st = write_all(f, ctx->doc_rtf ? "\n" K_RTF "1\n\n" : "\n" K_RTF "0\n\n",
                                        (u32)strlen("\n" K_RTF "1\n\n"));
        if (st == WP_OK) st = plat_file_write(f, buf, needed);
        if (st == WP_OK) st = plat_file_flush(f);   /* a full disk shows here */
        plat_file_close(f);
        /* A half-written copy would be offered next time as if it were
           the work: better none, and the caller refuses to leave. */
        if (st != WP_OK) (void)plat_file_delete(ctx->recovery_path);
    }
    mem_free(buf);
    if (st == WP_OK) ctx->recovery_owned = WP_TRUE;
    return st;
}

/* The whole file, read: NULL when it is missing, unreadable or not one of
   ours. *len_out is its length. The caller frees. */
static u8 *read_file(const wp_app_ctx *ctx, u32 *len_out)
{
    plat_file *f;
    u32 size = 0, got = 0;
    u8 *buf;
    u32 mlen = (u32)strlen(WP_RECOVERY_MAGIC);

    if (!have_slot(ctx) || !file_there(ctx->recovery_path)) return NULL;
    if (plat_file_open(ctx->recovery_path, PLAT_FILE_READ, &f) != WP_OK) return NULL;
    if (plat_file_size(f, &size) != WP_OK || size < mlen) {
        plat_file_close(f);
        return NULL;
    }
    buf = (u8 *)mem_alloc(size + 1);
    if (buf == NULL) {
        plat_file_close(f);
        return NULL;
    }
    if (plat_file_read(f, buf, size, &got) != WP_OK || got != size ||
        memcmp(buf, WP_RECOVERY_MAGIC, mlen) != 0) {
        plat_file_close(f);
        mem_free(buf);
        return NULL;
    }
    plat_file_close(f);
    buf[size] = 0;
    *len_out = size;
    return buf;
}

/* The header of a file read_file accepted: the name, the format, and
   where the body starts. WP_FALSE when the header does not end. */
static wp_bool parse_header(const u8 *buf, u32 len, char *path_out, u32 cap,
                            wp_bool *rtf_out, u32 *body_out)
{
    u32 i = (u32)strlen(WP_RECOVERY_MAGIC);

    if (path_out != NULL && cap > 0) path_out[0] = '\0';
    if (rtf_out != NULL) *rtf_out = WP_FALSE;
    while (i < len) {
        u32 start = i, n;

        while (i < len && buf[i] != '\n') i++;
        if (i >= len) return WP_FALSE;
        n = i - start;
        i++;                                    /* past the '\n' */
        if (n == 0) {
            *body_out = i;
            return WP_TRUE;
        }
        if (n >= 5 && memcmp(buf + start, K_PATH, 5) == 0) {
            if (path_out != NULL && n - 5 < cap) {
                memcpy(path_out, buf + start + 5, n - 5);
                path_out[n - 5] = '\0';
            }
        } else if (n >= 4 && memcmp(buf + start, K_RTF, 4) == 0) {
            if (rtf_out != NULL) *rtf_out = (wp_bool)(n > 4 && buf[start + 4] == '1');
        }
        /* any other line: a later version's, passed over */
    }
    return WP_FALSE;
}

wp_bool wp_recovery_pending(const wp_app_ctx *ctx, char *path_out, u32 cap, wp_bool *rtf_out)
{
    u32 len = 0, body = 0;
    u8 *buf = read_file(ctx, &len);
    wp_bool ok;

    if (buf == NULL) return WP_FALSE;
    ok = parse_header(buf, len, path_out, cap, rtf_out, &body);
    mem_free(buf);
    return ok;
}

wp_status wp_recovery_restore(wp_app_ctx *ctx, wp_image_sink *sink)
{
    char path[WP_APP_PATH_MAX];
    wp_bool rtf = WP_FALSE;
    u32 len = 0, body = 0;
    u8 *buf = read_file(ctx, &len);
    wp_document fresh;
    wp_status st;

    if (buf == NULL) return WP_ERR;
    if (!parse_header(buf, len, path, (u32)sizeof(path), &rtf, &body)) {
        mem_free(buf);
        return WP_ERR;
    }
    st = (sink != NULL) ? format_rtf_import_images(buf + body, len - body, &fresh, sink)
                        : format_rtf_import(buf + body, len - body, &fresh);
    mem_free(buf);
    if (st != WP_OK) return st;

    /* Not the document's copy yet, or the replace below would settle
       (delete) the very file this has just been read from. */
    ctx->recovery_owned = WP_FALSE;
    wp_action_replace_document(ctx, &fresh);
    if (path[0] != '\0') wp_action_remember_path(ctx, path);
    else                 ctx->doc_path[0] = '\0';
    ctx->doc_rtf = rtf;
    /* Nothing has saved these changes: Save, Quit and New must treat
       them as the unsaved work they are. */
    wp_document_mark_modified(ctx->doc);
    ctx->recovery_owned = WP_TRUE;
    return WP_OK;
}

void wp_recovery_drop(wp_app_ctx *ctx)
{
    if (!have_slot(ctx)) return;
    if (file_there(ctx->recovery_path)) (void)plat_file_delete(ctx->recovery_path);
    ctx->recovery_owned = WP_FALSE;
}

void wp_recovery_settled(wp_app_ctx *ctx)
{
    if (!have_slot(ctx) || !ctx->recovery_owned) return;
    wp_recovery_drop(ctx);
}

wp_recovery_answer wp_recovery_offer(wp_app_ctx *ctx, wp_image_sink *sink,
                                     char *path_out, u32 cap)
{
    static const char k_lead[] = "Unsaved changes to ";
    static const char k_untitled[] = "Unsaved changes to an untitled document";
    char path[WP_APP_PATH_MAX];
    char msg[160];
    const char *name, *p;
    const u8 *labels[2];
    u32 lens[2], chosen = 0, n;
    wp_bool cancelled = WP_FALSE;

    /* One line, as short as a GEM alert's width allows: an 8.3 name
       makes it 31 characters. Measured on the ST, where the longer
       "RetroWP was closed with unsaved changes to ..." lost the name. */
    if (path_out != NULL && cap > 0) path_out[0] = '\0';
    if (!wp_recovery_pending(ctx, path, (u32)sizeof(path), NULL)) return WP_RECOVERY_NONE;

    for (name = p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    }
    if (name[0] == '\0') {
        strcpy(msg, k_untitled);
    } else {
        n = (u32)strlen(name);
        if (n > sizeof(msg) - sizeof(k_lead) - 2) n = (u32)(sizeof(msg) - sizeof(k_lead) - 2);
        memcpy(msg, k_lead, sizeof(k_lead) - 1);
        memcpy(msg + sizeof(k_lead) - 1, name, n);
        msg[sizeof(k_lead) - 1 + n] = '\0';
    }
    labels[0] = (const u8 *)"Recover";
    lens[0] = 7;
    labels[1] = (const u8 *)(name[0] != '\0' ? "Use saved" : "Discard");
    lens[1] = (u32)strlen((const char *)labels[1]);

    if (plat_dialog_choice(ctx->window, (const u8 *)msg, (u32)strlen(msg),
                           labels, lens, 2u, 0u, &cancelled, &chosen) != WP_OK) {
        cancelled = WP_TRUE;
    }
    if (cancelled || chosen == 0u) {
        if (wp_recovery_restore(ctx, sink) == WP_OK) return WP_RECOVERY_RECOVERED;
        /* Unreadable after all: it stays where it is, for whoever can
           read it, and the run goes on as if there were none. */
        wp_action_tell(ctx, "Couldn't read the recovered changes; they are still in the recovery file");
        return WP_RECOVERY_NONE;
    }
    wp_recovery_drop(ctx);
    if (path_out != NULL && strlen(path) < cap) strcpy(path_out, path);
    return WP_RECOVERY_USE_SAVED;
}

wp_bool wp_action_term_request(wp_app_ctx *ctx, wp_image_source *img_src)
{
    if (!ctx->doc_loaded || !wp_document_is_modified(ctx->doc)) {
        ctx->quit_requested = WP_TRUE;
        return WP_TRUE;
    }
    if (wp_recovery_write(ctx, img_src) == WP_OK) {
        ctx->quit_requested = WP_TRUE;
        return WP_TRUE;
    }
    return WP_FALSE;
}
