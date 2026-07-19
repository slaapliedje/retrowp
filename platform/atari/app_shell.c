#include <gem.h>
#include <osbind.h>
#include <string.h>
#include "types.h"
#include "platform.h"
#include "docmodel.h"
#include "format_txt.h"
#include "layout.h"
#include "editing.h"
#include "app_shell.h"
#include "app_shell_internal.h"
#include "metrics_atari_internal.h"
#include "mem_atari.h"

/* Hand-built menu bar OBJECT tree: File (Open/Save/Quit) and Edit
   (Cut/Copy/Paste). There is no resource editor or .rsc file anywhere
   in this project's toolchain (no ResourceMaster/Human68k-style tool,
   no packaged .rsc -- see docs/atari-platform.md), so for a menu bar
   this small, a static hand-built OBJECT[] is the practical choice;
   .rsc loading (rsrc_load) remains the better tool for anything with
   real dialog layout (multi-field forms), which this project doesn't
   need yet.

   ob_next/ob_head/ob_tail are OBJECT-ARRAY INDICES, not pointers.
   Convention (verified against a running AES under Hatari, and cross-
   checked against EmuTOS's own desktop menu tree, since this is the
   single easiest thing to get silently wrong from memory):
     - ob_head/ob_tail on a parent bound its child sibling chain
       (first/last child index).
     - ob_next on a non-last sibling is the next sibling's index.
     - ob_next on the LAST sibling in a chain wraps back to its OWN
       PARENT's index (there is no -1 terminator inside a chain).
     - Only the true root (no parent) uses ob_next = -1.
   menu_bar() does NOT compute ob_x/ob_y/ob_width/ob_height from the
   string content -- those must be set by hand, in character-cell units
   relative to the parent (screen-relative only for the root). */
OBJECT wp_menu_tree[WP_MENU_OBJ_COUNT] = {
    /* next,                 head,                  tail,                  type,     flags,     state,     spec, x, y, w, h */
    { -1,                     WP_MENU_OBJ_FILE_TITLE, WP_MENU_OBJ_EDIT_TITLE, G_IBOX,   OF_NONE,   OS_NORMAL, {0}, 0, 0, 40, 1 },
    { WP_MENU_OBJ_EDIT_TITLE, WP_MENU_OBJ_FILE_BOX,   WP_MENU_OBJ_FILE_BOX,   G_TITLE,  OF_NONE,   OS_NORMAL, {0}, 0, 0, 6,  1 },
    { WP_MENU_OBJ_ROOT,       WP_MENU_OBJ_EDIT_BOX,   WP_MENU_OBJ_EDIT_BOX,   G_TITLE,  OF_NONE,   OS_NORMAL, {0}, 6, 0, 6,  1 },
    { WP_MENU_OBJ_FILE_TITLE, WP_MENU_OBJ_FILE_OPEN,  WP_MENU_OBJ_FILE_QUIT,  G_BOX,    OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  3 },
    { WP_MENU_OBJ_FILE_SAVE,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 0, 8,  1 },
    { WP_MENU_OBJ_FILE_QUIT,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  1 },
    { WP_MENU_OBJ_FILE_BOX,   -1,                     -1,                    G_STRING, OF_LASTOB, OS_NORMAL, {0}, 0, 2, 8,  1 },
    { WP_MENU_OBJ_EDIT_TITLE, WP_MENU_OBJ_EDIT_CUT,   WP_MENU_OBJ_EDIT_PASTE, G_BOX,    OF_NONE,   OS_NORMAL, {0}, 6, 1, 8,  3 },
    { WP_MENU_OBJ_EDIT_COPY,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 0, 8,  1 },
    { WP_MENU_OBJ_EDIT_PASTE, -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  1 },
    { WP_MENU_OBJ_EDIT_BOX,   -1,                     -1,                    G_STRING, OF_LASTOB, OS_NORMAL, {0}, 0, 2, 8,  1 }
};

static char g_text_file[]  = " File ";
static char g_text_edit[]  = " Edit ";
static char g_text_open[]  = "  Open";
static char g_text_save[]  = "  Save";
static char g_text_quit[]  = "  Quit";
static char g_text_cut[]   = "  Cut";
static char g_text_copy[]  = "  Copy";
static char g_text_paste[] = "  Paste";

void wp_menu_tree_init(void)
{
    wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_spec.free_string = g_text_file;
    wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_spec.free_string = g_text_edit;
    wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_spec.free_string = g_text_open;
    wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_spec.free_string = g_text_save;
    wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string = g_text_quit;
    wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_spec.free_string = g_text_cut;
    wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_spec.free_string = g_text_copy;
    wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string = g_text_paste;
}

wp_bool wp_menu_tree_selfcheck(void)
{
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_next != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_head != WP_MENU_OBJ_FILE_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_tail != WP_MENU_OBJ_EDIT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_type != G_IBOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_next != WP_MENU_OBJ_EDIT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_head != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_tail != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_type != G_TITLE) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_next != WP_MENU_OBJ_ROOT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_head != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_tail != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_type != G_TITLE) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_next != WP_MENU_OBJ_FILE_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_head != WP_MENU_OBJ_FILE_OPEN) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_tail != WP_MENU_OBJ_FILE_QUIT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_type != G_BOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_next != WP_MENU_OBJ_FILE_SAVE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_next != WP_MENU_OBJ_FILE_QUIT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_next != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_flags & OF_LASTOB) == 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_next != WP_MENU_OBJ_EDIT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_head != WP_MENU_OBJ_EDIT_CUT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_tail != WP_MENU_OBJ_EDIT_PASTE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_type != G_BOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_next != WP_MENU_OBJ_EDIT_COPY) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_next != WP_MENU_OBJ_EDIT_PASTE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_next != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_flags & OF_LASTOB) == 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_spec.free_string != g_text_file) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_spec.free_string != g_text_edit) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string != g_text_quit) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string != g_text_paste) return WP_FALSE;

    return WP_TRUE;
}

/* T4.2: a single hardcoded sample document, imported and laid out once
   at startup (and relaid-out on resize), then painted on every
   WM_REDRAW through the platform.h VDI metrics/draw seam. Kept as file-
   scope statics rather than threaded through wp_dispatch_message's
   signature so the smoke test (host/tools/atari_smoke.c) -- which never
   loads a document -- keeps working unchanged: g_doc_loaded stays
   WP_FALSE there, and paint_document() is a no-op in that case. */
static wp_document g_doc;
static wp_bool g_doc_loaded = WP_FALSE;
static wp_undo_journal g_journal;
static wp_pos g_cursor;
static i32 g_content_width_twips = 0;

static const char g_sample_text[] =
    "This is the Portable Word Processor Engine running on a real "
    "Atari ST under Hatari. Word wrap and on-screen rendering are "
    "driven entirely through the platform.h VDI metrics seam -- the "
    "engine itself has never heard of GEM.\n";

static void relayout_document(i32 content_width_twips)
{
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);
        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_relayout(&g_doc, para, content_width_twips);
        }
    }
}

/* Replaces the currently-open document (if any) with a freshly imported
   one, resetting the cursor to its end and the undo journal to empty --
   used at startup, and by Open/Paste (T4.3), which both have the same
   "load this text as the whole document" shape. */
static void set_document_from_text(const u8 *utf8, u32 len, i32 content_width_twips)
{
    wp_document new_doc;

    if (format_txt_import(utf8, len, &new_doc) != WP_OK) return;

    if (g_doc_loaded) {
        wp_undo_journal_free(&g_journal);
        wp_document_free(&g_doc);
    }

    g_doc = new_doc;
    g_doc_loaded = WP_TRUE;
    wp_undo_journal_init(&g_journal);
    relayout_document(content_width_twips);
    g_cursor = wp_pos_doc_end(&g_doc);
}

static void load_sample_document(i32 content_width_twips)
{
    set_document_from_text((const u8 *)g_sample_text, (u32)strlen(g_sample_text),
                            content_width_twips);
}

/* Paints every laid-out line of every paragraph, top to bottom, from
   the window's work-area origin. Called once per damaged rectangle in
   redraw_window() -- correctness (not re-painting the whole document
   needlessly) relies on VDI clipping (vs_clip, already set by the
   caller) to crop each call, not on this function tracking rectangles
   itself. A line whose top has scrolled past the work area's bottom
   stops the paragraph's remaining lines -- there is no scrolling yet
   (T4.3+), so anything past the first screenful is simply not drawn,
   not wrapped around or overwritten. */
static void paint_document(short window_handle, VdiHdl vdi, GRECT area)
{
    atari_gc gc;
    u32 si, pi, li;
    i32 y_px = area.g_y;
    plat_font *font = NULL;
    wp_cstyle cached_style = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent_px = 0;

    if (!g_doc_loaded) return;

    gc.vdi = vdi;
    gc.window_handle = window_handle;

    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

            if (layout == NULL) continue;

            for (li = 0; li < layout->line_count; li++) {
                wp_line *line = &layout->lines[li];
                char buf[512];
                u32 buf_len = 0;
                u32 r;
                i32 line_height_px;
                wp_cstyle line_style = g_doc.default_cstyle;

                if (y_px > area.g_y + area.g_h) break;

                for (r = line->start_run; r <= line->end_run && r < para->run_count; r++) {
                    wp_run *run = wp_paragraph_run_get(para, (u16)r);
                    const u8 *bytes = wp_strpool_get(&g_doc.strings, run->text);
                    u32 seg_start = (r == line->start_run) ? line->start_offset : 0;
                    u32 seg_end = (r == line->end_run) ? line->end_offset : run->text.length;
                    u32 k;

                    line_style = run->style;
                    for (k = seg_start; k < seg_end && buf_len < sizeof(buf) - 1; k++) {
                        buf[buf_len++] = (char)bytes[k];
                    }
                }

                if (!have_font || line_style != cached_style) {
                    const wp_char_style *cs = wp_document_cstyle_get(&g_doc, line_style);
                    plat_font_metrics fm;

                    if (cs != NULL) {
                        if (font != NULL) plat_font_close(font);
                        plat_font_open((const u8 *)"", 0, cs->size_twips, cs->flags, &font);
                        plat_font_metrics_get(font, &fm);
                        ascent_px = atari_twips_to_px(fm.ascent);
                        cached_style = line_style;
                        have_font = WP_TRUE;
                    }
                }

                line_height_px = atari_twips_to_px(line->height_twips);
                if (buf_len > 0 && font != NULL) {
                    plat_draw_text((plat_gc *)&gc, font, area.g_x, y_px + ascent_px,
                                    (const u8 *)buf, buf_len, 0, 0, 0);
                }
                y_px += line_height_px;
            }
        }
    }

    if (font != NULL) plat_font_close(font);
}

/* Inserting is always a point op regardless of where the cursor sits
   (docs/editing.md), so typing never needs special-casing. */
static void do_insert_char(u8 ch)
{
    wp_pos out_pos;

    if (!g_doc_loaded) return;
    if (wp_edit_insert_text(&g_doc, &g_journal, g_cursor, &ch, 1, &out_pos) == WP_OK) {
        g_cursor = out_pos;
        relayout_document(g_content_width_twips);
    }
}

/* Deleting the character before the cursor is always a SINGLE-run
   delete by construction: wp_pos_prev() lands either at an earlier
   offset within the cursor's own run, or (crossing a run boundary) at
   the end of the PREVIOUS run -- either way the deleted codepoint is
   entirely inside prev's run, matching wp_edit_delete_range's single-
   run-only contract (docs/editing.md) without this code needing to
   loop across runs itself. */
static void do_backspace(void)
{
    wp_pos prev;
    u32 end_offset;

    if (!g_doc_loaded) return;

    prev = wp_pos_prev(&g_doc, g_cursor);
    if (prev.section == g_cursor.section && prev.paragraph == g_cursor.paragraph &&
        prev.run == g_cursor.run && prev.offset == g_cursor.offset) {
        return; /* already at document start -- wp_pos_prev clamped */
    }

    if (prev.run == g_cursor.run && prev.paragraph == g_cursor.paragraph &&
        prev.section == g_cursor.section) {
        end_offset = g_cursor.offset;
    } else {
        wp_section *sec = wp_document_section_get(&g_doc, prev.section);
        wp_paragraph *para = wp_section_paragraph_get(sec, prev.paragraph);
        wp_run *run = wp_paragraph_run_get(para, (u16)prev.run);
        end_offset = run->text.length;
    }

    if (wp_edit_delete_range(&g_doc, &g_journal, prev.section, prev.paragraph,
                              (u16)prev.run, prev.offset, end_offset) == WP_OK) {
        g_cursor = prev;
        relayout_document(g_content_width_twips);
    }
}

/* fsel_input() rewrites `path`'s directory portion in place but keeps
   the trailing wildcard pattern (e.g. "C:\*.TXT" stays as typed unless
   the user navigates) -- truncate at the last backslash to recover just
   the directory, then append the chosen filename ourselves. Classic GEM
   convention: `path` needs a real wildcard pattern on entry, `fname`
   holds the selected 8.3 filename on a successful return. */
static wp_bool run_file_selector(char *path_inout, char *fname_inout, char *full_out, u32 full_cap)
{
    short button;
    int i, cut = -1;

    if (fsel_input(path_inout, fname_inout, &button) == 0) return WP_FALSE;
    if (button != FSEL_OK || fname_inout[0] == '\0') return WP_FALSE;

    for (i = 0; path_inout[i] != '\0'; i++) {
        if (path_inout[i] == '\\') cut = i;
    }
    if (cut < 0) return WP_FALSE;
    path_inout[cut + 1] = '\0';

    if (strlen(path_inout) + strlen(fname_inout) + 1 > full_cap) return WP_FALSE;
    strcpy(full_out, path_inout);
    strcat(full_out, fname_inout);
    return WP_TRUE;
}

static void do_file_open(void)
{
    char path[128];
    char fname[16];
    char fullpath[144];
    plat_file *f;
    u32 size, read_len;
    u8 *buf;

    strcpy(path, "C:\\*.TXT");
    fname[0] = '\0';
    if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK) { plat_file_close(f); return; }

    buf = (u8 *)mem_alloc(size + 1);
    if (buf != NULL) {
        if (plat_file_read(f, buf, size, &read_len) == WP_OK) {
            set_document_from_text(buf, read_len, g_content_width_twips);
        }
        mem_free(buf);
    }
    plat_file_close(f);
}

static void do_file_save(void)
{
    char path[128];
    char fname[16];
    char fullpath[144];
    plat_file *f;
    u32 needed;
    u8 *buf;

    if (!g_doc_loaded) return;

    strcpy(path, "C:\\*.TXT");
    strcpy(fname, "UNTITLED.TXT");
    if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;

    format_txt_export(&g_doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;

    if (format_txt_export(&g_doc, buf, needed, &needed) == WP_OK) {
        if (plat_file_open(fullpath, PLAT_FILE_WRITE, &f) == WP_OK) {
            plat_file_write(f, buf, needed);
            plat_file_close(f);
        }
    }
    mem_free(buf);
}

/* Cut/Copy/Paste operate on the WHOLE document, not a selection: there
   is no selection model yet (docs/editing.md notes the same gap for
   multi-run deletes), and inventing one is out of scope for wiring the
   platform.h clipboard seam. Copy exports the document as plaintext to
   the clipboard; Cut does that and then clears the document; Paste
   replaces the document with the clipboard's content -- the same
   "load this text as the whole document" path Open and startup use. */
static void do_edit_copy(void)
{
    u32 needed;
    u8 *buf;

    if (!g_doc_loaded) return;

    format_txt_export(&g_doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;

    if (format_txt_export(&g_doc, buf, needed, &needed) == WP_OK) {
        plat_clipboard_set_text(buf, needed);
    }
    mem_free(buf);
}

static void do_edit_cut(void)
{
    static const char empty_text[] = "\n";

    if (!g_doc_loaded) return;
    do_edit_copy();
    set_document_from_text((const u8 *)empty_text, (u32)strlen(empty_text), g_content_width_twips);
}

static void do_edit_paste(void)
{
    u8 buf[4096];
    u32 out_len;

    if (!g_doc_loaded) return;
    if (plat_clipboard_get_text(buf, (u32)sizeof(buf), &out_len) != WP_OK) return;
    set_document_from_text(buf, out_len, g_content_width_twips);
}

wp_bool wp_atari_layout_selfcheck(void)
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

wp_bool wp_atari_file_clipboard_selfcheck(void)
{
    static const char fixture[] = "atari file+clipboard round-trip fixture";
    static const char scratch_path[] = "C:\\WPTEST.TXT";
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

wp_bool wp_atari_memory_budget_selfcheck(long *free_before_out,
                                          long *free_after_load_out,
                                          long *free_after_free_out)
{
    static const char para_text[] =
        "The quick brown fox jumps over the lazy dog while the portable "
        "word processor engine proves it can handle real documents.\n";
    const u32 para_len = (u32)strlen(para_text);
    const u32 para_count = 60; /* ~7.5KB of source text -- comfortably
                                   multi-page at a real page size, and
                                   comfortably small next to 1MB. */
    u8 *big_text;
    u32 big_len, i;
    wp_document doc;
    wp_undo_journal journal;
    wp_section *sec;
    wp_pagination pagination;
    wp_pos pos, out_pos;
    u32 export_needed;
    u8 *export_buf;
    plat_file *f;
    static const char scratch_path[] = "C:\\WPBUDGET.TXT";
    wp_bool ok = WP_TRUE;
    long free_before, free_after_load, free_after_free;

    free_before = Malloc(-1L);
    if (free_before_out) *free_before_out = free_before;
    mem_atari_reset_counters();

    big_len = para_len * para_count;
    big_text = (u8 *)mem_alloc(big_len);
    if (big_text == NULL) return WP_FALSE;
    for (i = 0; i < para_count; i++) {
        memcpy(big_text + i * para_len, para_text, para_len);
    }

    if (format_txt_import(big_text, big_len, &doc) != WP_OK) {
        mem_free(big_text);
        return WP_FALSE;
    }
    mem_free(big_text);
    wp_undo_journal_init(&journal);

    /* Real US Letter page dimensions (1in margins), not the on-screen
       window's ad hoc width -- the first exercise of
       wp_document_paginate() on this target; T4.2/T4.3 only ever called
       wp_paragraph_relayout() directly for the single-window flow. */
    sec = wp_document_section_get(&doc, 0);
    sec->page_width = 12240;   /* 8.5in */
    sec->page_height = 15840;  /* 11in */
    sec->margin_left = 1440;   /* 1in */
    sec->margin_right = 1440;
    sec->margin_top = 1440;
    sec->margin_bottom = 1440;

    if (wp_document_paginate(&doc, &pagination) == WP_OK) {
        if (pagination.page_count < 2) ok = WP_FALSE;
        wp_pagination_free(&pagination);
    } else {
        ok = WP_FALSE;
    }

    free_after_load = Malloc(-1L);
    if (free_after_load_out) *free_after_load_out = free_after_load;
    if (free_after_load < 8192L) ok = WP_FALSE; /* comfortable headroom, not just "didn't crash" */

    /* A handful of edits at this document's real size, proving the
       editing engine doesn't break down beyond the tiny fixtures the
       earlier smoke checks use. */
    pos = wp_pos_doc_end(&doc);
    for (i = 0; i < 20; i++) {
        if (wp_edit_insert_text(&doc, &journal, pos, (const u8 *)"x", 1, &out_pos) != WP_OK) {
            ok = WP_FALSE;
            break;
        }
        pos = out_pos;
    }
    for (i = 0; i < 10; i++) {
        wp_pos prev = wp_pos_prev(&doc, pos);
        if (prev.section == pos.section && prev.paragraph == pos.paragraph && prev.run == pos.run) {
            if (wp_edit_delete_range(&doc, &journal, prev.section, prev.paragraph,
                                      (u16)prev.run, prev.offset, pos.offset) != WP_OK) {
                ok = WP_FALSE;
                break;
            }
        }
        pos = prev;
    }

    /* Save: exercises plat_file_write at this document's real size. */
    format_txt_export(&doc, NULL, 0, &export_needed);
    export_buf = (u8 *)mem_alloc(export_needed);
    if (export_buf == NULL) {
        ok = WP_FALSE;
    } else {
        if (format_txt_export(&doc, export_buf, export_needed, &export_needed) == WP_OK) {
            if (plat_file_open(scratch_path, PLAT_FILE_WRITE, &f) == WP_OK) {
                if (plat_file_write(f, export_buf, export_needed) != WP_OK) ok = WP_FALSE;
                plat_file_close(f);
            } else {
                ok = WP_FALSE;
            }
        } else {
            ok = WP_FALSE;
        }
        mem_free(export_buf);
    }
    plat_file_delete(scratch_path);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    free_after_free = Malloc(-1L);
    if (free_after_free_out) *free_after_free_out = free_after_free;

    /* The real leak signal: mintlib's malloc() claims a heap pool from
       GEMDOS once at startup and manages it internally, so Malloc(-1L)
       does NOT move for allocation traffic this small -- confirmed
       empirically during development (identical readings at all three
       checkpoints even before this counter existed). mem_atari_live_count()
       counts actual mem_alloc/mem_free calls directly, mirroring
       platform/host/mem_host.c's leak-counter pattern the host test
       suite already relies on. */
    if (mem_atari_live_count() != 0) ok = WP_FALSE;

    return ok;
}

static void redraw_window(short window_handle, VdiHdl vdi)
{
    GRECT r, work;
    short pxy[4];

    wind_get_grect(window_handle, WF_WORKXYWH, &work);

    wind_get_grect(window_handle, WF_FIRSTXYWH, &r);
    while (r.g_w > 0 && r.g_h > 0) {
        pxy[0] = r.g_x;
        pxy[1] = r.g_y;
        pxy[2] = (short)(r.g_x + r.g_w - 1);
        pxy[3] = (short)(r.g_y + r.g_h - 1);
        vs_clip(vdi, 1, pxy);
        vsf_interior(vdi, FIS_SOLID);
        vsf_color(vdi, G_WHITE);
        v_bar(vdi, pxy);
        paint_document(window_handle, vdi, work);
        wind_get_grect(window_handle, WF_NEXTXYWH, &r);
    }
    vs_clip(vdi, 0, pxy);
}

void wp_dispatch_key(short window_handle, VdiHdl vdi, short key)
{
    u8 ascii = (u8)(key & 0xFF);

    if (!g_doc_loaded) return;

    if (ascii == 0x08) {
        do_backspace();
    } else if (ascii >= 0x20 && ascii < 0x7F) {
        do_insert_char(ascii);
    } else {
        return; /* Enter, arrows, function keys, etc. -- no-op; see
                    app_shell_internal.h for why Enter isn't handled */
    }

    redraw_window(window_handle, vdi);
}

wp_bool wp_dispatch_message(short window_handle, VdiHdl vdi, short msg[8])
{
    switch (msg[0]) {
    case MN_SELECTED:
        menu_tnormal(wp_menu_tree, msg[3], UNHIGHLIGHT);
        switch (msg[4]) {
        case WP_MENU_OBJ_FILE_QUIT:
            return WP_TRUE;
        case WP_MENU_OBJ_FILE_OPEN:
            do_file_open();
            redraw_window(window_handle, vdi);
            break;
        case WP_MENU_OBJ_FILE_SAVE:
            do_file_save();
            break;
        case WP_MENU_OBJ_EDIT_CUT:
            do_edit_cut();
            redraw_window(window_handle, vdi);
            break;
        case WP_MENU_OBJ_EDIT_COPY:
            do_edit_copy();
            break;
        case WP_MENU_OBJ_EDIT_PASTE:
            do_edit_paste();
            redraw_window(window_handle, vdi);
            break;
        default:
            break;
        }
        return WP_FALSE;

    case WM_CLOSED:
        return WP_TRUE;

    case WM_TOPPED:
        wind_set(window_handle, WF_TOP, 0, 0, 0, 0);
        return WP_FALSE;

    case WM_REDRAW:
        redraw_window(window_handle, vdi);
        return WP_FALSE;

    case WM_MOVED:
        wind_set(window_handle, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
        return WP_FALSE;

    case WM_SIZED:
        wind_set(window_handle, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
        if (g_doc_loaded) {
            GRECT work;
            wind_get_grect(window_handle, WF_WORKXYWH, &work);
            relayout_document(atari_px_to_twips((i32)work.g_w));
        }
        return WP_FALSE;

    default:
        return WP_FALSE;
    }
}

void app_shell_run(void)
{
    short work_in[11];
    short work_out[57];
    short dummy;
    VdiHdl vdi_handle;
    short window_handle;
    GRECT desk;
    short msg[8];
    short mx, my, mbtn, mkstate, mkey, mcount;
    short events;
    wp_bool quit;

    appl_init();

    vdi_handle = graf_handle(&dummy, &dummy, &dummy, &dummy);
    for (dummy = 0; dummy < 10; dummy++) work_in[dummy] = 1;
    work_in[10] = 2; /* raster coordinates */
    v_opnvwk(work_in, &vdi_handle, work_out);

    wp_menu_tree_init();
    menu_bar(wp_menu_tree, MENU_INSTALL);

    /* Query the desktop work area AFTER installing the menu bar -- AES
       shrinks it to make room for the menu bar row. */
    wind_get_grect(0, WF_WORKXYWH, &desk);

    window_handle = wind_create_grect(NAME | MOVER | CLOSER | SIZER, &desk);
    wind_set_str(window_handle, WF_NAME, "Word Processor");
    wind_open_grect(window_handle, &desk);

    graf_mouse(ARROW, (const MFORM *)0);

    {
        GRECT work;
        wind_get_grect(window_handle, WF_WORKXYWH, &work);
        load_sample_document(atari_px_to_twips((i32)work.g_w));
    }

    quit = WP_FALSE;
    while (!quit) {
        events = evnt_multi(MU_MESAG | MU_KEYBD, 1, 1, 1,
                             0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0,
                             msg, 0,
                             &mx, &my, &mbtn, &mkstate, &mkey, &mcount);
        if (events & MU_MESAG) {
            quit = wp_dispatch_message(window_handle, vdi_handle, msg);
        }
        if (!quit && (events & MU_KEYBD)) {
            wp_dispatch_key(window_handle, vdi_handle, mkey);
        }
    }

    if (g_doc_loaded) {
        wp_undo_journal_free(&g_journal);
        wp_document_free(&g_doc);
        g_doc_loaded = WP_FALSE;
    }

    menu_bar(wp_menu_tree, MENU_REMOVE);
    wind_close(window_handle);
    wind_delete(window_handle);
    v_clsvwk(vdi_handle);
    appl_exit();
}
