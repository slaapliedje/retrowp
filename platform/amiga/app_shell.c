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
#include "format_txt.h"
#include "editing.h"
#include "vimode.h"
#include "keyevent.h"
#include "app_ctx.h"
#include "actions.h"

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

static const char g_sample_text[] =
    "RetroWP on Amiga (M9 T9.1b)\n"
    "\n"
    "This is a real MUI Application/Window with a working menu,\n"
    "built by folding platform/common/actions.c's g_wp_actions[]\n"
    "table over a Menustrip/Menu/Menuitem hierarchy.\n"
    "\n"
    "Press j/k to move the cursor down/up, i to enter Insert mode,\n"
    "Esc to return to Normal mode, :q Enter to quit.\n";

static void set_document_from_text(const u8 *utf8, u32 len)
{
    wp_document new_doc;

    if (format_txt_import(utf8, len, &new_doc) != WP_OK) return;
    if (g_ctx.doc_loaded) wp_document_free(&g_amiga_doc);
    g_amiga_doc = new_doc;
    wp_vimode_init(&g_ctx.vi, &g_amiga_doc, &g_amiga_journal);
    g_ctx.vi.cursor = wp_pos_doc_start(&g_amiga_doc);
    g_ctx.doc_loaded = WP_TRUE;
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

static void do_help_contents(void)
{
    MUI_Request(g_app, g_win, 0, (CONST_STRPTR)"RetroWP Help", (CONST_STRPTR)"Ok",
                (CONST_STRPTR)"vi-mode keys: hjkl move, i insert, Esc normal, :w save, :q quit.");
}

static void invoke_backend_action(wp_action_id id)
{
    switch (id) {
    case WP_ACT_FILE_OPEN:      do_file_open(); break;
    case WP_ACT_FILE_SAVE:      do_file_save(); break;
    case WP_ACT_EDIT_CUT:       do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE:     do_edit_paste(); break;
    case WP_ACT_HELP_CONTENTS:  do_help_contents(); break;
    default:
        /* Tools (Spellcheck/Thesaurus/Grammar/Chat) are out of scope for
           T9.1b -- offload-dependent features land with T9.3. A no-op
           here is an honest, flagged gap, not a crash. */
        break;
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
   verified empirically under amiberry, not just recalled. */
static void translate_rawkey(struct IntuiMessage *imsg, wp_key_event *out)
{
    UWORD code = imsg->Code;
    struct InputEvent ie;
    UBYTE buf[8];
    WORD n;

    out->codepoint = 0;
    out->special = (u16)WP_KEY_NONE;
    out->mods = 0;
    out->_pad = 0;
    g_last_raw_ascii = 0;

    if (code & IECODE_UP_PREFIX) return; /* key release -- ignore */

    switch (code) {
    case 0x41: out->special = (u16)WP_KEY_BACKSPACE; return;
    case 0x42: out->special = (u16)WP_KEY_TAB; return;
    case 0x44: out->special = (u16)WP_KEY_ENTER; return;
    case 0x45: out->special = (u16)WP_KEY_ESCAPE; return;
    case 0x46: out->special = (u16)WP_KEY_DELETE; return;
    case 0x4C: out->special = (u16)WP_KEY_UP; return;
    case 0x4D: out->special = (u16)WP_KEY_DOWN; return;
    case 0x4E: out->special = (u16)WP_KEY_RIGHT; return;
    case 0x4F: out->special = (u16)WP_KEY_LEFT; return;
    default: break;
    }
    if (code >= 0x50 && code <= 0x59) {
        out->special = (u16)(WP_KEY_F1 + (code - 0x50));
        return;
    }

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = (UBYTE)code;
    ie.ie_Qualifier = imsg->Qualifier;
    n = MapRawKey(&ie, (STRPTR)buf, (LONG)sizeof(buf), NULL);
    if (n <= 0) return;

    g_last_raw_ascii = buf[0];
    if (buf[0] < 0x20) {
        out->codepoint = (u32)(buf[0] + ('a' - 1));
        out->mods = (u8)WP_MOD_CTRL;
    } else {
        out->codepoint = (u32)buf[0];
    }
}

/* Mirrors platform/atari/app_shell.c's wp_dispatch_key: feed vi-mode
   first, then fall back to the action-shortcut table using the RAW
   ascii byte (not the translated wp_key_event) -- see g_last_raw_ascii's
   own comment for why. Returns WP_TRUE if the app should quit. */
static wp_bool amiga_dispatch_key(wp_key_event ev)
{
    wp_vimode_action action;
    const wp_action *act;

    if (!g_ctx.doc_loaded) return WP_FALSE;

    action = wp_vimode_feed_key(&g_ctx.vi, ev);

    switch ((wp_vimode_action_kind)action.kind) {
    case WP_VIACTION_QUIT:
        return WP_TRUE;
    case WP_VIACTION_SAVE_QUIT:
        do_file_save();
        return WP_TRUE;
    case WP_VIACTION_SAVE:
        do_file_save();
        break;
    default:
        break;
    }

    if (wp_vimode_get_mode(&g_ctx.vi) != WP_VIMODE_COMMAND) {
        act = wp_action_find_by_shortcut(g_last_raw_ascii);
        if (act != NULL) {
            if (act->handler != NULL) {
                act->handler(&g_ctx);
            } else {
                invoke_backend_action(act->id);
            }
        }
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

#define WP_EDITOR_LINE_HEIGHT_PX 9

static ULONG editor_draw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    u32 si, pi;
    i32 y;

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

    SetAPen(_rp(obj), _dri(obj)->dri_Pens[TEXTPEN]);
    SetBPen(_rp(obj), _dri(obj)->dri_Pens[BACKGROUNDPEN]);
    SetDrMd(_rp(obj), JAM2);

    if (!g_ctx.doc_loaded) return 0;

    y = _mtop(obj) + 8;
    for (si = 0; si < g_amiga_doc.section_count && y <= _mbottom(obj); si++) {
        wp_section *sec = wp_document_section_get(&g_amiga_doc, si);
        for (pi = 0; pi < sec->para_count && y <= _mbottom(obj); pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            char linebuf[256];
            u32 len = 0;
            u16 ri;
            wp_bool is_cursor_line =
                (g_ctx.vi.cursor.section == si && g_ctx.vi.cursor.paragraph == pi)
                ? WP_TRUE : WP_FALSE;

            for (ri = 0; ri < para->run_count; ri++) {
                wp_run *run = wp_paragraph_run_get(para, ri);
                const u8 *bytes = wp_strpool_get(&g_amiga_doc.strings, run->text);
                u32 n = run->text.length;

                if (len + n > sizeof(linebuf) - 1) n = (u32)sizeof(linebuf) - 1 - len;
                memcpy(linebuf + len, bytes, n);
                len += n;
            }

            if (is_cursor_line) {
                SetAPen(_rp(obj), _dri(obj)->dri_Pens[FILLPEN]);
                RectFill(_rp(obj), _mleft(obj), (LONG)(y - 7), _mleft(obj) + 1, (LONG)(y + 1));
                SetAPen(_rp(obj), _dri(obj)->dri_Pens[TEXTPEN]);
            }

            Move(_rp(obj), _mleft(obj) + 4, (LONG)y);
            Text(_rp(obj), (STRPTR)linebuf, (LONG)len);

            y += WP_EDITOR_LINE_HEIGHT_PX;
        }
    }
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
    MUI_Redraw(obj, MADF_DRAWOBJECT);
    if (quit) {
        DoMethod(_app(obj), MUIM_Application_ReturnID, (LONG)MUIV_Application_ReturnID_Quit);
    }
    return 0;
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
            char shortcut[2];

            if (act->group != groups[gi]) continue;

            shortcut[0] = act->shortcut_ascii
                ? (char)((act->shortcut_ascii + ('a' - 1)) - 'a' + 'A')
                : '\0';
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

void app_shell_run(void)
{
    Object *menustrip;
    ULONG open;

    if (!open_libraries()) { close_libraries(); return; }

    wp_app_ctx_init(&g_ctx, &g_amiga_doc, &g_amiga_journal);
    load_sample_document();

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
                const wp_action *act = wp_action_find((wp_action_id)(id - WP_RETURNID_ACTION_BASE));

                if (act != NULL) {
                    if (act->handler != NULL) {
                        act->handler(&g_ctx);
                    } else {
                        invoke_backend_action(act->id);
                    }
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
    close_libraries();
}
