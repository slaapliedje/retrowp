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
#include "format_txt.h"
#include "editing.h"
#include "keyevent.h"
#include "keymap.h"
#include "app_ctx.h"
#include "actions.h"

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

static const char g_sample_text[] =
    "RetroWP on Mac Classic (M12 Phase 2)\n"
    "\n"
    "This is a real Toolbox window with a working menu bar, built by\n"
    "folding platform/common/actions.c's g_wp_actions[] over Menu\n"
    "Manager calls (NewMenu/InsertMenuItem/InsertMenu).\n"
    "\n"
    "Press j/k to move the cursor down/up, i to enter Insert mode,\n"
    "Esc to return to Normal mode, :q Enter to quit.\n";

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

/* -- Document rendering: deliberately bypasses plat_draw_text/
   plat_measure_text entirely (metrics_mac.c's stubs return
   WP_UNSUPPORTED, unreachable today) via direct QuickDraw calls, same
   reasoning platform/amiga/app_shell.c's editor_draw gives -- Phase
   2's real AC is "menu, a document window, vi-mode Normal-mode
   movement, clean quit," not wrapped layout (Phase 3's job). One raw
   line per paragraph (its first run(s), truncated), no word wrap. */

#define WP_MAC_LINE_HEIGHT_PX 14

static void draw_document(WindowPtr w)
{
    Rect r;
    short y;
    u32 si, pi;

    SetPort(w);
    BeginUpdate(w);

    r = w->portRect;
    EraseRect(&r);

    if (g_ctx.doc_loaded) {
        y = (short)(r.top + 16);
        for (si = 0; si < g_mac_doc.section_count && y <= r.bottom; si++) {
            wp_section *sec = wp_document_section_get(&g_mac_doc, si);
            u32 pc = (sec != NULL) ? sec->para_count : 0;

            for (pi = 0; pi < pc && y <= r.bottom; pi++) {
                wp_paragraph *para = wp_section_paragraph_get(sec, pi);
                char linebuf[256];
                u32 len = 0;
                u16 ri;
                wp_bool is_cursor_line =
                    (wp_keymap_cursor(&g_ctx)->section == si &&
                     wp_keymap_cursor(&g_ctx)->paragraph == pi) ? WP_TRUE : WP_FALSE;

                for (ri = 0; ri < para->run_count; ri++) {
                    wp_run *run = wp_paragraph_run_get(para, ri);
                    const u8 *bytes = wp_strpool_get(&g_mac_doc.strings, run->text);
                    u32 n = run->text.length;

                    if (len + n > sizeof(linebuf) - 1) n = (u32)sizeof(linebuf) - 1 - len;
                    memcpy(linebuf + len, bytes, n);
                    len += n;
                }

                if (is_cursor_line) {
                    Rect mark;
                    SetRect(&mark, (short)(r.left + 2), (short)(y - 9),
                                    (short)(r.left + 5), (short)(y + 2));
                    PaintRect(&mark);
                }

                MoveTo((short)(r.left + 10), y);
                DrawText(linebuf, 0, (short)len);

                y = (short)(y + WP_MAC_LINE_HEIGHT_PX);
            }
        }
    }

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
