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
#include "menucheck.h"
#include "metrics_mac_internal.h"
#include "offload.h"
#include "config.h"
#include "help_doc.h"
#include "help_view.h"
#include "statusline.h"
#include "help_retrowp.h"
#include "endian.h"

/* Phase 4: the offload endpoint every Tools action and the transport
   selfcheck both target -- ".AIn"/".AOut" (the modem port) at a
   conservative, universally-supported rate. Mirrors
   platform/amiga/app_shell.c's WP_AMIGA_OFFLOAD_ENDPOINT exactly; the
   device field is unvalidated beyond non-empty (see transport_mac.c's
   own comment), kept for forward-compatibility only. */
#define WP_MAC_OFFLOAD_ENDPOINT "serial:modem:9600"

/* M15 T15.4: the lazy-once offload_init() pattern and the CAPS-bit
   pre-check helper platform/atari/app_shell.c's/platform/amiga/
   app_shell.c's own do_chat() already established -- Insert Image is
   the first offload-CONSUMING action wired up on this backend (every
   other Tools action stays the documented no-op above), so neither
   helper existed here before now. */
static wp_bool g_mac_offload_init_attempted = WP_FALSE;

static wp_bool ensure_offload(void)
{
    if (!g_mac_offload_init_attempted) {
        g_mac_offload_init_attempted = WP_TRUE;
        offload_init(WP_MAC_OFFLOAD_ENDPOINT); /* result read back via offload_available() */
    }
    return offload_available();
}

static wp_bool offload_caps_supports(const offload_capabilities *caps, offload_op op)
{
    return (wp_bool)((caps->ops_supported & (1uL << (u32)op)) != 0);
}

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

/* T169: how many text fragments the paint loop has actually drawn since
   startup. Reported through the CI marker so the gate can require the
   app to have rendered REAL DOCUMENT CONTENT, not merely to be alive --
   the signal that catches an app which comes up, paints its chrome and
   then shows an empty page. That is not hypothetical: the DOS backend
   was found doing exactly this while this gate was being built. */
static u32               g_mac_painted_frags;
static wp_bool           g_mac_ci_active;
static u32               g_mac_ci_events;

/* Raw ASCII byte for the current keystroke, in the SAME byte space
   actions.h's shortcut_ascii documents (printable ASCII, or a Ctrl-
   letter control code 0x01-0x1A) -- set by translate_mac_key, passed
   to wp_keymap_feed_key exactly like platform/amiga/app_shell.c's own
   g_last_raw_ascii, needed only by the WP_KEYMAP_VI arm (see
   keymap.h's own comment on wp_keymap_feed_key's raw_shortcut_byte
   parameter). */
static u8 g_last_ascii = 0;

/* Phase 5: the help system, reusing platform/common/help_doc.h/
   help_view.h unchanged -- mirrors platform/amiga/app_shell.c's own
   g_help_doc/g_help_doc_loaded/g_help_view/g_help_active exactly.
   g_help_active is a whole separate UI mode: while set,
   mac_dispatch_key routes every key to wp_help_view_feed_key instead
   of vi-mode, and draw_document paints the pager instead of the
   document -- see paint_help_mac() and the g_help_active checks in
   both. Reachable via F1 (mac_dispatch_key) or the real Help menu
   action (do_help_contents, below). */
static wp_help_doc        g_help_doc;
static wp_bool            g_help_doc_loaded = WP_FALSE;
static wp_help_view_state g_help_view;
static wp_bool            g_help_active = WP_FALSE;

/* Phase 5: a small, fixed-size font used ONLY for the status line's own
   height/drawing -- deliberately independent of the document's current
   cstyle (which changes per run, per toggle) so the reserved strip's
   height never shifts underneath the document as the user types.
   Matches platform/atari/app_shell.c's/platform/amiga/app_shell.c's own
   WP_ATARI_STATUSLINE_SIZE_TWIPS/WP_AMIGA_HINT_BAR_SIZE_TWIPS choice of
   160 (8pt) exactly, same reasoning. */
#define WP_MAC_STATUSLINE_SIZE_TWIPS 160
#define WP_MAC_HELP_BODY_SIZE_TWIPS  200

/* Phase 5: the config file path -- a bare filename (no volume prefix)
   resolves via vRefNum 0 to whatever the app's own current/default
   directory is (the classic Mac equivalent of Amiga's "PROGDIR:", and
   of a kind with Atari's own fixed boot-volume-root path) -- when
   launched from the ExtFS share (this project's own CI fixture), that's
   "Unix:"; for a real double-clicked app on real media, it's wherever
   the app itself lives. */
#define WP_MAC_CONFIG_PATH "RetroWP.cfg"

/* Phase 3: the width (in twips) every paragraph's real
   wp_paragraph_layout was last built against -- lets relayout_document
   tell a real resize (every line must re-break) apart from an ordinary
   redraw. Unlike platform/amiga/app_shell.c (whose MUI Area object has
   no real size until MUI's own layout pass runs), a Mac window's
   portRect is already known the moment NewWindow returns, so relayout
   could happen eagerly right after loading a document the way
   platform/atari/app_shell.c's replace_document does -- draw_document
   still checks on every draw too, cheap insurance against a real
   resize (this phase doesn't wire up the window's own grow box, a
   documented gap, but the check costs nothing and stays correct if
   that's ever added).

   There used to be a companion g_needs_relayout flag meaning "the
   document itself is new". It was removed with the stale-layout fix in
   draw_document: it only ever tracked whole-document REPLACEMENT, so it
   was blind to keystroke edits -- which is precisely how that bug got
   in. para->layout_dirty, which the engine already maintains per
   paragraph, subsumes it and covers edits too (a freshly imported
   document's paragraphs all start with layout == NULL, so the "new
   document" case still forces a rebuild without a separate flag). */
static i32 g_content_width_twips = 0;

static const char g_sample_text[] =
    "RetroWP on Mac Classic (M12 Phase 2)\n"
    "\n"
    "This is a real Toolbox window with a working menu bar, built by\n"
    "folding platform/common/actions.c's g_wp_actions[] over Menu\n"
    "Manager calls (NewMenu/InsertMenuItem/InsertMenu).\n"
    "\n"
    "Press j/k to move the cursor down/up, i to enter Insert mode,\n"
    "Esc to return to Normal mode, :q Enter to quit.\n";

/* Relayouts only what actually needs it, mirroring platform/amiga/
   app_shell.c's relayout_document exactly -- see docs/layout.md. A
   paragraph is rebuilt when its layout was never built (layout == NULL),
   when the engine marked it layout_dirty (any content edit -- edit.c
   sets it on every insert/delete), or when the column itself changed (a
   real resize, where nothing is dirty but every line must re-break).
   wp_paragraph_relayout has no early-out of its own (linebreak.c
   measures unconditionally), so this check is what makes calling
   relayout_document before every paint affordable. */
static void relayout_document(i32 content_width_twips)
{
    wp_bool width_changed = (wp_bool)(content_width_twips != g_content_width_twips);
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_mac_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_mac_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            if (!width_changed && para->layout != NULL && !para->layout_dirty) continue;
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
    /* No "document is new" flag to set: every paragraph format_txt_import
       just built has layout == NULL, which relayout_document already
       treats as needing a rebuild -- see g_content_width_twips's comment. */
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

/* Phase 5: opens the real help pager at the named node (falling back to
   node 0 if the name doesn't resolve), mirroring platform/amiga/
   app_shell.c's own open_help_node exactly. The caller (invoke_backend_
   action's menu dispatch, or mac_dispatch_key's F1 handling) already
   triggers an InvalRect afterward -- see both call sites -- so this
   doesn't force a redraw itself. */
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

/* Insert Image (M15 T15.4): pick a source file via the same Standard
   File picker do_file_open reuses, downconvert it through the real
   OFFLOAD_IMAGE op, cache the returned packed pixel bytes to disk, and
   insert a WP_PARA_IMAGE paragraph at the cursor. Unlike Atari/Amiga,
   this backend has NO alert/message mechanism anywhere (no ALRT/DITL
   resource in this toolchain's build -- dialog_mac.c's own comment
   documents plat_dialog_message as deliberately unimplemented here);
   every failure path below therefore silently returns rather than
   reporting anything to the user, the same accepted "honest, flagged
   gap" posture invoke_backend_action's own default: case already
   documents for this backend's other Tools actions -- not a new
   omission specific to this one. Real pixel rendering (reading
   blob_path back and blitting it) is a later phase (T15.6). */
#define WP_MAC_IMAGE_MAX_FILE_SIZE 65000uL /* wire frame's own WIRE_MAX_PAYLOAD (64K) minus the 6-byte request header */
#define WP_MAC_IMAGE_TIMEOUT_MS    90000uL
#define WP_MAC_IMAGE_TARGET_MAX_W  320u    /* WP_IMAGE_MAX_WIDTH_PX -- the ST low-res ceiling every target clamps against */
#define WP_MAC_IMAGE_TARGET_MAX_H  200u

static u32 g_mac_image_cache_counter = 0;

/* "WPIMG###.TMP" -- a bare filename (vRefNum 0, the real Mac
   equivalent of Amiga's PROGDIR: convention, per WP_MAC_CONFIG_PATH's
   own precedent), counter-suffixed so a second Insert Image in one
   session can't silently overwrite the first image's still-referenced
   pixel cache file underneath it. out must be >= 16 bytes. Wraps at
   1000, a real, accepted session-scope limit. */
static void mac_format_image_cache_path(char *out, u32 counter)
{
    u32 c = counter % 1000u;
    memcpy(out, "WPIMG", 5);
    out[5] = (char)('0' + (c / 100u) % 10u);
    out[6] = (char)('0' + (c / 10u) % 10u);
    out[7] = (char)('0' + c % 10u);
    memcpy(out + 8, ".TMP", 5); /* includes NUL */
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
    if (!run_sf_get_file(fullpath, (u32)sizeof(fullpath))) return;

    if (!ensure_offload()) return;
    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_IMAGE)) return;

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK || size == 0 || size > WP_MAC_IMAGE_MAX_FILE_SIZE) {
        plat_file_close(f);
        return;
    }

    req_buf = (u8 *)mem_alloc(size + 6);
    if (req_buf == NULL) { plat_file_close(f); return; }

    req_buf[0] = 2; /* target_max_colors: classic QuickDraw here is genuinely
                        monochrome (a plain NewWindow, not NewCWindow -- see
                        docs/mac-platform.md), so only black/white is real */
    req_buf[1] = 0x01; /* target_flags: TARGET_GRAYSCALE -- real dithering,
                           not a color palette this display can't show */
    be_write16(req_buf + 2, (u16)WP_MAC_IMAGE_TARGET_MAX_W);
    be_write16(req_buf + 4, (u16)WP_MAC_IMAGE_TARGET_MAX_H);

    if (plat_file_read(f, req_buf + 6, size, &read_len) != WP_OK) {
        mem_free(req_buf);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_buf;
    req.payload_len = read_len + 6;
    req.timeout_ms = (u32)WP_MAC_IMAGE_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        return;
    }
    mem_free(req_buf);

    if (resp.status != OFFLOAD_OK || resp.result_len < 5) {
        offload_response_free(&resp);
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
        char cache_path[16];
        wp_image_desc desc;
        wp_status st;
        u32 fname_start, k;

        if (palette_count < 1 || palette_count > 16 ||
            resp.result_len != 5u + (u32)palette_count * 3u + packed_len) {
            offload_response_free(&resp);
            return;
        }

        mac_format_image_cache_path(cache_path, g_mac_image_cache_counter);

        if (plat_file_open(cache_path, PLAT_FILE_WRITE, &f) != WP_OK) {
            offload_response_free(&resp);
            return;
        }
        st = plat_file_write(f, packed_pixels, packed_len);
        plat_file_close(f);
        if (st != WP_OK) {
            offload_response_free(&resp);
            return;
        }
        g_mac_image_cache_counter++;

        memset(&desc, 0, sizeof(desc));
        desc.width_px = (i32)width_px;
        desc.height_px = (i32)height_px;
        desc.display_width_twips = mac_px_to_twips((i32)width_px);
        desc.display_height_twips = mac_px_to_twips((i32)height_px);
        desc.palette_count = palette_count;
        memcpy(desc.palette, palette, (u32)palette_count * 3u);
        if (wp_strpool_append(&g_mac_doc.strings, (const u8 *)cache_path,
                               (u32)strlen(cache_path), &desc.blob_path) != WP_OK) {
            offload_response_free(&resp);
            return;
        }

        /* fullpath's own trailing ':' marks where the bare filename
           starts -- build_full_path's own "VolName:FileName" shape. */
        fname_start = 0;
        for (k = 0; fullpath[k] != '\0'; k++) {
            if (fullpath[k] == ':') fname_start = k + 1;
        }
        if (fullpath[fname_start] != '\0') {
            wp_strpool_append(&g_mac_doc.strings, (const u8 *)(fullpath + fname_start),
                               (u32)strlen(fullpath + fname_start), &desc.source_name);
        }
        offload_response_free(&resp);

        if (wp_edit_insert_image(&g_mac_doc, &g_mac_journal, (*wp_keymap_cursor(&g_ctx)), &desc) != WP_OK) {
            return;
        }
        relayout_document(g_content_width_twips);
    }
}

static void invoke_backend_action(wp_action_id id)
{
    /* Every Help entry opens the same document at a different node, so
       they share one arm keyed off the shared registry's own mapping
       rather than growing a case per node -- see actions.h for why that
       mapping lives in platform/common rather than being repeated here,
       on Amiga, on DOS and on Atari. */
    {
        u32 node_len = 0;
        const char *node = wp_action_help_node(id, &node_len);

        if (node != NULL) {
            open_help_node(node, node_len);
            return;
        }
    }

    switch (id) {
    case WP_ACT_FILE_OPEN:     do_file_open(); break;
    case WP_ACT_FILE_SAVE:     do_file_save(); break;
    case WP_ACT_EDIT_CUT:      do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE:    do_edit_paste(); break;
    case WP_ACT_TOOLS_INSERTIMAGE: do_insert_image(); break;
    default:
        /* Every other Tools action (Spellcheck/Thesaurus/Grammar/Chat)
           remains out of scope -- offload-dependent features beyond
           the transport seam itself (Phase 4) aren't wired to any UI
           yet, matching how platform/amiga/app_shell.c's own T9.3 also
           left Spellcheck as a no-op (a local-dictionary feature, no
           offload dependency). Insert Image (M15) is the first
           exception -- see its own comment for why it could be wired up
           without this backend's missing alert mechanism getting in the
           way.

           Still an honest gap -- but no longer a SILENT one. These are
           live, selectable menu items; picking one used to return here
           and do absolutely nothing, which a user cannot distinguish
           from the menu itself being broken. This backend has no alert
           mechanism (no ALRT/DITL resource, and none constructible at
           runtime here), so SysBeep is the honest minimum: it says "that
           did nothing" instead of pretending it worked. */
        SysBeep(10);
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

            /* Append a harmless placeholder, then set the real text with
               SetMenuItemText, rather than handing label_utf8 straight to
               InsertMenuItem as this used to. AppendMenu/InsertMenuItem
               PARSE the string they are given: ';' (or Return) splits one
               call into TWO items, '/' introduces a command key, '(' marks
               the item disabled, '^'/'!'/'<' set icon/mark/style, and a
               leading '-' makes a divider. Since do_menu_command maps a
               chosen item back to an action purely by counting ordinal
               position, one ';' in a future label would insert two items
               for one increment and make every item below it in that menu
               fire the WRONG action -- silently. SetMenuItemText parses
               nothing, so the label is always literal. */
            AppendMenu(menu, (ConstStringPtr) "\p ");
            item_after++;
            SetMenuItemText(menu, item_after, pitem);

            /* Command-key equivalents were never set at all -- no label
               carried a '/X' and SetItemCmd was never called -- so
               MenuKey() always returned 0 for these menus. That did more
               than just fail to work: do_menu_command(0) hits its early
               return, and the cmdKey branch in the event loop had already
               SWALLOWED the keystroke, so Cmd+O/S/Q/X/C/V did nothing and
               never reached the editor either. The letter comes from the
               shared rule rather than a local range test: Amiga had its
               own separate copy of the same decision and got it wrong a
               different way (offsetting every byte, so FORMAT_SIZE's
               0x1D became ']'). One function, two callers -- see
               platform/common/menucheck.h. */
            {
                u8 accel = wp_menu_accel_letter(act->shortcut_ascii);

                if (accel != 0) SetItemCmd(menu, item_after, (CharParameter)accel);
            }
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
            /* The Menu Manager saves and restores the bits under a
               pulled-down menu, so dismissing one generates NO update
               event -- nothing repaints unless we ask. Neither this
               function nor the inMenuBar case ever did, so Format >
               Bold/Italic/Underline (status-line change), Format >
               Size/Alignment (PopUpMenuSelect, which also saves bits)
               and Help > Contents all applied correctly and then showed
               nothing until some unrelated later event forced a redraw.
               invoke_backend_action's own comment claimed "the caller
               ... already triggers an InvalRect afterward -- see both
               call sites"; only the F1 keyboard call site actually did. */
            if (g_win != NULL) InvalRect(&g_win->portRect);
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

    /* Help is a whole separate UI mode (Phase 5): while active, every
       key goes to wp_help_view_feed_key instead of vi-mode, full stop,
       until the viewer itself reports a close request (Escape or 'q')
       -- mirrors platform/atari/app_shell.c's/platform/amiga/
       app_shell.c's own wp_dispatch_key/amiga_dispatch_key exactly. F1
       opens it from any mode/state; pressing F1 again while already
       showing help is a harmless no-op rather than resetting the
       user's position. */
    if (g_help_active) {
        if (wp_help_view_feed_key(&g_help_view, ev)) g_help_active = WP_FALSE;
        return WP_FALSE;
    }
    if (ev.special == (u16)WP_KEY_F1) {
        do_help_contents();
        return WP_FALSE;
    }

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

/* Phase 5: shared by the status bar and the help hint bar/body text
   below -- both need "how tall is one line of some font, in pixels"
   from real plat_font_metrics_get, never a hardcoded guess. Mirrors
   platform/atari/app_shell.c's/platform/amiga/app_shell.c's own
   font_line_height_px/amiga_font_line_height_px exactly. */
static i32 mac_font_line_height_px(i32 size_twips)
{
    plat_font *font;
    plat_font_metrics fm;
    i32 h_px;

    if (plat_font_open((const u8 *)"", 0, size_twips, 0, &font) != WP_OK) return 0;
    plat_font_metrics_get(font, &fm);
    h_px = mac_twips_to_px(fm.ascent + fm.descent + fm.leading);
    plat_font_close(font);
    return h_px;
}

static i32 statusline_height_px(void)
{
    return mac_font_line_height_px(WP_MAC_STATUSLINE_SIZE_TWIPS);
}

/* Paints the reserved bottom strip showing the active keymap scheme's
   current mode (and, in Command-line mode, the pending buffer being
   typed) -- see platform/common/statusline.h. Mirrors platform/atari/
   app_shell.c's paint_status_line/platform/amiga/app_shell.c's own
   equivalent exactly: always repaints in full, a no-op if no document
   is loaded or the window is too short to fit the bar. */
static void paint_status_line_mac(WindowPtr w, Rect full_rect, i32 bar_h_px)
{
    mac_gc gc;
    plat_font *font;
    wp_vi_status_view view;
    char mode_buf[40];
    Rect bar;

    if (!g_ctx.doc_loaded || bar_h_px <= 0 || bar_h_px > (i32)(full_rect.bottom - full_rect.top)) return;

    gc.window = w;

    SetRect(&bar, full_rect.left, (short)(full_rect.bottom - bar_h_px),
                  full_rect.right, full_rect.bottom);

    if (plat_font_open((const u8 *)"", 0, WP_MAC_STATUSLINE_SIZE_TWIPS, 0, &font) == WP_OK) {
        /* wp_keymap_get_status_view already folds the Command-line
           buffer into its one composed mode string and leaves
           pending_count/pending_cmd at 0/NULL -- see its own header
           comment for why (no per-mode accessor exists yet that
           wouldn't be more confusing shown alone; a documented, minor
           gap shared with every other backend, not fixed here). */
        wp_keymap_get_status_view(&g_ctx, mode_buf, (u32)sizeof(mode_buf), &view);

        wp_statusline_paint((plat_gc *)&gc, font, (i32)bar.left, (i32)bar.top,
                             (i32)(bar.right - bar.left), bar_h_px, &view);
        plat_font_close(font);
    }
}

/* Paints the help pager full-window (replacing the document body
   entirely while g_help_active is set) plus a small fixed hint bar
   along the bottom, reusing wp_statusline_paint for that hint exactly
   the way platform/atari/app_shell.c's/platform/amiga/app_shell.c's own
   paint_help does. */
static void paint_help_mac(WindowPtr w, Rect full_rect)
{
    mac_gc gc;
    plat_font *body_font;
    i32 hint_bar_h_px, body_h_px, total_h_px;
    i32 x_px, y_px, w_px;

    gc.window = w;

    x_px = full_rect.left;
    y_px = full_rect.top;
    w_px = (i32)(full_rect.right - full_rect.left);
    total_h_px = (i32)(full_rect.bottom - full_rect.top);

    hint_bar_h_px = statusline_height_px();
    if (hint_bar_h_px > total_h_px) hint_bar_h_px = 0;
    body_h_px = total_h_px - hint_bar_h_px;
    if (body_h_px < 0) body_h_px = 0;

    if (plat_font_open((const u8 *)"", 0, WP_MAC_HELP_BODY_SIZE_TWIPS, 0, &body_font) == WP_OK) {
        /* wp_help_view_paint positions text by BASELINE (plat_draw_text's
           own convention) -- ascent_px tells it where row 0's baseline
           sits below the viewport's own top edge (see help_view.h's own
           comment for the real bug this fixes if omitted). */
        plat_font_metrics body_fm;
        i32 body_ascent_px = 0;
        i32 body_line_h_px = mac_font_line_height_px(WP_MAC_HELP_BODY_SIZE_TWIPS);

        if (plat_font_metrics_get(body_font, &body_fm) == WP_OK) {
            body_ascent_px = mac_twips_to_px(body_fm.ascent);
        }
        wp_help_view_paint((plat_gc *)&gc, body_font, x_px, y_px, w_px, body_h_px,
                            mac_px_to_twips(w_px), body_line_h_px, body_ascent_px, &g_help_view);
        plat_font_close(body_font);
    }

    if (hint_bar_h_px > 0) {
        plat_font *hint_font;
        wp_vi_status_view view;
        static const u8 hint[] = "HELP -- j/k select, Enter follow, Backspace back, Esc/q close";

        if (plat_font_open((const u8 *)"", 0, WP_MAC_STATUSLINE_SIZE_TWIPS, 0, &hint_font) == WP_OK) {
            view.mode_label_utf8 = hint;
            view.mode_label_len = (u32)(sizeof(hint) - 1);
            view.pending_count = 0;
            view.pending_cmd_utf8 = NULL;
            view.pending_cmd_len = 0;
            wp_statusline_paint((plat_gc *)&gc, hint_font, x_px, y_px + total_h_px - hint_bar_h_px,
                                 w_px, hint_bar_h_px, &view);
            plat_font_close(hint_font);
        }
    }
}

static void draw_document(WindowPtr w)
{
    mac_gc gc;
    Rect r;
    short y;
    short doc_bottom;
    i32 bar_h_px;
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

    if (g_help_active) {
        paint_help_mac(w, r);
        EndUpdate(w);
        return;
    }

    if (!g_ctx.doc_loaded) {
        EndUpdate(w);
        return;
    }

    /* Reserve a bottom strip for the status line (Phase 5), the same
       way platform/atari/app_shell.c's/platform/amiga/app_shell.c's own
       redraw_window does -- the document body lays out and paints only
       within the reduced area; the strip itself is painted separately,
       below, against the FULL window rect. */
    bar_h_px = statusline_height_px();
    if (bar_h_px > (i32)(r.bottom - r.top)) bar_h_px = 0;
    doc_bottom = (short)(r.bottom - bar_h_px);

    /* Real per-window pixel width, converted through the SAME twips<->px
       ratio metrics_mac.c uses for measurement -- Phase 3's real point,
       "on-screen wrap matches engine layout," only holds if layout and
       paint agree on this number. */
    content_width_px = (i32)(r.right - r.left) - WP_DOC_LEFT_MARGIN_PX - WP_DOC_RIGHT_MARGIN_PX;
    if (content_width_px < WP_DOC_MIN_CONTENT_PX) content_width_px = WP_DOC_MIN_CONTENT_PX;
    content_width_twips = mac_px_to_twips(content_width_px);

    /* Unconditional, because the previous gate ("relayout only on a
       resize or a whole-document replace") could not see a KEYSTROKE
       edit at all: nothing in the engine's edit path (vimode.c/edit.c)
       or platform/common/keymap.c relayouts after an edit -- it only
       sets para->layout_dirty. So typed text landed in the model as a
       real wp_run byte edit while this loop kept walking a STALE
       para->layout that didn't know about the new bytes. relayout_document
       consults layout_dirty itself, so a redraw that changed nothing
       still does no measuring. platform/dos/app_shell.c already carried
       this fix; Mac, Amiga and Atari never got it. */
    relayout_document(content_width_twips);

    gc.window = w;

    y = (short)(r.top + WP_DOC_TOP_MARGIN_PX);
    for (si = 0; si < g_mac_doc.section_count && y <= doc_bottom; si++) {
        wp_section *sec = wp_document_section_get(&g_mac_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc && y <= doc_bottom; pi++) {
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

            for (li = 0; li < layout->line_count && y <= doc_bottom; li++) {
                wp_line *line = &layout->lines[li];
                i32 x_px = r.left + WP_DOC_LEFT_MARGIN_PX;
                i32 line_origin_px;
                i32 extra_twips;
                wp_line_fragment_iter iter;
                wp_line_fragment frag;

                x_px += mac_twips_to_px(
                    mac_align_offset_twips(align, content_width_twips, line->width_twips));
                line_origin_px = x_px;   /* the fragment walk below advances x_px past it */

                /* M15 T15.6: an image paragraph's single line has no run
                   bytes to fragment -- paint its real indexed-bitmap
                   content (read back from blob_path on demand, per
                   services/spell.c's own "stream from disk, don't load
                   the model whole" precedent), dithered down to this
                   backend's hard-capped 1-bit tier, and move straight to
                   the next line, skipping the fragment-walk/font-cache/
                   cursor-marker logic below entirely. Falls back to a
                   solid palette[0] rect if the cache file can't be read,
                   rather than drawing nothing at all. */
                if (para->kind == (u8)WP_PARA_IMAGE) {
                    const wp_image_desc *img = wp_document_image_get(&g_mac_doc, para->image);

                    if (img != NULL && img->palette_count > 0) {
                        i32 w_px = mac_twips_to_px(line->width_twips);
                        i32 h_px = mac_twips_to_px(line->height_twips);
                        wp_bool drew_real = WP_FALSE;

                        if (img->blob_path.length > 0 && img->blob_path.length < 256) {
                            char ipath[256];
                            const u8 *span_bytes = wp_strpool_get(&g_mac_doc.strings, img->blob_path);
                            plat_file *imgf;

                            memcpy(ipath, span_bytes, img->blob_path.length);
                            ipath[img->blob_path.length] = '\0';

                            if (plat_file_open(ipath, PLAT_FILE_READ, &imgf) == WP_OK) {
                                u32 row_bytes = ((u32)img->width_px + 1u) / 2u;
                                u32 packed_len = row_bytes * (u32)img->height_px;
                                u8 *ipixels = (u8 *)mem_alloc(packed_len);

                                if (ipixels != NULL) {
                                    u32 read_len;
                                    if (plat_file_read(imgf, ipixels, packed_len, &read_len) == WP_OK &&
                                        read_len == packed_len) {
                                        if (plat_draw_image((plat_gc *)&gc, x_px, y, w_px, h_px,
                                                             ipixels, img->width_px, img->height_px,
                                                             img->palette, img->palette_count) == WP_OK) {
                                            drew_real = WP_TRUE;
                                        }
                                    }
                                    mem_free(ipixels);
                                }
                                plat_file_close(imgf);
                            }
                        }

                        if (!drew_real) {
                            plat_fill_rect((plat_gc *)&gc, x_px, y, w_px, h_px,
                                           img->palette[0][0], img->palette[0][1], img->palette[0][2]);
                        }
                    }
                    y = (short)(y + mac_twips_to_px(line->height_twips));
                    continue;
                }

                extra_twips = wp_justify_space_extra_twips(
                    &g_mac_doc, para, line, align, (wp_bool)(li == layout->line_count - 1),
                    content_width_twips);

                wp_line_fragment_iter_init(&iter, line);
                while (wp_line_next_fragment(&g_mac_doc, para, line, &iter, &frag)) {
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
                    bytes = wp_strpool_get(&g_mac_doc.strings, run->text);
                    if (bytes == NULL) continue;

                    if (!have_font || run->style != cached_style) {
                        const wp_char_style *cs = wp_document_cstyle_get(&g_mac_doc, run->style);

                        if (cs != NULL) {
                            plat_font_metrics fm;

                            /* Clear `font` between the close and the open:
                               plat_font_open does NOT write *out when it
                               fails (WP_NOMEM), so leaving the old value in
                               place made plat_font_metrics_get read freed
                               memory and the plat_font_close after this loop
                               free the same block twice. Same fix applied to
                               all four backends. */
                            if (font != NULL) {
                                plat_font_close(font);
                                font = NULL;
                            }
                            have_font = WP_FALSE;
                            if (plat_font_open((const u8 *)"", 0, cs->size_twips,
                                                cs->flags, &font) == WP_OK &&
                                plat_font_metrics_get(font, &fm) == WP_OK) {
                                ascent_px = mac_twips_to_px(fm.ascent);
                                cached_style = run->style;
                                have_font = WP_TRUE;
                            } else {
                                font = NULL;
                            }
                        }
                    }

                    if (frag_len == 0 || font == NULL) continue;

                    plat_measure_text(font, bytes + frag.byte_start, frag_len, &frag_width_twips);

                    if (!frag.is_space) {
                        g_mac_painted_frags++;
                        plat_draw_text((plat_gc *)&gc, font, x_px, y + ascent_px,
                                        bytes + frag.byte_start, frag_len, 0, 0, 0);
                    }

                    x_px += mac_twips_to_px(frag_width_twips);
                    if (frag.is_space) x_px += mac_twips_to_px(extra_twips);
                }

                /* The caret x used to be a hardcoded r.left + 2: the guard
                   picked the right LINE, but nothing ever derived a column
                   from the cursor's run/offset, so the marker sat at the
                   left margin no matter where the cursor really was. That
                   was the only caret feedback this backend has, and it
                   read as a broken feature rather than a misplaced marker
                   -- searching "/quit" moved the cursor correctly but drew
                   the caret at the start of the paragraph while the match
                   was near the end of its second line, which looked like
                   the search itself had failed. wp_line_cursor_x_twips
                   does the fragment walk (shared, host-tested, and
                   justify-aware, so a JUSTIFY line's stretched gaps count
                   the same way this loop's own extra_twips advance does). */
                if (is_cursor_para && li == cursor_line_index) {
                    Rect mark;
                    i32 caret_px = line_origin_px + mac_twips_to_px(
                        wp_line_cursor_x_twips(&g_mac_doc, para, line,
                                                wp_keymap_cursor(&g_ctx)->run,
                                                wp_keymap_cursor(&g_ctx)->offset,
                                                extra_twips));

                    /* y is the line's TOP -- the draw loop above derives
                       its baseline as y + ascent_px. The old rect spanned
                       (y - ascent_px)..(y + 2), which is the correct
                       shape measured from a BASELINE, so the caret was
                       painted one full ascent above the line it marks:
                       visible in this fix's own before/after screenshots
                       as a bar floating in the gap above the text.
                       Same shape, re-anchored to the line top. */
                    SetRect(&mark, (short)caret_px, (short)y,
                                    (short)(caret_px + 3), (short)(y + ascent_px + 2));
                    PaintRect(&mark);
                }

                y = (short)(y + mac_twips_to_px(line->height_twips));
            }
        }
    }

    if (font != NULL) plat_font_close(font);

    paint_status_line_mac(w, r, bar_h_px);

    EndUpdate(w);
}

/* -- Window/menu construction + event loop ---------------------------
   No .r resource file (see this section's own top comment) -- the
   window is a plain NewWindow, matching Retro68's own WDEF sample's
   MakeNewWindow (Samples/WDEF/wdefshell.c), which builds a real
   document window the identical resource-free way. */

/* ------------------------------------------------------------------
   CI hook (T169) -- identical contract to platform/atari/app_shell.c's
   and platform/dos/app_shell.c's own pairs; see the Atari file for why
   the marker carries a heartbeat COUNT plus a painted-fragment count
   rather than being a one-shot "alive" file.

   "Unix:<name>" resolves to the ExtFS share regardless of vRefNum (see
   platform/mac/file_mac.c's own comment, confirmed empirically during
   M12 Phase 0), which is a real host directory -- so both markers are
   read straight off the host filesystem, the same channel
   host/tools/mac/smoke.c's own mac_smoke.out already uses.

   Inert unless the trigger file exists. */
#define WP_MAC_CI_TRIGGER  "Unix:RETROWP.CI"
#define WP_MAC_CI_ALIVE    "Unix:RETROWP.ALV"

static wp_bool mac_ci_mode(void)
{
    plat_file *f;

    if (plat_file_open(WP_MAC_CI_TRIGGER, PLAT_FILE_READ, &f) != WP_OK) return WP_FALSE;
    plat_file_close(f);
    return WP_TRUE;
}

static u32 mac_ci_decimal(char *out, u32 cap, u32 v)
{
    char rev[12];
    u32 rlen = 0, len = 0, i;

    if (v == 0) { if (cap > 0) out[len++] = '0'; return len; }
    while (v > 0 && rlen < (u32)sizeof(rev)) { rev[rlen++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < rlen && len < cap; i++) out[len++] = rev[rlen - 1 - i];
    return len;
}

/* Marker payload is "<events> <painted_fragments>\n". */
static void mac_ci_write_alive(void)
{
    char buf[16];
    u32 len;
    plat_file *f;

    if (plat_file_open(WP_MAC_CI_ALIVE, PLAT_FILE_WRITE, &f) != WP_OK) return;
    len = mac_ci_decimal(buf, (u32)sizeof(buf), g_mac_ci_events);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)" ", 1);
    len = mac_ci_decimal(buf, (u32)sizeof(buf), g_mac_painted_frags);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)"\n", 1);
    plat_file_close(f);
}

void app_shell_run(void)
{
    Rect winRect;
    MenuHandle appleMenu;
    EventRecord e;
    wp_config cfg;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    /* Phase 5: config-driven keymap selection (docs/keymaps.md) --
       WP_MAC_CONFIG_PATH missing or unreadable leaves cfg.keymap_id at
       its documented default (WP_KEYMAP_VI), matching every other
       backend's own zero-config behavior exactly. */
    wp_config_load(WP_MAC_CONFIG_PATH, &cfg);
    wp_app_ctx_init(&g_ctx, (wp_keymap_id)cfg.keymap_id, &g_mac_doc, &g_mac_journal);
    load_sample_document();

    /* Phase 5: parsed ONCE at startup from the generated
       g_help_source_retrowp array (build/generated/help_retrowp.h,
       shared verbatim with every other backend -- produced from
       docs/help/retrowp.help by host/tools/gen_help_header.c, see
       docs/help.md). If parsing ever fails, g_help_doc_loaded stays
       WP_FALSE and F1/Help are a silent no-op rather than a crash
       (open_help_node's own guard). */
    g_help_doc_loaded = (wp_bool)(wp_help_doc_parse(g_help_source_retrowp,
                                                     (u32)g_help_source_retrowp_len,
                                                     &g_help_doc) == WP_OK);

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

    /* Written after the menu bar is drawn and the window is open, so
       its existence really does mean "the app came up". */
    g_mac_ci_active = mac_ci_mode();
    if (g_mac_ci_active) mac_ci_write_alive();

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

        /* After dispatch, never before: a dispatch that crashes or
           wedges must not get to bump the count first. WaitNextEvent
           returns on a null event too, so this counts real events only. */
        if (g_mac_ci_active && e.what != nullEvent) {
            g_mac_ci_events++;
            mac_ci_write_alive();
        }
    }

    if (g_ctx.doc_loaded) {
        wp_keymap_free(&g_ctx);
        wp_document_free(&g_mac_doc);
    }
    if (g_help_doc_loaded) wp_help_doc_free(&g_help_doc);
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

    /* T13.2: mirrors wp_atari_layout_selfcheck's/wp_amiga_layout_
       selfcheck's own new JUSTIFY check -- can't hand-compute exact
       widths against a real font on real hardware, so this only checks
       what every real backend's paint loop actually depends on:
       interior lines' slack is never negative, and the paragraph's
       last line always stays exactly 0. */
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

/* Phase 5: proves wp_config_load resolves all five real keymap= values
   against a REAL file at WP_MAC_CONFIG_PATH, written and read through
   real File Manager I/O -- mirrors platform/atari/app_shell.c's/
   platform/amiga/app_shell.c's own wp_atari_config_selfcheck/
   wp_amiga_config_selfcheck exactly (host/tests/test_config.c already
   proves the string-matching logic itself; this proves the real
   on-target file round trip). Leaves the fixture exactly as clean as
   it found it. */
wp_bool wp_mac_config_selfcheck(void)
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

        if (plat_file_open(WP_MAC_CONFIG_PATH, PLAT_FILE_WRITE, &f) != WP_OK) { ok = WP_FALSE; continue; }
        if (plat_file_write(f, (const u8 *)cases[i].content, (u32)strlen(cases[i].content)) != WP_OK) ok = WP_FALSE;
        plat_file_close(f);

        wp_config_load(WP_MAC_CONFIG_PATH, &cfg);
        if (cfg.keymap_id != cases[i].expected) ok = WP_FALSE;
    }

    plat_file_delete(WP_MAC_CONFIG_PATH);
    return ok;
}

/* Menu construction, checked without a real menu bar.
   ------------------------------------------------------------------
   The Mac menu bugs were all decidable from the table plus this file's
   own two helper rules, no Menu Manager needed:

     - no item ever got a command-key equivalent, so MenuKey() returned
       0 while the cmdKey branch had already swallowed the keystroke;
     - labels went straight into a call that PARSES metacharacters,
       while do_menu_command maps a chosen item back to an action by
       counting ordinal position;
     - menu IDs are derived by arithmetic on a group enum, which shares
       its base with the Apple menu.

   All three are checked here. */
wp_bool wp_mac_menu_selfcheck(void)
{
    static const wp_menu_group groups[] = {
        WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
    };
    const u32 group_count = (u32)(sizeof(groups) / sizeof(groups[0]));
    unsigned char pbuf[256];
    u32 i, j;

    if (!wp_menu_model_selfcheck()) return WP_FALSE;

    /* Menu IDs must be distinct from each other AND from the Apple menu.
       do_menu_command already special-cases menuID == WP_MENU_APPLE_ID
       and returns early, so a group whose derived ID collided with it
       would have every one of its items silently swallowed. The
       arithmetic is WP_MENU_APPLE_ID + group, and WP_MENU_NONE is 0 --
       so this holds only because no built group is WP_MENU_NONE, which
       is exactly the kind of thing worth pinning rather than assuming. */
    for (i = 0; i < group_count; i++) {
        short id_i = (short)(WP_MENU_APPLE_ID + (int)groups[i]);

        if (id_i == WP_MENU_APPLE_ID) return WP_FALSE;
        for (j = 0; j < group_count; j++) {
            if (j == i) continue;
            if (id_i == (short)(WP_MENU_APPLE_ID + (int)groups[j])) return WP_FALSE;
        }
        if (group_title(groups[i]) == (const char *)0) return WP_FALSE;
    }

    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        const wp_action *act = &g_wp_actions[i];
        u32 n;

        if (act->group == WP_MENU_NONE) continue;

        /* Every menu-bound action's group must map back to exactly one
           built menu -- do_menu_command finds an action by re-deriving
           WP_MENU_APPLE_ID + act->group and comparing to the picked
           menuID, so an action in an unbuilt group is unreachable. */
        {
            wp_bool built = WP_FALSE;

            for (j = 0; j < group_count; j++) {
                if (act->group == groups[j]) built = WP_TRUE;
            }
            if (!built) return WP_FALSE;
        }

        /* Labels must survive Pascal conversion intact: c_to_pstr caps
           at 255 and the buffers here are 256, so a longer label would
           be silently truncated into a different menu entry. */
        c_to_pstr(act->label_utf8, pbuf);
        n = (u32)pbuf[0];
        if (n == 0) return WP_FALSE;
        if (n > 255) return WP_FALSE;
        for (j = 0; j < n; j++) {
            if (pbuf[j + 1] != (unsigned char)act->label_utf8[j]) return WP_FALSE;
        }
        if (act->label_utf8[n] != '\0') return WP_FALSE; /* truncated */

        /* And must contain nothing AppendMenu/InsertMenuItem would parse.
           This backend deliberately avoids that parsing (placeholder +
           SetMenuItemText), so this is a tripwire on the labels rather
           than on the call -- it protects the next backend, and any
           future label edit, from reintroducing the ordinal drift. */
        if (!wp_menu_label_is_literal_safe(act->label_utf8)) return WP_FALSE;
    }

    /* The command-key letter must round-trip: MenuKey() hands back the
       letter the user typed, and do_menu_command resolves it by ordinal,
       so a letter that does not match what SetItemCmd was given makes
       the accelerator dead in exactly the way it already was once. */
    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        u8 accel = wp_menu_accel_letter(g_wp_actions[i].shortcut_ascii);

        if (accel == 0) continue;
        if (accel < 'A' || accel > 'Z') return WP_FALSE;
        if (wp_action_find_by_shortcut(g_wp_actions[i].shortcut_ascii) != &g_wp_actions[i]) {
            return WP_FALSE;
        }
    }

    return WP_TRUE;
}

wp_bool wp_mac_image_selfcheck(void)
{
    offload_capabilities caps;
    wp_bool ok = WP_TRUE;
    wp_document doc;
    u32 sec_idx, para_idx;

    memset(&caps, 0, sizeof(caps));
    caps.ops_supported = (1uL << OFFLOAD_IMAGE);

    if (!offload_caps_supports(&caps, OFFLOAD_IMAGE)) ok = WP_FALSE;
    if (offload_caps_supports(&caps, OFFLOAD_CHAT)) ok = WP_FALSE;

    /* M15 Phase 5: position/size assertion -- an image paragraph must
       relayout to EXACTLY its descriptor's display_width_twips/
       display_height_twips, the same proof wp_mac_layout_selfcheck
       already gives text paragraphs, now extended to WP_PARA_IMAGE.
       Pure engine-side (linebreak.c), no window needed, so this runs
       safely from the headless mac_smoke binary. */
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
       on degenerate input -- no real window exists in this headless
       smoke binary, so a zeroed mac_gc (window == NULL) is the only gc
       available; every case below must be turned away by
       plat_draw_image's own guard clauses before ever touching
       mgc->window (SetPort on a NULL WindowPtr would crash for real).
       Doesn't assert exact pixel content -- same "real hardware, not
       hand-computed" allowance T13.2's own JUSTIFY selfcheck already
       established -- only that these calls return safely. */
    {
        mac_gc bad_gc;
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

/* Phase 5: proves the help system's non-blocking pieces against the
   REAL compiled-in help content and REAL QuickDraw font metrics,
   mirroring platform/atari/app_shell.c's/platform/amiga/app_shell.c's
   own wp_atari_help_selfcheck/wp_amiga_help_selfcheck exactly (same
   fixture text, same two assertions). Deliberately does NOT call
   wp_help_view_paint or actually open the pager (F1) -- same no-real-
   window/no-blocking-call boundary every other selfcheck in this file
   respects. What IS real and worth proving here: the parser accepts
   the shipped content and the "Main" node resolves, and
   wp_help_wrap_line -- the greedy word-wrap algorithm the pager relies
   on -- runs correctly against a REAL QuickDraw-measured font rather
   than host's simplified fake-monospace one. */
#define WP_MAC_HELP_SELFCHECK_MAX_ROWS 8

wp_bool wp_mac_help_selfcheck(void)
{
    wp_help_doc doc;
    plat_font *font;
    u32 starts[WP_MAC_HELP_SELFCHECK_MAX_ROWS];
    u32 lens[WP_MAC_HELP_SELFCHECK_MAX_ROWS];
    u32 row_count;
    static const char sample[] =
        "RetroWP wraps this sentence across more than one real Mac QuickDraw-measured row.";

    if (wp_help_doc_parse(g_help_source_retrowp, (u32)g_help_source_retrowp_len, &doc) != WP_OK) {
        return WP_FALSE;
    }
    if (doc.node_count == 0 ||
        wp_help_doc_find_node(&doc, (const u8 *)"Main", 4) == WP_HELP_NODE_NONE) {
        wp_help_doc_free(&doc);
        return WP_FALSE;
    }
    wp_help_doc_free(&doc);

    if (plat_font_open((const u8 *)"", 0, WP_MAC_HELP_BODY_SIZE_TWIPS, 0, &font) != WP_OK) {
        return WP_FALSE;
    }
    /* A narrow column forces real wrapping (not just a single row) --
       proves more than "it returned something" against real
       measurement. */
    row_count = wp_help_wrap_line(font, (const u8 *)sample, (u32)strlen(sample), 3000,
                                   starts, lens, WP_MAC_HELP_SELFCHECK_MAX_ROWS);
    plat_font_close(font);

    if (row_count < 2 || row_count > WP_MAC_HELP_SELFCHECK_MAX_ROWS) return WP_FALSE;
    {
        u32 i;
        for (i = 0; i < row_count; i++) {
            if (lens[i] == 0) return WP_FALSE; /* every row must carry real content */
        }
    }
    return WP_TRUE;
}
