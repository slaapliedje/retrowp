/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tools.h"
#include "app_ctx.h"
#include "keymap.h"
#include "offload.h"
#include "endian.h"
#include "platform.h"
#include "spell.h"
#include "grammar.h"

#define WP_TOOLS_WORD_MAX        64
#define WP_TOOLS_PARAGRAPH_MAX   2000uL /* matches the gateway's CHAT_PROMPT_MAX_LEN */
#define WP_TOOLS_TIMEOUT_MS      90000uL /* generous: a cold model load on the Pi */
#define WP_TOOLS_REPLY_SHOWN     180
#define WP_TOOLS_SUGGESTIONS     3
#define WP_TOOLS_GRAMMAR_ISSUES  16u
#define WP_TOOLS_MSG_CAP         260

/* ---- text helpers ---------------------------------------------------- */

static wp_bool is_alpha(u8 c)
{
    return (wp_bool)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static void append_str(char *buf, u32 cap, u32 *pos, const char *s)
{
    while (*s != '\0' && *pos + 1 < cap) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void append_bytes(char *buf, u32 cap, u32 *pos, const u8 *s, u32 len)
{
    u32 i;

    for (i = 0; i < len && *pos + 1 < cap; i++) buf[(*pos)++] = (char)s[i];
    buf[*pos] = '\0';
}

static void append_u32(char *buf, u32 cap, u32 *pos, u32 value)
{
    char digits[12];
    u32 n = 0;

    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < (u32)sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && *pos + 1 < cap) buf[(*pos)++] = digits[--n];
    buf[*pos] = '\0';
}

/* The ASCII word ending at (or just before) the caret, within its run. */
static wp_bool word_before_cursor(wp_app_ctx *ctx, u8 *out, u32 cap, u32 *out_len)
{
    const wp_pos *cur = wp_keymap_cursor(ctx);
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    const u8 *bytes;
    u32 offset, end, start;

    if (!ctx->doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(ctx->doc, cur->section);
    para = wp_section_paragraph_get(sec, cur->paragraph);
    if (cur->run >= para->run_count) return WP_FALSE;
    run = wp_paragraph_run_get(para, (u16)cur->run);
    bytes = wp_strpool_get(&ctx->doc->strings, run->text);
    if (bytes == NULL) return WP_FALSE;

    offset = cur->offset;
    if (offset > run->text.length) offset = run->text.length;
    end = offset;
    while (end > 0 && !is_alpha(bytes[end - 1])) end--;
    start = end;
    while (start > 0 && is_alpha(bytes[start - 1])) start--;
    if (start == end || end - start >= cap) return WP_FALSE;

    memcpy(out, bytes + start, end - start);
    *out_len = end - start;
    return WP_TRUE;
}

/* The caret's paragraph as one run of bytes. */
static wp_bool paragraph_text(wp_app_ctx *ctx, u8 *out, u32 cap, u32 *out_len)
{
    const wp_pos *cur = wp_keymap_cursor(ctx);
    wp_section *sec;
    wp_paragraph *para;
    u32 written = 0;
    u16 i;

    if (!ctx->doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(ctx->doc, cur->section);
    para = wp_section_paragraph_get(sec, cur->paragraph);
    for (i = 0; i < para->run_count; i++) {
        wp_run *run = wp_paragraph_run_get(para, i);

        if (run->text.length > 0) {
            const u8 *bytes = wp_strpool_get(&ctx->doc->strings, run->text);

            if (bytes == NULL || written + run->text.length > cap) return WP_FALSE;
            memcpy(out + written, bytes, run->text.length);
            written += run->text.length;
        }
    }
    *out_len = written;
    return (wp_bool)(written > 0);
}

/* ---- Thesaurus ------------------------------------------------------- */

wp_status wp_tools_thesaurus(wp_app_ctx *ctx, wp_tools_show_fn show)
{
    u8 word[WP_TOOLS_WORD_MAX];
    u32 word_len = 0;
    offload_request req;
    offload_response resp;
    char msg[WP_TOOLS_MSG_CAP];
    u32 pos = 0;

    if (!word_before_cursor(ctx, word, (u32)sizeof(word), &word_len)) {
        show(ctx, "Thesaurus", "Place the cursor right after a word, then choose Thesaurus again.");
        return WP_OK;
    }
    if (!offload_available()) {
        show(ctx, "Thesaurus", "No offload device connected.");
        return WP_ERR;
    }

    req.op = OFFLOAD_THESAURUS;
    req.payload = word;
    req.payload_len = word_len;
    req.timeout_ms = (u32)WP_TOOLS_TIMEOUT_MS;
    if (offload_submit(&req, &resp) != WP_OK) {
        show(ctx, "Thesaurus", "Thesaurus request failed or timed out.");
        return WP_ERR;
    }
    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        show(ctx, "Thesaurus", "The connected gateway doesn't support Thesaurus.");
        return WP_UNSUPPORTED;
    }

    append_str(msg, (u32)sizeof(msg), &pos, "Synonyms for ");
    append_bytes(msg, (u32)sizeof(msg), &pos, word, word_len);
    append_str(msg, (u32)sizeof(msg), &pos, ": ");
    {
        /* u16 count, then count x (u16 length, bytes). */
        u16 count = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
        u32 rpos = 2;
        u32 i;

        if (count == 0) append_str(msg, (u32)sizeof(msg), &pos, "(none found)");
        for (i = 0; i < count && rpos + 2 <= resp.result_len; i++) {
            u16 wlen = be_read16(resp.result + rpos);

            rpos += 2;
            if (rpos + wlen > resp.result_len) break;
            if (i > 0) append_str(msg, (u32)sizeof(msg), &pos, ", ");
            append_bytes(msg, (u32)sizeof(msg), &pos, resp.result + rpos, wlen);
            rpos += wlen;
        }
    }
    offload_response_free(&resp);
    show(ctx, "Thesaurus", msg);
    return WP_OK;
}

/* ---- Chat ------------------------------------------------------------ */

wp_status wp_tools_chat(wp_app_ctx *ctx, wp_tools_show_fn show)
{
    u8 *paragraph;
    u32 paragraph_len = 0;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    char msg[WP_TOOLS_MSG_CAP];
    u32 pos = 0;
    u32 shown;

    if (!ctx->doc_loaded) return WP_OK;
    paragraph = (u8 *)mem_alloc(WP_TOOLS_PARAGRAPH_MAX);
    if (paragraph == NULL) return WP_NOMEM;

    if (!paragraph_text(ctx, paragraph, (u32)WP_TOOLS_PARAGRAPH_MAX, &paragraph_len)) {
        mem_free(paragraph);
        show(ctx, "Chat", "Place the cursor in a paragraph with some text, then choose Chat again.");
        return WP_OK;
    }
    if (!offload_available()) {
        mem_free(paragraph);
        show(ctx, "Chat", "No offload device connected.");
        return WP_ERR;
    }
    /* The capability check before the request, so a gateway without a
       model answers at once instead of after a timeout. */
    if (offload_query(&caps) != WP_OK ||
        (caps.ops_supported & (1uL << (u32)OFFLOAD_CHAT)) == 0) {
        mem_free(paragraph);
        show(ctx, "Chat", "This gateway doesn't offer Chat.");
        return WP_UNSUPPORTED;
    }

    req.op = OFFLOAD_CHAT;
    req.payload = paragraph;
    req.payload_len = paragraph_len;
    req.timeout_ms = (u32)WP_TOOLS_TIMEOUT_MS;
    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(paragraph);
        show(ctx, "Chat", "Chat request failed or timed out.");
        return WP_ERR;
    }
    mem_free(paragraph);
    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        show(ctx, "Chat", "The connected gateway couldn't answer that.");
        return WP_ERR;
    }

    shown = (resp.result_len < (u32)WP_TOOLS_REPLY_SHOWN) ? resp.result_len : (u32)WP_TOOLS_REPLY_SHOWN;
    append_bytes(msg, (u32)sizeof(msg), &pos, resp.result, shown);
    if (shown < resp.result_len) append_str(msg, (u32)sizeof(msg), &pos, "...");
    offload_response_free(&resp);
    show(ctx, "Chat", msg);
    return WP_OK;
}

/* ---- Grammar Check --------------------------------------------------- */

/* The gateway's issue list: u16 count, then per issue u16 start, u16
   span, u16 message length, message, u16 suggestion length, suggestion.
   Copies the first into *out. */
static wp_bool first_gateway_issue(const u8 *buf, u32 len, grammar_issue *out)
{
    u16 mlen, slen;
    u32 pos = 2;

    if (len < 8 || be_read16(buf) == 0) return WP_FALSE;
    out->start_offset = be_read16(buf + pos); pos += 2;
    out->span_len = be_read16(buf + pos); pos += 2;
    mlen = be_read16(buf + pos); pos += 2;
    if (pos + mlen + 2 > len) return WP_FALSE;
    {
        u32 n = (mlen < (u32)sizeof(out->message) - 1) ? mlen : (u32)sizeof(out->message) - 1;
        memcpy(out->message, buf + pos, n);
        out->message[n] = '\0';
    }
    pos += mlen;
    slen = be_read16(buf + pos); pos += 2;
    if (pos + slen > len) return WP_FALSE;
    {
        u32 n = (slen < (u32)sizeof(out->suggestion) - 1) ? slen : (u32)sizeof(out->suggestion) - 1;
        memcpy(out->suggestion, buf + pos, n);
        out->suggestion[n] = '\0';
    }
    return WP_TRUE;
}

wp_status wp_tools_grammar_check(wp_app_ctx *ctx, wp_tools_show_fn show)
{
    u8 *paragraph;
    u32 paragraph_len = 0;
    u32 issue_count = 0;
    grammar_issue first;
    wp_bool have_first = WP_FALSE;
    wp_bool used_gateway;
    char msg[WP_TOOLS_MSG_CAP];
    u32 pos = 0;

    if (!ctx->doc_loaded) return WP_OK;
    paragraph = (u8 *)mem_alloc(WP_TOOLS_PARAGRAPH_MAX);
    if (paragraph == NULL) return WP_NOMEM;

    if (!paragraph_text(ctx, paragraph, (u32)WP_TOOLS_PARAGRAPH_MAX, &paragraph_len)) {
        mem_free(paragraph);
        show(ctx, "Grammar Check", "Place the cursor in a paragraph with some text, then choose Grammar Check again.");
        return WP_OK;
    }

    used_gateway = offload_available();
    if (used_gateway) {
        offload_request req;
        offload_response resp;

        req.op = OFFLOAD_GRAMMAR_CHECK;
        req.payload = paragraph;
        req.payload_len = paragraph_len;
        req.timeout_ms = (u32)WP_TOOLS_TIMEOUT_MS;
        if (offload_submit(&req, &resp) != WP_OK) {
            mem_free(paragraph);
            show(ctx, "Grammar Check", "Grammar Check request failed or timed out.");
            return WP_ERR;
        }
        if (resp.status != OFFLOAD_OK) {
            offload_response_free(&resp);
            mem_free(paragraph);
            show(ctx, "Grammar Check", "The connected gateway doesn't support Grammar Check.");
            return WP_UNSUPPORTED;
        }
        issue_count = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
        if (issue_count > 0) have_first = first_gateway_issue(resp.result, resp.result_len, &first);
        offload_response_free(&resp);
    } else {
        grammar_issue local[WP_TOOLS_GRAMMAR_ISSUES];

        issue_count = grammar_lite_check(paragraph, paragraph_len, local, (u32)WP_TOOLS_GRAMMAR_ISSUES);
        if (issue_count > 0) {
            first = local[0];
            have_first = WP_TRUE;
        }
    }
    mem_free(paragraph);

    if (issue_count == 0) {
        show(ctx, "Grammar Check", "No grammar issues found.");
        return WP_OK;
    }
    append_str(msg, (u32)sizeof(msg), &pos,
               used_gateway ? "Gateway found " : "No offload device attached -- local check found ");
    append_u32(msg, (u32)sizeof(msg), &pos, issue_count);
    append_str(msg, (u32)sizeof(msg), &pos, (issue_count == 1) ? " issue." : " issues.");
    if (have_first) {
        append_str(msg, (u32)sizeof(msg), &pos, " First: ");
        append_str(msg, (u32)sizeof(msg), &pos, first.message);
        if (first.suggestion[0] != '\0') {
            append_str(msg, (u32)sizeof(msg), &pos, " Suggestion: ");
            append_str(msg, (u32)sizeof(msg), &pos, first.suggestion);
        }
    }
    show(ctx, "Grammar Check", msg);
    return WP_OK;
}

/* ---- Spell Check ----------------------------------------------------- */

static spell_dict *g_dict = NULL;
static wp_bool g_dict_tried = WP_FALSE;

static spell_dict *dictionary(const char *dict_path)
{
    if (!g_dict_tried) {
        g_dict_tried = WP_TRUE;
        if (spell_dict_open(dict_path, &g_dict) != WP_OK) g_dict = NULL;
    }
    return g_dict;
}

void wp_tools_shutdown(void)
{
    if (g_dict != NULL) spell_dict_close(g_dict);
    g_dict = NULL;
    g_dict_tried = WP_FALSE;
}

/* The first ASCII word in document order the dictionary does not know. */
static wp_bool first_unknown_word(wp_app_ctx *ctx, spell_dict *dict, u8 *out, u32 *out_len)
{
    u32 si, pi;

    for (si = 0; si < ctx->doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(ctx->doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            u32 r;

            for (r = 0; r < para->run_count; r++) {
                wp_run *run = wp_paragraph_run_get(para, (u16)r);
                const u8 *bytes = wp_strpool_get(&ctx->doc->strings, run->text);
                u32 k = 0;

                if (bytes == NULL) continue;
                while (k < run->text.length) {
                    u32 start, wlen;
                    wp_bool known = WP_TRUE;

                    while (k < run->text.length && !is_alpha(bytes[k])) k++;
                    start = k;
                    while (k < run->text.length && is_alpha(bytes[k])) k++;
                    wlen = k - start;
                    if (wlen == 0 || wlen >= (u32)SPELL_MAX_WORD_LEN) continue;
                    if (spell_check_word(dict, bytes + start, wlen, &known) == WP_OK && !known) {
                        memcpy(out, bytes + start, wlen);
                        *out_len = wlen;
                        return WP_TRUE;
                    }
                }
            }
        }
    }
    return WP_FALSE;
}

wp_status wp_tools_spell_check(wp_app_ctx *ctx, const char *dict_path, wp_tools_show_fn show)
{
    spell_dict *dict;
    u8 word[SPELL_MAX_WORD_LEN];
    u32 word_len = 0;
    char suggestions[WP_TOOLS_SUGGESTIONS][SPELL_MAX_WORD_LEN];
    u32 sugg_count = 0;
    char msg[WP_TOOLS_MSG_CAP];
    u32 pos = 0;
    u32 i;

    if (!ctx->doc_loaded) return WP_OK;
    dict = dictionary(dict_path);
    if (dict == NULL) {
        append_str(msg, (u32)sizeof(msg), &pos, "Spell-check dictionary not found. Expected at ");
        append_str(msg, (u32)sizeof(msg), &pos, dict_path);
        show(ctx, "Spell Check", msg);
        return WP_ERR;
    }
    if (!first_unknown_word(ctx, dict, word, &word_len)) {
        show(ctx, "Spell Check", "No misspellings found.");
        return WP_OK;
    }

    spell_suggest(dict, word, word_len, suggestions, (u32)WP_TOOLS_SUGGESTIONS, &sugg_count);
    append_str(msg, (u32)sizeof(msg), &pos, "Unknown word: ");
    append_bytes(msg, (u32)sizeof(msg), &pos, word, word_len);
    if (sugg_count > 0) {
        append_str(msg, (u32)sizeof(msg), &pos, ". Suggestions: ");
        for (i = 0; i < sugg_count; i++) {
            if (i > 0) append_str(msg, (u32)sizeof(msg), &pos, ", ");
            append_str(msg, (u32)sizeof(msg), &pos, suggestions[i]);
        }
    } else {
        append_str(msg, (u32)sizeof(msg), &pos, " (no suggestions)");
    }
    show(ctx, "Spell Check", msg);
    return WP_OK;
}
