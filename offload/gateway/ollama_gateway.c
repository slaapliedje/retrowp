/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "types.h"
#include "wire.h"
#include "caps_codec.h"
#include "offload.h"
#include "gw_common.h"
#include "http_client.h"

/* Real-backend gateway (post-M6; GRAMMAR_CHECK added M8 T8.1; CHAT
   added M8 T8.3): answers OFFLOAD_THESAURUS, OFFLOAD_GRAMMAR_CHECK, and
   OFFLOAD_CHAT with actual Ollama (http://localhost:11434 by default)
   calls instead of mock_gateway.c's fixture table -- "not just a faked
   endpoint" per the request that prompted this file. Same wire
   protocol, same TCP/serial transport choice, same HELLO/CAPS handshake
   shape as mock_gateway.c (sharing gw_common.c so neither drifts); only
   these three request handlers' backend differs.

   Scope: THESAURUS, GRAMMAR_CHECK, and CHAT only. Every other op,
   including SPELL_CHECK (which mock_gateway.c does serve, from
   fixtures) and IMAGE (which NO gateway anywhere in this repo
   implements -- see docs/formats/offload-protocol.md for why), responds
   OFFLOAD_UNSUPPORTED here -- the same honest "absent gracefully" path
   the protocol already defines for ops no gateway implements. Leaving
   SPELL_CHECK unimplemented here was a deliberate choice made after
   testing, not an oversight: spelling correction needs to be RIGHT, and
   this project's on-device services/spell already covers that
   deterministically; a quick trial against deepseek-coder-v2:16b
   reported "teh" as a correctly spelled word, which is a bad user
   experience swap for the deterministic local checker.

   THREE independently-defaulted models, not one shared default:
   - THESAURUS keeps its M6 default, `deepseek-coder-v2:16b`
     (--model to override) -- untouched, still the only non-"thinking"
     model this gateway originally verified against, and there's no
     reason to disturb already-working, already-tested behavior just
     because a second op landed.
   - GRAMMAR_CHECK gets its OWN default, `gemma2:9b` (--grammar-model to
     override), chosen empirically: prompted with a paragraph containing
     several real errors (subject-verb agreement, "alot", "their" for
     "they're"), gemma2:9b reliably produced the rigid
     `QUOTE :: MESSAGE :: SUGGESTION` line format this file's parser
     depends on, with every QUOTE a genuine verbatim substring of the
     source paragraph. mistral:7b, tried the same way twice (including a
     retry with a few-shot example added to the prompt), never produced
     a clean 3-field line either time, and on the retry fabricated words
     that don't appear anywhere in the source paragraph at all -- see
     docs/formats/offload-protocol.md for the full comparison. THESAURUS
     has a much lower correctness bar (a real-vocabulary word list is
     valuable even if not exhaustive) than GRAMMAR_CHECK (a wrong quote
     or a badly-parsed line is actively worse than no answer), which is
     exactly why the two ops don't share one "good enough" default.
     Paying Ollama's cold-load cost separately the first time each op is
     used after a gap is the accepted cost of keeping THESAURUS's
     already-verified choice untouched.
   - CHAT shares GRAMMAR_CHECK's `gemma2:9b` default (--chat-model to
     override, independent of both --model and --grammar-model) rather
     than getting a third distinct default: CHAT is "freeform assist"
     with no rigid output format to parse (unlike GRAMMAR_CHECK) and no
     narrow-vocabulary lookup task (unlike THESAURUS) -- a general-
     purpose instruct model is simply the right category of model for
     it, and gemma2:9b already proved itself a reliable instruct
     follower during GRAMMAR_CHECK's own model comparison. Giving it an
     independent override flag anyway (rather than hard-reusing
     --grammar-model) keeps every op's model choice tunable on its own,
     the same "one op, one flag" convention THESAURUS and GRAMMAR_CHECK
     already established.

   **Never trust the model's own claimed position.** GRAMMAR_CHECK's
   wire response carries a byte offset/length for each issue, but an
   LLM is not reliably able to count bytes/characters to produce a
   correct one -- a well known weak spot for token-based models. So
   do_grammar_check() never uses a number the model produced: it asks
   the model to quote the offending text verbatim instead, then finds
   that exact quote in the ORIGINAL request paragraph via a real,
   deterministic substring search (find_substring, below) and computes
   the offset from that match. A quote that isn't found verbatim (a
   paraphrase, or an outright hallucination) is silently dropped --
   same "don't propagate garbage" convention parse_word_list already
   uses for THESAURUS. Every issue a caller ever receives therefore
   points at text that is genuinely present in the paragraph it asked
   about, independent of how faithfully the model followed instructions.

   --model/--grammar-model/--chat-model are plain CLI overrides, so a
   differently-equipped machine (or a smaller/faster instruct model
   pulled later) isn't locked to any default. Whatever model is loaded
   costs real
   wall-clock time on a cold start -- ~20s to load an 8.9GB model into
   memory was the measured cost in this project's own dev environment
   even on an otherwise-idle GPU, BEFORE the model can answer the first
   request -- so --ollama-timeout-ms defaults generously (60s) rather
   than to something that suits an already-warm model. Ollama unloads an
   idle model after its own keep_alive window, so the first request
   after a gap repays that cost again; this is inherent to the
   "optional, offloaded" nature of the feature (CLAUDE.md's Offload
   Rules), not a bug in this file.

   No libcurl dependency: http_client.c hand-rolls the one HTTP/1.1 POST
   this needs directly over a socket. */

#define OLLAMA_DEFAULT_HOST "127.0.0.1"
#define OLLAMA_DEFAULT_PORT 11434
#define OLLAMA_DEFAULT_MODEL "deepseek-coder-v2:16b"
#define OLLAMA_DEFAULT_GRAMMAR_MODEL "gemma2:9b"
#define OLLAMA_DEFAULT_CHAT_MODEL "gemma2:9b"
#define OLLAMA_DEFAULT_TIMEOUT_MS 60000u

#define THESAURUS_MAX_WORDS    8
#define THESAURUS_MAX_WORD_LEN 64
#define GATEWAY_WORD_MAX_LEN   128 /* truncation bound for the incoming THESAURUS request word */

#define GRAMMAR_MAX_ISSUES        16
#define GRAMMAR_QUOTE_MAX_LEN     256
#define GRAMMAR_MSG_MAX_LEN       160
#define GRAMMAR_SUGG_MAX_LEN      160
#define GRAMMAR_PARAGRAPH_MAX_LEN 2000 /* truncation bound for the incoming GRAMMAR_CHECK paragraph */

#define CHAT_PROMPT_MAX_LEN 2000 /* truncation bound for the incoming CHAT prompt */

static int parse_int_flag(const char *arg, const char *name, int *out)
{
    size_t nlen = strlen(name);

    if (strncmp(arg, name, nlen) == 0 && arg[nlen] == '=') {
        *out = atoi(arg + nlen + 1);
        return 1;
    }
    return 0;
}

static int parse_str_flag(const char *arg, const char *name, const char **out)
{
    size_t nlen = strlen(name);

    if (strncmp(arg, name, nlen) == 0 && arg[nlen] == '=') {
        *out = arg + nlen + 1;
        return 1;
    }
    return 0;
}

/* Escapes word_len bytes of word into out as the CONTENTS of a JSON
   string (no surrounding quotes). Control characters other than
   \n/\r/\t are emitted as \u00XX; everything else (including any raw
   UTF-8 continuation bytes) passes through unchanged -- JSON strings
   are UTF-8-safe as long as ", \, and control bytes are escaped, which
   is all this needs to guarantee for a single word embedded in a
   prompt. */
static u32 json_escape(const u8 *word, u32 word_len, char *out, u32 out_cap)
{
    static const char hex[] = "0123456789abcdef";
    u32 i, pos = 0;

    for (i = 0; i < word_len && pos + 6 < out_cap; i++) {
        u8 c = word[i];

        switch (c) {
        case '"':  out[pos++] = '\\'; out[pos++] = '"';  break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n';  break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r';  break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't';  break;
        default:
            if (c < 0x20) {
                out[pos++] = '\\'; out[pos++] = 'u'; out[pos++] = '0'; out[pos++] = '0';
                out[pos++] = hex[(c >> 4) & 0xF];
                out[pos++] = hex[c & 0xF];
            } else {
                out[pos++] = (char)c;
            }
        }
    }
    out[pos] = '\0';
    return pos;
}

/* Builds the /api/generate request body. The prompt is deliberately
   rigid ("ONLY a comma-separated list... no explanation") because this
   gateway's parser (parse_word_list below) has no fallback for free-form
   prose -- tested against the default model, this phrasing reliably
   produces a bare list (see docs/formats/offload-protocol.md for the
   observed examples, including how an unknown word degrades to an empty
   answer rather than an error). Returns the body length, or 0 if it
   didn't fit (out_cap too small for this word + template). */
static u32 build_generate_request(const char *model, const u8 *word, u32 word_len,
                                   char *out, u32 out_cap)
{
    char escaped[GATEWAY_WORD_MAX_LEN * 6 + 1];
    int written;

    json_escape(word, word_len, escaped, (u32)sizeof(escaped));

    written = snprintf(out, out_cap,
        "{\"model\":\"%s\",\"prompt\":\"Word: %s\\nList up to %d English synonyms for "
        "this word. Reply with ONLY a comma-separated list of words, no numbering, no "
        "explanation. If you do not know this word or it has no synonyms, reply with an "
        "empty line.\",\"stream\":false}",
        model, escaped, THESAURUS_MAX_WORDS);

    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (u32)written;
}

/* Finds "field_name":"..." within json (json_len bytes, not necessarily
   NUL-terminated) and copies the unescaped string value into out
   (out_cap bytes, NUL-terminated). Handles \", \\, \n, \r, \t; \uXXXX
   escapes are skipped (dropped, not translated) -- English synonym
   lists are not expected to need them, and this gateway's ASCII-ish
   scope elsewhere (services/spell is ASCII-only too) makes that an
   acceptable simplification, not a silent correctness gap in the
   feature this exists for. Returns WP_FALSE if the field isn't found. */
static wp_bool extract_json_string_field(const u8 *json, u32 json_len, const char *field_name,
                                          char *out, u32 out_cap, u32 *out_len)
{
    char needle[32];
    u32 needle_len;
    u32 i;

    snprintf(needle, sizeof(needle), "\"%s\":\"", field_name);
    needle_len = (u32)strlen(needle);

    for (i = 0; i + needle_len <= json_len; i++) {
        u32 pos;
        u32 out_pos;

        if (memcmp(json + i, needle, needle_len) != 0) continue;

        pos = i + needle_len;
        out_pos = 0;
        while (pos < json_len && json[pos] != '"' && out_pos + 1 < out_cap) {
            if (json[pos] == '\\' && pos + 1 < json_len) {
                u8 esc = json[pos + 1];
                switch (esc) {
                case '"':  out[out_pos++] = '"';  pos += 2; break;
                case '\\': out[out_pos++] = '\\'; pos += 2; break;
                case 'n':  out[out_pos++] = '\n'; pos += 2; break;
                case 't':  out[out_pos++] = '\t'; pos += 2; break;
                case 'r':  out[out_pos++] = '\r'; pos += 2; break;
                case 'u':  pos += (json_len - pos >= 6) ? 6 : (json_len - pos); break;
                default:   out[out_pos++] = (char)esc; pos += 2; break;
                }
            } else {
                out[out_pos++] = (char)json[pos];
                pos++;
            }
        }
        out[out_pos] = '\0';
        if (out_len) *out_len = out_pos;
        return WP_TRUE;
    }
    return WP_FALSE;
}

/* Splits text on commas/newlines into up to max_words tokens, trims
   surrounding whitespace, and keeps only tokens that look like a single
   plausible word (letters, hyphen, apostrophe; 1..THESAURUS_MAX_WORD_LEN-1
   bytes) -- silently dropping anything else (an empty token from a
   trailing comma, a stray explanatory sentence the model added despite
   being told not to, and so on) rather than propagating garbage into
   the response payload. Returns the number of words written to out. */
static u32 parse_word_list(const char *text, u32 text_len,
                            char out[][THESAURUS_MAX_WORD_LEN], u32 max_words)
{
    u32 count = 0;
    u32 i = 0;

    while (i < text_len && count < max_words) {
        u32 start, end, len, k;
        wp_bool valid;

        while (i < text_len && (text[i] == ',' || text[i] == '\n' || text[i] == ' ' ||
                                 text[i] == '\t' || text[i] == '\r')) {
            i++;
        }
        start = i;
        while (i < text_len && text[i] != ',' && text[i] != '\n') i++;

        end = i;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                                text[end - 1] == '\r')) {
            end--;
        }
        len = end - start;

        if (len == 0 || len >= THESAURUS_MAX_WORD_LEN) continue;

        valid = WP_TRUE;
        for (k = 0; k < len; k++) {
            char c = text[start + k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '\'')) {
                valid = WP_FALSE;
                break;
            }
        }
        if (!valid) continue;

        memcpy(out[count], text + start, len);
        out[count][len] = '\0';
        count++;
    }
    return count;
}

/* The one real capability this gateway has: word -> Ollama -> synonym
   list, wire-encoded exactly like mock_gateway's fixture-backed answer.
   Returns the offload_status byte for the response frame; *resp_len is
   left at 0 unless status is OFFLOAD_OK. count==0 (an empty word list)
   is still OFFLOAD_OK -- "no synonyms found" is a valid answer, same
   convention as the fixture-backed gateway. */
static u8 do_thesaurus(const char *ollama_host, u16 ollama_port, const char *model,
                        u32 ollama_timeout_ms, const u8 *word, u32 word_len,
                        u8 *resp_payload, u32 resp_cap, u32 *resp_len)
{
    char req_body[GATEWAY_WORD_MAX_LEN * 6 + 512];
    u32 req_len;
    u8 http_body[8192];
    u32 http_body_len = 0;
    char field[4096];
    u32 field_len = 0;
    char words[THESAURUS_MAX_WORDS][THESAURUS_MAX_WORD_LEN];
    const char *word_ptrs[THESAURUS_MAX_WORDS];
    u32 count, i;
    u32 clamped_len = (word_len > GATEWAY_WORD_MAX_LEN) ? GATEWAY_WORD_MAX_LEN : word_len;

    *resp_len = 0;

    req_len = build_generate_request(model, word, clamped_len, req_body, (u32)sizeof(req_body));
    if (req_len == 0) return (u8)OFFLOAD_ERR;

    if (http_post_json(ollama_host, ollama_port, "/api/generate",
                        (const u8 *)req_body, req_len, ollama_timeout_ms,
                        http_body, (u32)sizeof(http_body), &http_body_len) != WP_OK) {
        return (u8)OFFLOAD_ERR;
    }

    if (!extract_json_string_field(http_body, http_body_len, "response",
                                    field, (u32)sizeof(field), &field_len)) {
        return (u8)OFFLOAD_ERR;
    }

    count = parse_word_list(field, field_len, words, THESAURUS_MAX_WORDS);
    for (i = 0; i < count; i++) word_ptrs[i] = words[i];

    *resp_len = gw_encode_word_list(word_ptrs, count, resp_payload, resp_cap);
    return (u8)OFFLOAD_OK;
}

/* Builds the /api/generate request body for GRAMMAR_CHECK. Deliberately
   rigid, same spirit as build_generate_request above: this file's line
   parser (below) has no fallback for free-form prose, so the model is
   told exactly what shape to answer in and given one worked example
   (found during prompt testing to noticeably improve format compliance
   over the instruction alone). See the file header for why the actual
   position numbers are never trusted even when a model DOES follow this
   format faithfully. */
static u32 build_grammar_request(const char *model, const u8 *paragraph, u32 paragraph_len,
                                  char *out, u32 out_cap)
{
    char escaped[GRAMMAR_PARAGRAPH_MAX_LEN * 6 + 1];
    int written;

    json_escape(paragraph, paragraph_len, escaped, (u32)sizeof(escaped));

    written = snprintf(out, out_cap,
        "{\"model\":\"%s\",\"prompt\":\"Paragraph: %s\\nFind grammar and usage issues in this "
        "paragraph. For each issue, output one line in EXACTLY this format, no other words on "
        "the line:\\nQUOTE :: MESSAGE :: SUGGESTION\\nExample line: teh dog :: Misspelled word "
        ":: the dog\\nQUOTE must be copied verbatim, character for character, from the paragraph "
        "above -- do not paraphrase it. Do not repeat the words QUOTE, MESSAGE, or SUGGESTION "
        "anywhere in your output; use only the :: separator. Output ONLY these lines, no "
        "numbering, no preamble, no summary. If there are no issues, output exactly: "
        "NONE\",\"stream\":false}",
        model, escaped);

    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (u32)written;
}

/* Byte-for-byte search for needle within hay -- deliberately NOT
   libc memmem (a GNU/BSD extension, not standard C89/POSIX baseline)
   to keep this file's portability posture consistent with the rest of
   offload/gateway (see http_client.c's own "no libcurl" rationale).
   Returns a pointer to the first match, or NULL. */
static const u8 *find_substring(const u8 *hay, u32 hay_len, const u8 *needle, u32 needle_len)
{
    u32 i;

    if (needle_len == 0 || needle_len > hay_len) return NULL;
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return hay + i;
    }
    return NULL;
}

/* Finds the next "::" in text[from..len). Returns its byte offset, or
   len if not found -- used both as a delimiter search and, via the
   returned len-on-failure sentinel, as the "this line doesn't have
   enough fields" signal that lets a malformed line (mistral:7b's
   failure mode -- see the file header) fall through and get dropped
   without any special-case handling. */
static u32 find_delim(const char *text, u32 len, u32 from)
{
    u32 i;

    for (i = from; i + 1 < len; i++) {
        if (text[i] == ':' && text[i + 1] == ':') return i;
    }
    return len;
}

/* Trims leading/trailing spaces/tabs/CRs from text[start..end) and
   copies the result into out (NUL-terminated, truncated to out_cap - 1
   bytes if needed). Returns the copied length. */
static u32 trim_copy(const char *text, u32 start, u32 end, char *out, u32 out_cap)
{
    u32 len;

    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) end--;

    len = end - start;
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, text + start, len);
    out[len] = '\0';
    return len;
}

/* Real capability #2: paragraph -> Ollama -> a verified issue list.
   Parses `field` (the model's raw text response) one line at a time,
   requiring exactly two "::" separators (three fields); any line that
   doesn't match that shape, or whose QUOTE field isn't a real, exact
   substring of the ORIGINAL request paragraph, is silently dropped --
   this is what makes an out-of-format response (mistral:7b) or a
   hallucinated quote degrade to "fewer issues reported," never a wrong
   or unusable offset. A completely clean paragraph, or a "NONE" answer
   (which never matches the "::" line shape and so is naturally dropped
   without any special case), both correctly yield issue_count == 0. */
static u32 parse_grammar_issues(const char *field, u32 field_len,
                                 const u8 *paragraph, u32 paragraph_len,
                                 u16 *starts, u16 *lens,
                                 char messages[][GRAMMAR_MSG_MAX_LEN],
                                 char suggestions[][GRAMMAR_SUGG_MAX_LEN],
                                 u32 max_issues)
{
    u32 issue_count = 0;
    u32 line_start = 0;
    u32 i;

    for (i = 0; i <= field_len && issue_count < max_issues; i++) {
        if (i == field_len || field[i] == '\n') {
            u32 line_end = i;
            u32 d1 = find_delim(field, line_end, line_start);

            if (d1 < line_end) {
                u32 d2 = find_delim(field, line_end, d1 + 2);

                if (d2 < line_end) {
                    char quote_buf[GRAMMAR_QUOTE_MAX_LEN];
                    u32 quote_len = trim_copy(field, line_start, d1, quote_buf, sizeof(quote_buf));

                    if (quote_len > 0) {
                        const u8 *match = find_substring(paragraph, paragraph_len,
                                                          (const u8 *)quote_buf, quote_len);

                        if (match != NULL) {
                            trim_copy(field, d1 + 2, d2, messages[issue_count], GRAMMAR_MSG_MAX_LEN);
                            trim_copy(field, d2 + 2, line_end, suggestions[issue_count],
                                      GRAMMAR_SUGG_MAX_LEN);
                            starts[issue_count] = (u16)(match - paragraph);
                            lens[issue_count] = (u16)quote_len;
                            issue_count++;
                        }
                    }
                }
            }
            line_start = i + 1;
        }
    }
    return issue_count;
}

static u8 do_grammar_check(const char *ollama_host, u16 ollama_port, const char *model,
                            u32 ollama_timeout_ms, const u8 *paragraph, u32 paragraph_len,
                            u8 *resp_payload, u32 resp_cap, u32 *resp_len)
{
    char req_body[GRAMMAR_PARAGRAPH_MAX_LEN * 6 + 1024];
    u32 req_len;
    u8 http_body[16384];
    u32 http_body_len = 0;
    char field[16384];
    u32 field_len = 0;
    u32 clamped_len = (paragraph_len > GRAMMAR_PARAGRAPH_MAX_LEN) ? GRAMMAR_PARAGRAPH_MAX_LEN
                                                                   : paragraph_len;

    u16 starts[GRAMMAR_MAX_ISSUES];
    u16 lens[GRAMMAR_MAX_ISSUES];
    char messages[GRAMMAR_MAX_ISSUES][GRAMMAR_MSG_MAX_LEN];
    char suggestions[GRAMMAR_MAX_ISSUES][GRAMMAR_SUGG_MAX_LEN];
    const char *message_ptrs[GRAMMAR_MAX_ISSUES];
    const char *suggestion_ptrs[GRAMMAR_MAX_ISSUES];
    u32 issue_count, i;

    *resp_len = 0;

    req_len = build_grammar_request(model, paragraph, clamped_len, req_body, (u32)sizeof(req_body));
    if (req_len == 0) return (u8)OFFLOAD_ERR;

    if (http_post_json(ollama_host, ollama_port, "/api/generate",
                        (const u8 *)req_body, req_len, ollama_timeout_ms,
                        http_body, (u32)sizeof(http_body), &http_body_len) != WP_OK) {
        return (u8)OFFLOAD_ERR;
    }

    if (!extract_json_string_field(http_body, http_body_len, "response",
                                    field, (u32)sizeof(field), &field_len)) {
        return (u8)OFFLOAD_ERR;
    }

    issue_count = parse_grammar_issues(field, field_len, paragraph, clamped_len,
                                        starts, lens, messages, suggestions, GRAMMAR_MAX_ISSUES);
    for (i = 0; i < issue_count; i++) {
        message_ptrs[i] = messages[i];
        suggestion_ptrs[i] = suggestions[i];
    }

    *resp_len = gw_encode_grammar_issues(starts, lens, message_ptrs, suggestion_ptrs,
                                          issue_count, resp_payload, resp_cap);
    return (u8)OFFLOAD_OK;
}

/* Builds the /api/generate request body for CHAT -- no rigid output
   format requested (unlike build_grammar_request), since there's
   nothing to parse out of the reply: the whole "response" field IS the
   answer, shipped back byte-for-byte. */
static u32 build_chat_request(const char *model, const u8 *prompt, u32 prompt_len,
                               char *out, u32 out_cap)
{
    char escaped[CHAT_PROMPT_MAX_LEN * 6 + 1];
    int written;

    json_escape(prompt, prompt_len, escaped, (u32)sizeof(escaped));

    written = snprintf(out, out_cap, "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false}",
                        model, escaped);

    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (u32)written;
}

/* Real capability #3: prompt -> Ollama -> reply, shipped back verbatim.
   No candidate parsing, no verification pass -- unlike THESAURUS (a
   comma list to split) and GRAMMAR_CHECK (a quote to re-verify against
   the source), a chat reply has no internal structure this gateway
   needs to trust or distrust; it either fits in resp_cap or gets
   truncated, the same bounded-but-not-failing posture every other
   bulk-text response in this project takes. */
static u8 do_chat(const char *ollama_host, u16 ollama_port, const char *model,
                   u32 ollama_timeout_ms, const u8 *prompt, u32 prompt_len,
                   u8 *resp_payload, u32 resp_cap, u32 *resp_len)
{
    char req_body[CHAT_PROMPT_MAX_LEN * 6 + 256];
    u32 req_len;
    u8 http_body[16384];
    u32 http_body_len = 0;
    char field[16384];
    u32 field_len = 0;
    u32 clamped_len = (prompt_len > CHAT_PROMPT_MAX_LEN) ? CHAT_PROMPT_MAX_LEN : prompt_len;

    *resp_len = 0;

    req_len = build_chat_request(model, prompt, clamped_len, req_body, (u32)sizeof(req_body));
    if (req_len == 0) return (u8)OFFLOAD_ERR;

    if (http_post_json(ollama_host, ollama_port, "/api/generate",
                        (const u8 *)req_body, req_len, ollama_timeout_ms,
                        http_body, (u32)sizeof(http_body), &http_body_len) != WP_OK) {
        return (u8)OFFLOAD_ERR;
    }

    if (!extract_json_string_field(http_body, http_body_len, "response",
                                    field, (u32)sizeof(field), &field_len)) {
        return (u8)OFFLOAD_ERR;
    }

    if (field_len > resp_cap) field_len = resp_cap;
    memcpy(resp_payload, field, field_len);
    *resp_len = field_len;
    return (u8)OFFLOAD_OK;
}

int main(int argc, char **argv)
{
    const char *ollama_host = OLLAMA_DEFAULT_HOST;
    int ollama_port = OLLAMA_DEFAULT_PORT;
    const char *model = OLLAMA_DEFAULT_MODEL;
    const char *grammar_model = OLLAMA_DEFAULT_GRAMMAR_MODEL;
    const char *chat_model = OLLAMA_DEFAULT_CHAT_MODEL;
    int ollama_timeout_ms = (int)OLLAMA_DEFAULT_TIMEOUT_MS;
    int hello_timeout_ms = 5000;
    const char *serial_rx = NULL;
    const char *serial_tx = NULL;
    int i;

    int listen_fd = -1;
    int conn_fd = -1;
    struct sockaddr_in addr;
    socklen_t addrlen;
    gw_ctx ctx;

    for (i = 1; i < argc; i++) {
        if (parse_str_flag(argv[i], "--ollama-host", &ollama_host)) continue;
        if (parse_int_flag(argv[i], "--ollama-port", &ollama_port)) continue;
        if (parse_str_flag(argv[i], "--model", &model)) continue;
        if (parse_str_flag(argv[i], "--grammar-model", &grammar_model)) continue;
        if (parse_str_flag(argv[i], "--chat-model", &chat_model)) continue;
        if (parse_int_flag(argv[i], "--ollama-timeout-ms", &ollama_timeout_ms)) continue;
        if (parse_int_flag(argv[i], "--hello-timeout-ms", &hello_timeout_ms)) continue;
        if (strncmp(argv[i], "--serial-rx=", 12) == 0) { serial_rx = argv[i] + 12; continue; }
        if (strncmp(argv[i], "--serial-tx=", 12) == 0) { serial_tx = argv[i] + 12; continue; }
    }

    if (serial_rx != NULL && serial_tx != NULL) {
        int rx_fd = gw_open_fifo_rdwr(serial_rx);
        int tx_fd = gw_open_fifo_rdwr(serial_tx);

        if (rx_fd < 0 || tx_fd < 0) return 1;

        ctx.read_fd = rx_fd;
        ctx.write_fd = tx_fd;

        printf("serial\n");
        fflush(stdout);
    } else {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return 1;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;
        if (listen(listen_fd, 1) != 0) return 1;

        addrlen = sizeof(addr);
        if (getsockname(listen_fd, (struct sockaddr *)&addr, &addrlen) != 0) return 1;

        printf("%d\n", (int)ntohs(addr.sin_port));
        fflush(stdout);

        conn_fd = accept(listen_fd, NULL, NULL);
        close(listen_fd);
        if (conn_fd < 0) return 1;

        ctx.read_fd = conn_fd;
        ctx.write_fd = conn_fd;
    }

    /* --- handshake: expect HELLO, respond CAPS (THESAURUS only) --- */
    {
        wire_header hdr;
        u8 dummy[1];
        u32 dummy_len;
        offload_capabilities caps;
        u8 caps_payload[CAPS_PAYLOAD_SIZE];

        if (wire_read_frame(gw_read, &ctx, (u32)hello_timeout_ms, &hdr, dummy, 0, &dummy_len) != WP_OK
            || hdr.type != WIRE_MSG_HELLO) {
            close(ctx.read_fd);
            if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
            return 1;
        }

        memset(&caps, 0, sizeof(caps));
        caps.protocol_version = WIRE_VERSION;
        caps.ops_supported = (1uL << OFFLOAD_THESAURUS) | (1uL << OFFLOAD_GRAMMAR_CHECK) |
                             (1uL << OFFLOAD_CHAT);
        caps.max_payload_bytes = 4096;
        strncpy((char *)caps.gateway_name, "ollama-gateway", sizeof(caps.gateway_name) - 1);

        caps_encode(&caps, caps_payload);
        gw_send_frame(ctx.write_fd, WIRE_MSG_CAPS, 0, 0, 0, caps_payload, CAPS_PAYLOAD_SIZE, WP_FALSE);
    }

    /* --- request/response loop --- */
    for (;;) {
        wire_header req_hdr;
        /* Sized for a whole paragraph (GRAMMAR_CHECK), not just a word --
           THESAURUS requests still fit comfortably inside this same
           buffer, just using far less of it. */
        u8 word_buf[2048];
        u32 word_len;
        wp_status st;
        u8 resp_payload[8192];
        u32 resp_len = 0;
        u8 status;

        st = wire_read_frame(gw_read, &ctx, 120000, &req_hdr, word_buf, sizeof(word_buf), &word_len);
        if (st != WP_OK) break; /* client disconnected, or nothing for 120s -- exit cleanly */

        if (req_hdr.op == OFFLOAD_THESAURUS) {
            status = do_thesaurus(ollama_host, (u16)ollama_port, model, (u32)ollama_timeout_ms,
                                   word_buf, word_len, resp_payload, sizeof(resp_payload), &resp_len);
        } else if (req_hdr.op == OFFLOAD_GRAMMAR_CHECK) {
            status = do_grammar_check(ollama_host, (u16)ollama_port, grammar_model,
                                       (u32)ollama_timeout_ms, word_buf, word_len,
                                       resp_payload, sizeof(resp_payload), &resp_len);
        } else if (req_hdr.op == OFFLOAD_CHAT) {
            status = do_chat(ollama_host, (u16)ollama_port, chat_model,
                              (u32)ollama_timeout_ms, word_buf, word_len,
                              resp_payload, sizeof(resp_payload), &resp_len);
        } else {
            status = (u8)OFFLOAD_UNSUPPORTED;
            resp_len = 0;
        }

        gw_send_frame(ctx.write_fd, WIRE_MSG_RESPONSE, req_hdr.op, status, req_hdr.seq,
                      resp_payload, resp_len, WP_FALSE);
    }

    close(ctx.read_fd);
    if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
    return 0;
}
