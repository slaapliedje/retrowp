#include <string.h>

#include <Quickdraw.h>
#include <Windows.h>
#include <Menus.h>
#include <Fonts.h>
#include <TextEdit.h>
#include <Dialogs.h>

#include "platform.h"
#include "app_shell.h"
#include "app_shell_internal.h"
#include "docmodel.h"
#include "strpool.h"
#include "layout.h"
#include "format_txt.h"
#include "editing.h"
#include "keyevent.h"
#include "keymap.h"
#include "app_ctx.h"
#include "actions.h"
#include "metrics_mac_internal.h"
#include "offload.h"

/* Phase 4: the offload endpoint every Tools action and the transport
   selfcheck both target -- ".AIn"/".AOut" (the modem port) at a
   conservative, universally-supported rate. Mirrors
   platform/amiga/app_shell.c's WP_AMIGA_OFFLOAD_ENDPOINT exactly; the
   device field is unvalidated beyond non-empty (see transport_mac.c's
   own comment), kept for forward-compatibility only. */
#define WP_MAC_OFFLOAD_ENDPOINT "serial:modem:9600"

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


/* ===================================================================
 * M12 Phase 2: the real Toolbox shell -- a window, a menu bar built
 * from platform/common/actions.c's g_wp_actions[] (mirroring
 * platform/amiga/app_shell.c's own build_menustrip, translated to
 * Menu Manager calls), a WaitNextEvent loop with vi-mode key dispatch,
 * File Open/Save via the Standard File Package, Cut/Copy/Paste, clean
 * quit. Deliberately no .r resource file (see docs/mac-platform.md):
 * the window is built via NewWindow, the menu bar via NewMenu/
 * InsertMenuItem/InsertMenu, all at runtime from data this project
 * already owns, rather than duplicating action labels into a separate
 * static resource that could drift out of sync.
 * =================================================================== */

#define WP_MENU_APPLE_ID 128

/* -- Application state (mirrors platform/atari/app_shell.c's/
   platform/amiga/app_shell.c's own g_doc/g_ctx) ---------------------- */

static wp_document      g_mac_doc;
static wp_undo_journal  g_mac_journal;
static wp_app_ctx        g_ctx;
static WindowPtr         g_win;
static wp_bool           g_quit;

/* Raw ASCII byte for the current keystroke, in the SAME byte space
   actions.h's shortcut_ascii documents (printable ASCII, or a Ctrl-
   letter control code 0x01-0x1A) -- set by translate_mac_key, passed
   to wp_keymap_feed_key exactly like platform/amiga/app_shell.c's own
   g_last_raw_ascii, needed only by the WP_KEYMAP_VI arm (see
   keymap.h's own comment on wp_keymap_feed_key's raw_shortcut_byte
   parameter). */
static u8 g_last_ascii = 0;

/* Phase 3: the width (in twips) every paragraph's real
   wp_paragraph_layout was last built against, plus a "the document
   itself just changed" flag -- together these tell draw_document below
   when it must call relayout_document again. Unlike platform/amiga/
   app_shell.c (whose MUI Area object has no real size until MUI's own
   layout pass runs), a Mac window's portRect is already known the
   moment NewWindow returns, so relayout could happen eagerly right
   after loading a document the way platform/atari/app_shell.c's
   replace_document does -- this still checks on every draw too, cheap
   insurance against a real resize (this phase doesn't wire up the
   window's own grow box, a documented gap, but the check costs
   nothing and stays correct if that's ever added). */
static i32 g_content_width_twips = 0;
static wp_bool g_needs_relayout = WP_TRUE;

static const char g_sample_text[] =
    "RetroWP on Mac Classic (M12 Phase 2)\n"
    "\n"
    "This is a real Toolbox window with a working menu bar, built by\n"
    "folding platform/common/actions.c's g_wp_actions[] over Menu\n"
    "Manager calls (NewMenu/InsertMenuItem/InsertMenu).\n"
    "\n"
    "Press j/k to move the cursor down/up, i to enter Insert mode,\n"
    "Esc to return to Normal mode, :q Enter to quit.\n";

/* Relayouts every paragraph at content_width_twips, mirroring platform/
   amiga/app_shell.c's relayout_document exactly -- see docs/layout.md.
   Records the width used so draw_document can tell a real resize
   (width changed) apart from "nothing to do" without re-relayouting
   every single redraw. */
static void relayout_document(i32 content_width_twips)
{
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_mac_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_mac_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_relayout(&g_mac_doc, para, content_width_twips);
        }
    }
}

static void set_document_from_text(const u8 *utf8, u32 len)
{
    wp_document new_doc;

    if (format_txt_import(utf8, len, &new_doc) != WP_OK) return;
    if (g_ctx.doc_loaded) wp_document_free(&g_mac_doc);
    g_mac_doc = new_doc;
    /* Re-points the active scheme's own .doc/.journal at the new
       document -- keymap_id itself never changes here, only re-selects
       the same arm. Matches platform/amiga/app_shell.c's own
       set_document_from_text exactly, including NOT calling
       wp_keymap_free first (a real, pre-existing asymmetry vs. Atari's
       own replace_document, not introduced here and not fixed here
       either -- see that file's own comment). */
    wp_keymap_init(&g_ctx, (wp_keymap_id)g_ctx.keymap_id, &g_mac_doc, &g_mac_journal);
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_start(&g_mac_doc);
    g_ctx.doc_loaded = WP_TRUE;
    g_needs_relayout = WP_TRUE; /* new paragraphs -- see this flag's own comment */
}

static void load_sample_document(void)
{
    set_document_from_text((const u8 *)g_sample_text, (u32)strlen(g_sample_text));
}

/* -- C string -> Pascal string, shared by menu/path construction ---- */

static void c_to_pstr(const char *c, unsigned char *p)
{
    u32 len = 0;

    while (c[len] != '\0' && len < 255) len++;
    p[0] = (unsigned char)len;
    {
        u32 i;
        for (i = 0; i < len; i++) p[i + 1] = (unsigned char)c[i];
    }
}

/* -- File Open/Save via the Standard File Package -------------------
   SFReply gives back a vRefNum + a Str63 filename, not a ready-to-use
   path -- resolving the volume's own name and combining it into a
   single "VolName:FileName" string is what lets file_mac.c's
   plat_file_open (which always passes vRefNum 0, the default volume,
   to FSOpen/Create) reach a file on whatever volume the user actually
   picked, not just the default one -- the same fully-qualified-colon-
   path mechanism "Unix:name" already relies on throughout M12
   Phase 0/1.

   GetVInfo/GetVol/GetVRefNum are declared in Retro68's headers but
   have no linkable glue implementation at all (a real gap, found by
   the linker rejecting `GETVINFO` as undefined, not guessed) --
   PBHGetVInfoSync (the raw HFS parameter-block call it would otherwise
   wrap) works directly: ioVolIndex 0 + ioVRefNum selects the volume by
   reference number, and ioNamePtr (given a buffer) receives its name. */

static wp_bool build_full_path(short vRefNum, const unsigned char *fname,
                                char *out, u32 out_cap)
{
    HParamBlockRec pb;
    unsigned char volName[64];
    u32 pos = 0;
    u32 n, i;

    memset(&pb, 0, sizeof(pb));
    volName[0] = 0;
    pb.volumeParam.ioNamePtr = volName;
    pb.volumeParam.ioVRefNum = vRefNum;
    pb.volumeParam.ioVolIndex = 0;
    if (PBHGetVInfoSync(&pb) != noErr) return WP_FALSE;

    n = volName[0];
    for (i = 0; i < n && pos < out_cap - 1; i++) out[pos++] = (char)volName[i + 1];
    if (pos < out_cap - 1) out[pos++] = ':';

    n = fname[0];
    for (i = 0; i < n && pos < out_cap - 1; i++) out[pos++] = (char)fname[i + 1];

    out[pos] = '\0';
    return WP_TRUE;
}

static wp_bool run_sf_get_file(char *out_path, u32 out_cap)
{
    SFReply reply;
    Point pt;

    pt.v = 100;
    pt.h = 80;
    SFGetFile(pt, (ConstStringPtr) "\p", NULL, -1, NULL, NULL, &reply);
    if (!reply.good) return WP_FALSE;
    return build_full_path(reply.vRefNum, reply.fName, out_path, out_cap);
}

static wp_bool run_sf_put_file(char *out_path, u32 out_cap)
{
    SFReply reply;
    Point pt;

    pt.v = 100;
    pt.h = 80;
    SFPutFile(pt, (ConstStringPtr) "\pSave document as:", (ConstStringPtr) "\pUntitled", NULL, &reply);
    if (!reply.good) return WP_FALSE;
    return build_full_path(reply.vRefNum, reply.fName, out_path, out_cap);
}

static void do_file_open(void)
{
    char fullpath[256];
    plat_file *f;
    u32 size, read_len;
    u8 *buf;

    if (!run_sf_get_file(fullpath, (u32)sizeof(fullpath))) return;
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
    if (!run_sf_put_file(fullpath, (u32)sizeof(fullpath))) return;

    format_txt_export(&g_mac_doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;

    if (format_txt_export(&g_mac_doc, buf, needed, &needed) == WP_OK) {
        if (plat_file_open(fullpath, PLAT_FILE_WRITE, &f) == WP_OK) {
            plat_file_write(f, buf, needed);
            plat_file_close(f);
        }
    }
    mem_free(buf);
}

/* Cut/Paste operate on the whole document, matching platform/atari/
   app_shell.c's/platform/amiga/app_shell.c's do_edit_cut/do_edit_paste
   exactly (same "no selection model yet" scope note -- see actions.c's
   act_edit_copy comment). Copy itself has a real portable handler
   already (act_edit_copy in platform/common/actions.c). */
static void do_edit_cut(void)
{
    static const char empty_text[] = "\n";
    const wp_action *copy_act;

    if (!g_ctx.doc_loaded) return;
    copy_act = wp_action_find(WP_ACT_EDIT_COPY);
    if (copy_act != NULL && copy_act->handler != NULL) copy_act->handler(&g_ctx);
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

static void invoke_backend_action(wp_action_id id)
{
    switch (id) {
    case WP_ACT_FILE_OPEN:  do_file_open(); break;
    case WP_ACT_FILE_SAVE:  do_file_save(); break;
    case WP_ACT_EDIT_CUT:   do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE: do_edit_paste(); break;
    default:
        /* Tools (Spellcheck/Thesaurus/Grammar/Chat) are out of scope
           for Phase 2 -- offload-dependent features land with Phase 4
           (serial transport); a real help viewer is Phase 5
           (platform/common retrofit), matching how
           platform/amiga/app_shell.c deferred the identical set of
           actions in its own T9.1b. A no-op here is an honest, flagged
           gap, not a crash. */
        break;
    }
}

static void invoke_action(const wp_action *act)
{
    if (act == NULL) return;
    if (act->handler != NULL) {
        act->handler(&g_ctx);
    } else {
        invoke_backend_action(act->id);
    }
}

/* -- Menu construction: folds g_wp_actions[] grouped by .group into
   one Menu Manager menu per group, mirroring platform/amiga/
   app_shell.c's build_menustrip exactly in spirit. Menu IDs are
   128 + wp_menu_group (129=File, 130=Edit, 131=Format, 132=Tools,
   133=Help) so do_menu_command below can map a MenuSelect/MenuKey
   result straight back to a group without a separate lookup table. */

static const char *group_title(wp_menu_group group)
{
    switch (group) {
    case WP_MENU_FILE:   return "File";
    case WP_MENU_EDIT:   return "Edit";
    case WP_MENU_FORMAT: return "Format";
    case WP_MENU_TOOLS:  return "Tools";
    case WP_MENU_HELP:   return "Help";
    default: return NULL;
    }
}

static void build_action_menus(void)
{
    static const wp_menu_group groups[] = {
        WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
    };
    u32 gi, ai;

    for (gi = 0; gi < (u32)(sizeof(groups) / sizeof(groups[0])); gi++) {
        unsigned char ptitle[256];
        MenuHandle menu;
        wp_bool any = WP_FALSE;
        short item_after = 0;

        c_to_pstr(group_title(groups[gi]), ptitle);
        menu = NewMenu((short)(WP_MENU_APPLE_ID + (int)groups[gi]), ptitle);
        if (menu == NULL) continue;

        for (ai = 0; ai < (u32)WP_ACT_COUNT; ai++) {
            const wp_action *act = &g_wp_actions[ai];
            unsigned char pitem[256];

            if (act->group != groups[gi]) continue;

            c_to_pstr(act->label_utf8, pitem);
            InsertMenuItem(menu, pitem, item_after);
            item_after++;
            any = WP_TRUE;
        }

        if (any) {
            InsertMenu(menu, 0);
        } else {
            DisposeMenu(menu);
        }
    }
}

/* Apple-menu items dispatch nowhere yet (no About box -- a real one
   needs either a resource-authored ALRT/DITL or an equivalent runtime
   construction this phase doesn't build; an honest, documented gap,
   same category as the deferred Tools/Help actions above). */
static void do_menu_command(long menu_choice)
{
    short menuID = (short)(menu_choice >> 16);
    short item = (short)(menu_choice & 0xFFFF);
    u32 ai;
    short count = 0;

    if (menu_choice == 0 || menuID == WP_MENU_APPLE_ID) {
        HiliteMenu(0);
        return;
    }

    for (ai = 0; ai < (u32)WP_ACT_COUNT; ai++) {
        const wp_action *act = &g_wp_actions[ai];

        if ((short)(WP_MENU_APPLE_ID + (int)act->group) != menuID) continue;
        count++;
        if (count == item) {
            invoke_action(act);
            break;
        }
    }
    HiliteMenu(0);
}

/* -- keyDown -> wp_key_event. Dedicated keys are resolved by VIRTUAL
   KEY CODE first (Return/Tab/Backspace/Escape/arrows/F-keys all have
   their own stable ADB codes regardless of modifiers, the standard
   Apple Extended Keyboard numbering unchanged from 1987 through modern
   USB keyboards), since charCode alone is ambiguous: Return's charCode
   (0x03 or 0x0D) collides with Control-C/Control-M's own charCode
   range. Only after the dedicated-key check falls through does charCode
   drive the printable/control-code path -- a charCode below 0x20 there
   is Control+letter, encoded by the Toolbox as a literal control code
   (0x01-0x1A), the SAME byte space actions.h's shortcut_ascii and
   platform/amiga/app_shell.c's translate_rawkey already document.

   mods is deliberately left at 0 for printable/control-code output,
   NOT populated from shiftKey/optionKey -- engine/src/vimode.c requires
   key.mods == 0 for its whole printable-command dispatch path (`if
   (key.special == WP_KEY_NONE && key.mods == 0 && key.codepoint >=
   0x20)`), and shift is already baked into the Toolbox's own charCode
   (':' vs ';', 'A' vs 'a') the same way it is on every other backend --
   setting WP_MOD_SHIFT on top of that caused every shifted character
   (':', capital letters) to be silently rejected by vi-mode's normal-
   mode dispatch, found by testing real keystrokes under BasiliskII, not
   by inspection alone. platform/amiga/app_shell.c's own translate_rawkey
   never sets WP_MOD_SHIFT either, for the identical reason. */
static void translate_mac_key(EventRecord *e, wp_key_event *out)
{
    short keyCode = (short)((e->message & keyCodeMask) >> 8);
    u8 ch = (u8)(e->message & charCodeMask);

    out->codepoint = 0;
    out->special = (u16)WP_KEY_NONE;
    out->mods = 0;
    out->_pad = 0;
    g_last_ascii = ch;

    switch (keyCode) {
    case 0x24: out->special = (u16)WP_KEY_ENTER;     return; /* Return */
    case 0x4C: out->special = (u16)WP_KEY_ENTER;     return; /* Enter (keypad) */
    case 0x30: out->special = (u16)WP_KEY_TAB;       return;
    case 0x33: out->special = (u16)WP_KEY_BACKSPACE; return;
    case 0x35: out->special = (u16)WP_KEY_ESCAPE;    return;
    case 0x7B: out->special = (u16)WP_KEY_LEFT;      return;
    case 0x7C: out->special = (u16)WP_KEY_RIGHT;     return;
    case 0x7D: out->special = (u16)WP_KEY_DOWN;      return;
    case 0x7E: out->special = (u16)WP_KEY_UP;        return;
    case 0x7A: out->special = (u16)WP_KEY_F1;        return;
    case 0x78: out->special = (u16)WP_KEY_F2;        return;
    case 0x63: out->special = (u16)WP_KEY_F3;        return;
    case 0x76: out->special = (u16)WP_KEY_F4;        return;
    case 0x60: out->special = (u16)WP_KEY_F5;        return;
    case 0x61: out->special = (u16)WP_KEY_F6;        return;
    case 0x62: out->special = (u16)WP_KEY_F7;        return;
    case 0x64: out->special = (u16)WP_KEY_F8;        return;
    case 0x65: out->special = (u16)WP_KEY_F9;        return;
    case 0x6D: out->special = (u16)WP_KEY_F10;       return;
    default: break;
    }

    if (ch < 0x20) {
        out->codepoint = (u32)(ch + ('a' - 1));
        out->mods |= (u8)WP_MOD_CTRL;
        return;
    }

    out->codepoint = (u32)ch;
}

static wp_bool mac_dispatch_key(wp_key_event ev)
{
    wp_keymap_outcome outcome;

    if (!g_ctx.doc_loaded) return WP_FALSE;

    outcome = wp_keymap_feed_key(&g_ctx, ev, g_last_ascii);

    switch ((wp_keymap_result_kind)outcome.kind) {
    case WP_KEYMAP_RESULT_QUIT:
        return WP_TRUE;
    case WP_KEYMAP_RESULT_SAVE_QUIT:
        do_file_save();
        return WP_TRUE;
    case WP_KEYMAP_RESULT_ACTION:
        invoke_action(wp_action_find((wp_action_id)outcome.action_id));
        break;
    default:
        break;
    }

    return g_ctx.quit_requested;
}

/* -- Document rendering (Phase 3): real wp_paragraph_relayout output,
   painted through the real plat_draw_text/plat_measure_text seam
   (metrics_mac.c/draw_mac.c) instead of Phase 2's direct-QuickDraw,
   one-raw-line-per-paragraph bypass -- mirrors platform/amiga/
   app_shell.c's own editor_draw rewrite in M9 T9.2. */

#define WP_DOC_LEFT_MARGIN_PX  6
#define WP_DOC_RIGHT_MARGIN_PX 4
#define WP_DOC_TOP_MARGIN_PX   8
#define WP_DOC_MIN_CONTENT_PX  20

/* Pure arithmetic behind a line's per-alignment horizontal offset,
   pulled out of the paint loop exactly the way platform/atari/
   app_shell.c's atari_align_offset_twips and platform/amiga/
   app_shell.c's amiga_align_offset_twips both are -- same three-way
   LEFT/CENTER/RIGHT behavior (JUSTIFY isn't implemented, a documented
   gap shared with both other backends). Returns a twips offset;
   callers convert to pixels themselves since the twips<->px ratio is a
   metrics concern, not an alignment one. */
static i32 mac_align_offset_twips(u8 align, i32 content_width_twips, i32 line_width_twips)
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
   that a paragraph can genuinely be more than one line. Mirrors
   platform/amiga/app_shell.c's line_contains_pos exactly. */
static wp_bool line_contains_pos(const wp_line *line, u32 run, u32 offset)
{
    if (run < line->start_run || run > line->end_run) return WP_FALSE;
    if (run == line->start_run && offset < line->start_offset) return WP_FALSE;
    if (run == line->end_run && offset > line->end_offset) return WP_FALSE;
    return WP_TRUE;
}

static void draw_document(WindowPtr w)
{
    mac_gc gc;
    Rect r;
    short y;
    i32 content_width_px;
    i32 content_width_twips;
    u32 si, pi, li;
    plat_font *font = NULL;
    wp_cstyle cached_style = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent_px = 0;

    SetPort(w);
    BeginUpdate(w);

    r = w->portRect;
    EraseRect(&r);

    if (!g_ctx.doc_loaded) {
        EndUpdate(w);
        return;
    }

    /* Real per-window pixel width, converted through the SAME twips<->px
       ratio metrics_mac.c uses for measurement -- Phase 3's real point,
       "on-screen wrap matches engine layout," only holds if layout and
       paint agree on this number. Relayout only when it actually
       changes (a real resize) or the document itself just changed. */
    content_width_px = (i32)(r.right - r.left) - WP_DOC_LEFT_MARGIN_PX - WP_DOC_RIGHT_MARGIN_PX;
    if (content_width_px < WP_DOC_MIN_CONTENT_PX) content_width_px = WP_DOC_MIN_CONTENT_PX;
    content_width_twips = mac_px_to_twips(content_width_px);

    if (g_needs_relayout || content_width_twips != g_content_width_twips) {
        relayout_document(content_width_twips);
        g_needs_relayout = WP_FALSE;
    }

    gc.window = w;

    y = (short)(r.top + WP_DOC_TOP_MARGIN_PX);
    for (si = 0; si < g_mac_doc.section_count && y <= r.bottom; si++) {
        wp_section *sec = wp_document_section_get(&g_mac_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc && y <= r.bottom; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
            const wp_para_style *pstyle = wp_document_pstyle_get(&g_mac_doc, para->style);
            u8 align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;
            wp_bool is_cursor_para =
                (wp_keymap_cursor(&g_ctx)->section == si &&
                 wp_keymap_cursor(&g_ctx)->paragraph == pi) ? WP_TRUE : WP_FALSE;
            u32 cursor_line_index = 0;

            if (layout == NULL) continue;

            if (is_cursor_para && layout->line_count > 0) {
                cursor_line_index = layout->line_count - 1; /* fallback: last line */
                for (li = 0; li < layout->line_count; li++) {
                    if (line_contains_pos(&layout->lines[li], wp_keymap_cursor(&g_ctx)->run,
                                           wp_keymap_cursor(&g_ctx)->offset)) {
                        cursor_line_index = li;
                        break;
                    }
                }
            }

            for (li = 0; li < layout->line_count && y <= r.bottom; li++) {
                wp_line *line = &layout->lines[li];
                char buf[256];
                u32 buf_len = 0;
                u32 run_i;
                i32 x_px = r.left + WP_DOC_LEFT_MARGIN_PX;
                i32 line_height_px;
                wp_cstyle line_style = g_mac_doc.default_cstyle;

                x_px += mac_twips_to_px(
                    mac_align_offset_twips(align, content_width_twips, line->width_twips));

                for (run_i = line->start_run; run_i <= line->end_run && run_i < para->run_count; run_i++) {
                    wp_run *run = wp_paragraph_run_get(para, (u16)run_i);
                    const u8 *bytes = wp_strpool_get(&g_mac_doc.strings, run->text);
                    u32 seg_start = (run_i == line->start_run) ? line->start_offset : 0;
                    u32 seg_end = (run_i == line->end_run) ? line->end_offset : run->text.length;
                    u32 k;

                    line_style = run->style;
                    for (k = seg_start; k < seg_end && buf_len < sizeof(buf) - 1; k++) {
                        buf[buf_len++] = (char)bytes[k];
                    }
                }

                if (!have_font || line_style != cached_style) {
                    const wp_char_style *cs = wp_document_cstyle_get(&g_mac_doc, line_style);
                    plat_font_metrics fm;

                    if (cs != NULL) {
                        if (font != NULL) plat_font_close(font);
                        plat_font_open((const u8 *)"", 0, cs->size_twips, cs->flags, &font);
                        plat_font_metrics_get(font, &fm);
                        ascent_px = mac_twips_to_px(fm.ascent);
                        cached_style = line_style;
                        have_font = WP_TRUE;
                    }
                }

                if (is_cursor_para && li == cursor_line_index) {
                    Rect mark;
                    SetRect(&mark, (short)(r.left + 2), (short)(y - ascent_px),
                                    (short)(r.left + 5), (short)(y + 2));
                    PaintRect(&mark);
                }

                line_height_px = mac_twips_to_px(line->height_twips);
                if (buf_len > 0 && font != NULL) {
                    plat_draw_text((plat_gc *)&gc, font, x_px, y + ascent_px,
                                    (const u8 *)buf, buf_len, 0, 0, 0);
                }
                y = (short)(y + line_height_px);
            }
        }
    }

    if (font != NULL) plat_font_close(font);
    EndUpdate(w);
}

/* -- Window/menu construction + event loop ---------------------------
   No .r resource file (see this section's own top comment) -- the
   window is a plain NewWindow, matching Retro68's own WDEF sample's
   MakeNewWindow (Samples/WDEF/wdefshell.c), which builds a real
   document window the identical resource-free way. */

void app_shell_run(void)
{
    Rect winRect;
    MenuHandle appleMenu;
    EventRecord e;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    wp_app_ctx_init(&g_ctx, (wp_keymap_id)WP_KEYMAP_VI, &g_mac_doc, &g_mac_journal);
    load_sample_document();

    /* "\024" is the Apple-logo character (0x14) -- the Menu Manager's
       own documented convention for recognizing the Apple menu. */
    appleMenu = NewMenu(WP_MENU_APPLE_ID, (ConstStringPtr) "\p\024");
    AppendMenu(appleMenu, (ConstStringPtr) "\pAbout RetroWP...");
    InsertMenu(appleMenu, 0);

    build_action_menus();
    DrawMenuBar();

    SetRect(&winRect, 40, 60, 520, 340);
    g_win = NewWindow(NULL, &winRect, (ConstStringPtr) "\pRetroWP", true,
                       documentProc, (WindowPtr) -1L, true, 0);
    if (g_win == NULL) return;
    SetPort(g_win);

    g_quit = WP_FALSE;
    for (;;) {
        if (WaitNextEvent(everyEvent, &e, 15, NULL)) {
            switch (e.what) {
            case keyDown:
            case autoKey:
                if (e.modifiers & cmdKey) {
                    do_menu_command(MenuKey((CharParameter)(e.message & charCodeMask)));
                } else {
                    wp_key_event ev;

                    translate_mac_key(&e, &ev);
                    if (mac_dispatch_key(ev)) g_quit = WP_TRUE;
                    InvalRect(&g_win->portRect);
                }
                break;

            case mouseDown: {
                WindowPtr whichWindow;

                switch (FindWindow(e.where, &whichWindow)) {
                case inMenuBar:
                    do_menu_command(MenuSelect(e.where));
                    break;
                case inSysWindow:
                    SystemClick(&e, whichWindow);
                    break;
                case inDrag:
                    DragWindow(whichWindow, e.where, &qd.screenBits.bounds);
                    break;
                case inGoAway:
                    if (TrackGoAway(whichWindow, e.where)) g_quit = WP_TRUE;
                    break;
                case inContent:
                    if (whichWindow != FrontWindow()) SelectWindow(whichWindow);
                    break;
                default:
                    break;
                }
                break;
            }

            case updateEvt:
                draw_document((WindowPtr)e.message);
                break;

            default:
                break;
            }
        }
        if (g_ctx.quit_requested) g_quit = WP_TRUE;
        if (g_quit) break;
    }

    if (g_ctx.doc_loaded) {
        wp_keymap_free(&g_ctx);
        wp_document_free(&g_mac_doc);
    }
    DisposeWindow(g_win);
}

/* Phase 3: imports a short multi-word paragraph, relayouts it at a
   column width deliberately narrow enough to force wrapping, and
   checks two properties quantitatively through the REAL QuickDraw
   metrics seam (metrics_mac.c) rather than by eyeballing a screenshot:
   more than one line results, and every line's measured width stays
   within the requested column -- the exact mirror of platform/atari/
   app_shell.c's wp_atari_layout_selfcheck and platform/amiga/
   app_shell.c's wp_amiga_layout_selfcheck (same fixture text, same
   900-twip column, same two assertions), proving the same engine code
   (linebreak.c) behaves correctly against a third, independent
   plat_font_open/plat_measure_text implementation. */
wp_bool wp_mac_layout_selfcheck(void)
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

    wp_document_free(&doc);
    return ok;
}

/* Phase 4: proves transport_mac.c's real Device Manager async I/O
   lifecycle (OpenDriver/PBReadAsync/PBWriteAsync/KillIO/CloseDriver)
   and offload_client.c's graceful-no-gateway behavior, mirroring
   platform/atari/app_shell.c's wp_atari_transport_selfcheck and
   platform/amiga/app_shell.c's wp_amiga_transport_selfcheck exactly:
   open, write a fixture, confirm a read against an unconnected port
   times out (not hangs, not errors) within a sane window -- twice, to
   rule out a one-shot fluke -- then confirm the portable
   offload_init/offload_available/offload_shutdown API degrades
   gracefully with no live gateway attached. What this deliberately does
   NOT prove: a real end-to-end round trip against a live gateway over
   actual emulated RS-232 -- see docs/mac-platform.md for whether that
   was achievable in this environment and why. */
/* Unlike platform/amiga/app_shell.c's own wp_amiga_transport_selfcheck
   (safe to open+write+read against amiga's serial.device, which is a
   real Kickstart-level device always present regardless of any prefs
   file), this deliberately does NOT attempt a real read-timeout cycle
   against an opened port. Found the hard way, not guessed: with a real
   socat-created PTY wired in as BasiliskII's own `seriala` (confirmed
   to work at all -- unlike amiberry, BasiliskII's serial_unix.cpp just
   opens whatever host path is given, no hardware enumeration --
   OpenDriver/SerReset/SerHShake/plat_transport_write all genuinely
   succeeded against it), plat_transport_read hung indefinitely rather
   than honoring its own timeout. Root cause, confirmed by reading
   BasiliskII's own source (src/Unix/serial_unix.cpp's input_func): its
   read thread does a single plain blocking host read(fd, buf, len) and
   only checks its own io_killed cancellation flag AFTER that call
   returns -- against a genuinely idle line (nothing ever written to
   the PTY's other end), that read() blocks forever, and KillIO has no
   way to interrupt an in-progress blocking syscall on another thread.
   This is a real BasiliskII limitation, not a bug in transport_mac.c's
   own PBReadAsync/KillIO usage (which matches the documented Device
   Manager async I/O contract) -- see docs/mac-platform.md. */
wp_bool wp_mac_transport_selfcheck(void)
{
    plat_transport *t;
    static const u8 fixture[] = "RetroWP transport selfcheck";
    u32 before, after, elapsed;
    wp_status st;
    wp_bool ok = WP_TRUE;

    /* Endpoint parsing is safe to test fully without any real device:
       neither path below ever reaches OpenDriver. */
    if (plat_transport_open("tcp:modem:9600", 2000, &t) != WP_UNSUPPORTED) ok = WP_FALSE;
    if (plat_transport_open("serial:modem:1", 2000, &t) != WP_ERR) ok = WP_FALSE; /* 1 baud: not in the SCC's fixed rate set */

    /* Opening the real endpoint: in this project's standard fixture
       (no `seriala` configured), OpenDriver itself fails, and does so
       quickly -- verified to complete well under a second in manual
       testing. The bound below only catches a genuine hang; it is not
       tuned to any particular fast-path timing. If a real serial
       device DOES happen to be configured (e.g. a future manual
       verification pass), succeeding is fine too -- write a fixture
       (proven safe above) and close immediately, deliberately never
       calling plat_transport_read here given the hang this file's own
       comment just described. */
    before = plat_ticks_ms();
    st = plat_transport_open(WP_MAC_OFFLOAD_ENDPOINT, 2000, &t);
    after = plat_ticks_ms();
    elapsed = after - before;
    if (elapsed > 5000) ok = WP_FALSE;

    if (st == WP_OK) {
        if (plat_transport_write(t, fixture, (u32)(sizeof(fixture) - 1), 2000) != WP_OK) ok = WP_FALSE;
        plat_transport_close(t);
    }

    /* offload_init/offload_available/offload_shutdown's graceful-no-
       gateway behavior, mirroring wp_amiga_transport_selfcheck exactly
       -- safe regardless of whether a real serial device is configured,
       since no live gateway ever answers the HELLO handshake either
       way. */
    before = plat_ticks_ms();
    st = offload_init(WP_MAC_OFFLOAD_ENDPOINT);
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
