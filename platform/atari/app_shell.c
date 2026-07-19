#include <gem.h>
#include <osbind.h>
#include <string.h>
#include "types.h"
#include "platform.h"
#include "docmodel.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "layout.h"
#include "editing.h"
#include "app_shell.h"
#include "app_shell_internal.h"
#include "metrics_atari_internal.h"
#include "mem_atari.h"
#include "spell.h"
#include "offload.h"
#include "endian.h"

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
    { -1,                     WP_MENU_OBJ_FILE_TITLE, WP_MENU_OBJ_TOOLS_TITLE, G_IBOX,   OF_NONE,   OS_NORMAL, {0}, 0, 0, 40, 1 },
    { WP_MENU_OBJ_EDIT_TITLE, WP_MENU_OBJ_FILE_BOX,   WP_MENU_OBJ_FILE_BOX,   G_TITLE,  OF_NONE,   OS_NORMAL, {0}, 0, 0, 6,  1 },
    { WP_MENU_OBJ_TOOLS_TITLE, WP_MENU_OBJ_EDIT_BOX,  WP_MENU_OBJ_EDIT_BOX,   G_TITLE,  OF_NONE,   OS_NORMAL, {0}, 6, 0, 6,  1 },
    { WP_MENU_OBJ_FILE_TITLE, WP_MENU_OBJ_FILE_OPEN,  WP_MENU_OBJ_FILE_QUIT,  G_BOX,    OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  3 },
    { WP_MENU_OBJ_FILE_SAVE,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 0, 8,  1 },
    { WP_MENU_OBJ_FILE_QUIT,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  1 },
    { WP_MENU_OBJ_FILE_BOX,   -1,                     -1,                    G_STRING, OF_LASTOB, OS_NORMAL, {0}, 0, 2, 8,  1 },
    { WP_MENU_OBJ_EDIT_TITLE, WP_MENU_OBJ_EDIT_CUT,   WP_MENU_OBJ_EDIT_PASTE, G_BOX,    OF_NONE,   OS_NORMAL, {0}, 6, 1, 8,  3 },
    { WP_MENU_OBJ_EDIT_COPY,  -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 0, 8,  1 },
    { WP_MENU_OBJ_EDIT_PASTE, -1,                     -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 1, 8,  1 },
    { WP_MENU_OBJ_EDIT_BOX,   -1,                     -1,                    G_STRING, OF_LASTOB, OS_NORMAL, {0}, 0, 2, 8,  1 },
    { WP_MENU_OBJ_ROOT,       WP_MENU_OBJ_TOOLS_BOX,  WP_MENU_OBJ_TOOLS_BOX, G_TITLE,  OF_NONE,   OS_NORMAL, {0}, 12, 0, 7,  1 },
    { WP_MENU_OBJ_TOOLS_TITLE, WP_MENU_OBJ_TOOLS_SPELLCHECK, WP_MENU_OBJ_TOOLS_GRAMMARCHECK, G_BOX, OF_NONE, OS_NORMAL, {0}, 12, 1, 16, 3 },
    { WP_MENU_OBJ_TOOLS_THESAURUS, -1,                -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 0, 16, 1 },
    { WP_MENU_OBJ_TOOLS_GRAMMARCHECK, -1,              -1,                    G_STRING, OF_NONE,   OS_NORMAL, {0}, 0, 1, 16, 1 },
    { WP_MENU_OBJ_TOOLS_BOX,  -1,                     -1,                    G_STRING, OF_LASTOB, OS_NORMAL, {0}, 0, 2, 16, 1 }
};

static char g_text_file[]  = " File ";
static char g_text_edit[]  = " Edit ";
static char g_text_tools[] = " Tools ";
static char g_text_open[]  = "  Open";
static char g_text_save[]  = "  Save";
static char g_text_quit[]  = "  Quit";
static char g_text_cut[]   = "  Cut";
static char g_text_copy[]  = "  Copy";
static char g_text_paste[] = "  Paste";
static char g_text_spellcheck[] = "  Spell Check";
static char g_text_thesaurus[] = "  Thesaurus";
static char g_text_grammarcheck[] = "  Grammar Check";

void wp_menu_tree_init(void)
{
    wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_spec.free_string = g_text_file;
    wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_spec.free_string = g_text_edit;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_spec.free_string = g_text_tools;
    wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_spec.free_string = g_text_open;
    wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_spec.free_string = g_text_save;
    wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string = g_text_quit;
    wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_spec.free_string = g_text_cut;
    wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_spec.free_string = g_text_copy;
    wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string = g_text_paste;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_spec.free_string = g_text_spellcheck;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_spec.free_string = g_text_thesaurus;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_spec.free_string = g_text_grammarcheck;
}

wp_bool wp_menu_tree_selfcheck(void)
{
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_next != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_head != WP_MENU_OBJ_FILE_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_tail != WP_MENU_OBJ_TOOLS_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_type != G_IBOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_next != WP_MENU_OBJ_EDIT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_head != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_tail != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_type != G_TITLE) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_next != WP_MENU_OBJ_TOOLS_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_head != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_tail != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_type != G_TITLE) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_next != WP_MENU_OBJ_ROOT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_head != WP_MENU_OBJ_TOOLS_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_tail != WP_MENU_OBJ_TOOLS_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_type != G_TITLE) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_next != WP_MENU_OBJ_TOOLS_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_head != WP_MENU_OBJ_TOOLS_SPELLCHECK) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_tail != WP_MENU_OBJ_TOOLS_GRAMMARCHECK) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_type != G_BOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_next != WP_MENU_OBJ_TOOLS_THESAURUS) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_next != WP_MENU_OBJ_TOOLS_GRAMMARCHECK) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_next != WP_MENU_OBJ_TOOLS_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_flags & OF_LASTOB) == 0) return WP_FALSE;

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
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_spec.free_string != g_text_tools) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string != g_text_quit) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string != g_text_paste) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_spec.free_string != g_text_spellcheck) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_spec.free_string != g_text_thesaurus) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_spec.free_string != g_text_grammarcheck) return WP_FALSE;

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

/* Replaces the currently-open document (if any) with new_doc (already
   built and owned by the caller -- this function takes it over),
   resetting the cursor to its end and the undo journal to empty. Shared
   by every "load this as the whole document" path: startup, Paste,
   and both Open formats (plaintext via set_document_from_text below,
   RTF via do_file_open directly, since RTF import has its own error
   path that doesn't fit set_document_from_text's void-return shape). */
static void replace_document(wp_document *new_doc, i32 content_width_twips)
{
    if (g_doc_loaded) {
        wp_undo_journal_free(&g_journal);
        wp_document_free(&g_doc);
    }

    g_doc = *new_doc;
    g_doc_loaded = WP_TRUE;
    wp_undo_journal_init(&g_journal);
    relayout_document(content_width_twips);
    g_cursor = wp_pos_doc_end(&g_doc);
}

static void set_document_from_text(const u8 *utf8, u32 len, i32 content_width_twips)
{
    wp_document new_doc;

    if (format_txt_import(utf8, len, &new_doc) != WP_OK) return;
    replace_document(&new_doc, content_width_twips);
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

/* Case-insensitive "does path end with suffix" (suffix normally a
   dotted extension like ".RTF"). Used to pick RTF vs plaintext
   import/export by filename rather than adding a format-choice control
   to the file selector -- fsel_input() has no such control, and typing
   a different extension into its filename field is the standard classic-
   GEM way an app distinguishes formats at Open/Save (the file selector
   itself is format-blind). */
static wp_bool str_ends_with_ci(const char *s, const char *suffix)
{
    u32 slen = (u32)strlen(s);
    u32 suflen = (u32)strlen(suffix);
    u32 i;

    if (slen < suflen) return WP_FALSE;
    for (i = 0; i < suflen; i++) {
        char a = s[slen - suflen + i];
        char b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return WP_FALSE;
    }
    return WP_TRUE;
}

#define WP_ATARI_OFFLOAD_ENDPOINT "serial:aux:9600"

static wp_bool g_offload_init_attempted = WP_FALSE;

/* Tries offload_init() exactly ONCE, lazily, on first use (same pattern
   as get_spell_dict() above) -- an unattached/absent device fails within
   OFFLOAD_CONNECT_TIMEOUT_MS + OFFLOAD_HANDSHAKE_TIMEOUT_MS
   (engine/src/offload_client.c: 2s + 2s), a bounded, one-time cost, not
   a multi-second stall on every single offload-backed action thereafter
   (Thesaurus, and now ODT/PDF Open/Save). Defined here, before its first
   real caller (ODT/PDF below), rather than down near Thesaurus where it
   was originally written -- Thesaurus still uses it unchanged. */
static wp_bool ensure_offload(void)
{
    if (!g_offload_init_attempted) {
        g_offload_init_attempted = WP_TRUE;
        offload_init(WP_ATARI_OFFLOAD_ENDPOINT); /* result read back via offload_available() */
    }
    return offload_available();
}

/* ODT/PDF (post-M7): CONVERT_IMPORT/CONVERT_EXPORT ship the WHOLE
   foreign-format file as one offload request/response payload (unlike
   Thesaurus's single-word lookups), so this needs its own generous
   bounds. Real RS-232 at 9600 baud (this target's only real transport,
   T6.3) moves roughly 960 bytes/sec (8N1 framing = 10 bits/byte), so a
   modest multi-KB document can genuinely take tens of real seconds each
   way -- the timeout below is sized for that, not for a fast local link,
   and offload_client.c's own OFFLOAD_MAX_RESPONSE_PAYLOAD (32768, see
   its own comment) is what actually bounds how large a response this
   app can ever receive regardless of what's requested here. */
#define WP_ATARI_ODT_MAX_FILE_SIZE  60000uL /* comfortably under WIRE_MAX_PAYLOAD (64K) with room for the format_id byte + frame overhead */
#define WP_ATARI_CONVERT_TIMEOUT_MS 120000uL /* ~2 minutes -- see the bandwidth math above */
#define WP_ATARI_FORMAT_ODT 1u /* matches docs/formats/offload-protocol.md's FORMAT_ODT */
#define WP_ATARI_FORMAT_PDF 2u /* matches docs/formats/offload-protocol.md's FORMAT_PDF -- export only, no import */

/* Shared by ODT open and both ODT/PDF save paths: ensures the offload
   link is up (reusing ensure_offload()'s lazy-once pattern from
   Thesaurus) and reports its absence the same way every other offload
   feature in this app does -- gracefully, via form_alert(), never a
   crash or a silently corrupted document. */
static wp_bool require_offload_for_convert(void)
{
    if (!ensure_offload()) {
        form_alert(1, FA_ERROR "[No offload device available.|ODT/PDF needs a connected gateway.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        return WP_FALSE;
    }
    return WP_TRUE;
}

/* Reads a whole .odt file, ships it through OFFLOAD_CONVERT_IMPORT, and
   replaces the current document with the RTF the gateway sends back --
   format_rtf_import() does the actual parsing, exactly like the local
   .RTF Open path above; only where the RTF bytes come FROM differs. */
static void do_odt_open(const char *fullpath)
{
    plat_file *f;
    u32 size, read_len;
    u8 *req_buf;
    offload_request req;
    offload_response resp;

    if (!require_offload_for_convert()) return;

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK || size > WP_ATARI_ODT_MAX_FILE_SIZE) {
        plat_file_close(f);
        form_alert(1, FA_ERROR "[That ODT file is too large|for this app to open.][ OK ]");
        return;
    }

    req_buf = (u8 *)mem_alloc(size + 1);
    if (req_buf == NULL) { plat_file_close(f); return; }

    req_buf[0] = (u8)WP_ATARI_FORMAT_ODT;
    if (plat_file_read(f, req_buf + 1, size, &read_len) != WP_OK) {
        mem_free(req_buf);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = req_buf;
    req.payload_len = read_len + 1;
    req.timeout_ms = (u32)WP_ATARI_CONVERT_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        form_alert(1, FA_ERROR "[Opening that ODT file failed|or timed out.][ OK ]");
        return;
    }
    mem_free(req_buf);

    if (resp.status == OFFLOAD_OK) {
        wp_document new_doc;
        if (format_rtf_import(resp.result, resp.result_len, &new_doc) == WP_OK) {
            replace_document(&new_doc, g_content_width_twips);
        } else {
            form_alert(1, FA_ERROR "[The gateway's reply couldn't be read|as a document.][ OK ]");
        }
    } else {
        form_alert(1, FA_ERROR "[The connected gateway couldn't|convert that ODT file.][ OK ]");
    }
    offload_response_free(&resp);
}

/* Exports the current document to RTF (the same bytes the local .RTF
   Save path would write), ships it through OFFLOAD_CONVERT_EXPORT with
   the requested format_id, and writes whatever comes back straight to
   disk -- an opaque .odt or .pdf file this app never parses itself. */
static void do_odt_pdf_save(const char *fullpath, u8 format_id)
{
    u32 rtf_needed;
    u8 *req_buf;
    offload_request req;
    offload_response resp;
    plat_file *f;

    if (!g_doc_loaded) return;
    if (!require_offload_for_convert()) return;

    format_rtf_export(&g_doc, NULL, 0, &rtf_needed);
    req_buf = (u8 *)mem_alloc(rtf_needed + 1);
    if (req_buf == NULL) return;

    req_buf[0] = format_id;
    if (format_rtf_export(&g_doc, req_buf + 1, rtf_needed, &rtf_needed) != WP_OK) {
        mem_free(req_buf);
        return;
    }

    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = req_buf;
    req.payload_len = rtf_needed + 1;
    req.timeout_ms = (u32)WP_ATARI_CONVERT_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        form_alert(1, FA_ERROR "[Saving that file failed or timed out.][ OK ]");
        return;
    }
    mem_free(req_buf);

    if (resp.status != OFFLOAD_OK) {
        form_alert(1, FA_ERROR "[The connected gateway couldn't produce|that file format.][ OK ]");
        offload_response_free(&resp);
        return;
    }

    if (plat_file_open(fullpath, PLAT_FILE_WRITE, &f) == WP_OK) {
        plat_file_write(f, resp.result, resp.result_len);
        plat_file_close(f);
    } else {
        form_alert(1, FA_ERROR "[Couldn't write that file to disk.][ OK ]");
    }
    offload_response_free(&resp);
}

static void do_file_open(void)
{
    char path[128];
    char fname[16];
    char fullpath[144];
    plat_file *f;
    u32 size, read_len;
    u8 *buf;

    /* "*.*" (not "*.TXT") so RTF/ODT files are visible too -- see
       str_ends_with_ci above for how the format is actually picked. */
    strcpy(path, "C:\\*.*");
    fname[0] = '\0';
    if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;

    if (str_ends_with_ci(fullpath, ".ODT")) {
        do_odt_open(fullpath);
        return;
    }

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK) { plat_file_close(f); return; }

    buf = (u8 *)mem_alloc(size + 1);
    if (buf != NULL) {
        if (plat_file_read(f, buf, size, &read_len) == WP_OK) {
            if (str_ends_with_ci(fullpath, ".RTF")) {
                wp_document new_doc;
                if (format_rtf_import(buf, read_len, &new_doc) == WP_OK) {
                    replace_document(&new_doc, g_content_width_twips);
                }
            } else {
                set_document_from_text(buf, read_len, g_content_width_twips);
            }
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
    wp_bool as_rtf;

    if (!g_doc_loaded) return;

    strcpy(path, "C:\\*.*");
    strcpy(fname, "UNTITLED.TXT");
    if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;

    if (str_ends_with_ci(fullpath, ".ODT")) {
        do_odt_pdf_save(fullpath, (u8)WP_ATARI_FORMAT_ODT);
        return;
    }
    if (str_ends_with_ci(fullpath, ".PDF")) {
        do_odt_pdf_save(fullpath, (u8)WP_ATARI_FORMAT_PDF);
        return;
    }

    as_rtf = str_ends_with_ci(fullpath, ".RTF");
    if (as_rtf) {
        format_rtf_export(&g_doc, NULL, 0, &needed);
    } else {
        format_txt_export(&g_doc, NULL, 0, &needed);
    }
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;

    if ((as_rtf ? format_rtf_export(&g_doc, buf, needed, &needed)
                : format_txt_export(&g_doc, buf, needed, &needed)) == WP_OK) {
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

/* Spell Check: like everything else that touches the whole document
   (Cut/Copy/Paste), scoped to "no selection model yet". The dictionary
   is queried from disk, never loaded whole (services/spell/spell.c,
   docs/services/spell.md) -- a user who wants real spell-check on
   WP.PRG drops a sorted word-list file at this fixed path (see
   docs/atari-platform.md's deployment note; there is no shipped
   dictionary asset, and none is needed for this feature to build/link/
   run -- a missing file is reported through the same form_alert path as
   a real miss, not a crash). */
#define WP_ATARI_SPELL_DICT_PATH "C:\\WPSPELL.DIC"
#define WP_ATARI_SPELL_MAX_SUGGESTIONS 3

static spell_dict *g_spell_dict = NULL;
static wp_bool g_spell_dict_open_attempted = WP_FALSE;

static spell_dict *get_spell_dict(void)
{
    if (!g_spell_dict_open_attempted) {
        g_spell_dict_open_attempted = WP_TRUE;
        if (spell_dict_open(WP_ATARI_SPELL_DICT_PATH, &g_spell_dict) != WP_OK) {
            g_spell_dict = NULL;
        }
    }
    return g_spell_dict;
}

static wp_bool ascii_is_alpha(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* Scans the whole document for the first word the dictionary doesn't
   recognize. Tokenizes WITHIN each run only -- a word split across a
   style boundary (e.g. "wo" bold + "rld" plain) is checked as two
   separate fragments, not reassembled into "world" first. A real gap,
   same class as the existing "whole document, no selection model"
   narrowing, documented rather than silently accepted. Returns WP_TRUE
   and fills out_word/out_word_len (both out-params) if one was found. */
static wp_bool find_first_misspelling(spell_dict *dict, u8 *out_word, u32 *out_word_len)
{
    u32 si, pi;

    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            u32 r;

            for (r = 0; r < para->run_count; r++) {
                wp_run *run = wp_paragraph_run_get(para, (u16)r);
                const u8 *bytes = wp_strpool_get(&g_doc.strings, run->text);
                u32 k = 0;

                while (k < run->text.length) {
                    u32 start;
                    u32 wlen;
                    wp_bool known;

                    while (k < run->text.length && !ascii_is_alpha(bytes[k])) k++;
                    start = k;
                    while (k < run->text.length && ascii_is_alpha(bytes[k])) k++;
                    wlen = k - start;
                    if (wlen == 0 || wlen >= SPELL_MAX_WORD_LEN) continue;

                    if (spell_check_word(dict, bytes + start, wlen, &known) == WP_OK && !known) {
                        memcpy(out_word, bytes + start, wlen);
                        *out_word_len = wlen;
                        return WP_TRUE;
                    }
                }
            }
        }
    }
    return WP_FALSE;
}

/* Appends s to buf (NUL-terminated, capacity cap), truncating rather
   than overflowing if it doesn't fit -- form_alert's own internal
   line-length limits are a real vintage-AES constraint this doesn't
   attempt to reproduce exactly (never exercised by CI -- see
   wp_atari_rtf_spell_selfcheck), but this buffer itself never
   overruns regardless. */
static void append_str(char *buf, u32 cap, u32 *pos, const char *s)
{
    while (*s != '\0' && *pos + 1 < cap) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void do_spell_check(void)
{
    spell_dict *dict;
    u8 word[SPELL_MAX_WORD_LEN];
    u32 word_len;

    if (!g_doc_loaded) return;

    dict = get_spell_dict();
    if (dict == NULL) {
        form_alert(1, FA_ERROR "[Spell-check dictionary not found.|Expected at " WP_ATARI_SPELL_DICT_PATH "][ OK ]");
        return;
    }

    if (!find_first_misspelling(dict, word, &word_len)) {
        form_alert(1, FA_INFO "[No misspellings found.][ OK ]");
        return;
    }

    {
        char word_buf[SPELL_MAX_WORD_LEN];
        char suggestions[WP_ATARI_SPELL_MAX_SUGGESTIONS][SPELL_MAX_WORD_LEN];
        u32 sugg_count = 0;
        char msg[200];
        u32 pos = 0;

        memcpy(word_buf, word, word_len);
        word_buf[word_len] = '\0';

        spell_suggest(dict, word, word_len, suggestions, WP_ATARI_SPELL_MAX_SUGGESTIONS, &sugg_count);

        append_str(msg, (u32)sizeof(msg), &pos, FA_INFO "[Unknown word: ");
        append_str(msg, (u32)sizeof(msg), &pos, word_buf);
        if (sugg_count > 0) {
            u32 i;
            append_str(msg, (u32)sizeof(msg), &pos, "|Suggestions: ");
            for (i = 0; i < sugg_count; i++) {
                if (i > 0) append_str(msg, (u32)sizeof(msg), &pos, ", ");
                append_str(msg, (u32)sizeof(msg), &pos, suggestions[i]);
            }
        } else {
            append_str(msg, (u32)sizeof(msg), &pos, "|(no suggestions)");
        }
        append_str(msg, (u32)sizeof(msg), &pos, "][ OK ]");

        form_alert(1, msg);
    }
}

/* Thesaurus (post-M6): the first real menu action to use the offload
   link (T6.3's "Known gaps" note in docs/atari-platform.md called this
   out explicitly -- "no 'Thesaurus' menu action wired to the offload
   link"). Looks up the word immediately BEFORE the cursor, same
   directionality as Backspace, and scoped to the CURRENT RUN only, same
   boundary do_backspace already has -- there is still no mouse-driven
   word selection (no selection model at all, per docs/editing.md), and
   after typing a word the cursor naturally sits right after it, so this
   matches how the app is actually used today rather than inventing a
   word-under-cursor hit-test this editor has no input path to drive.
   (WP_ATARI_OFFLOAD_ENDPOINT and ensure_offload() itself now live above,
   near str_ends_with_ci -- ODT/PDF Open/Save (post-M7) need them earlier
   in the file than Thesaurus does.) */
#define WP_ATARI_THESAURUS_MAX_WORD_LEN 64
#define WP_ATARI_THESAURUS_TIMEOUT_MS   90000uL /* generous: a real Pi/LLM
                                                    gateway's cold model
                                                    load can take tens of
                                                    seconds, same
                                                    rationale as
                                                    ollama_gateway.c's
                                                    --ollama-timeout-ms
                                                    default -- see
                                                    docs/formats/offload-
                                                    protocol.md. */

static wp_bool extract_word_before_cursor(u8 *out_word, u32 *out_word_len)
{
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    const u8 *bytes;
    u32 offset, end, start;

    if (!g_doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_doc, g_cursor.section);
    para = wp_section_paragraph_get(sec, g_cursor.paragraph);
    if (g_cursor.run >= para->run_count) return WP_FALSE;

    run = wp_paragraph_run_get(para, (u16)g_cursor.run);
    bytes = wp_strpool_get(&g_doc.strings, run->text);

    offset = g_cursor.offset;
    if (offset > run->text.length) offset = run->text.length;

    end = offset;
    while (end > 0 && !ascii_is_alpha(bytes[end - 1])) end--;
    start = end;
    while (start > 0 && ascii_is_alpha(bytes[start - 1])) start--;

    if (start == end) return WP_FALSE;
    if (end - start >= WP_ATARI_THESAURUS_MAX_WORD_LEN) return WP_FALSE;

    memcpy(out_word, bytes + start, end - start);
    *out_word_len = end - start;
    return WP_TRUE;
}

static void do_thesaurus(void)
{
    u8 word[WP_ATARI_THESAURUS_MAX_WORD_LEN];
    u32 word_len;
    char word_buf[WP_ATARI_THESAURUS_MAX_WORD_LEN];
    offload_request req;
    offload_response resp;
    char msg[220];
    u32 pos = 0;

    if (!extract_word_before_cursor(word, &word_len)) {
        form_alert(1, FA_INFO "[Place the cursor right after a word,|then choose Thesaurus again.][ OK ]");
        return;
    }
    memcpy(word_buf, word, word_len);
    word_buf[word_len] = '\0';

    if (!ensure_offload()) {
        form_alert(1, FA_ERROR "[No offload device available.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        return;
    }

    req.op = OFFLOAD_THESAURUS;
    req.payload = word;
    req.payload_len = word_len;
    req.timeout_ms = (u32)WP_ATARI_THESAURUS_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        form_alert(1, FA_ERROR "[Thesaurus request failed or timed out.][ OK ]");
        return;
    }

    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        form_alert(1, FA_INFO "[The connected gateway doesn't support|Thesaurus.][ OK ]");
        return;
    }

    append_str(msg, (u32)sizeof(msg), &pos, FA_INFO "[Synonyms for ");
    append_str(msg, (u32)sizeof(msg), &pos, word_buf);
    append_str(msg, (u32)sizeof(msg), &pos, ": ");

    {
        u16 count = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
        u32 rpos = 2;
        u32 i;

        if (count == 0) {
            append_str(msg, (u32)sizeof(msg), &pos, "|(none found)");
        } else {
            for (i = 0; i < count && rpos + 2 <= resp.result_len; i++) {
                u16 wlen = be_read16(resp.result + rpos);
                char tmp[WP_ATARI_THESAURUS_MAX_WORD_LEN];
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
    append_str(msg, (u32)sizeof(msg), &pos, "][ OK ]");

    offload_response_free(&resp);
    form_alert(1, msg);
}

/* Grammar Check (M8 T8.1): sends the CURRENT paragraph's plain text
   over the offload link and, for each issue the gateway reports back,
   shows the flagged text/explanation/suggestion one at a time via
   form_alert() and lets the user Apply, Skip, or Stop. There is no GEM
   control that shows an arbitrary-length list in one dialog and no
   resource-editor toolchain in this project to build one with (see the
   OBJECT[] comment above), so this cycles through issues sequentially
   instead of inventing a custom list widget.

   Every issue's quoted span was already verified by the GATEWAY via a
   byte-exact substring search against the paragraph text this app sent
   it (docs/formats/offload-protocol.md) -- but this app additionally
   never trusts a wire offset against the LIVE document once earlier
   fixes in the same session may have shifted it; see
   map_paragraph_offset_to_run below. */
#define WP_ATARI_GRAMMAR_MAX_PARAGRAPH_LEN 2000uL /* matches ollama_gateway.c's GRAMMAR_PARAGRAPH_MAX_LEN */
#define WP_ATARI_GRAMMAR_TIMEOUT_MS        90000uL /* same cold-model-load rationale as Thesaurus */
#define WP_ATARI_GRAMMAR_MAX_ISSUES        16u /* matches gw_common.h's GRAMMAR_MAX_ISSUES */
#define WP_ATARI_GRAMMAR_DISPLAY_LEN       40 /* dialog-text truncation only -- never touches what gets applied */

/* Concatenates the plain UTF-8 text of the CURRENT paragraph (g_cursor's
   section/paragraph, every run in order) into out. Returns WP_FALSE if
   there's no document loaded, the paragraph is empty, or the text
   wouldn't fit in out_cap -- all three are "nothing to check" for the
   caller, not distinguished further. Mirrors format_txt_export's inner
   loop (engine/src/format_txt.c) narrowed to one paragraph; no engine
   primitive already does this (only whole-document export exists). */
static wp_bool extract_current_paragraph_text(u8 *out, u32 out_cap, u32 *out_len)
{
    wp_section *sec;
    wp_paragraph *para;
    u32 written = 0;
    u16 i;

    if (!g_doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_doc, g_cursor.section);
    para = wp_section_paragraph_get(sec, g_cursor.paragraph);

    for (i = 0; i < para->run_count; i++) {
        wp_run *run = wp_paragraph_run_get(para, i);

        if (run->text.length > 0) {
            const u8 *bytes;

            if (written + run->text.length > out_cap) return WP_FALSE;
            bytes = wp_strpool_get(&g_doc.strings, run->text);
            memcpy(out + written, bytes, run->text.length);
            written += run->text.length;
        }
    }
    if (written == 0) return WP_FALSE;
    *out_len = written;
    return WP_TRUE;
}

/* Maps a paragraph-relative byte range [start, end) in the CURRENT
   paragraph to a single run's index and run-local offsets, re-walking
   g_doc fresh every call rather than holding a pointer across an
   earlier edit (wp_paragraph_run_add can reallocate para->runs --
   docmodel.h says so explicitly). Returns WP_FALSE if the range isn't
   entirely within ONE run: it may cross a style boundary, or (after an
   earlier fix in this same Grammar Check pass shifted the text) may no
   longer line up with real run boundaries at all -- either way,
   declining to auto-apply is the safe answer, since
   wp_edit_delete_range's contract is single-run-only (docs/editing.md)
   and forcing a bad range through would risk corrupting text this
   action never meant to touch. */
static wp_bool map_paragraph_offset_to_run(u32 start, u32 end, u16 *out_run_index,
                                            u32 *out_run_start, u32 *out_run_end)
{
    wp_section *sec;
    wp_paragraph *para;
    u32 base = 0;
    u16 i;

    sec = wp_document_section_get(&g_doc, g_cursor.section);
    para = wp_section_paragraph_get(sec, g_cursor.paragraph);

    for (i = 0; i < para->run_count; i++) {
        wp_run *run = wp_paragraph_run_get(para, i);
        u32 run_len = run->text.length;

        if (start >= base && end <= base + run_len) {
            *out_run_index = i;
            *out_run_start = start - base;
            *out_run_end = end - base;
            return WP_TRUE;
        }
        base += run_len;
    }
    return WP_FALSE;
}

typedef struct {
    u16       start_offset;
    u16       span_len;
    const u8 *message;
    u32       message_len;
    const u8 *suggestion;
    u32       suggestion_len;
} wp_atari_grammar_issue;

/* Decodes issue `index` from the OFFLOAD_GRAMMAR_CHECK response shape
   (u16 issue_count, then issue_count x (u16 start_offset, u16 span_len,
   u16 message_len + bytes, u16 suggestion_len + bytes) --
   docs/formats/offload-protocol.md), mirroring host/tests/
   test_offload.c's grammar_issue_at(). Returns WP_FALSE on a short/
   malformed buffer or an out-of-range index. */
static wp_bool decode_grammar_issue_at(const u8 *buf, u32 len, u32 index, wp_atari_grammar_issue *out)
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
        if (i == index) { out->message = buf + pos; out->message_len = mlen; }
        pos += mlen;

        slen = be_read16(buf + pos); pos += 2;
        if (pos + slen > len) return WP_FALSE;
        if (i == index) { out->suggestion = buf + pos; out->suggestion_len = slen; }
        pos += slen;
    }
    return WP_TRUE;
}

/* Appends up to WP_ATARI_GRAMMAR_DISPLAY_LEN bytes of s into buf via
   append_str -- keeps one long quote/message/suggestion from crowding
   the rest of a form_alert dialog off the tiny ST screen. Truncates the
   DIALOG TEXT only; the wire values actually applied to the document
   are never touched by this. */
static void append_str_n(char *buf, u32 cap, u32 *pos, const u8 *s, u32 s_len)
{
    char tmp[WP_ATARI_GRAMMAR_DISPLAY_LEN + 1];
    u32 tlen = (s_len < (u32)sizeof(tmp) - 1) ? s_len : (u32)sizeof(tmp) - 1;

    memcpy(tmp, s, tlen);
    tmp[tlen] = '\0';
    append_str(buf, cap, pos, tmp);
}

static void do_grammar_check(void)
{
    u8 *paragraph;
    u32 paragraph_len;
    offload_request req;
    offload_response resp;
    u32 issue_count;
    wp_atari_grammar_issue issues[WP_ATARI_GRAMMAR_MAX_ISSUES];
    u32 order[WP_ATARI_GRAMMAR_MAX_ISSUES];
    u32 applied_start[WP_ATARI_GRAMMAR_MAX_ISSUES];
    u32 applied_end[WP_ATARI_GRAMMAR_MAX_ISSUES];
    u32 applied_count = 0;
    i32 delta = 0;
    wp_bool any_applied = WP_FALSE;
    u32 n, i, j;

    if (!g_doc_loaded) return;

    paragraph = (u8 *)mem_alloc(WP_ATARI_GRAMMAR_MAX_PARAGRAPH_LEN);
    if (paragraph == NULL) return;

    if (!extract_current_paragraph_text(paragraph, (u32)WP_ATARI_GRAMMAR_MAX_PARAGRAPH_LEN, &paragraph_len)) {
        mem_free(paragraph);
        form_alert(1, FA_INFO "[Place the cursor in a paragraph|with some text, then choose|Grammar Check again.][ OK ]");
        return;
    }

    if (!ensure_offload()) {
        mem_free(paragraph);
        form_alert(1, FA_ERROR "[No offload device available.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        return;
    }

    req.op = OFFLOAD_GRAMMAR_CHECK;
    req.payload = paragraph;
    req.payload_len = paragraph_len;
    req.timeout_ms = (u32)WP_ATARI_GRAMMAR_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(paragraph);
        form_alert(1, FA_ERROR "[Grammar Check request failed or timed out.][ OK ]");
        return;
    }

    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        mem_free(paragraph);
        form_alert(1, FA_INFO "[The connected gateway doesn't support|Grammar Check.][ OK ]");
        return;
    }

    n = (resp.result_len >= 2) ? be_read16(resp.result) : 0;
    if (n > (u32)WP_ATARI_GRAMMAR_MAX_ISSUES) n = (u32)WP_ATARI_GRAMMAR_MAX_ISSUES;

    issue_count = 0;
    for (i = 0; i < n; i++) {
        if (!decode_grammar_issue_at(resp.result, resp.result_len, i, &issues[issue_count])) break;
        issue_count++;
    }

    if (issue_count == 0) {
        offload_response_free(&resp);
        mem_free(paragraph);
        form_alert(1, FA_INFO "[No grammar issues found.][ OK ]");
        return;
    }

    /* Process in ascending start_offset order regardless of how the
       gateway emitted them -- applying an earlier fix shifts byte
       positions for everything after it in this same paragraph, and
       the `delta` bookkeeping below only works walking left to right.
       Plain insertion sort: issue_count is capped at 16. */
    for (i = 0; i < issue_count; i++) order[i] = i;
    for (i = 1; i < issue_count; i++) {
        u32 key = order[i];

        j = i;
        while (j > 0 && issues[order[j - 1]].start_offset > issues[key].start_offset) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    for (i = 0; i < issue_count; i++) {
        wp_atari_grammar_issue *issue = &issues[order[i]];
        char msg[220];
        u32 pos = 0;
        wp_bool has_suggestion = (issue->suggestion_len > 0);
        wp_bool overlaps_applied = WP_FALSE;
        short btn;
        u32 k;

        /* An LLM occasionally flags overlapping spans (two issues both
           touching the same word); comparing against ALREADY-APPLIED
           ranges in ORIGINAL-paragraph coordinates (stable, never
           shifted) is simple and catches this directly -- applying a
           second fix into text a first fix already replaced would
           corrupt the document. */
        for (k = 0; k < applied_count; k++) {
            u32 a_start = applied_start[k], a_end = applied_end[k];
            u32 b_start = (u32)issue->start_offset, b_end = (u32)issue->start_offset + (u32)issue->span_len;

            if (!(a_end <= b_start || b_end <= a_start)) { overlaps_applied = WP_TRUE; break; }
        }

        append_str(msg, (u32)sizeof(msg), &pos, FA_INFO "[");
        append_str_n(msg, (u32)sizeof(msg), &pos, issue->message, issue->message_len);
        append_str(msg, (u32)sizeof(msg), &pos, "|Text: \"");
        if ((u32)issue->start_offset + (u32)issue->span_len <= paragraph_len) {
            append_str_n(msg, (u32)sizeof(msg), &pos, paragraph + issue->start_offset, issue->span_len);
        }
        append_str(msg, (u32)sizeof(msg), &pos, "\"|Suggestion: ");
        if (has_suggestion) {
            append_str(msg, (u32)sizeof(msg), &pos, "\"");
            append_str_n(msg, (u32)sizeof(msg), &pos, issue->suggestion, issue->suggestion_len);
            append_str(msg, (u32)sizeof(msg), &pos, "\"");
        } else {
            append_str(msg, (u32)sizeof(msg), &pos, "(none offered)");
        }
        append_str(msg, (u32)sizeof(msg), &pos, "][ Apply | Skip | Stop ]");

        btn = form_alert(1, msg);

        if (btn == 3) break; /* Stop: leave any remaining issues unshown */
        if (btn != 1 || !has_suggestion) continue; /* Skip, or Apply with nothing to apply */

        if (overlaps_applied) {
            form_alert(1, FA_ERROR "[Couldn't apply -- this fix overlaps|a fix already applied.][ OK ]");
            continue;
        }

        {
            u32 adj_start = (u32)((i32)issue->start_offset + delta);
            u32 adj_end = adj_start + (u32)issue->span_len;
            u16 run_index;
            u32 run_start, run_end;

            if (!map_paragraph_offset_to_run(adj_start, adj_end, &run_index, &run_start, &run_end)) {
                form_alert(1, FA_ERROR "[Couldn't apply -- this fix spans|multiple text styles.][ OK ]");
                continue;
            }

            if (wp_edit_delete_range(&g_doc, &g_journal, g_cursor.section, g_cursor.paragraph,
                                      run_index, run_start, run_end) == WP_OK) {
                wp_pos insert_pos;
                wp_pos out_pos;

                insert_pos.section = g_cursor.section;
                insert_pos.paragraph = g_cursor.paragraph;
                insert_pos.run = (u32)run_index;
                insert_pos.offset = run_start;

                if (wp_edit_insert_text(&g_doc, &g_journal, insert_pos,
                                         issue->suggestion, issue->suggestion_len, &out_pos) == WP_OK) {
                    delta += (i32)issue->suggestion_len - (i32)issue->span_len;
                    if (applied_count < (u32)WP_ATARI_GRAMMAR_MAX_ISSUES) {
                        applied_start[applied_count] = (u32)issue->start_offset;
                        applied_end[applied_count] = (u32)issue->start_offset + (u32)issue->span_len;
                        applied_count++;
                    }
                    any_applied = WP_TRUE;
                } else {
                    form_alert(1, FA_ERROR "[Couldn't apply that fix.][ OK ]");
                }
            } else {
                form_alert(1, FA_ERROR "[Couldn't apply that fix.][ OK ]");
            }
        }
    }

    offload_response_free(&resp);
    mem_free(paragraph);

    /* At least one edit landed somewhere in this paragraph -- g_cursor's
       old run/offset may now be a stale index into a run array that was
       resized by those edits (run_remove_at/run_insert_at can shift or
       shrink it). Resetting to this paragraph's start is always valid:
       every delete-then-insert pair here leaves the paragraph with at
       least one run (insert_span_at_pos's para->run_count == 0 case
       creates one), so run 0 / offset 0 is guaranteed to exist. */
    if (any_applied) {
        g_cursor.run = 0;
        g_cursor.offset = 0;
        relayout_document(g_content_width_twips);
    }
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

wp_bool wp_atari_rtf_spell_selfcheck(void)
{
    static const char scratch_rtf_path[] = "C:\\WPTEST.RTF";
    static const char scratch_dict_path[] = "C:\\WPTEST.DIC";
    static const char sample_text[] = "hello world\n";
    static const char dict_contents[] = "cat\ndog\nhello\nworld\n";
    wp_document doc;
    u32 rtf_needed;
    u8 *rtf_buf;
    plat_file *f;
    wp_bool ok = WP_TRUE;
    spell_dict *dict;
    wp_bool known;
    char suggestions[3][SPELL_MAX_WORD_LEN];
    u32 sugg_count;

    if (!str_ends_with_ci("REPORT.RTF", ".RTF")) return WP_FALSE;
    if (str_ends_with_ci("REPORT.TXT", ".RTF")) return WP_FALSE;
    if (!str_ends_with_ci(".RTF", ".RTF")) return WP_FALSE;

    /* RTF export -> real plat_file_write -> real plat_file_read ->
       RTF import -> txt export, proving format_rtf_* actually runs
       (not just cross-compiles) through this target's real file I/O. */
    if (format_txt_import((const u8 *)sample_text, (u32)strlen(sample_text), &doc) != WP_OK) return WP_FALSE;

    format_rtf_export(&doc, NULL, 0, &rtf_needed);
    rtf_buf = (u8 *)mem_alloc(rtf_needed);
    if (rtf_buf == NULL) { wp_document_free(&doc); return WP_FALSE; }

    if (format_rtf_export(&doc, rtf_buf, rtf_needed, &rtf_needed) != WP_OK) ok = WP_FALSE;
    wp_document_free(&doc);

    if (ok) {
        if (plat_file_open(scratch_rtf_path, PLAT_FILE_WRITE, &f) != WP_OK) {
            ok = WP_FALSE;
        } else {
            if (plat_file_write(f, rtf_buf, rtf_needed) != WP_OK) ok = WP_FALSE;
            plat_file_close(f);
        }
    }
    mem_free(rtf_buf);

    if (ok) {
        u8 readback[512];
        u32 read_len;

        if (plat_file_open(scratch_rtf_path, PLAT_FILE_READ, &f) != WP_OK) {
            ok = WP_FALSE;
        } else {
            if (plat_file_read(f, readback, (u32)sizeof(readback), &read_len) != WP_OK) ok = WP_FALSE;
            plat_file_close(f);

            if (ok) {
                wp_document roundtrip_doc;
                if (format_rtf_import(readback, read_len, &roundtrip_doc) != WP_OK) {
                    ok = WP_FALSE;
                } else {
                    u32 export_needed;
                    u8 txt_buf[64];

                    format_txt_export(&roundtrip_doc, NULL, 0, &export_needed);
                    if (export_needed > sizeof(txt_buf)) {
                        ok = WP_FALSE;
                    } else if (format_txt_export(&roundtrip_doc, txt_buf, export_needed, &export_needed) != WP_OK) {
                        ok = WP_FALSE;
                    } else if (export_needed != (u32)strlen(sample_text) ||
                               memcmp(txt_buf, sample_text, export_needed) != 0) {
                        ok = WP_FALSE;
                    }
                    wp_document_free(&roundtrip_doc);
                }
            }
        }
    }
    plat_file_delete(scratch_rtf_path);

    if (!ok) return WP_FALSE;

    /* Spell dictionary lookups through real plat_file_seek/read
       (services/spell/spell.c's binary search) against a scratch
       dictionary this test writes itself -- no shipped dictionary
       asset needed for CI; see docs/atari-platform.md for the real-
       deployment convention (C:\WPSPELL.DIC, get_spell_dict above). */
    if (plat_file_open(scratch_dict_path, PLAT_FILE_WRITE, &f) != WP_OK) return WP_FALSE;
    if (plat_file_write(f, (const u8 *)dict_contents, (u32)strlen(dict_contents)) != WP_OK) {
        plat_file_close(f);
        plat_file_delete(scratch_dict_path);
        return WP_FALSE;
    }
    plat_file_close(f);

    if (spell_dict_open(scratch_dict_path, &dict) != WP_OK) {
        plat_file_delete(scratch_dict_path);
        return WP_FALSE;
    }

    if (spell_check_word(dict, (const u8 *)"hello", 5, &known) != WP_OK || known != WP_TRUE) ok = WP_FALSE;
    if (spell_check_word(dict, (const u8 *)"zzqx", 4, &known) != WP_OK || known != WP_FALSE) ok = WP_FALSE;
    if (spell_suggest(dict, (const u8 *)"helo", 4, suggestions, 3, &sugg_count) != WP_OK) ok = WP_FALSE;
    if (sugg_count == 0) ok = WP_FALSE; /* "helo" -> "hello" is one insertion away */

    spell_dict_close(dict);
    plat_file_delete(scratch_dict_path);

    return ok;
}

/* extract_word_before_cursor() walks backward from the cursor over any
   non-alpha bytes FIRST, then over the alpha run behind them -- so it
   finds the nearest preceding word even when the cursor sits in
   trailing whitespace/punctuation, not just when it's immediately
   adjacent to a word. Deliberate (a forgiving "look up what I just
   typed" behavior, the same spirit as many editors' Ctrl+Backspace),
   not an accident -- this selfcheck's offset=6 case exists specifically
   to pin that behavior down, not just the immediately-adjacent case. */
wp_bool wp_atari_thesaurus_selfcheck(void)
{
    static const char text[] = "hello world\n";
    static const char blank_text[] = "   \n";
    u8 word[WP_ATARI_THESAURUS_MAX_WORD_LEN];
    u32 word_len;
    wp_bool ok = WP_TRUE;

    if (format_txt_import((const u8 *)text, (u32)strlen(text), &g_doc) != WP_OK) return WP_FALSE;
    g_doc_loaded = WP_TRUE;

    /* cursor at document end ("hello world|") -> "world" */
    g_cursor = wp_pos_doc_end(&g_doc);
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "world", 5) != 0) {
        ok = WP_FALSE;
    }

    /* cursor at the very start of the run -> nothing before it at all */
    g_cursor.offset = 0;
    if (extract_word_before_cursor(word, &word_len)) ok = WP_FALSE;

    /* cursor right after "hello" ("hello| world") -> "hello" */
    g_cursor.offset = 5;
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "hello", 5) != 0) {
        ok = WP_FALSE;
    }

    /* cursor after the space, before "world" ("hello |world") -- skips
       back over the space to find "hello", the nearest preceding word */
    g_cursor.offset = 6;
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "hello", 5) != 0) {
        ok = WP_FALSE;
    }

    wp_document_free(&g_doc);
    g_doc_loaded = WP_FALSE;

    /* a run with no alpha bytes anywhere before the cursor -> no word,
       not just "no word immediately adjacent" */
    if (format_txt_import((const u8 *)blank_text, (u32)strlen(blank_text), &g_doc) != WP_OK) return WP_FALSE;
    g_doc_loaded = WP_TRUE;
    g_cursor = wp_pos_doc_end(&g_doc);
    if (extract_word_before_cursor(word, &word_len)) ok = WP_FALSE;

    wp_document_free(&g_doc);
    g_doc_loaded = WP_FALSE;

    return ok;
}

wp_bool wp_atari_grammar_check_selfcheck(void)
{
    wp_bool ok = WP_TRUE;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span1, span2;
    u16 run_idx;
    u8 out_buf[64];
    u32 out_len;
    u8 canned[32];
    u32 canned_len;
    wp_atari_grammar_issue issue;

    if (wp_document_init(&g_doc) != WP_OK) return WP_FALSE;
    g_doc_loaded = WP_TRUE;

    wp_document_section_add(&g_doc, &sec_idx);
    sec = wp_document_section_get(&g_doc, sec_idx);
    wp_section_paragraph_add(sec, &para_idx);
    para = wp_section_paragraph_get(sec, para_idx);

    /* Two runs: "plain " (6 bytes) + "bold" (4 bytes) -- paragraph text
       "plain bold" (10 bytes), style boundary at byte offset 6. (para
       itself stays valid across wp_paragraph_run_add below -- only
       para->runs' internal buffer can be reallocated, per docmodel.h;
       run_insert_at in edit.c relies on the exact same fact.) */
    wp_strpool_append(&g_doc.strings, (const u8 *)"plain ", 6, &span1);
    wp_paragraph_run_add(para, span1, g_doc.default_cstyle, &run_idx);
    wp_strpool_append(&g_doc.strings, (const u8 *)"bold", 4, &span2);
    wp_paragraph_run_add(para, span2, g_doc.default_cstyle, &run_idx);

    g_cursor.section = sec_idx;
    g_cursor.paragraph = para_idx;
    g_cursor.run = 0;
    g_cursor.offset = 0;

    /* extract_current_paragraph_text concatenates both runs regardless
       of the style boundary between them. */
    if (!extract_current_paragraph_text(out_buf, (u32)sizeof(out_buf), &out_len) ||
        out_len != 10 || memcmp(out_buf, "plain bold", 10) != 0) {
        ok = WP_FALSE;
    }

    /* [0,5) "plain" sits entirely inside run 0. */
    {
        u16 ri; u32 rs, re;
        if (!map_paragraph_offset_to_run(0, 5, &ri, &rs, &re) || ri != 0 || rs != 0 || re != 5) {
            ok = WP_FALSE;
        }
    }
    /* [6,10) "bold" sits entirely inside run 1. */
    {
        u16 ri; u32 rs, re;
        if (!map_paragraph_offset_to_run(6, 10, &ri, &rs, &re) || ri != 1 || rs != 0 || re != 4) {
            ok = WP_FALSE;
        }
    }
    /* [4,8) "n bo" straddles the run boundary at 6 -- must decline
       rather than guess which run to touch. */
    {
        u16 ri; u32 rs, re;
        if (map_paragraph_offset_to_run(4, 8, &ri, &rs, &re)) ok = WP_FALSE;
    }

    /* decode_grammar_issue_at against a hand-built canned response: one
       issue (start_offset=6, span_len=4, message="msg", suggestion="fix")
       -- same wire shape host/tests/test_offload.c's grammar_issue_at()
       proves on the host side, exercised here against THIS app's own
       independent decode loop. */
    canned_len = 0;
    be_write16(canned + canned_len, 1); canned_len += 2;
    be_write16(canned + canned_len, 6); canned_len += 2;
    be_write16(canned + canned_len, 4); canned_len += 2;
    be_write16(canned + canned_len, 3); canned_len += 2;
    memcpy(canned + canned_len, "msg", 3); canned_len += 3;
    be_write16(canned + canned_len, 3); canned_len += 2;
    memcpy(canned + canned_len, "fix", 3); canned_len += 3;

    if (!decode_grammar_issue_at(canned, canned_len, 0, &issue) ||
        issue.start_offset != 6 || issue.span_len != 4 ||
        issue.message_len != 3 || memcmp(issue.message, "msg", 3) != 0 ||
        issue.suggestion_len != 3 || memcmp(issue.suggestion, "fix", 3) != 0) {
        ok = WP_FALSE;
    }
    if (decode_grammar_issue_at(canned, canned_len, 1, &issue)) ok = WP_FALSE; /* out of range */

    wp_document_free(&g_doc);
    g_doc_loaded = WP_FALSE;

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
        case WP_MENU_OBJ_TOOLS_SPELLCHECK:
            do_spell_check();
            break;
        case WP_MENU_OBJ_TOOLS_THESAURUS:
            do_thesaurus();
            break;
        case WP_MENU_OBJ_TOOLS_GRAMMARCHECK:
            do_grammar_check();
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

    if (g_spell_dict != NULL) {
        spell_dict_close(g_spell_dict);
        g_spell_dict = NULL;
    }

    offload_shutdown(); /* safe even if ensure_offload() was never called -- see offload_client.c */

    menu_bar(wp_menu_tree, MENU_REMOVE);
    wind_close(window_handle);
    wind_delete(window_handle);
    v_clsvwk(vdi_handle);
    appl_exit();
}
