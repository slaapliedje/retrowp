/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "pistore.h"
#include "app_ctx.h"
#include "actions.h"
#include "settings.h"
#include "offload.h"
#include "platform.h"

#define PI_NAME_MAX      63u        /* doc_store.py's NAME_MAX */
#define PI_ONE_FRAME     60000uL    /* as actions.c's convert bounds */
#define PI_LARGE         512000uL
#define PI_TIMEOUT_MS    120000uL   /* a file's worth at 9600 baud */
#define PI_LIST_TIMEOUT  20000uL
#define PI_PAGE          8u         /* and "More...": the ST's nine options */
#define PI_LABEL_CAP     32u        /* the ST dialog's option width */
#define PI_LIST_MAX      64u        /* documents (or versions) offered */

static wp_bool g_told_failure = WP_FALSE;

static wp_bool link_up(wp_app_ctx *ctx)
{
    return (ctx->ensure_offload != NULL) ? ctx->ensure_offload() : offload_available();
}

wp_bool wp_pistore_able(wp_app_ctx *ctx)
{
    offload_capabilities caps;
    u32 need = (1uL << (u32)OFFLOAD_STORE) | (1uL << (u32)OFFLOAD_LIST) | (1uL << (u32)OFFLOAD_FETCH);

    if (!link_up(ctx) || offload_query(&caps) != WP_OK) return WP_FALSE;
    return (wp_bool)((caps.ops_supported & need) == need);
}

/* The file's own name: what follows the last separator any machine uses. */
static const char *leaf(const char *path)
{
    const char *s = path, *p;

    for (p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') s = p + 1;
    }
    return s;
}

/* A u8 length and the bytes; WP_FALSE when it is too long for the store. */
static wp_bool put_name(u8 *out, u32 *pos, const char *name)
{
    u32 n = (u32)strlen(name);

    if (n == 0 || n > PI_NAME_MAX) return WP_FALSE;
    out[(*pos)++] = (u8)n;
    memcpy(out + *pos, name, n);
    *pos += n;
    return WP_TRUE;
}

/* The gateway's one line, told as it came (cut to what a status line or
   an alert holds), or `fallback` when it sent none. */
static void tell_reply(wp_app_ctx *ctx, const offload_response *resp, const char *fallback)
{
    char line[61];
    u32 n = resp->result_len < 60u ? resp->result_len : 60u, i;

    if (resp->result == NULL || n == 0) {
        wp_action_tell(ctx, fallback);
        return;
    }
    for (i = 0; i < n; i++) {
        u8 c = resp->result[i];
        line[i] = (char)((c >= 0x20 && c < 0x7F) ? c : ' ');
    }
    line[n] = '\0';
    wp_action_tell(ctx, line);
}

static void fail(wp_app_ctx *ctx, wp_bool quiet, const char *msg)
{
    if (quiet && g_told_failure) return;
    g_told_failure = WP_TRUE;
    wp_action_tell(ctx, msg);
}

wp_status wp_pistore_put(wp_app_ctx *ctx, const char *native_path,
                         const u8 *bytes, u32 len, wp_bool quiet)
{
    const char *machine = wp_settings_machine(ctx);
    const char *name = leaf(native_path);
    u32 head = 2u + (u32)strlen(machine) + (u32)strlen(name), pos = 0;
    u8 *payload;
    offload_request req;
    offload_response resp;
    wp_status st;

    if (!wp_pistore_able(ctx)) {
        fail(ctx, quiet, "Not copied: no gateway that keeps documents");
        return WP_ERR;
    }
    if (len > (offload_takes_large_requests() ? PI_LARGE : PI_ONE_FRAME) - head) {
        fail(ctx, quiet, "Not copied: too large to send to the Pi");
        return WP_RANGE;
    }
    payload = (u8 *)mem_alloc(head + len);
    if (payload == NULL) {
        fail(ctx, quiet, "Not copied to the Pi: not enough memory");
        return WP_NOMEM;
    }
    if (!put_name(payload, &pos, machine) || !put_name(payload, &pos, name)) {
        mem_free(payload);
        fail(ctx, quiet, "Not copied: that name is too long for the Pi");
        return WP_RANGE;
    }
    memcpy(payload + pos, bytes, len);

    /* The window whole again before the wait: the file selector that
       named the file has just closed, and the Pi's answer would come up
       over its pieces (tos-47's printing, the same). */
    if (ctx->repaint != NULL) ctx->repaint(ctx);

    req.op = OFFLOAD_STORE;
    req.payload = payload;
    req.payload_len = pos + len;
    req.timeout_ms = (u32)PI_TIMEOUT_MS;
    resp.result = NULL;
    resp.result_len = 0;
    st = offload_submit(&req, &resp);
    mem_free(payload);
    if (st != WP_OK) {
        fail(ctx, quiet, "Not copied: the Pi did not answer");
        return st;
    }
    if (resp.status != OFFLOAD_OK) {
        if (!(quiet && g_told_failure)) tell_reply(ctx, &resp, "The Pi couldn't keep the copy");
        g_told_failure = WP_TRUE;
        offload_response_free(&resp);
        return WP_ERR;
    }
    g_told_failure = WP_FALSE;
    if (!quiet) tell_reply(ctx, &resp, "Kept on the Pi");
    offload_response_free(&resp);
    return WP_OK;
}

/* ---- Open from Pi --------------------------------------------------- */

/* One request answered with text lines; resp->result holds them. */
static wp_status ask_list(wp_app_ctx *ctx, const char *name, offload_response *resp)
{
    u8 payload[2u + 2u * PI_NAME_MAX];
    u32 pos = 0;
    offload_request req;
    wp_status st;

    if (!put_name(payload, &pos, wp_settings_machine(ctx))) {
        wp_action_tell(ctx, "This machine's name is too long for the Pi");
        return WP_RANGE;
    }
    if (name != NULL && !put_name(payload, &pos, name)) return WP_RANGE;
    req.op = OFFLOAD_LIST;
    req.payload = payload;
    req.payload_len = pos;
    req.timeout_ms = (u32)PI_LIST_TIMEOUT;
    resp->result = NULL;
    resp->result_len = 0;
    st = offload_submit(&req, resp);
    if (st != WP_OK) {
        wp_action_tell(ctx, "The Pi did not answer");
        return st;
    }
    if (resp->status != OFFLOAD_OK) {
        tell_reply(ctx, resp, "The Pi couldn't list the documents");
        offload_response_free(resp);
        return WP_ERR;
    }
    return WP_OK;
}

/* Splits the reply into lines (each ends at LF, which becomes NUL) and
   the first field of each at its TAB. Returns how many, at most max. */
static u32 split_lines(u8 *text, u32 len, u8 **lines, u32 max)
{
    u32 n = 0, i, start = 0;

    for (i = 0; i < len && n < max; i++) {
        if (text[i] == '\n') {
            text[i] = '\0';
            if (i > start) lines[n++] = text + start;
            start = i + 1;
        }
    }
    return n;
}

/* The line's field `which` (0-based, TAB-separated), as a pointer to its
   first byte and its length. */
static const u8 *field(const u8 *line, u32 which, u32 *len)
{
    const u8 *p = line, *e;

    while (which > 0 && *p != '\0') {
        if (*p++ == '\t') which--;
    }
    for (e = p; *e != '\0' && *e != '\t'; e++) {}
    *len = (u32)(e - p);
    return p;
}

/* A choice among `count` labels, eight and "More..." to a page. WP_TRUE
   with the index chosen. One alone gets a "Cancel" of ours beside it:
   a choice of one is a message to every backend (platform.h), which
   would show the title and an OK, and neither the name nor a way out. */
static wp_bool choose(wp_app_ctx *ctx, const char *title, u8 labels[][PI_LABEL_CAP],
                      u32 count, u32 *out)
{
    const u8 *ptrs[PI_PAGE + 1];
    u32 lens[PI_PAGE + 1];
    u32 first = 0;

    if (count == 1) {
        u32 chosen = 0;
        wp_bool cancelled = WP_TRUE;

        ptrs[0] = labels[0];
        lens[0] = (u32)strlen((const char *)labels[0]);
        ptrs[1] = (const u8 *)"Cancel";
        lens[1] = 6;
        if (plat_dialog_choice(ctx->window, (const u8 *)title, (u32)strlen(title), ptrs, lens,
                               2, 0, &cancelled, &chosen) != WP_OK || cancelled || chosen != 0) {
            return WP_FALSE;
        }
        *out = 0;
        return WP_TRUE;
    }

    for (;;) {
        u32 n = 0, chosen = 0;
        wp_bool more = (wp_bool)(count > PI_PAGE), cancelled = WP_TRUE;

        for (; n < PI_PAGE && first + n < count; n++) {
            ptrs[n] = labels[first + n];
            lens[n] = (u32)strlen((const char *)labels[first + n]);
        }
        if (more) {
            ptrs[n] = (const u8 *)"More...";
            lens[n] = 7;
        }
        if (plat_dialog_choice(ctx->window, (const u8 *)title, (u32)strlen(title), ptrs, lens,
                               n + (more ? 1u : 0u), 0, &cancelled, &chosen) != WP_OK || cancelled) {
            return WP_FALSE;
        }
        if (more && chosen == n) {
            first += PI_PAGE;
            if (first >= count) first = 0;
            continue;
        }
        *out = first + chosen;
        return WP_TRUE;
    }
}

/* "4 KB", "980 bytes": short, for a label. */
static u32 put_size(char *out, const u8 *digits, u32 len)
{
    u32 v = 0, i, n = 0;
    char tmp[12];
    u32 t = 0;
    const char *unit;

    for (i = 0; i < len && digits[i] >= '0' && digits[i] <= '9'; i++) v = v * 10u + (u32)(digits[i] - '0');
    if (v >= 1024u) { v = (v + 1023u) / 1024u; unit = " KB"; }
    else unit = " bytes";
    do { tmp[t++] = (char)('0' + v % 10u); v /= 10u; } while (v > 0 && t < sizeof(tmp));
    while (t > 0) out[n++] = tmp[--t];
    for (i = 0; unit[i] != '\0'; i++) out[n++] = unit[i];
    return n;
}

/* `a`, two spaces, `b` (a size) -- cut to the label's width. */
static void make_label(u8 *out, const u8 *a, u32 alen, const u8 *size, u32 slen)
{
    char sz[24];
    u32 sn = put_size(sz, size, slen), n = 0, i;

    for (i = 0; i < alen && n + 1 < PI_LABEL_CAP; i++) out[n++] = a[i];
    if (n + 2 + sn < PI_LABEL_CAP) {
        out[n++] = ' ';
        out[n++] = ' ';
        for (i = 0; i < sn; i++) out[n++] = (u8)sz[i];
    }
    out[n] = '\0';
}

/* Where a fetched document goes: the documents folder, or the program's
   own (the configuration file's), joined to `name`. */
static wp_bool restore_path(wp_app_ctx *ctx, const char *name, char *out, u32 cap)
{
    const char *dir = ctx->settings.docdir;
    u32 dl = (u32)strlen(dir), nl = (u32)strlen(name);
    wp_bool add;

    if (dl == 0 && ctx->config_path != NULL) {
        dir = ctx->config_path;
        dl = (u32)(leaf(dir) - dir);
    }
    add = (wp_bool)(dl > 0 && dir[dl - 1] != '\\' && dir[dl - 1] != '/' && dir[dl - 1] != ':');
    if (dl + (add ? 1u : 0u) + nl + 1u > cap) return WP_FALSE;
    memcpy(out, dir, dl);
    if (add) out[dl++] = ctx->path_sep;
    memcpy(out + dl, name, nl + 1u);
    return WP_TRUE;
}

static wp_bool file_exists(const char *path)
{
    wp_bool there = WP_FALSE;

    return (wp_bool)(plat_file_exists(path, &there) == WP_OK && there);
}

static wp_bool ask_replace(wp_app_ctx *ctx, const char *name)
{
    static const char *const k_opts[] = { "Replace", "Cancel" };
    char title[PI_NAME_MAX + 24];
    const u8 *labels[2];
    u32 lens[2], n = (u32)strlen(name);
    wp_bool cancelled = WP_TRUE;
    u32 chosen = 1;

    if (n > 40u) n = 40u;
    memcpy(title, "Replace ", 8);
    memcpy(title + 8, name, n);
    memcpy(title + 8 + n, " here?", 7);
    labels[0] = (const u8 *)k_opts[0]; lens[0] = 7;
    labels[1] = (const u8 *)k_opts[1]; lens[1] = 6;
    if (plat_dialog_choice(ctx->window, (const u8 *)title, (u32)strlen(title), labels, lens, 2, 1,
                           &cancelled, &chosen) != WP_OK) return WP_FALSE;
    return (wp_bool)(!cancelled && chosen == 0);
}

static wp_status fetch_to(wp_app_ctx *ctx, const char *name, u32 version, const char *path)
{
    u8 payload[2u + 2u * PI_NAME_MAX + 4u];
    u32 pos = 0;
    offload_request req;
    offload_response resp;
    wp_status st;

    if (!put_name(payload, &pos, wp_settings_machine(ctx)) || !put_name(payload, &pos, name)) return WP_RANGE;
    payload[pos++] = (u8)(version >> 24);
    payload[pos++] = (u8)(version >> 16);
    payload[pos++] = (u8)(version >> 8);
    payload[pos++] = (u8)version;
    req.op = OFFLOAD_FETCH;
    req.payload = payload;
    req.payload_len = pos;
    req.timeout_ms = (u32)PI_TIMEOUT_MS;
    resp.result = NULL;
    resp.result_len = 0;
    st = offload_submit(&req, &resp);
    if (st != WP_OK || resp.status != OFFLOAD_OK) {
        if (st == WP_OK) offload_response_free(&resp);
        wp_action_tell(ctx, st != WP_OK ? "The Pi did not answer" : "The Pi couldn't find that copy");
        return WP_ERR;
    }
    st = wp_action_safe_write(path, resp.result, resp.result_len);
    if (st == WP_UNSUPPORTED) st = wp_action_write_whole(path, resp.result, resp.result_len);
    offload_response_free(&resp);
    if (st != WP_OK) wp_action_tell(ctx, "Couldn't write the copy from the Pi");
    return st;
}

wp_bool wp_pistore_pick(wp_app_ctx *ctx, char *out, u32 cap)
{
    static u8 labels[PI_LIST_MAX][PI_LABEL_CAP];
    static char name[PI_NAME_MAX + 1];
    static u8 *lines[PI_LIST_MAX];      /* static: the Amiga's 4 KB stack */
    offload_response resp;
    u32 n, picked, version = 0, len, vlen, slen;
    const u8 *f, *s;

    if (!wp_pistore_able(ctx)) {
        wp_action_tell(ctx, "Open from Pi needs a gateway that keeps documents");
        return WP_FALSE;
    }
    if (ctx->repaint != NULL) ctx->repaint(ctx);     /* the unsaved-changes question's pieces */

    /* Which document. */
    if (ask_list(ctx, NULL, &resp) != WP_OK) return WP_FALSE;
    n = split_lines(resp.result, resp.result_len, lines, PI_LIST_MAX);
    if (n == 0) {
        offload_response_free(&resp);
        wp_action_tell(ctx, "The Pi keeps no documents for this machine");
        return WP_FALSE;
    }
    for (picked = 0; picked < n; picked++) {
        f = field(lines[picked], 0, &len);
        s = field(lines[picked], 2, &slen);
        make_label(labels[picked], f, len, s, slen);
    }
    if (!choose(ctx, "Open from Pi", labels, n, &picked)) {
        offload_response_free(&resp);
        return WP_FALSE;
    }
    f = field(lines[picked], 0, &len);
    if (len > PI_NAME_MAX) len = PI_NAME_MAX;
    memcpy(name, f, len);
    name[len] = '\0';
    f = field(lines[picked], 1, &vlen);
    n = 0;
    while (vlen-- > 0 && *f >= '0' && *f <= '9') n = n * 10u + (u32)(*f++ - '0');
    offload_response_free(&resp);

    /* Which version, when there is a choice. */
    if (n > 1) {
        if (ask_list(ctx, name, &resp) != WP_OK) return WP_FALSE;
        n = split_lines(resp.result, resp.result_len, lines, PI_LIST_MAX);
        for (picked = 0; picked < n; picked++) {
            f = field(lines[picked], 2, &len);
            s = field(lines[picked], 1, &slen);
            make_label(labels[picked], f, len, s, slen);
        }
        if (n == 0 || !choose(ctx, "Which copy?", labels, n, &picked)) {
            offload_response_free(&resp);
            return WP_FALSE;
        }
        f = field(lines[picked], 0, &vlen);
        while (vlen-- > 0 && *f >= '0' && *f <= '9') version = version * 10u + (u32)(*f++ - '0');
        offload_response_free(&resp);
    }

    if (!restore_path(ctx, name, out, cap)) {
        wp_action_tell(ctx, "That name is too long for this machine");
        return WP_FALSE;
    }
    if (file_exists(out) && !ask_replace(ctx, name)) return WP_FALSE;
    if (ctx->repaint != NULL) ctx->repaint(ctx);     /* the choices' pieces gone */
    return (wp_bool)(fetch_to(ctx, name, version, out) == WP_OK);
}
