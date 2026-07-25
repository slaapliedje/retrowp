#include <string.h>
#include <stddef.h>

#include <exec/types.h>
#include <intuition/classusr.h>
#include <intuition/classes.h>
#include <intuition/screens.h>
#include <utility/tagitem.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <devices/inputevent.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <proto/keymap.h>
#include <proto/asl.h>

#include "platform.h"
#include "app_shell.h"
#include "app_shell_internal.h"
#include "docmodel.h"
#include "layout.h"
#include "format_txt.h"
#include "editing.h"
#include "vimode.h"
#include "keyevent.h"
#include "keymap.h"
#include "config.h"
#include "app_ctx.h"
#include "actions.h"
#include "metrics_amiga_internal.h"
#include "offload.h"
#include "grammar.h"
#include "endian.h"
#include "help_doc.h"
#include "help_view.h"
#include "statusline.h"
#include "help_retrowp.h"

/* M9 T9.1a: non-UI platform.h seams only (memory/file/clipboard/timer) --
   no Intuition/MUI here yet. Grows into the real MUI Application/Window
   shell in T9.1b, the same way platform/atari/app_shell.c grew from a
   bare AES bring-up into the full menu/dialog/help shell across M4-M8.5. */

wp_bool wp_amiga_file_clipboard_selfcheck(void)
{
    static const char fixture[] = "amiga file+clipboard round-trip fixture";
    static const char scratch_path[] = "SYS:WPTEST.TXT";
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


/* ===================================================================
 * M9 T9.1b: the real MUI shell -- Application/Window/Menustrip, a
 * custom Area subclass for the document body (IDCMP_RAWKEY -> vi-mode),
 * File Open/Save via asl.library, Cut/Copy/Paste, clean quit.
 * =================================================================== */

/* IPTR isn't provided by this NDK's headers directly (it's an SDI_
   compiler.h-ism this project doesn't vendor) -- ULONG is exactly what
   it expands to on a 32-bit target, used the same way the verified real
   Class1.c example passes plain tag values. */
#define IPTR ULONG

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

/* Library bases: unlike dos.library/exec.library (pre-opened by Kickstart
   for every process, confirmed by the Phase 0 muitest probe needing no
   explicit OpenLibrary), muimaster.library/intuition.library/graphics.
   library/utility.library/asl.library/keymap.library all need an
   explicit OpenLibrary before any of their pragma/inline stubs can be
   called -- verified against the real MUI SDK example sources on this
   machine (~/src/amiga-gcc/build/mui/SDK/MUI/C/Examples/demo.h's own
   init()/fail() functions do exactly this), not assumed. Each library's
   own proto/*.h header already `extern`-declares its base global (e.g.
   proto/intuition.h declares `struct IntuitionBase *IntuitionBase`) --
   this file only assigns them, never redeclares. */

static wp_bool open_libraries(void)
{
    /* MUIMASTER_VMIN (mui.h's own suggested minimum, 20) is newer than
       the actually-installed MUI 3.8's library version (19, per the
       aminet mui38usr.lha Install-MUI script's own "current_libver 19")
       -- OpenLibrary with that minimum fails on this real fixture.
       Version 0 (accept whatever is installed) is what the Phase 0
       muitest probe already proved works. */
    MUIMasterBase = OpenLibrary((CONST_STRPTR)MUIMASTER_NAME, 0);
    if (MUIMasterBase == NULL) return WP_FALSE;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GfxBase       = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
    UtilityBase   = OpenLibrary((CONST_STRPTR)"utility.library", 37);
    AslBase       = OpenLibrary((CONST_STRPTR)"asl.library", 37);
    KeymapBase    = OpenLibrary((CONST_STRPTR)"keymap.library", 37);
    return (IntuitionBase && GfxBase && UtilityBase && AslBase && KeymapBase)
           ? WP_TRUE : WP_FALSE;
}

static void close_libraries(void)
{
    if (KeymapBase)    CloseLibrary(KeymapBase);
    if (AslBase)       CloseLibrary(AslBase);
    if (UtilityBase)   CloseLibrary(UtilityBase);
    if (GfxBase)        CloseLibrary(GfxBase);
    if (IntuitionBase) CloseLibrary(IntuitionBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
}

/* Dispatcher calling convention for a custom BOOPSI class under plain
   GCC/m68k (not MorphOS/AROS, which need a trampoline): verified against
   the real, compilable MUI SDK example sources on this machine
   (Examples/Class1.c + Examples/SDI_hook.h's non-MorphOS/non-AROS
   branch), not assembled from memory -- struct IClass, Object and Msg
   are passed in registers a0, a2 and a1 respectively; __saveds preserves
   the caller's A4 (data segment register). */
#define WP_DISPATCHER(name) \
    __saveds ULONG name(struct IClass *cl __asm("a0"), \
                         Object *obj __asm("a2"), Msg msg __asm("a1"))

/* -- Application state (mirrors platform/atari/app_shell.c's g_doc/g_ctx) */

static wp_document      g_amiga_doc;
static wp_undo_journal  g_amiga_journal;
static wp_app_ctx        g_ctx;
static Object           *g_app;
static Object           *g_win;
static Object           *g_editor;

/* Raw ASCII byte for the current keystroke, in the SAME byte space
   actions.h's shortcut_ascii documents (printable ASCII, or a Ctrl-
   letter control code 0x01-0x1A) -- set by translate_rawkey, consulted
   by amiga_dispatch_key via wp_action_find_by_shortcut, exactly
   mirroring how platform/atari/app_shell.c's wp_dispatch_key looks up
   shortcuts from the ORIGINAL raw key value rather than the decoded
   wp_key_event (see that function's own comment). */
static u8 g_last_raw_ascii = 0;

/* M9 Phase 6: the help system, reusing platform/common/help_doc.h/
   help_view.h unchanged -- mirrors platform/atari/app_shell.c's own
   g_help_doc/g_help_doc_loaded/g_help_view/g_help_active exactly.
   g_help_active is a whole separate UI mode: while set,
   amiga_dispatch_key routes every key to wp_help_view_feed_key instead
   of vi-mode, and editor_draw paints the pager instead of the document
   -- see paint_help_amiga() and the g_help_active checks in both.
   Reachable via F1 (amiga_dispatch_key) or the real Help menu action
   (do_help_contents, below) -- unlike Atari, which only ever wired F1
   (no menu path existed for it at the time), Amiga gets both from the
   start since the Help menu was already real since Phase 2. */
static wp_help_doc       g_help_doc;
static wp_bool           g_help_doc_loaded = WP_FALSE;
static wp_help_view_state g_help_view;
static wp_bool           g_help_active = WP_FALSE;

/* T9.2: the width (in twips) every paragraph's real wp_paragraph_layout
   was last built against -- lets relayout_document tell a real resize
   (every line must re-break) apart from an ordinary redraw. Unlike
   platform/atari/app_shell.c, which knows its window's real pixel
   geometry the moment wind_open_grect returns, a MUI Area subclass has
   no real size until MUI's own layout pass has run (which happens only
   once the Application/Window is actually opened) -- so relaying out
   eagerly right after loading a document, the way Atari's
   replace_document does, isn't possible here. The first relayout
   happens lazily, on the first editor_draw that sees a real width.

   There used to be a companion g_needs_relayout flag meaning "the
   document itself is new". It was removed with the stale-layout fix in
   editor_draw: it only ever tracked whole-document REPLACEMENT, so it
   was blind to keystroke edits -- which is precisely how that bug got
   in. para->layout_dirty, which the engine already maintains per
   paragraph, subsumes it and covers edits too (a freshly imported
   document's paragraphs all start with layout == NULL, so the "new
   document" case still forces a rebuild without a separate flag). */
static i32 g_content_width_twips = 0;

static const char g_sample_text[] =
    "RetroWP on Amiga (M9 T9.1b)\n"
    "\n"
    "This is a real MUI Application/Window with a working menu,\n"
    "built by folding platform/common/actions.c's g_wp_actions[]\n"
    "table over a Menustrip/Menu/Menuitem hierarchy.\n"
    "\n"
    "Press j/k to move the cursor down/up, i to enter Insert mode,\n"
    "Esc to return to Normal mode, :q Enter to quit.\n";

/* Relayouts every paragraph at content_width_twips, mirroring platform/
   atari/app_shell.c's relayout_document exactly -- see docs/layout.md.
   Records the width used so editor_draw can tell a real resize (width
   changed) apart from "nothing to do" without re-relayouting every
   single redraw. */
static void relayout_document(i32 content_width_twips)
{
    wp_bool width_changed = (wp_bool)(content_width_twips != g_content_width_twips);
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_amiga_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_amiga_doc, si);
        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            /* Only what actually needs it: never-built (layout == NULL),
               engine-marked dirty (any content edit -- edit.c sets it on
               every insert/delete), or every paragraph when the column
               itself changed (a real resize, where nothing is dirty but
               every line must re-break). wp_paragraph_relayout has no
               early-out of its own (linebreak.c measures unconditionally),
               so this check is what makes calling relayout_document
               before every paint affordable. */
            if (!width_changed && para->layout != NULL && !para->layout_dirty) continue;
            wp_paragraph_relayout(&g_amiga_doc, para, content_width_twips);
        }
    }
}

/* Rebuilds the layout of whatever the engine just marked dirty, to be
   called after ANY edit and before the repaint that follows it.

   This is deliberately NOT done inside editor_draw. Doing it there is
   what platform/dos/app_shell.c does (its paint is a plain text-cell
   write with no toolkit underneath), but on Amiga it crashed the app
   outright: RETROWP died with "Program failed (error #80000004)" --
   an illegal-instruction trap -- on the first keystroke, reproducibly.
   wp_paragraph_relayout allocates and opens/measures/closes fonts
   through the metrics seam, and running all of that from inside a MUI
   Area subclass's own MUIM_Draw is not safe. Relayouting here instead
   -- in the event handler, before MUI_Redraw -- keeps Draw a pure
   painter, which is what MUI expects of it.

   Safe to call before the first real draw: g_content_width_twips is 0
   until editor_draw has seen a real object width, and this no-ops in
   that case (editor_draw does that first layout itself). */
static void amiga_relayout_after_edit(void)
{
    if (!g_ctx.doc_loaded) return;
    if (g_content_width_twips <= 0) return; /* no real width known yet */
    relayout_document(g_content_width_twips);
}

static void set_document_from_text(const u8 *utf8, u32 len)
{
    wp_document new_doc;

    if (format_txt_import(utf8, len, &new_doc) != WP_OK) return;
    if (g_ctx.doc_loaded) wp_document_free(&g_amiga_doc);
    g_amiga_doc = new_doc;
    /* M10: re-points the active scheme's own .doc/.journal at the new
       document -- keymap_id itself never changes here, only re-selects
       the same arm. Matches this function's own pre-M10 behavior
       exactly, including NOT calling wp_keymap_free first (unlike
       platform/atari/app_shell.c's own replace_document, which does --
       a real, pre-existing asymmetry between the two backends, not
       introduced by this refactor and not fixed here either). */
    wp_keymap_init(&g_ctx, (wp_keymap_id)g_ctx.keymap_id, &g_amiga_doc, &g_amiga_journal);
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_start(&g_amiga_doc);
    g_ctx.doc_loaded = WP_TRUE;
    /* No "document is new" flag to set: every paragraph format_txt_import
       just built has layout == NULL, which relayout_document already
       treats as needing a rebuild -- see g_content_width_twips's comment. */
}

static void load_sample_document(void)
{
    set_document_from_text((const u8 *)g_sample_text, (u32)strlen(g_sample_text));
}

/* -- File Open/Save via asl.library (the real native picker, mirroring
   platform/atari/app_shell.c's run_file_selector/fsel_input role) ---- */

static wp_bool run_asl_file_requester(wp_bool save_mode, char *out_path, u32 out_cap)
{
    struct FileRequester *fr;
    wp_bool ok = WP_FALSE;

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_InitialDrawer, (IPTR)"SYS:",
        ASLFR_DoSaveMode, (IPTR)(save_mode ? (LONG)1 : (LONG)0),
        ASLFR_TitleText, save_mode ? (IPTR)"Save Document" : (IPTR)"Open Document",
        TAG_DONE);
    if (fr == NULL) return WP_FALSE;

    if (AslRequest(fr, NULL)) {
        if (fr->rf_File[0] != '\0') {
            char full[256];

            strcpy(full, (char *)fr->rf_Dir);
            AddPart((STRPTR)full, fr->rf_File, (ULONG)sizeof(full));
            if (strlen(full) < out_cap) {
                strcpy(out_path, full);
                ok = WP_TRUE;
            }
        }
    }
    FreeAslRequest(fr);
    return ok;
}

static void do_file_open(void)
{
    char fullpath[256];
    plat_file *f;
    u32 size, read_len;
    u8 *buf;

    if (!run_asl_file_requester(WP_FALSE, fullpath, (u32)sizeof(fullpath))) return;
    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK) { plat_file_close(f); return; }

    buf = (u8 *)mem_alloc(size + 1);
    if (buf != NULL) {
        if (plat_file_read(f, buf, size, &read_len) == WP_OK) {
            set_document_from_text(buf, read_len);
        }
        mem_free(buf);
    }
    plat_file_close(f);
}

static void do_file_save(void)
{
    char fullpath[256];
    plat_file *f;
    u32 needed;
    u8 *buf;

    if (!g_ctx.doc_loaded) return;
    if (!run_asl_file_requester(WP_TRUE, fullpath, (u32)sizeof(fullpath))) return;

    format_txt_export(&g_amiga_doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;

    if (format_txt_export(&g_amiga_doc, buf, needed, &needed) == WP_OK) {
        if (plat_file_open(fullpath, PLAT_FILE_WRITE, &f) == WP_OK) {
            plat_file_write(f, buf, needed);
            plat_file_close(f);
        }
    }
    mem_free(buf);
}

/* Cut/Paste operate on the whole document, matching platform/atari/
   app_shell.c's do_edit_cut/do_edit_paste exactly (same "no selection
   model yet" scope note -- see actions.c's act_edit_copy comment).
   Copy itself has a real portable handler already (act_edit_copy in
   platform/common/actions.c), invoked via act->handler below. */
static void do_edit_cut(void)
{
    static const char empty_text[] = "\n";

    if (!g_ctx.doc_loaded) return;
    wp_action_find(WP_ACT_EDIT_COPY)->handler(&g_ctx);
    set_document_from_text((const u8 *)empty_text, (u32)strlen(empty_text));
}

static void do_edit_paste(void)
{
    u8 buf[4096];
    u32 out_len;

    if (!g_ctx.doc_loaded) return;
    if (plat_clipboard_get_text(buf, (u32)sizeof(buf), &out_len) != WP_OK) return;
    set_document_from_text(buf, out_len);
}

/* M9 Phase 6: opens the real help pager at the named node (falling back
   to node 0 if the name doesn't resolve), mirroring platform/atari/
   app_shell.c's own open_help_node exactly. Replaces the earlier
   static-MUI_Request placeholder (Phase 2/4) now that the real pager's
   platform.h dependencies (plat_draw_text/plat_fill_rect/plat_gc_flush)
   exist (Phase 3). The caller (invoke_backend_action's menu dispatch,
   or amiga_dispatch_key's F1 handling) already triggers a MUI_Redraw
   afterward -- see both call sites -- so this doesn't force one itself. */
static void open_help_node(const char *node_name, u32 node_name_len)
{
    if (!g_help_doc_loaded) return;
    {
        u32 node = wp_help_doc_find_node(&g_help_doc, (const u8 *)node_name, node_name_len);
        wp_help_view_init(&g_help_view, &g_help_doc, (node != WP_HELP_NODE_NONE) ? node : 0);
    }
    g_help_active = WP_TRUE;
}

static void do_help_contents(void)
{
    open_help_node("Main", 4);
}

/* -- Tools: Thesaurus/Chat/Grammar Check via the offload link (T9.3) --
   Mirrors platform/atari/app_shell.c's own do_thesaurus/do_chat/
   do_grammar_check in spirit: same offload_client.c API, same lazy-once
   ensure_offload() pattern, same plain-text extraction helpers -- only
   the display mechanism differs (MUI_Request here, GEM's form_alert()
   there). Grammar Check is intentionally NOT a full port: Atari's
   version is an interactive per-issue Accept/Skip/Cancel loop with
   byte-offset-shifting bookkeeping across edits (~200 lines) -- this
   backend reports what was found (a count plus the first issue's
   message/suggestion) via one MUI_Request instead of ever applying
   anything automatically. A real, honestly-scoped-down gap versus
   Atari's feature, not a silent one -- see docs/amiga-platform.md.

   MUI_Request's `format` parameter is NOT a plain display string -- it
   is a real printf-style format that MUI itself expands (`%s`/`%d`
   substitutions against the varargs that follow). Every message below
   that contains real document/gateway text (never fully caller-
   controlled, unlike do_help_contents' fixed literal above) is passed
   through a literal "%s" format with the actual text as the vararg,
   never interpolated into the format string directly -- otherwise a
   stray '%' in a word, a gateway's reply, or a grammar message would be
   misinterpreted as a format specifier by MUI_Request itself. */
static void show_message(const char *title, const char *msg)
{
    MUI_Request(g_app, g_win, 0, (CONST_STRPTR)title, (CONST_STRPTR)"Ok",
                (CONST_STRPTR)"%s", (CONST_STRPTR)msg);
}

static void append_str(char *buf, u32 cap, u32 *pos, const char *s)
{
    while (*s != '\0' && *pos + 1 < cap) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void append_str_n(char *buf, u32 cap, u32 *pos, const u8 *s, u32 s_len, u32 max_len)
{
    char tmp[192]; /* generous fixed cap: the largest max_len any caller
                       passes today (WP_AMIGA_CHAT_DISPLAY_LEN) plus
                       headroom for a future caller -- mirrors
                       draw_atari.c's own append_str_n exactly */
    u32 tlen = (s_len < max_len) ? s_len : max_len;

    if (tlen >= (u32)sizeof(tmp)) tlen = (u32)sizeof(tmp) - 1;
    memcpy(tmp, s, tlen);
    tmp[tlen] = '\0';
    append_str(buf, cap, pos, tmp);
}

static void append_u32(char *buf, u32 cap, u32 *pos, u32 value)
{
    char digits[12];
    u32 n = 0;

    if (value == 0) {
        digits[n++] = '0';
    } else {
        while (value > 0 && n < (u32)sizeof(digits)) {
            digits[n++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    while (n > 0) {
        if (*pos + 1 < cap) buf[(*pos)++] = digits[--n];
        else n = 0;
    }
    buf[*pos] = '\0';
}

static wp_bool ascii_is_alpha(u8 c)
{
    return (wp_bool)((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

#define WP_AMIGA_OFFLOAD_ENDPOINT       "serial:aux:9600"
#define WP_AMIGA_THESAURUS_MAX_WORD_LEN 64
#define WP_AMIGA_THESAURUS_TIMEOUT_MS   90000uL /* generous: a real Pi/LLM
                                                    gateway's cold model
                                                    load can genuinely take
                                                    this long, matching
                                                    Atari's own budget */
#define WP_AMIGA_PARAGRAPH_EXTRACT_MAX  2000uL  /* matches ollama_gateway.c's
                                                    CHAT_PROMPT_MAX_LEN/
                                                    GRAMMAR_PARAGRAPH_MAX_LEN */
#define WP_AMIGA_CHAT_TIMEOUT_MS        90000uL
#define WP_AMIGA_CHAT_DISPLAY_LEN       180
#define WP_AMIGA_GRAMMAR_TIMEOUT_MS     90000uL
#define WP_AMIGA_GRAMMAR_MAX_ISSUES     16u

static wp_bool g_offload_init_attempted = WP_FALSE;

static wp_bool ensure_offload(void)
{
    if (!g_offload_init_attempted) {
        g_offload_init_attempted = WP_TRUE;
        offload_init(WP_AMIGA_OFFLOAD_ENDPOINT); /* result read back via offload_available() */
    }
    return offload_available();
}

static wp_bool offload_caps_supports(const offload_capabilities *caps, offload_op op)
{
    return (wp_bool)((caps->ops_supported & (1uL << (u32)op)) != 0);
}

/* Looks up the word immediately BEFORE the cursor, mirroring
   platform/atari/app_shell.c's own extract_word_before_cursor exactly
   (same "after typing a word the cursor naturally sits right after it"
   reasoning). */
static wp_bool extract_word_before_cursor(u8 *out_word, u32 *out_word_len)
{
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    const u8 *bytes;
    u32 offset, end, start;

    if (!g_ctx.doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_amiga_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);
    if ((*wp_keymap_cursor(&g_ctx)).run >= para->run_count) return WP_FALSE;

    run = wp_paragraph_run_get(para, (u16)(*wp_keymap_cursor(&g_ctx)).run);
    bytes = wp_strpool_get(&g_amiga_doc.strings, run->text);

    offset = (*wp_keymap_cursor(&g_ctx)).offset;
    if (offset > run->text.length) offset = run->text.length;

    end = offset;
    while (end > 0 && !ascii_is_alpha(bytes[end - 1])) end--;
    start = end;
    while (start > 0 && ascii_is_alpha(bytes[start - 1])) start--;

    if (start == end) return WP_FALSE;
    if (end - start >= WP_AMIGA_THESAURUS_MAX_WORD_LEN) return WP_FALSE;

    memcpy(out_word, bytes + start, end - start);
    *out_word_len = end - start;
    return WP_TRUE;
}

/* Concatenates the plain UTF-8 text of the CURRENT paragraph, mirroring
   platform/atari/app_shell.c's own extract_current_paragraph_text
   exactly. */
static wp_bool extract_current_paragraph_text(u8 *out, u32 out_cap, u32 *out_len)
{
    wp_section *sec;
    wp_paragraph *para;
    u32 written = 0;
    u16 i;

    if (!g_ctx.doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_amiga_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);

    for (i = 0; i < para->run_count; i++) {
        wp_run *run = wp_paragraph_run_get(para, i);

        if (run->text.length > 0) {
            const u8 *bytes;

            if (written + run->text.length > out_cap) return WP_FALSE;
            bytes = wp_strpool_get(&g_amiga_doc.strings, run->text);
            memcpy(out + written, bytes, run->text.length);
            written += run->text.length;
        }
    }
    if (written == 0) return WP_FALSE;
    *out_len = written;
    return WP_TRUE;
}

static void do_thesaurus(void)
{
    u8 word[WP_AMIGA_THESAURUS_MAX_WORD_LEN];
    u32 word_len;
    char word_buf[WP_AMIGA_THESAURUS_MAX_WORD_LEN];
    offload_request req;
    offload_response resp;
    char msg[220];
    u32 pos = 0;

    if (!extract_word_before_cursor(word, &word_len)) {
        show_message("Thesaurus", "Place the cursor right after a word, then choose Thesaurus again.");
        return;
    }
    memcpy(word_buf, word, word_len);
    word_buf[word_len] = '\0';

    if (!ensure_offload()) {
        show_message("Thesaurus", "No offload device available (tried " WP_AMIGA_OFFLOAD_ENDPOINT ").");
        return;
    }

    req.op = OFFLOAD_THESAURUS;
    req.payload = word;
    req.payload_len = word_len;
    req.timeout_ms = (u32)WP_AMIGA_THESAURUS_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        show_message("Thesaurus", "Thesaurus request failed or timed out.");
        return;
    }

    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        show_message("Thesaurus", "The connected gateway doesn't support Thesaurus.");
        return;
    }

    append_str(msg, (u32)sizeof(msg), &pos, "Synonyms for ");
    append_str(msg, (u32)sizeof(msg), &pos, word_buf);
    append_str(msg, (u32)sizeof(msg), &pos, ": ");

    {
        u16 count = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
        u32 rpos = 2;
        u32 i;

        if (count == 0) {
            append_str(msg, (u32)sizeof(msg), &pos, "(none found)");
        } else {
            for (i = 0; i < count && rpos + 2 <= resp.result_len; i++) {
                u16 wlen = be_read16(resp.result + rpos);
                char tmp[WP_AMIGA_THESAURUS_MAX_WORD_LEN];
                u32 tlen;

                rpos += 2;
                if (rpos + wlen > resp.result_len) break;

                tlen = (wlen < (u32)sizeof(tmp) - 1) ? wlen : (u32)sizeof(tmp) - 1;
                memcpy(tmp, resp.result + rpos, tlen);
                tmp[tlen] = '\0';

                if (i > 0) append_str(msg, (u32)sizeof(msg), &pos, ", ");
                append_str(msg, (u32)sizeof(msg), &pos, tmp);
                rpos += wlen;
            }
        }
    }

    offload_response_free(&resp);
    show_message("Thesaurus", msg);
}

static void do_chat(void)
{
    u8 *paragraph;
    u32 paragraph_len;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    char msg[220];
    u32 pos = 0;

    if (!g_ctx.doc_loaded) return;

    paragraph = (u8 *)mem_alloc(WP_AMIGA_PARAGRAPH_EXTRACT_MAX);
    if (paragraph == NULL) return;

    if (!extract_current_paragraph_text(paragraph, (u32)WP_AMIGA_PARAGRAPH_EXTRACT_MAX, &paragraph_len)) {
        mem_free(paragraph);
        show_message("Chat", "Place the cursor in a paragraph with some text, then choose Chat again.");
        return;
    }

    if (!ensure_offload()) {
        mem_free(paragraph);
        show_message("Chat", "No offload device available (tried " WP_AMIGA_OFFLOAD_ENDPOINT ").");
        return;
    }

    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_CHAT)) {
        mem_free(paragraph);
        show_message("Chat", "This gateway doesn't offer Chat.");
        return;
    }

    req.op = OFFLOAD_CHAT;
    req.payload = paragraph;
    req.payload_len = paragraph_len;
    req.timeout_ms = (u32)WP_AMIGA_CHAT_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(paragraph);
        show_message("Chat", "Chat request failed or timed out.");
        return;
    }
    mem_free(paragraph);

    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        show_message("Chat", "The connected gateway couldn't answer that.");
        return;
    }

    append_str_n(msg, (u32)sizeof(msg), &pos, resp.result, resp.result_len, (u32)WP_AMIGA_CHAT_DISPLAY_LEN);
    offload_response_free(&resp);
    show_message("Chat", msg);
}

/* Insert Image (M15 T15.4): pick a source file via the same ASL
   requester do_file_open reuses, downconvert it through the real
   OFFLOAD_IMAGE op (do_chat's own CAPS-pre-check pattern -- offload_
   query() + offload_caps_supports() BEFORE building a request), cache
   the returned packed pixel bytes to disk, and insert a WP_PARA_IMAGE
   paragraph at the cursor. Real pixel rendering (reading blob_path
   back and blitting it) is a later phase (T15.6) -- this phase only
   needs the paragraph to exist at the right size. */
#define WP_AMIGA_IMAGE_MAX_FILE_SIZE 65000uL /* wire frame's own WIRE_MAX_PAYLOAD (64K) minus the 6-byte request header */
#define WP_AMIGA_IMAGE_TIMEOUT_MS    90000uL /* matches this backend's other offload budgets */
#define WP_AMIGA_IMAGE_TARGET_MAX_W  320u    /* WP_IMAGE_MAX_WIDTH_PX -- the ST low-res ceiling every target clamps against */
#define WP_AMIGA_IMAGE_TARGET_MAX_H  200u

static u32 g_amiga_image_cache_counter = 0;

/* "PROGDIR:WPIMG###.TMP" -- counter-suffixed so a second Insert Image
   in one session can't silently overwrite the first image's still-
   referenced pixel cache file underneath it (a single fixed name
   would). Mirrors wp_amiga_config_selfcheck's own PROGDIR: scratch-
   file convention. out must be >= 24 bytes. Wraps at 1000, a real,
   accepted session-scope limit. */
static void amiga_format_image_cache_path(char *out, u32 counter)
{
    u32 c = counter % 1000u;
    strcpy(out, "PROGDIR:WPIMG");
    out[13] = (char)('0' + (c / 100u) % 10u);
    out[14] = (char)('0' + (c / 10u) % 10u);
    out[15] = (char)('0' + c % 10u);
    strcpy(out + 16, ".TMP");
}

/* Real, runtime-queried screen depth this app's MUI window actually
   opened on (M9's own finding: MUI opens on whatever the user's real
   Workbench screen depth is, never fixed at compile time) -- clamped
   to 16, the most colors any packed-4-bit response can ever carry
   regardless of what a deeper screen could otherwise show. Falls back
   to a conservative 4 if the screen/bitmap isn't queryable for any
   reason, rather than failing the request outright.

   Queries via GetAttr(MUIA_Window_Screen, ...), NOT the _screen(obj)
   convenience macro -- that macro reads muiRenderInfo(obj)->mri_Screen,
   valid only "between MUIM_Setup/Cleanup" for the SPECIFIC object whose
   own Setup populated it (confirmed via libraries/mui.h's own comment);
   g_win itself never reliably has that render info populated, a real
   bug found here via a live diagnostic (an on-target dump showed depth
   0, i.e. a silently NULL screen pointer) after _screen(g_win) compiled
   clean but returned NULL at runtime. GetAttr is a real MUI Window
   attribute query that works on the window object directly, regardless
   of custom-class Setup/Cleanup state. */
static u32 amiga_screen_color_count(void)
{
    struct Screen *scr = NULL;
    GetAttr(MUIA_Window_Screen, g_win, (ULONG *)&scr);
    if (scr == NULL || scr->BitMap.Depth < 1) return 4u;
    if (scr->BitMap.Depth >= 4) return 16u;
    return (u32)(1uL << (u32)scr->BitMap.Depth);
}

static void do_insert_image(void)
{
    char fullpath[256];
    plat_file *f;
    u32 size, read_len;
    u8 *req_buf;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;

    if (!g_ctx.doc_loaded) return;
    if (!run_asl_file_requester(WP_FALSE, fullpath, (u32)sizeof(fullpath))) return;

    if (!ensure_offload()) {
        show_message("Insert Image", "No offload device available (tried " WP_AMIGA_OFFLOAD_ENDPOINT ").");
        return;
    }
    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_IMAGE)) {
        show_message("Insert Image", "This gateway doesn't offer Insert Image.");
        return;
    }

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) {
        show_message("Insert Image", "Couldn't open that file.");
        return;
    }
    if (plat_file_size(f, &size) != WP_OK || size == 0 || size > WP_AMIGA_IMAGE_MAX_FILE_SIZE) {
        plat_file_close(f);
        show_message("Insert Image", "That image file is too large for this app to send.");
        return;
    }

    req_buf = (u8 *)mem_alloc(size + 6);
    if (req_buf == NULL) { plat_file_close(f); return; }

    req_buf[0] = (u8)amiga_screen_color_count();
    req_buf[1] = 0; /* target_flags: color, not grayscale -- Amiga's real screen depth already gives a real palette */
    be_write16(req_buf + 2, (u16)WP_AMIGA_IMAGE_TARGET_MAX_W);
    be_write16(req_buf + 4, (u16)WP_AMIGA_IMAGE_TARGET_MAX_H);

    if (plat_file_read(f, req_buf + 6, size, &read_len) != WP_OK) {
        mem_free(req_buf);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_buf;
    req.payload_len = read_len + 6;
    req.timeout_ms = (u32)WP_AMIGA_IMAGE_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        show_message("Insert Image", "Insert Image failed or timed out.");
        return;
    }
    mem_free(req_buf);

    if (resp.status != OFFLOAD_OK || resp.result_len < 5) {
        offload_response_free(&resp);
        show_message("Insert Image", "The connected gateway couldn't convert that image.");
        return;
    }

    {
        u16 width_px = be_read16(resp.result);
        u16 height_px = be_read16(resp.result + 2);
        u8 palette_count = resp.result[4];
        const u8 *palette = resp.result + 5;
        const u8 *packed_pixels = palette + (u32)palette_count * 3u;
        u32 row_bytes = ((u32)width_px + 1u) / 2u;
        u32 packed_len = row_bytes * (u32)height_px;
        char cache_path[24];
        wp_image_desc desc;
        wp_status st;
        u32 fname_start, k;

        if (palette_count < 1 || palette_count > 16 ||
            resp.result_len != 5u + (u32)palette_count * 3u + packed_len) {
            offload_response_free(&resp);
            show_message("Insert Image", "The gateway's reply was malformed.");
            return;
        }

        amiga_format_image_cache_path(cache_path, g_amiga_image_cache_counter);

        if (plat_file_open(cache_path, PLAT_FILE_WRITE, &f) != WP_OK) {
            offload_response_free(&resp);
            show_message("Insert Image", "Couldn't write the image cache file to disk.");
            return;
        }
        st = plat_file_write(f, packed_pixels, packed_len);
        plat_file_close(f);
        if (st != WP_OK) {
            offload_response_free(&resp);
            show_message("Insert Image", "Couldn't write the image cache file to disk.");
            return;
        }
        g_amiga_image_cache_counter++;

        memset(&desc, 0, sizeof(desc));
        desc.width_px = (i32)width_px;
        desc.height_px = (i32)height_px;
        desc.display_width_twips = amiga_px_to_twips_x((i32)width_px);
        desc.display_height_twips = amiga_px_to_twips_y((i32)height_px);
        desc.palette_count = palette_count;
        memcpy(desc.palette, palette, (u32)palette_count * 3u);
        if (wp_strpool_append(&g_amiga_doc.strings, (const u8 *)cache_path,
                               (u32)strlen(cache_path), &desc.blob_path) != WP_OK) {
            offload_response_free(&resp);
            return;
        }

        /* fullpath's own final path separator (':' or '/') marks where
           the bare filename starts -- AddPart's own convention, same
           directory-vs-file split run_asl_file_requester's own callers
           never needed until now. */
        fname_start = 0;
        for (k = 0; fullpath[k] != '\0'; k++) {
            if (fullpath[k] == ':' || fullpath[k] == '/') fname_start = k + 1;
        }
        if (fullpath[fname_start] != '\0') {
            wp_strpool_append(&g_amiga_doc.strings, (const u8 *)(fullpath + fname_start),
                               (u32)strlen(fullpath + fname_start), &desc.source_name);
        }
        offload_response_free(&resp);

        if (wp_edit_insert_image(&g_amiga_doc, &g_amiga_journal, (*wp_keymap_cursor(&g_ctx)), &desc) != WP_OK) {
            show_message("Insert Image", "Couldn't insert that image into the document.");
            return;
        }
        relayout_document(g_content_width_twips);
    }
}

/* Source-agnostic issue record, populated either from
   decode_grammar_issue_at() (the wire response) or directly from
   grammar_lite_check() (services/grammar) when no offload device is
   attached -- mirrors platform/atari/app_shell.c's own
   wp_atari_grammar_issue. Only the FIRST issue is kept (this backend's
   own reduced scope, see do_grammar_check's comment); Atari keeps all
   of them for its interactive apply loop. */
typedef struct {
    u16  start_offset;
    u16  span_len;
    char message[80];
    char suggestion[80];
} wp_amiga_grammar_issue;

static void copy_bounded(char *out, u32 out_cap, const u8 *s, u32 s_len)
{
    u32 len = (s_len < out_cap - 1) ? s_len : out_cap - 1;

    memcpy(out, s, len);
    out[len] = '\0';
}

/* Decodes issue `index` from the OFFLOAD_GRAMMAR_CHECK response shape
   (u16 issue_count, then issue_count x (u16 start_offset, u16 span_len,
   u16 message_len + bytes, u16 suggestion_len + bytes) --
   docs/formats/offload-protocol.md), mirroring platform/atari/
   app_shell.c's own decode_grammar_issue_at exactly. */
static wp_bool decode_grammar_issue_at(const u8 *buf, u32 len, u32 index, wp_amiga_grammar_issue *out)
{
    u16 count;
    u32 pos, i;

    if (len < 2) return WP_FALSE;
    count = be_read16(buf);
    pos = 2;
    if (index >= count) return WP_FALSE;

    for (i = 0; i <= index; i++) {
        u16 mlen, slen;

        if (pos + 6 > len) return WP_FALSE;
        out->start_offset = be_read16(buf + pos); pos += 2;
        out->span_len     = be_read16(buf + pos); pos += 2;
        mlen = be_read16(buf + pos); pos += 2;

        if (pos + mlen + 2 > len) return WP_FALSE;
        if (i == index) copy_bounded(out->message, (u32)sizeof(out->message), buf + pos, mlen);
        pos += mlen;

        slen = be_read16(buf + pos); pos += 2;
        if (pos + slen > len) return WP_FALSE;
        if (i == index) copy_bounded(out->suggestion, (u32)sizeof(out->suggestion), buf + pos, slen);
        pos += slen;
    }
    return WP_TRUE;
}

/* Reduced-scope Grammar Check (see this section's own top comment):
   runs the real offload check (or, per CLAUDE.md's Offload Rules, the
   local grammar_lite_check() fallback when no device is attached) and
   reports a count plus the first issue's message/suggestion via one
   MUI_Request -- never applies anything to the document automatically,
   unlike Atari's full interactive Accept/Skip/Cancel loop. */
static void do_grammar_check(void)
{
    u8 *paragraph;
    u32 paragraph_len;
    offload_request req;
    offload_response resp;
    u32 issue_count = 0;
    wp_amiga_grammar_issue first_issue;
    wp_bool have_first = WP_FALSE;
    wp_bool used_offload;
    char msg[220];
    u32 pos = 0;

    if (!g_ctx.doc_loaded) return;

    paragraph = (u8 *)mem_alloc(WP_AMIGA_PARAGRAPH_EXTRACT_MAX);
    if (paragraph == NULL) return;

    if (!extract_current_paragraph_text(paragraph, (u32)WP_AMIGA_PARAGRAPH_EXTRACT_MAX, &paragraph_len)) {
        mem_free(paragraph);
        show_message("Grammar Check", "Place the cursor in a paragraph with some text, then choose Grammar Check again.");
        return;
    }

    used_offload = ensure_offload();

    if (used_offload) {
        req.op = OFFLOAD_GRAMMAR_CHECK;
        req.payload = paragraph;
        req.payload_len = paragraph_len;
        req.timeout_ms = (u32)WP_AMIGA_GRAMMAR_TIMEOUT_MS;

        if (offload_submit(&req, &resp) != WP_OK) {
            mem_free(paragraph);
            show_message("Grammar Check", "Grammar Check request failed or timed out.");
            return;
        }
        if (resp.status != OFFLOAD_OK) {
            offload_response_free(&resp);
            mem_free(paragraph);
            show_message("Grammar Check", "The connected gateway doesn't support Grammar Check.");
            return;
        }

        issue_count = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
        if (issue_count > 0) have_first = decode_grammar_issue_at(resp.result, resp.result_len, 0, &first_issue);
        offload_response_free(&resp);
    } else {
        grammar_issue local_issues[WP_AMIGA_GRAMMAR_MAX_ISSUES];

        issue_count = grammar_lite_check(paragraph, paragraph_len, local_issues, (u32)WP_AMIGA_GRAMMAR_MAX_ISSUES);
        if (issue_count > 0) {
            first_issue.start_offset = (u16)local_issues[0].start_offset;
            first_issue.span_len = (u16)local_issues[0].span_len;
            copy_bounded(first_issue.message, (u32)sizeof(first_issue.message),
                         (const u8 *)local_issues[0].message, (u32)strlen(local_issues[0].message));
            copy_bounded(first_issue.suggestion, (u32)sizeof(first_issue.suggestion),
                         (const u8 *)local_issues[0].suggestion, (u32)strlen(local_issues[0].suggestion));
            have_first = WP_TRUE;
        }
    }

    mem_free(paragraph);

    if (issue_count == 0) {
        show_message("Grammar Check", "No grammar issues found.");
        return;
    }

    append_str(msg, (u32)sizeof(msg), &pos, used_offload ? "Gateway found " : "No offload device attached -- local check found ");
    append_u32(msg, (u32)sizeof(msg), &pos, issue_count);
    append_str(msg, (u32)sizeof(msg), &pos, (issue_count == 1) ? " issue.\n" : " issues.\n");
    if (have_first) {
        append_str(msg, (u32)sizeof(msg), &pos, "First: ");
        append_str(msg, (u32)sizeof(msg), &pos, first_issue.message);
        if (first_issue.suggestion[0] != '\0') {
            append_str(msg, (u32)sizeof(msg), &pos, "\nSuggestion: ");
            append_str(msg, (u32)sizeof(msg), &pos, first_issue.suggestion);
        }
    }
    show_message("Grammar Check", msg);
}

static void invoke_backend_action(wp_action_id id)
{
    switch (id) {
    case WP_ACT_FILE_OPEN:      do_file_open(); break;
    case WP_ACT_FILE_SAVE:      do_file_save(); break;
    case WP_ACT_EDIT_CUT:       do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE:     do_edit_paste(); break;
    case WP_ACT_HELP_CONTENTS:  do_help_contents(); break;
    case WP_ACT_TOOLS_THESAURUS:    do_thesaurus(); break;
    case WP_ACT_TOOLS_GRAMMARCHECK: do_grammar_check(); break;
    case WP_ACT_TOOLS_CHAT:         do_chat(); break;
    case WP_ACT_TOOLS_INSERTIMAGE:  do_insert_image(); break;
    default:
        /* Spellcheck is still not implemented here: it's a LOCAL,
           file-based dictionary feature (services/spell -- no offload
           dependency at all, per the M9 plan's own Phase 4 intro), not
           part of T9.3's offload scope, and this app has no dictionary
           file on its fixture yet. That remains an honest gap -- but it
           used to be a SILENT one: the menu item was live, and picking
           it did nothing at all, with no message and no beep, which is
           indistinguishable to a user from the menu being broken. Say
           so instead. */
        show_message("Not available",
                     "Spell Check needs a local dictionary file,\n"
                     "which this build does not ship yet.");
        break;
    }
}

/* Dispatches a wp_action_id exactly the way the pre-M10 vi-mode
   shortcut-fallback path always has (handler-or-backend, no menu-
   checkmark sync here -- this backend never had one, unlike Atari's own
   sync_format_check) -- now the one place that logic lives, shared
   between that fallback (inside wp_keymap_feed_key's own VI arm) and
   any other scheme's WP_KEYMAP_RESULT_ACTION outcome. */
static void invoke_action(wp_action_id id)
{
    const wp_action *act = wp_action_find(id);

    /* wp_action_find returns NULL for an out-of-range id, and the id
       arrives here as a u8 out of wp_keymap_feed_key's outcome struct --
       so a stale or garbage id from a future keymap scheme would crash
       here rather than no-op. Cheap to rule out. */
    if (act == NULL) return;

    if (act->handler != NULL) {
        act->handler(&g_ctx);
    } else {
        invoke_backend_action(id);
    }
}

/* -- IDCMP_RAWKEY -> wp_key_event, mirroring platform/atari/app_shell.c's
   atari_translate_key exactly in spirit: scan/raw code is consulted
   FIRST for keys needing a dedicated wp_special_key (early return),
   everything else falls through to MapRawKey()'s ASCII/Latin-1 result.
   Raw key codes below (Backspace 0x41, Tab 0x42, Enter 0x44, Esc 0x45,
   Del 0x46, arrows 0x4C-0x4F, F1-F10 0x50-0x59) are the standard, stable
   Amiga hardware scan codes unchanged since the A1000 -- no offline RKM
   was available on this machine to cross-check against (see docs/
   amiga-platform.md's own honest note in the M9 plan), so these are
   verified empirically under amiberry, not just recalled.

   M10 Phase 2: imsg->Qualifier (real INTUI IEQUALIFIER bits, verified
   against the real NDK header ~/opt/amiga/m68k-amigaos/ndk-include/
   devices/inputevent.h -- IEQUALIFIER_CONTROL=0x0008, LALT=0x0010,
   RALT=0x0020 -- NOT the 0x0200/0x0400 pair some online references
   cite) was previously captured only to feed MapRawKey()'s own ASCII
   translation. Alt is now also read directly from it and ORed into
   out->mods at the single exit point below -- MapRawKey folds Ctrl
   into a C0 control code (the existing buf[0]<0x20 check), but does
   NOT fold Alt into the ASCII result the same way, so Alt needs this
   separate, explicit read. Applied uniformly regardless of which
   branch produced *out (a special key, a Ctrl-chord, or a plain
   codepoint can each also be Alt-held), same reasoning
   atari_translate_key's own header comment gives. */
static void translate_rawkey(struct IntuiMessage *imsg, wp_key_event *out)
{
    UWORD code = imsg->Code;
    struct InputEvent ie;
    UBYTE buf[8];
    WORD n;
    u8 alt = (imsg->Qualifier & (IEQUALIFIER_LALT | IEQUALIFIER_RALT)) ? (u8)WP_MOD_ALT : 0;

    out->codepoint = 0;
    out->special = (u16)WP_KEY_NONE;
    out->mods = 0;
    out->_pad = 0;
    g_last_raw_ascii = 0;

    if (code & IECODE_UP_PREFIX) goto done; /* key release -- ignore */

    switch (code) {
    case 0x41: out->special = (u16)WP_KEY_BACKSPACE; goto done;
    case 0x42: out->special = (u16)WP_KEY_TAB; goto done;
    case 0x44: out->special = (u16)WP_KEY_ENTER; goto done;
    case 0x45: out->special = (u16)WP_KEY_ESCAPE; goto done;
    case 0x46: out->special = (u16)WP_KEY_DELETE; goto done;
    case 0x4C: out->special = (u16)WP_KEY_UP; goto done;
    case 0x4D: out->special = (u16)WP_KEY_DOWN; goto done;
    case 0x4E: out->special = (u16)WP_KEY_RIGHT; goto done;
    case 0x4F: out->special = (u16)WP_KEY_LEFT; goto done;
    default: break;
    }
    if (code >= 0x50 && code <= 0x59) {
        out->special = (u16)(WP_KEY_F1 + (code - 0x50));
        goto done;
    }

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = (UBYTE)code;
    ie.ie_Qualifier = imsg->Qualifier;
    n = MapRawKey(&ie, (STRPTR)buf, (LONG)sizeof(buf), NULL);
    if (n <= 0) goto done;

    g_last_raw_ascii = buf[0];
    if (buf[0] < 0x20) {
        out->codepoint = (u32)(buf[0] + ('a' - 1));
        out->mods = (u8)WP_MOD_CTRL;
    } else {
        out->codepoint = (u32)buf[0];
    }

done:
    out->mods |= alt;
}

/* Mirrors platform/atari/app_shell.c's wp_dispatch_key: feed the active
   keymap scheme first (which, for WP_KEYMAP_VI, itself falls back to
   the action-shortcut table using the RAW ascii byte -- not the
   translated wp_key_event -- see g_last_raw_ascii's own comment for
   why). Returns WP_TRUE if the app should quit. */
static wp_bool amiga_dispatch_key(wp_key_event ev)
{
    wp_keymap_outcome outcome;

    /* Help is a whole separate UI mode (M9 Phase 6): while active,
       every key goes to wp_help_view_feed_key instead of the active
       keymap scheme, full stop, until the viewer itself reports a close
       request (Escape or 'q' -- see help_view.h) -- mirrors platform/
       atari/app_shell.c's own wp_dispatch_key exactly. F1 opens it from
       any scheme/mode/state; pressing F1 again while already showing
       help is a harmless no-op rather than resetting the user's
       position. */
    if (g_help_active) {
        if (wp_help_view_feed_key(&g_help_view, ev)) g_help_active = WP_FALSE;
        return WP_FALSE;
    }
    if (ev.special == (u16)WP_KEY_F1) {
        do_help_contents();
        return WP_FALSE;
    }

    if (!g_ctx.doc_loaded) return WP_FALSE;

    outcome = wp_keymap_feed_key(&g_ctx, ev, g_last_raw_ascii);

    switch ((wp_keymap_result_kind)outcome.kind) {
    case WP_KEYMAP_RESULT_QUIT:
        return WP_TRUE;
    case WP_KEYMAP_RESULT_SAVE_QUIT:
        do_file_save();
        return WP_TRUE;
    case WP_KEYMAP_RESULT_ACTION:
        invoke_action((wp_action_id)outcome.action_id);
        break;
    default:
        break;
    }

    return g_ctx.quit_requested;
}

/* -- Custom Area subclass: the document body. AskMinMax/Draw give it a
   size and a (deliberately minimal -- see docs/amiga-platform.md) plain-
   text render of the document via direct RastPort calls, bypassing
   plat_draw_text/plat_measure_text entirely: those don't exist yet
   (T9.2's job, per the M9 plan's own phasing), and T9.1b's real AC is
   "menu, a document window, vi-mode Normal-mode movement, clean quit" --
   not wrapped layout. Setup/Cleanup register/deregister an event handler
   for IDCMP_RAWKEY (MUIM_HandleEvent), the verified real path for raw
   keyboard input in a custom MUI class (guide/MUIdev.guide node
   CSC_HANDLEEVENT). */

struct wp_editor_data {
    struct MUI_EventHandlerNode ehn;
};

static WP_DISPATCHER(wp_editor_dispatch);

static struct MUI_CustomClass *g_editor_class;

static ULONG editor_new(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct wp_editor_data *data;

    obj = (Object *)DoSuperMethodA(cl, obj, (Msg)msg);
    if (obj == NULL) return 0;
    data = (struct wp_editor_data *)INST_DATA(cl, obj);
    memset(data, 0, sizeof(*data));
    return (ULONG)obj;
}

static ULONG editor_setup(struct IClass *cl, Object *obj, Msg msg)
{
    struct wp_editor_data *data = (struct wp_editor_data *)INST_DATA(cl, obj);

    if (!DoSuperMethodA(cl, obj, msg)) return 0;

    data->ehn.ehn_Object = obj;
    data->ehn.ehn_Class = cl;
    data->ehn.ehn_Priority = 0;
    data->ehn.ehn_Flags = MUI_EHF_GUIMODE;
    data->ehn.ehn_Events = IDCMP_RAWKEY;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, &data->ehn);
    return 1;
}

static ULONG editor_cleanup(struct IClass *cl, Object *obj, Msg msg)
{
    struct wp_editor_data *data = (struct wp_editor_data *)INST_DATA(cl, obj);

    DoMethod(_win(obj), MUIM_Window_RemEventHandler, &data->ehn);
    return DoSuperMethodA(cl, obj, msg);
}

static ULONG editor_askminmax(struct IClass *cl, Object *obj, struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);
    msg->MinMaxInfo->MinWidth  += 300;
    msg->MinMaxInfo->DefWidth  += 600;
    msg->MinMaxInfo->MaxWidth  += MUI_MAXMAX;
    msg->MinMaxInfo->MinHeight += 100;
    msg->MinMaxInfo->DefHeight += 350;
    msg->MinMaxInfo->MaxHeight += MUI_MAXMAX;
    return 0;
}

/* Left margin reserved for the cursor-line marker plus a small gap
   before text starts; matches the fixed 4px text inset the pre-T9.2
   version of this function used, now also leaving room for the marker
   drawn at the object's own left edge. */
#define WP_EDITOR_LEFT_MARGIN_PX 6
#define WP_EDITOR_RIGHT_MARGIN_PX 4
#define WP_EDITOR_TOP_MARGIN_PX 8
#define WP_EDITOR_MIN_CONTENT_PX 20

/* Pure arithmetic behind paint's per-line alignment offset, pulled out
   of the paint loop exactly the way platform/atari/app_shell.c's
   atari_align_offset_twips is -- same three-way LEFT/CENTER/RIGHT
   behavior (JUSTIFY isn't implemented -- documented gap, see
   docs/amiga-platform.md, same as Atari's). Returns a TWIPS offset;
   callers convert to pixels themselves since the twips<->px ratio is a
   metrics concern, not an alignment one. */
static i32 amiga_align_offset_twips(u8 align, i32 content_width_twips, i32 line_width_twips)
{
    i32 extra_twips = content_width_twips - line_width_twips;

    if (extra_twips <= 0) return 0;
    if (align == WP_ALIGN_CENTER) return extra_twips / 2;
    if (align == WP_ALIGN_RIGHT) return extra_twips;
    return 0;
}

/* WP_TRUE if (run, offset) falls within [line->start_run,start_offset)
   .. [line->end_run,end_offset] -- used to find which of a paragraph's
   real wrapped lines the vi-mode cursor is currently sitting on, now
   that a paragraph can genuinely be more than one line (T9.2; before
   this, the whole paragraph was always exactly one drawn line, so which
   line the cursor was on was never in question). */
static wp_bool line_contains_pos(const wp_line *line, u32 run, u32 offset)
{
    if (run < line->start_run || run > line->end_run) return WP_FALSE;
    if (run == line->start_run && offset < line->start_offset) return WP_FALSE;
    if (run == line->end_run && offset > line->end_offset) return WP_FALSE;
    return WP_TRUE;
}

/* M9 Phase 6: help pager sizing, mirroring platform/atari/app_shell.c's
   own WP_ATARI_HELP_BODY_SIZE_TWIPS/WP_ATARI_STATUSLINE_SIZE_TWIPS and
   font_line_height_px exactly -- "how tall is one line of some font, in
   pixels" always comes from real plat_font_metrics_get, never a
   hardcoded guess. */
#define WP_AMIGA_HELP_BODY_SIZE_TWIPS 200
#define WP_AMIGA_HINT_BAR_SIZE_TWIPS  160

static i32 amiga_font_line_height_px(i32 size_twips)
{
    plat_font *font;
    plat_font_metrics fm;
    i32 h_px;

    if (plat_font_open((const u8 *)"", 0, size_twips, 0, &font) != WP_OK) return 0;
    plat_font_metrics_get(font, &fm);
    h_px = amiga_twips_to_px_y(fm.ascent + fm.descent + fm.leading);
    plat_font_close(font);
    return h_px;
}

/* Paints the help pager full-object (replacing the document body
   entirely while g_help_active is set -- see editor_draw's own check)
   plus a small fixed hint bar along the bottom, reusing
   wp_statusline_paint for that hint exactly the way platform/atari/
   app_shell.c's paint_help does (platform/common/statusline.c is
   deliberately generic, not vi-mode-specific, per its own header
   comment -- a static hint string is exactly the kind of use it names). */
static void paint_help_amiga(amiga_gc *gc, Object *obj)
{
    plat_font *body_font;
    i32 hint_bar_h_px, body_line_h_px, body_h_px, total_h_px;
    i32 x_px, y_px, w_px;

    x_px = (i32)_mleft(obj);
    y_px = (i32)_mtop(obj);
    w_px = (i32)(_mright(obj) - _mleft(obj) + 1);
    total_h_px = (i32)(_mbottom(obj) - _mtop(obj) + 1);

    hint_bar_h_px = amiga_font_line_height_px(WP_AMIGA_HINT_BAR_SIZE_TWIPS);
    if (hint_bar_h_px > total_h_px) hint_bar_h_px = 0;
    body_line_h_px = amiga_font_line_height_px(WP_AMIGA_HELP_BODY_SIZE_TWIPS);
    body_h_px = total_h_px - hint_bar_h_px;
    if (body_h_px < 0) body_h_px = 0;

    if (plat_font_open((const u8 *)"", 0, WP_AMIGA_HELP_BODY_SIZE_TWIPS, 0, &body_font) == WP_OK) {
        /* wp_help_view_paint positions text by BASELINE (plat_draw_text's
           own convention) -- ascent_px tells it where row 0's baseline
           sits below the viewport's own top edge. A bare y_px with no
           ascent offset drew row 0's ascending strokes above the
           viewport entirely, into the window's own title-bar area --
           a real bug found via THIS interactive test (see
           help_view.h's own comment for the full story) and fixed at
           the shared platform/common/help_view.c level, not just papered
           over here. */
        plat_font_metrics body_fm;
        i32 body_ascent_px = 0;
        if (plat_font_metrics_get(body_font, &body_fm) == WP_OK) {
            body_ascent_px = amiga_twips_to_px_y(body_fm.ascent);
        }
        wp_help_view_paint((plat_gc *)gc, body_font, x_px, y_px, w_px, body_h_px,
                            amiga_px_to_twips_x(w_px), body_line_h_px, body_ascent_px, &g_help_view);
        plat_font_close(body_font);
    }

    if (hint_bar_h_px > 0) {
        plat_font *hint_font;
        wp_vi_status_view view;
        static const u8 hint[] = "HELP -- j/k select, Enter follow, Backspace back, Esc/q close";

        if (plat_font_open((const u8 *)"", 0, WP_AMIGA_HINT_BAR_SIZE_TWIPS, 0, &hint_font) == WP_OK) {
            view.mode_label_utf8 = hint;
            view.mode_label_len = (u32)(sizeof(hint) - 1);
            view.pending_count = 0;
            view.pending_cmd_utf8 = NULL;
            view.pending_cmd_len = 0;
            wp_statusline_paint((plat_gc *)gc, hint_font, x_px, y_px + total_h_px - hint_bar_h_px,
                                 w_px, hint_bar_h_px, &view);
            plat_font_close(hint_font);
        }
    }
}

static ULONG editor_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    amiga_gc gc;
    u32 si, pi, li;
    i32 y;
    i32 content_width_px;
    i32 content_width_twips;
    plat_font *font = NULL;
    wp_cstyle cached_style = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent_px = 0;

    DoSuperMethodA(cl, obj, (Msg)msg);
    if (!(msg->flags & MADF_DRAWOBJECT)) return 0;

    /* MUI_Redraw()-triggered repaints (as opposed to the window's own
       initial open) do NOT reliably clear stale pixels first -- verified
       under amiberry: repeated key-driven redraws left ghosted/garbled
       text where a line's old and new glyphs overlapped (see
       docs/amiga-platform.md). Area class's own MADF_DRAWOBJECT handling
       apparently isn't a full-region clear in this path, so this class
       clears its own area explicitly every time rather than trusting it. */
    SetAPen(_rp(obj), _dri(obj)->dri_Pens[BACKGROUNDPEN]);
    RectFill(_rp(obj), _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj));

    if (g_help_active) {
        amiga_gc help_gc;
        help_gc.rp = _rp(obj);
        help_gc.dri = _dri(obj);
        help_gc.screen = NULL; /* the help pager never draws an image */
        paint_help_amiga(&help_gc, obj);
        return 0;
    }

    if (!g_ctx.doc_loaded) return 0;

    /* Real per-object pixel width, converted through the SAME twips<->px
       ratio metrics_amiga.c uses for measurement (T9.2's actual point:
       "on-screen wrap matches engine layout" only holds if layout and
       paint agree on this number). MUI has no equivalent of Atari's
       wind_open-time-known geometry, so the first relayout can't happen
       any earlier than the first real Draw. */
    content_width_px = (i32)(_mright(obj) - _mleft(obj) + 1)
                        - WP_EDITOR_LEFT_MARGIN_PX - WP_EDITOR_RIGHT_MARGIN_PX;
    if (content_width_px < WP_EDITOR_MIN_CONTENT_PX) content_width_px = WP_EDITOR_MIN_CONTENT_PX;
    content_width_twips = amiga_px_to_twips_x(content_width_px);

    /* Layout happens here ONLY for the two cases that can't be handled
       anywhere else: the very first draw (before which this object has
       no real width at all) and a genuine resize. Edit-driven relayout
       deliberately does NOT happen inside Draw -- see
       amiga_relayout_after_edit's own comment for the crash that
       caused. */
    if (g_content_width_twips == 0 || content_width_twips != g_content_width_twips) {
        relayout_document(content_width_twips);
    }

    gc.rp = _rp(obj);
    gc.dri = _dri(obj);
    GetAttr(MUIA_Window_Screen, g_win, (ULONG *)&gc.screen);

    y = _mtop(obj) + WP_EDITOR_TOP_MARGIN_PX;
    for (si = 0; si < g_amiga_doc.section_count && y <= _mbottom(obj); si++) {
        wp_section *sec = wp_document_section_get(&g_amiga_doc, si);

        for (pi = 0; pi < sec->para_count && y <= _mbottom(obj); pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
            const wp_para_style *pstyle = wp_document_pstyle_get(&g_amiga_doc, para->style);
            u8 align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;
            wp_bool is_cursor_para =
                ((*wp_keymap_cursor(&g_ctx)).section == si && (*wp_keymap_cursor(&g_ctx)).paragraph == pi)
                ? WP_TRUE : WP_FALSE;
            u32 cursor_line_index = 0;

            if (layout == NULL) continue;

            if (is_cursor_para && layout->line_count > 0) {
                cursor_line_index = layout->line_count - 1; /* fallback: last line */
                for (li = 0; li < layout->line_count; li++) {
                    if (line_contains_pos(&layout->lines[li], (*wp_keymap_cursor(&g_ctx)).run,
                                           (*wp_keymap_cursor(&g_ctx)).offset)) {
                        cursor_line_index = li;
                        break;
                    }
                }
            }

            for (li = 0; li < layout->line_count && y <= _mbottom(obj); li++) {
                wp_line *line = &layout->lines[li];
                i32 x_px = _mleft(obj) + WP_EDITOR_LEFT_MARGIN_PX;
                i32 extra_twips;
                wp_line_fragment_iter iter;
                wp_line_fragment frag;

                x_px += amiga_twips_to_px_x(
                    amiga_align_offset_twips(align, content_width_twips, line->width_twips));

                /* M15 T15.6: an image paragraph's single line has no run
                   bytes to fragment -- paint its real indexed-bitmap
                   content (read back from blob_path on demand, per
                   services/spell.c's own "stream from disk, don't load
                   the model whole" precedent) and move straight to the
                   next line, skipping the fragment-walk/font-cache/
                   cursor-marker logic below entirely. Falls back to a
                   solid palette[0] rect (the original T15.6a proof) if
                   the cache file can't be read for any reason, rather
                   than drawing nothing at all. */
                if (para->kind == (u8)WP_PARA_IMAGE) {
                    const wp_image_desc *img = wp_document_image_get(&g_amiga_doc, para->image);

                    if (img != NULL && img->palette_count > 0) {
                        i32 w_px = amiga_twips_to_px_x(line->width_twips);
                        i32 h_px = amiga_twips_to_px_y(line->height_twips);
                        wp_bool drew_real = WP_FALSE;

                        if (img->blob_path.length > 0 && img->blob_path.length < 256) {
                            char path[256];
                            const u8 *span_bytes = wp_strpool_get(&g_amiga_doc.strings, img->blob_path);
                            plat_file *f;

                            memcpy(path, span_bytes, img->blob_path.length);
                            path[img->blob_path.length] = '\0';

                            if (plat_file_open(path, PLAT_FILE_READ, &f) == WP_OK) {
                                u32 row_bytes = ((u32)img->width_px + 1u) / 2u;
                                u32 packed_len = row_bytes * (u32)img->height_px;
                                u8 *pixels = (u8 *)mem_alloc(packed_len);

                                if (pixels != NULL) {
                                    u32 read_len;
                                    if (plat_file_read(f, pixels, packed_len, &read_len) == WP_OK &&
                                        read_len == packed_len) {
                                        if (plat_draw_image((plat_gc *)&gc, x_px, y, w_px, h_px,
                                                             pixels, img->width_px, img->height_px,
                                                             img->palette, img->palette_count) == WP_OK) {
                                            drew_real = WP_TRUE;
                                        }
                                    }
                                    mem_free(pixels);
                                }
                                plat_file_close(f);
                            }
                        }

                        if (!drew_real) {
                            plat_fill_rect((plat_gc *)&gc, x_px, y, w_px, h_px,
                                           img->palette[0][0], img->palette[0][1], img->palette[0][2]);
                        }
                    }
                    y += amiga_twips_to_px_y(line->height_twips);
                    continue;
                }

                extra_twips = wp_justify_space_extra_twips(
                    &g_amiga_doc, para, line, align, (wp_bool)(li == layout->line_count - 1),
                    content_width_twips);

                wp_line_fragment_iter_init(&iter, line);
                while (wp_line_next_fragment(&g_amiga_doc, para, line, &iter, &frag)) {
                    wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
                    const u8 *bytes;
                    u32 frag_len = frag.byte_end - frag.byte_start;
                    i32 frag_width_twips = 0;

                    /* The layout is rebuilt from the current runs before
                       any paint, so an out-of-range run index should be
                       impossible here -- but this used to dereference the
                       result unchecked, which turned any future
                       stale-layout regression into a wild read rather than
                       a visibly wrong glyph. Cheap insurance on the one
                       path that walks engine-supplied indices. */
                    if (run == NULL) continue;
                    bytes = wp_strpool_get(&g_amiga_doc.strings, run->text);
                    if (bytes == NULL) continue;

                    if (!have_font || run->style != cached_style) {
                        const wp_char_style *cs = wp_document_cstyle_get(&g_amiga_doc, run->style);

                        if (cs != NULL) {
                            plat_font_metrics fm;

                            /* Clear `font` between the close and the open.
                               plat_font_open does NOT write *out when it
                               fails (WP_NOMEM), so the old code left `font`
                               pointing at the font it had just closed:
                               plat_font_metrics_get then read freed memory,
                               and the plat_font_close after the paint loop
                               freed the same block a second time. A double
                               free corrupts the allocator's own structures,
                               which on AmigaOS takes down whatever task
                               allocates next -- not necessarily this one.
                               (Observed: "«IPrefs» Program failed (error
                               #80000004)" while typing in RetroWP.) */
                            if (font != NULL) {
                                plat_font_close(font);
                                font = NULL;
                            }
                            have_font = WP_FALSE;
                            if (plat_font_open((const u8 *)"", 0, cs->size_twips,
                                                cs->flags, &font) == WP_OK &&
                                plat_font_metrics_get(font, &fm) == WP_OK) {
                                ascent_px = amiga_twips_to_px_y(fm.ascent);
                                cached_style = run->style;
                                have_font = WP_TRUE;
                            } else {
                                font = NULL; /* nothing valid to draw or free with */
                            }
                        }
                    }

                    if (frag_len == 0 || font == NULL) continue;

                    plat_measure_text(font, bytes + frag.byte_start, frag_len, &frag_width_twips);

                    if (!frag.is_space) {
                        plat_draw_text((plat_gc *)&gc, font, x_px, y + ascent_px,
                                        bytes + frag.byte_start, frag_len, 0, 0, 0);
                    }

                    x_px += amiga_twips_to_px_x(frag_width_twips);
                    if (frag.is_space) x_px += amiga_twips_to_px_x(extra_twips);
                }

                if (is_cursor_para && li == cursor_line_index) {
                    SetAPen(gc.rp, (UBYTE)gc.dri->dri_Pens[FILLPEN]);
                    RectFill(gc.rp, _mleft(obj), (LONG)(y - ascent_px),
                             _mleft(obj) + 1, (LONG)(y + 1));
                }

                y += amiga_twips_to_px_y(line->height_twips);
            }
        }
    }

    if (font != NULL) plat_font_close(font);
    return 0;
}

static ULONG editor_handleevent(struct IClass *cl, Object *obj, struct MUIP_HandleEvent *msg)
{
    wp_key_event ev;
    wp_bool quit;

    WP_UNUSED(cl);
    if (msg->imsg == NULL || msg->imsg->Class != IDCMP_RAWKEY) return 0;

    translate_rawkey(msg->imsg, &ev);
    quit = amiga_dispatch_key(ev);
    amiga_relayout_after_edit();
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    if (quit) {
        DoMethod(_app(obj), MUIM_Application_ReturnID, (LONG)MUIV_Application_ReturnID_Quit);
    }

    /* Real, empirically-found bug (M9 Phase 6): returning a plain 0 here
       means "this event was not consumed" (MUI's own documented
       MUIM_HandleEvent contract, libraries/mui.h's own comment on
       MUI_EventHandlerRC_Eat -- "stop MUI from calling other
       handlers"), which was letting Escape keep propagating past this
       class to whatever handles it next -- and something further along
       that chain was treating an unconsumed Escape as equivalent to a
       close-gadget click, closing the whole window (confirmed by
       instrumenting this function directly: the raw key WAS reaching
       here, amiga_dispatch_key correctly returned WP_FALSE for it via
       vi-mode's own Escape-is-harmless-in-Normal-mode behavior, yet the
       window still closed). This wasn't Phase 6-specific -- any bare
       Escape press would have hit it since Phase 2 -- it just had never
       been pressed during this project's own interactive verification
       until the help pager's own close key made that unavoidable. Every
       raw key this app receives is meant to be fully handled by it, so
       this always eats the event now, never falling through to
       whatever default handling caused the spurious close. */
    return MUI_EventHandlerRC_Eat;
}

static WP_DISPATCHER(wp_editor_dispatch)
{
    switch (msg->MethodID) {
    case OM_NEW:           return editor_new(cl, obj, (struct opSet *)msg);
    case MUIM_Setup:       return editor_setup(cl, obj, msg);
    case MUIM_Cleanup:     return editor_cleanup(cl, obj, msg);
    case MUIM_AskMinMax:   return editor_askminmax(cl, obj, (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:        return editor_draw(cl, obj, (struct MUIP_Draw *)msg);
    case MUIM_HandleEvent: return editor_handleevent(cl, obj, (struct MUIP_HandleEvent *)msg);
    default: break;
    }
    return DoSuperMethodA(cl, obj, msg);
}

/* -- Menu construction: folds g_wp_actions[] grouped by .group over a
   Menustrip/Menu/Menuitem hierarchy, per the M9 plan's architecture
   table and actions.h's own anticipated design. Children are attached
   dynamically via OM_ADDMEMBER (doc/MUI_Group.doc's documented "add/
   remove children at will" mechanism), since the action count per group
   isn't known at compile time as a literal nested tag list. Each item's
   MUIA_Menuitem_Trigger is wired to MUIM_Application_ReturnID with a
   per-action numeric ID (WP_RETURNID_ACTION_BASE + id) -- the SAME
   verified real pattern doc/MUI_Application.doc's own
   MUIA_Application_Iconified example uses for custom notification IDs,
   checked against in the main loop below. */

#define WP_RETURNID_ACTION_BASE 1000

static const char *group_title(wp_menu_group group)
{
    switch (group) {
    case WP_MENU_FILE:   return "Project";
    case WP_MENU_EDIT:   return "Edit";
    case WP_MENU_FORMAT: return "Format";
    case WP_MENU_TOOLS:  return "Tools";
    case WP_MENU_HELP:   return "Help";
    default: return NULL;
    }
}

/* MUI does not copy the strings handed to it. MUIA_Menuitem_Shortcut is
   an [I..] attribute and MUI_Menuitem.doc is explicit that the buffer
   "must remain valid throughout the object's life time"
   (MUIA_Menuitem_CopyStrings defaults to FALSE). The menustrip is only
   realized into a real menu when the window opens -- long after
   build_menustrip() has returned -- so the `char shortcut[2]` this used
   to declare INSIDE the item loop was a dangling pointer into a dead
   stack frame by the time MUI read it, and every shortcut-bearing item
   was handed the very same address. Titles were never affected (those
   are string literals in actions.c), which is why this surfaced as
   wrong or blank Amiga-key glyphs rather than as a crash, and why the
   menu strip passing a visual check on its top-level titles never
   caught it. Process-lifetime storage, one slot per action, indexed by
   action id -- safe because test_actions_table_row_order_matches_enum
   now pins .id to its table row. */
static char g_menu_shortcut[WP_ACT_COUNT][2];

static Object *build_menustrip(void)
{
    static const wp_menu_group groups[] = {
        WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
    };
    Object *strip;
    u32 gi, ai;

    strip = MenustripObject, End;
    if (strip == NULL) return NULL;

    for (gi = 0; gi < (u32)(sizeof(groups) / sizeof(groups[0])); gi++) {
        Object *menu;
        wp_bool any = WP_FALSE;

        menu = MenuObjectT((CONST_STRPTR)group_title(groups[gi])), End;
        if (menu == NULL) continue;

        for (ai = 0; ai < (u32)WP_ACT_COUNT; ai++) {
            const wp_action *act = &g_wp_actions[ai];
            Object *item;
            char *shortcut = g_menu_shortcut[act->id];

            if (act->group != groups[gi]) continue;

            /* Only a real Ctrl+<letter> maps onto an Amiga-key letter.
               The previous math reduced to shortcut_ascii + 0x40 applied
               unconditionally, which is right for 0x01..0x1A ('A'..'Z')
               but turned WP_ACT_FORMAT_SIZE's 0x1D into ']' -- a
               meaningless accelerator glyph, and on an item whose
               key_hint_utf8 is deliberately 0 ("show no hint") at that.
               actions.h explicitly allows printable ASCII in this byte
               space too, so the range has to be checked, not assumed. */
            if (act->shortcut_ascii >= 0x01 && act->shortcut_ascii <= 0x1A) {
                shortcut[0] = (char)('A' + act->shortcut_ascii - 1);
            } else {
                shortcut[0] = '\0';
            }
            shortcut[1] = '\0';

            item = MenuitemObject,
                MUIA_Menuitem_Title, (IPTR)act->label_utf8,
                MUIA_Menuitem_Shortcut, (IPTR)(shortcut[0] ? shortcut : NULL),
                End;
            if (item == NULL) continue;

            DoMethod(item, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
                     MUIV_Notify_Application, 2, MUIM_Application_ReturnID,
                     (LONG)(WP_RETURNID_ACTION_BASE + act->id));
            DoMethod(menu, OM_ADDMEMBER, item);
            any = WP_TRUE;
        }

        if (any) {
            DoMethod(strip, OM_ADDMEMBER, menu);
        } else {
            MUI_DisposeObject(menu);
        }
    }

    return strip;
}

/* -- plat_dialog_choice (platform.h): dialog_amiga.c's real content
   lives here rather than a separate file, since it needs direct access
   to the Application/Window objects app_shell.c already owns (g_app/
   g_win) -- MUI_Request's multi-gadget form ("Yes|No|Cancel"-style,
   documented in doc/muimaster.doc's MUI_Request entry) is a real,
   existing primitive for exactly this "choose one of N" shape, no new
   dialog class needed. Gadget-index-to-out_index mapping (whether the
   FIRST listed option returns 1 or the LAST does) is verified on-target
   under amiberry -- see docs/amiga-platform.md. */
wp_status plat_dialog_choice(plat_window *parent,
                              const u8 *title_utf8, u32 title_len,
                              const u8 * const *option_labels_utf8,
                              const u32 *option_label_lens,
                              u32 option_count, u32 default_index,
                              wp_bool *out_cancelled, u32 *out_index)
{
    char gadgets[256];
    char titlebuf[64];
    u32 i, pos, n;
    LONG result;

    WP_UNUSED(parent);
    WP_UNUSED(default_index);

    pos = 0;
    for (i = 0; i < option_count; i++) {
        n = option_label_lens[i];
        if (i > 0 && pos < sizeof(gadgets) - 1) gadgets[pos++] = '|';
        if (pos + n > sizeof(gadgets) - 1) n = (u32)sizeof(gadgets) - 1 - pos;
        memcpy(gadgets + pos, option_labels_utf8[i], n);
        pos += n;
    }
    gadgets[pos] = '\0';

    n = (title_len < sizeof(titlebuf) - 1) ? title_len : (u32)sizeof(titlebuf) - 1;
    memcpy(titlebuf, title_utf8, n);
    titlebuf[n] = '\0';

    result = MUI_Request(g_app, g_win, 0, (CONST_STRPTR)titlebuf,
                          (CONST_STRPTR)gadgets, (CONST_STRPTR)"");
    if (result <= 0) {
        if (out_cancelled) *out_cancelled = WP_TRUE;
        return WP_OK;
    }
    if (out_cancelled) *out_cancelled = WP_FALSE;
    if (out_index) *out_index = (u32)(result - 1);
    return WP_OK;
}

/* -- Application/Window construction + event loop -------------------- */

/* M10: PROGDIR: is a standard, always-available AmigaDOS assign (the
   directory the program launched from) -- no dependency on a full
   ENV:/Preferences tree, which docs/amiga-platform.md already notes
   this fixture's own WB install doesn't fully have working (icon.library
   etc.). See docs/keymaps.md. */
#define WP_AMIGA_CONFIG_PATH "PROGDIR:RetroWP.cfg"

void app_shell_run(void)
{
    Object *menustrip;
    ULONG open;
    wp_config cfg;

    if (!open_libraries()) { close_libraries(); return; }

    /* M10: read once at startup, fixed for the process lifetime.
       Missing/unreadable/malformed all quietly yield the documented
       default (WP_KEYMAP_VI), so an existing zero-config install
       behaves exactly as it always has. */
    wp_config_load(WP_AMIGA_CONFIG_PATH, &cfg);
    wp_app_ctx_init(&g_ctx, (wp_keymap_id)cfg.keymap_id, &g_amiga_doc, &g_amiga_journal);
    load_sample_document();

    /* M9 Phase 6: parsed ONCE at startup from the generated
       g_help_source_retrowp array (build/generated/help_retrowp.h,
       shared verbatim with the Atari backend -- produced from
       docs/help/retrowp.help by host/tools/gen_help_header.c, see
       docs/help.md), same as platform/atari/app_shell.c. If parsing
       ever fails, g_help_doc_loaded stays WP_FALSE and F1/Help are a
       silent no-op rather than a crash (open_help_node's own guard). */
    g_help_doc_loaded = (wp_bool)(wp_help_doc_parse(g_help_source_retrowp,
                                                     (u32)g_help_source_retrowp_len,
                                                     &g_help_doc) == WP_OK);

    g_editor_class = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
                                            sizeof(struct wp_editor_data),
                                            (APTR)wp_editor_dispatch);
    if (g_editor_class == NULL) { close_libraries(); return; }

    /* Built before the Application exists (build_menustrip's per-item
       notification targets MUIV_Notify_Application, not a literal g_app
       pointer, so this ordering is safe) and passed in the INITIAL tag
       list below, not via a later set() -- MUIA_Application_Menustrip
       is documented [ISG], but empirically a post-creation SetAttrs on
       an already-open window did not make the menu strip populate
       (verified under amiberry, see docs/amiga-platform.md). */
    menustrip = build_menustrip();

    g_app = ApplicationObject,
        MUIA_Application_Title,       (IPTR)"RetroWP",
        MUIA_Application_Version,     (IPTR)"$VER: RetroWP 0.9 (M9)",
        MUIA_Application_Copyright,   (IPTR)"RetroWP contributors",
        MUIA_Application_Author,      (IPTR)"RetroWP contributors",
        MUIA_Application_Description, (IPTR)"Portable word processor -- Amiga/MUI backend",
        MUIA_Application_Base,        (IPTR)"RETROWP",
        MUIA_Application_Menustrip,   (IPTR)menustrip,

        SubWindow, g_win = WindowObject,
            MUIA_Window_Title, (IPTR)"RetroWP",
            MUIA_Window_ID,    MAKE_ID('R','W','P','1'),
            /* Real, empirically-found bug (M9 Phase 6, see
               editor_handleevent's own comment for how this was
               isolated): MUI's GLOBAL keyboard-hotkey system
               (independent of any per-object IDCMP_RAWKEY event
               handler -- see libraries/mui.h's own MUIKEY_ enum and
               the MUIA_Window_DisableKeys attribute) binds
               MUIKEY_WINDOW_CLOSE to Escape by default, closing the
               whole window before vi-mode ever gets a chance to treat
               Escape as "return to Normal mode". Disabled here -- and
               ONLY here, not the other MUIKEYF_ bits (Cut/Copy/Paste/
               Undo/Redo etc, which don't apply to this app's own
               custom Area class anyway, since it isn't a MUI text/
               string gadget) -- so Escape reaches this app's own
               dispatch instead. */
            MUIA_Window_DisableKeys, (IPTR)MUIKEYF_WINDOW_CLOSE,
            WindowContents, VGroup,
                Child, g_editor = NewObject(g_editor_class->mcc_Class, NULL, TAG_DONE),
            End,
        End,
    End;

    if (g_app == NULL) {
        MUI_DeleteCustomClass(g_editor_class);
        close_libraries();
        return;
    }

    DoMethod(g_win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             g_app, 2, MUIM_Application_ReturnID, (LONG)MUIV_Application_ReturnID_Quit);

    set(g_win, MUIA_Window_Open, TRUE);
    get(g_win, MUIA_Window_Open, &open);

    if (open) {
        ULONG sigs = 0;
        LONG id;

        for (;;) {
            id = (LONG)DoMethod(g_app, MUIM_Application_NewInput, &sigs);
            if (id == (LONG)MUIV_Application_ReturnID_Quit) break;
            if (id >= WP_RETURNID_ACTION_BASE && id < WP_RETURNID_ACTION_BASE + WP_ACT_COUNT) {
                wp_action_id act_id = (wp_action_id)(id - WP_RETURNID_ACTION_BASE);

                /* The keyboard path hard-routes every key to the help
                   pager while it is open (see amiga_dispatch_key), but
                   this path never checked, so picking Bold/Size/Cut from
                   the menu while the help viewer was showing mutated the
                   document invisibly behind it. Quit still has to work,
                   or help becomes a trap with no menu way out. */
                if (g_help_active && act_id != WP_ACT_FILE_QUIT) {
                    /* swallowed on purpose while help is up */
                } else {
                    /* Was an inline copy of invoke_action()'s own
                       handler-or-backend branch. Two copies of one
                       policy means anything added to invoke_action --
                       an Atari-style sync_format_check, say -- would
                       silently not apply to menu picks. Call it. */
                    invoke_action(act_id);
                    /* Same reason the keyboard path calls this: Cut,
                       Paste and Alignment all mutate the document, and
                       Draw is not allowed to relayout on this backend.
                       See amiga_relayout_after_edit. */
                    amiga_relayout_after_edit();
                    MUI_Redraw(g_editor, MADF_DRAWOBJECT);
                    if (g_ctx.quit_requested) break;
                }
            }
            if (sigs) {
                sigs = Wait(sigs | SIGBREAKF_CTRL_C);
                if (sigs & SIGBREAKF_CTRL_C) break;
            }
        }
        set(g_win, MUIA_Window_Open, FALSE);
    }

    MUI_DisposeObject(g_app);
    MUI_DeleteCustomClass(g_editor_class);
    if (g_ctx.doc_loaded) wp_document_free(&g_amiga_doc);
    if (g_help_doc_loaded) wp_help_doc_free(&g_help_doc);
    close_libraries();
}

/* T9.2: imports a short multi-word paragraph, relayouts it at a column
   width deliberately narrow enough to force wrapping, and checks two
   properties quantitatively through the REAL graphics.library/diskfont
   metrics seam (metrics_amiga.c) rather than by eyeballing a screenshot:
   more than one line results, and every line's measured width stays
   within the requested column -- the on-target equivalent of the host
   suite's test_layout_greedy_wrap_hand_computed_fixture (minus exact
   hand-computed widths, since a real TextFont's per-character advances
   aren't known ahead of time the same way the host stub's fake
   monospace metrics are), and the exact mirror of platform/atari/
   app_shell.c's wp_atari_layout_selfcheck -- same fixture text, same
   900-twip column, same two assertions, proving the same engine code
   (linebreak.c) behaves correctly against a second, independent
   plat_font_open/plat_measure_text implementation. */
wp_bool wp_amiga_layout_selfcheck(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char text[] = "The quick brown fox jumps over the lazy dog\n";
    const i32 content_width_twips = 900; /* narrow enough to force >1 line */
    wp_bool ok = WP_TRUE;
    u32 i;

    if (format_txt_import((const u8 *)text, (u32)strlen(text), &doc) != WP_OK) return WP_FALSE;

    sec = wp_document_section_get(&doc, 0);
    para = wp_section_paragraph_get(sec, 0);

    if (wp_paragraph_relayout(&doc, para, content_width_twips) != WP_OK) {
        wp_document_free(&doc);
        return WP_FALSE;
    }

    layout = (wp_paragraph_layout *)para->layout;
    if (layout == NULL || layout->line_count < 2) ok = WP_FALSE;

    for (i = 0; ok && i < layout->line_count; i++) {
        if (layout->lines[i].width_twips <= 0) ok = WP_FALSE;
        if (layout->lines[i].width_twips > content_width_twips) ok = WP_FALSE;
    }

    /* T13.2: mirrors wp_atari_layout_selfcheck's own new JUSTIFY check --
       can't hand-compute exact widths against a real font on real
       hardware, so this only checks what every real backend's paint
       loop actually depends on: interior lines' slack is never
       negative, and the paragraph's last line always stays exactly 0. */
    if (ok) {
        wp_para_style jps;
        wp_pstyle jpstyle;

        memset(&jps, 0, sizeof(jps));
        jps.align = WP_ALIGN_JUSTIFY;
        if (wp_document_pstyle_add(&doc, &jps, &jpstyle) != WP_OK) ok = WP_FALSE;
        if (ok) wp_paragraph_pstyle_set(para, jpstyle);
        if (ok && wp_paragraph_relayout(&doc, para, content_width_twips) != WP_OK) ok = WP_FALSE;

        if (ok) {
            layout = (wp_paragraph_layout *)para->layout;
            for (i = 0; ok && i < layout->line_count; i++) {
                wp_bool is_last = (wp_bool)(i == layout->line_count - 1);
                i32 extra = wp_justify_space_extra_twips(&doc, para, &layout->lines[i],
                                                          WP_ALIGN_JUSTIFY, is_last,
                                                          content_width_twips);
                if (extra < 0) ok = WP_FALSE;
                if (is_last && extra != 0) ok = WP_FALSE;
            }
        }
    }

    wp_document_free(&doc);
    return ok;
}

/* T9.3: proves transport_amiga.c's real serial.device IORequest lifecycle
   (OpenDevice/SDCMD_SETPARAMS/CMD_WRITE/CMD_READ/CloseDevice, and the
   SendIO/CheckIO/AbortIO/WaitIO deadline-racing behind the read/write
   timeouts) works on real target hardware -- self-contained, calling
   plat_transport_ and offload_ functions directly rather than through
   g_ctx/g_amiga_doc, so it works unchanged whether or not app_shell_run() has
   ever built a window (same boundary wp_amiga_file_clipboard_selfcheck
   already respects).

   This amiberry fixture has no host-side serial_port wired up (real,
   investigated limitation -- see docs/amiga-platform.md: neither a
   FIFO nor a real socat-created PTY was accepted by this amiberry
   build's serial-port passthrough, which validates against a real-
   hardware enumeration that finds nothing on this machine), so there is
   no live peer to loop bytes back through, AND (a second, independently
   discovered real finding, also documented there) this specific
   amiberry build's serial.device answers SDCMD_QUERY successfully but
   rejects SDCMD_SETPARAMS outright with no host backing configured --
   which is why transport_amiga.c treats a SETPARAMS failure as
   non-fatal rather than aborting plat_transport_open (see that file's
   own comment). What IS provable without a live peer, and what this
   checks:
     - OpenDevice/CloseDevice succeed -- the real exec.library device
       lifecycle works, repeatable across multiple open/close cycles
       with no crash or leak-shaped failure.
     - CMD_WRITE succeeds -- bytes are structurally accepted by the
       device (same as plugging a real serial cable into nothing: the
       UART side of this still works even though nothing receives them).
     - CMD_READ against a short timeout genuinely returns WP_TIMEOUT
       (not an instant no-op, not a hang) within a real, bounded window
       around the requested budget, proving the SendIO/CheckIO/AbortIO/
       WaitIO deadline race itself is correct on real hardware -- the
       one genuinely novel, Amiga-specific mechanism this file adds.
     - offload_init() against the same endpoint, with no live gateway,
       returns cleanly (not hanging) and leaves offload_available()
       false afterward -- CLAUDE.md's own Offload Rule ("the engine must
       be fully functional WITHOUT an offload device attached") holds on
       this backend too, proven through the real transport, not just
       assumed.
   What this deliberately does NOT prove -- the same honestly-drawn
   boundary wp_atari_thesaurus_selfcheck/wp_atari_grammar_check_selfcheck
   already establish for their own backend -- is that bytes written on
   one end are correctly received on the other, or that SDCMD_SETPARAMS
   itself succeeds against real parameters on real hardware; both are
   real, new, Amiga-specific engineering (this file and transport_amiga.c),
   but proving them live needs a peer and a real backing serial port this
   environment cannot currently provide. */
wp_bool wp_amiga_transport_selfcheck(void)
{
    plat_transport *t;
    static const u8 fixture[] = "RetroWP transport selfcheck";
    u8 readbuf[16];
    u32 read_len;
    u32 before, after, elapsed;
    wp_status st;
    wp_bool ok = WP_TRUE;
    int cycle;

    for (cycle = 0; cycle < 2; cycle++) {
        if (plat_transport_open(WP_AMIGA_OFFLOAD_ENDPOINT, 2000, &t) != WP_OK) return WP_FALSE;

        if (plat_transport_write(t, fixture, (u32)(sizeof(fixture) - 1), 2000) != WP_OK) ok = WP_FALSE;

        before = plat_ticks_ms();
        st = plat_transport_read(t, readbuf, (u32)sizeof(readbuf), 500, &read_len);
        after = plat_ticks_ms();
        elapsed = after - before;

        if (st != WP_TIMEOUT) ok = WP_FALSE;
        /* Real emulation/scheduling jitter is expected; the deadline
           logic itself is only wrong if it fires wildly early (an
           off-by-something bug returning almost immediately) or never
           (a hang) -- generous bounds catch both without being flaky. */
        if (elapsed < 300 || elapsed > 5000) ok = WP_FALSE;

        plat_transport_close(t);
    }

    before = plat_ticks_ms();
    st = offload_init(WP_AMIGA_OFFLOAD_ENDPOINT);
    after = plat_ticks_ms();
    elapsed = after - before;

    if (st == WP_OK) ok = WP_FALSE; /* no live gateway is attached; a real
                                        success here would mean this
                                        selfcheck's own environment
                                        assumption above no longer holds */
    if (offload_available()) ok = WP_FALSE;
    if (elapsed > 15000) ok = WP_FALSE; /* CONNECT_TIMEOUT_MS + HANDSHAKE_TIMEOUT_MS
                                            (2000+2000, engine/src/offload_client.c)
                                            is the real budget; generous headroom
                                            for real emulator timing */
    offload_shutdown();
    if (offload_available()) ok = WP_FALSE;

    return ok;
}

/* M9 Phase 6: proves the help system's non-blocking pieces against the
   REAL compiled-in help content and REAL graphics.library/diskfont.library
   font metrics, mirroring platform/atari/app_shell.c's own
   wp_atari_help_selfcheck exactly (same fixture text, same two
   assertions). Deliberately does NOT call wp_help_view_paint or
   actually open the pager (F1) -- same no-real-window/no-blocking-call
   boundary every other selfcheck in this file respects. What IS real
   and worth proving here: the parser accepts the shipped content and
   the "Main" node resolves (every link resolving at all is already a
   build-time guarantee via host/tests/test_help_content.c -- this just
   re-confirms the same compiled bytes still parse after a genuine m68k
   cross-compile), and wp_help_wrap_line -- the greedy word-wrap
   algorithm the pager relies on -- runs correctly against a REAL
   diskfont-measured font rather than host's simplified fake-monospace
   one. */
#define WP_AMIGA_HELP_SELFCHECK_MAX_ROWS 8

wp_bool wp_amiga_help_selfcheck(void)
{
    wp_help_doc doc;
    plat_font *font;
    u32 starts[WP_AMIGA_HELP_SELFCHECK_MAX_ROWS];
    u32 lens[WP_AMIGA_HELP_SELFCHECK_MAX_ROWS];
    u32 row_count;
    static const char sample[] =
        "RetroWP wraps this sentence across more than one real Amiga diskfont-measured row.";

    if (wp_help_doc_parse(g_help_source_retrowp, (u32)g_help_source_retrowp_len, &doc) != WP_OK) {
        return WP_FALSE;
    }
    if (doc.node_count == 0 ||
        wp_help_doc_find_node(&doc, (const u8 *)"Main", 4) == WP_HELP_NODE_NONE) {
        wp_help_doc_free(&doc);
        return WP_FALSE;
    }
    wp_help_doc_free(&doc);

    if (plat_font_open((const u8 *)"", 0, WP_AMIGA_HELP_BODY_SIZE_TWIPS, 0, &font) != WP_OK) {
        return WP_FALSE;
    }
    /* A narrow column forces real wrapping (not just a single row) --
       proves more than "it returned something" against real measurement. */
    row_count = wp_help_wrap_line(font, (const u8 *)sample, (u32)strlen(sample), 3000,
                                   starts, lens, WP_AMIGA_HELP_SELFCHECK_MAX_ROWS);
    plat_font_close(font);

    if (row_count < 2 || row_count > WP_AMIGA_HELP_SELFCHECK_MAX_ROWS) return WP_FALSE;
    {
        u32 i;
        for (i = 0; i < row_count; i++) {
            if (lens[i] == 0) return WP_FALSE; /* every row must carry real content */
        }
    }
    return WP_TRUE;
}

/* M10 Phase 2: proves translate_rawkey's new IEQUALIFIER-bit Alt
   extraction against real raw key codes (Escape/arrows/F-keys -- the
   same "standard, stable Amiga hardware scan codes unchanged since the
   A1000" translate_rawkey's own header comment already cites, not
   guessed). Deliberately stays on the special-key (early-goto) path
   only, not the MapRawKey-derived plain-codepoint/Ctrl-chord path --
   that would need a real loaded system keymap and keymap.library open
   (real, on-target-only state this narrow check has no need to pull
   in), and every path through translate_rawkey applies Alt via the
   exact same `out->mods |= alt;` at its one shared exit point, so
   proving it here proves the mechanism itself, not just one branch of
   it. Not host-testable at all -- translate_rawkey takes a real struct
   IntuiMessage, the same boundary wp_atari_key_translate_selfcheck's
   own header comment explains for its Atari counterpart. */
wp_bool wp_amiga_key_translate_selfcheck(void)
{
    static const struct {
        UWORD code;
        UWORD qualifier;
        u16   special;
        u8    mods;
    } cases[] = {
        { 0x45, 0, (u16)WP_KEY_ESCAPE, 0 },                                     /* Escape, no qualifier */
        { 0x45, (UWORD)IEQUALIFIER_LALT, (u16)WP_KEY_ESCAPE, (u8)WP_MOD_ALT },   /* Left Alt */
        { 0x45, (UWORD)IEQUALIFIER_RALT, (u16)WP_KEY_ESCAPE, (u8)WP_MOD_ALT },   /* Right Alt too */
        { 0x4C, (UWORD)IEQUALIFIER_LALT, (u16)WP_KEY_UP, (u8)WP_MOD_ALT },       /* Alt+Up -- a different special key */
        { 0x50, (UWORD)IEQUALIFIER_LALT, (u16)WP_KEY_F1, (u8)WP_MOD_ALT },       /* Alt+F1 */
        { 0x45, (UWORD)IEQUALIFIER_CONTROL, (u16)WP_KEY_ESCAPE, 0 } /* an unrelated qualifier bit set, Alt clear -- must NOT set WP_MOD_ALT */
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct IntuiMessage imsg;
        wp_key_event ev;

        memset(&imsg, 0, sizeof(imsg));
        imsg.Code = cases[i].code;
        imsg.Qualifier = cases[i].qualifier;

        translate_rawkey(&imsg, &ev);
        if (ev.special != cases[i].special) return WP_FALSE;
        if (ev.mods != cases[i].mods) return WP_FALSE;
    }
    return WP_TRUE;
}

/* M10 Phase 6: proves wp_config_load resolves all five real keymap=
   values against a REAL file at WP_AMIGA_CONFIG_PATH, written and read
   through real AmigaDOS file I/O -- mirrors platform/atari/app_shell.c's
   own wp_atari_config_selfcheck exactly, see its own comment for the
   full reasoning (host/tests/test_config.c already proves the string-
   matching logic itself; this proves the real on-target file round
   trip). Leaves the fixture exactly as clean as it found it. */
wp_bool wp_amiga_config_selfcheck(void)
{
    static const struct { const char *content; u8 expected; } cases[] = {
        { "keymap=vim\n", (u8)WP_KEYMAP_VI },
        { "keymap=wordstar\n", (u8)WP_KEYMAP_WORDSTAR },
        { "keymap=emacs\n", (u8)WP_KEYMAP_EMACS },
        { "keymap=wordperfect\n", (u8)WP_KEYMAP_WORDPERFECT },
        { "keymap=msword\n", (u8)WP_KEYMAP_MSWORD }
    };
    wp_bool ok = WP_TRUE;
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        plat_file *f;
        wp_config cfg;

        if (plat_file_open(WP_AMIGA_CONFIG_PATH, PLAT_FILE_WRITE, &f) != WP_OK) { ok = WP_FALSE; continue; }
        if (plat_file_write(f, (const u8 *)cases[i].content, (u32)strlen(cases[i].content)) != WP_OK) ok = WP_FALSE;
        plat_file_close(f);

        wp_config_load(WP_AMIGA_CONFIG_PATH, &cfg);
        if (cfg.keymap_id != cases[i].expected) ok = WP_FALSE;
    }

    plat_file_delete(WP_AMIGA_CONFIG_PATH);
    return ok;
}

wp_bool wp_amiga_image_selfcheck(void)
{
    offload_capabilities caps;
    wp_bool ok = WP_TRUE;
    wp_document doc;
    u32 sec_idx, para_idx;

    memset(&caps, 0, sizeof(caps));
    caps.ops_supported = (1uL << OFFLOAD_THESAURUS) | (1uL << OFFLOAD_CHAT) | (1uL << OFFLOAD_IMAGE);

    if (!offload_caps_supports(&caps, OFFLOAD_IMAGE)) ok = WP_FALSE;
    if (!offload_caps_supports(&caps, OFFLOAD_CHAT)) ok = WP_FALSE;
    if (offload_caps_supports(&caps, OFFLOAD_GRAMMAR_CHECK)) ok = WP_FALSE;

    /* M15 Phase 5: position/size assertion -- an image paragraph must
       relayout to EXACTLY its descriptor's display_width_twips/
       display_height_twips, the same "does the engine's own layout math
       agree with what the paint loop will use" proof wp_amiga_layout_
       selfcheck already gives text paragraphs, now extended to
       WP_PARA_IMAGE. Pure engine-side (linebreak.c), no window/screen
       needed, so this runs safely from the headless AMISMOKE binary. */
    if (wp_document_init(&doc) == WP_OK) {
        wp_section *sec;
        wp_paragraph *para;
        wp_image_desc desc;
        wp_image handle;

        if (wp_document_section_add(&doc, &sec_idx) != WP_OK) ok = WP_FALSE;
        else {
            sec = wp_document_section_get(&doc, sec_idx);
            memset(&desc, 0, sizeof(desc));
            desc.width_px = 4;
            desc.height_px = 4;
            desc.display_width_twips = 320;
            desc.display_height_twips = 240;
            desc.palette_count = 2;
            desc.palette[0][0] = 0;   desc.palette[0][1] = 0;   desc.palette[0][2] = 0;
            desc.palette[1][0] = 255; desc.palette[1][1] = 255; desc.palette[1][2] = 255;

            if (wp_document_image_add(&doc, &desc, &handle) != WP_OK ||
                wp_section_paragraph_add(sec, &para_idx) != WP_OK) {
                ok = WP_FALSE;
            } else {
                wp_paragraph_layout *layout;

                para = wp_section_paragraph_get(sec, para_idx);
                para->kind = (u8)WP_PARA_IMAGE;
                para->image = handle;
                para->style = doc.default_pstyle;

                if (wp_paragraph_relayout(&doc, para, 1000) != WP_OK) {
                    ok = WP_FALSE;
                } else {
                    layout = (wp_paragraph_layout *)para->layout;
                    if (layout == NULL || layout->line_count != 1) ok = WP_FALSE;
                    else if (layout->lines[0].width_twips != desc.display_width_twips) ok = WP_FALSE;
                    else if (layout->lines[0].height_twips != desc.display_height_twips) ok = WP_FALSE;
                }
            }
        }
        wp_document_free(&doc);
    } else {
        ok = WP_FALSE;
    }

    /* Bounds/no-crash: plat_draw_image must fail SAFELY (never crash)
       on degenerate input -- no real window/screen exists in this
       headless smoke binary, so a zeroed amiga_gc (screen == NULL) is
       the only gc available; every case below must be turned away by
       plat_draw_image's own guard clauses rather than dereferencing
       anything. Doesn't assert exact pixel content -- same "real
       hardware, not hand-computed" allowance T13.2's own JUSTIFY
       selfcheck already established -- only that these calls return
       safely. */
    {
        amiga_gc bad_gc;
        u8 dummy_pixels[8];
        u8 palette[2][3];

        memset(&bad_gc, 0, sizeof(bad_gc));
        memset(dummy_pixels, 0, sizeof(dummy_pixels));
        palette[0][0] = 0;   palette[0][1] = 0;   palette[0][2] = 0;
        palette[1][0] = 255; palette[1][1] = 255; palette[1][2] = 255;

        if (plat_draw_image(NULL, 0, 0, 4, 4, dummy_pixels, 4, 4, palette, 2) == WP_OK) ok = WP_FALSE;
        if (plat_draw_image((plat_gc *)&bad_gc, 0, 0, 4, 4, dummy_pixels, 4, 4, palette, 2) == WP_OK) ok = WP_FALSE;
        if (plat_draw_image((plat_gc *)&bad_gc, 0, 0, 0, 4, dummy_pixels, 4, 4, palette, 2) == WP_OK) ok = WP_FALSE;
        if (plat_draw_image((plat_gc *)&bad_gc, 0, 0, 4, 4, dummy_pixels, 4, 4, palette, 0) == WP_OK) ok = WP_FALSE;
        if (plat_draw_image((plat_gc *)&bad_gc, 0, 0, 4, 4, dummy_pixels, 4, 4, palette, 17) == WP_OK) ok = WP_FALSE;
    }

    return ok;
}
