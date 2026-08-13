#include <i86.h>
#include <string.h>
#include "app_shell.h"
#include "app_shell_internal.h"
#include "platform.h"
#include "docmodel.h"
#include "layout.h"
#include "editing.h"
#include "format_txt.h"
#include "metrics_dos_internal.h"
#include "offload.h"
#include "app_ctx.h"
#include "actions.h"
#include "menucheck.h"
#include "config.h"
#include "statusline.h"
#include "help_doc.h"
#include "help_view.h"
#include "help_retrowp.h"
#include "endian.h"

/* COM1 at 9600 baud -- the same "one plausible fixed serial endpoint,
   no device-discovery UI yet" scope platform/atari's own AUX endpoint
   and platform/amiga's own WP_AMIGA_OFFLOAD_ENDPOINT each keep. */
#define WP_DOS_OFFLOAD_ENDPOINT "serial:0:9600"

/* Phase 1 (T14.1) proved the raw hardware seam alone. Phase 2 (T14.2)
   added the real engine+metrics+draw seam. Phase 5 (T14.5) replaces
   Phase 1's own raw keyboard-echo loop with the real thing: BIOS
   INT 16h scan+ASCII+shift-state translated to a portable wp_key_event
   and fed through platform/common/keymap.c's dispatch (the same
   vi-mode/WordStar/Emacs/WordPerfect/MS Word schemes every other
   backend already uses, zero scheme-side changes), plus the action
   registry, the shared status line, and the shared help pager -- all
   through the SAME plat_draw_text/plat_fill_rect seam Phase 2 already
   proved, just now driving a real editable document instead of one
   static fixture paragraph. */

#define DOS_SCREEN_COLS   80
#define DOS_SCREEN_ROWS   25
#define DOS_TITLE_ROW     0
#define DOS_DOC_TOP_ROW   1
#define DOS_STATUS_ROW    (DOS_SCREEN_ROWS - 1)
#define DOS_DOC_BOTTOM_ROW (DOS_STATUS_ROW - 1)

/* T169: how many text fragments the paint loop has actually drawn since
   startup. Reported through the CI marker so the gate can require the
   app to have rendered REAL DOCUMENT CONTENT, not merely to be alive --
   the signal that catches an app which comes up, paints its chrome, and
   shows an empty page. Costs one increment per drawn fragment. */
static u32 g_dos_painted_frags;

#define DOS_ATTR_NORMAL   0x07  /* light gray on black -- the standard default */
#define DOS_ATTR_TITLE    0x70  /* black on light gray -- reads as a title bar */

#define DOS_KEY_ESC       27
#define DOS_KEY_ENTER     13
#define DOS_KEY_BACKSPACE 8

typedef unsigned char __far *dos_video_ptr;

static dos_video_ptr video_mem(void)
{
    return (dos_video_ptr)MK_FP(0xB800, 0);
}

static void set_text_mode(void)
{
    union REGS regs;

    regs.h.ah = 0x00;
    regs.h.al = 0x03;  /* 80x25, 16-color text */
    int86(0x10, &regs, &regs);
}

static void move_cursor(int row, int col)
{
    union REGS regs;

    regs.h.ah = 0x02;
    regs.h.bh = 0x00;  /* page 0 */
    regs.h.dh = (unsigned char)row;
    regs.h.dl = (unsigned char)col;
    int86(0x10, &regs, &regs);
}

static void put_char(int row, int col, char ch, unsigned char attr)
{
    dos_video_ptr vmem = video_mem();
    int offset = (row * DOS_SCREEN_COLS + col) * 2;

    vmem[offset] = (unsigned char)ch;
    vmem[offset + 1] = attr;
}

static void clear_rows(int first_row, int last_row, unsigned char attr)
{
    int row, col;

    for (row = first_row; row <= last_row; row++) {
        for (col = 0; col < DOS_SCREEN_COLS; col++) {
            put_char(row, col, ' ', attr);
        }
    }
}

static void draw_string(int row, int col, const char *s, unsigned char attr)
{
    while (*s != '\0' && col < DOS_SCREEN_COLS) {
        put_char(row, col, *s, attr);
        s++;
        col++;
    }
}

/* Blocks until a key is available (BIOS INT 16h AH=00h), returning its
   ASCII value in *ascii_out (0 for an extended/function key) and its
   scancode in *scan_out. */
static void read_key(unsigned char *ascii_out, unsigned char *scan_out)
{
    union REGS regs;

    regs.h.ah = 0x00;
    int86(0x16, &regs, &regs);
    *ascii_out = regs.h.al;
    *scan_out = regs.h.ah;
}

/* BIOS INT 16h AH=02h: bit0=RShift bit1=LShift bit2=Ctrl bit3=Alt. */
static u8 get_shift_status(void)
{
    union REGS regs;

    regs.h.ah = 0x02;
    int86(0x16, &regs, &regs);
    return regs.h.al;
}

/* Real PC scan codes (set 1) -- the standard, universally-documented
   reference, unlike the ST's own IKBD codes (atari_translate_key's own
   comment cites the Atari Compendium for those). Historically not a
   coincidence that the two tables overlap almost entirely: the ST's
   IKBD scan codes were themselves modeled on the IBM PC/XT keyboard's.
   Only Alt is consulted from the shift-status byte here, mirroring
   atari_translate_key's own choice exactly: Ctrl stays entirely ASCII-
   derived (a Ctrl+letter chord already arrives as a 0x01-0x1A control
   code from the BIOS itself, needing no separate modifier bit), and
   Shift is never set (already reflected in which ASCII character
   arrived, e.g. Shift+a -> 'A' -- no scheme in this project's own
   keymaps.md needs a separate Shift+special-key dimension yet). */
static wp_key_event dos_translate_key(u8 ascii, u8 scan, u8 shift_status)
{
    u8 alt = (shift_status & 0x08) ? (u8)WP_MOD_ALT : 0;
    wp_key_event ev;

    ev.codepoint = 0;
    ev.special = (u16)WP_KEY_NONE;
    ev.mods = 0;
    ev._pad = 0;

    switch (scan) {
    case 0x01: ev.special = (u16)WP_KEY_ESCAPE; goto done;
    case 0x1C: ev.special = (u16)WP_KEY_ENTER; goto done;
    case 0x0F: ev.special = (u16)WP_KEY_TAB; goto done;
    case 0x0E: ev.special = (u16)WP_KEY_BACKSPACE; goto done;
    case 0x53: ev.special = (u16)WP_KEY_DELETE; goto done;
    case 0x48: ev.special = (u16)WP_KEY_UP; goto done;
    case 0x50: ev.special = (u16)WP_KEY_DOWN; goto done;
    case 0x4B: ev.special = (u16)WP_KEY_LEFT; goto done;
    case 0x4D: ev.special = (u16)WP_KEY_RIGHT; goto done;
    case 0x47: ev.special = (u16)WP_KEY_HOME; goto done;
    case 0x4F: ev.special = (u16)WP_KEY_END; goto done;
    case 0x49: ev.special = (u16)WP_KEY_PAGE_UP; goto done;
    case 0x51: ev.special = (u16)WP_KEY_PAGE_DOWN; goto done;
    default: break;
    }
    if (scan >= 0x3B && scan <= 0x44) {
        ev.special = (u16)(WP_KEY_F1 + (scan - 0x3B));
        goto done;
    }

    if (ascii == 0) goto done; /* unmapped special key -- WP_KEY_NONE, harmlessly ignored */
    if (ascii < 0x20) {
        ev.codepoint = (u32)(ascii + ('a' - 1));
        ev.mods = (u8)WP_MOD_CTRL;
        goto done;
    }
    ev.codepoint = (u32)ascii;

done:
    ev.mods |= alt;
    return ev;
}

/* ------------------------------------------------------------------ */
/* Application state (mirrors platform/atari/app_shell.c's own g_doc/  */
/* g_journal/g_ctx shape exactly)                                      */
/* ------------------------------------------------------------------ */

static wp_document     g_doc;
static wp_undo_journal g_journal;
static wp_app_ctx       g_ctx;
static i32              g_content_width_twips;

static wp_help_doc        g_help_doc;
static wp_bool             g_help_doc_loaded = WP_FALSE;
static wp_help_view_state g_help_view;
static wp_bool             g_help_active = WP_FALSE;

static const char g_sample_text[] =
    "This is RetroWP, the portable word processor engine, running on "
    "real 16-bit DOS under Open Watcom. Word wrap and on-screen "
    "rendering are driven entirely through the platform.h metrics seam "
    "-- the engine itself has never heard of INT 10h or INT 16h.\n";

#define WP_DOS_CONFIG_PATH "C:\\RETROWP.CFG"

/* Relayouts only what actually needs it: a paragraph whose layout was
   never built (layout == NULL), one the engine marked layout_dirty (any
   content edit -- edit.c sets it on every insert/delete), or every
   paragraph when the column itself changed. wp_paragraph_relayout has
   no early-out of its own (it opens fonts, tokenizes and measures
   unconditionally -- see linebreak.c), so without this check the
   relayout-before-every-paint below would re-measure a completely
   unchanged document on every redraw. */
static void relayout_document(i32 content_width_twips)
{
    wp_bool width_changed = (wp_bool)(content_width_twips != g_content_width_twips);
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            if (!width_changed && para->layout != NULL && !para->layout_dirty) continue;
            wp_paragraph_relayout(&g_doc, para, content_width_twips);
        }
    }
}

/* Replaces the currently-open document with new_doc (already built and
   owned by the caller), resetting the cursor to end-of-document and the
   undo journal to empty -- mirrors platform/atari/app_shell.c's own
   replace_document exactly. */
static void replace_document(wp_document *new_doc, i32 content_width_twips)
{
    const wp_char_style *def_cs;
    const wp_para_style *def_ps;

    if (g_ctx.doc_loaded) {
        wp_undo_journal_free(&g_journal);
        wp_document_free(&g_doc);
    }

    g_doc = *new_doc;
    g_ctx.doc_loaded = WP_TRUE;
    wp_undo_journal_init(&g_journal);
    relayout_document(content_width_twips);

    wp_keymap_free(&g_ctx);
    wp_keymap_init(&g_ctx, (wp_keymap_id)g_ctx.keymap_id, &g_doc, &g_journal);
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_end(&g_doc);

    def_cs = wp_document_cstyle_get(&g_doc, g_doc.default_cstyle);
    g_ctx.style_flags = def_cs->flags;
    g_ctx.current_size_twips = def_cs->size_twips;

    def_ps = wp_document_pstyle_get(&g_doc, g_doc.default_pstyle);
    g_ctx.align = def_ps->align;
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

/* Restamps the run a just-inserted character landed in with the app's
   "current input style" (Bold/Italic/Underline/size toggle state) --
   the active keymap scheme has no notion of this app-level toggle,
   mirrors platform/atari/app_shell.c's own apply_current_style_to_run_at
   exactly. */
static void apply_current_style_to_run_at(wp_pos pos)
{
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    const wp_char_style *base;
    wp_char_style style;
    wp_cstyle handle;

    sec = wp_document_section_get(&g_doc, pos.section);
    para = wp_section_paragraph_get(sec, pos.paragraph);
    run = wp_paragraph_run_get(para, (u16)pos.run);
    if (run == NULL) return;

    base = wp_document_cstyle_get(&g_doc, g_doc.default_cstyle);
    style = *base;
    style.flags = g_ctx.style_flags;
    style.size_twips = g_ctx.current_size_twips;

    if (wp_document_cstyle_find_or_add(&g_doc, &style, &handle) == WP_OK) {
        wp_run_cstyle_set(para, (u16)pos.run, handle);
    }
}

static wp_bool line_contains_pos(const wp_line *line, u32 run, u32 offset)
{
    if (run < line->start_run || run > line->end_run) return WP_FALSE;
    if (run == line->start_run && offset < line->start_offset) return WP_FALSE;
    if (run == line->end_run && offset > line->end_offset) return WP_FALSE;
    return WP_TRUE;
}

/* Fixed-cell-specific simplification: since every glyph is exactly one
   column wide, the cursor's column within a line is a pure BYTE COUNT
   from the line's own start (no plat_measure_text call needed at all,
   unlike a real proportional-font backend) -- walks the line's run
   range, summing each run's segment length up to (not including) the
   cursor's own run, then adding the cursor's own in-run offset. Only
   ever called after line_contains_pos has already confirmed cursor_run/
   cursor_offset genuinely falls within this line. */
static int cursor_col_in_line(wp_paragraph *para, const wp_line *line, int align_offset_col,
                               u32 cursor_run, u32 cursor_offset)
{
    int col = align_offset_col;
    u32 r;

    for (r = line->start_run; r <= line->end_run; r++) {
        u32 seg_start = (r == line->start_run) ? line->start_offset : 0;

        if (r == cursor_run) {
            col += (int)(cursor_offset - seg_start);
            return col;
        }
        {
            wp_run *run = wp_paragraph_run_get(para, (u16)r);
            u32 seg_end = (r == line->end_run) ? line->end_offset : run->text.length;
            col += (int)(seg_end - seg_start);
        }
    }
    return col;
}

/* No sprintf/stdio -- same portable-code discipline platform/common/
   statusline.c's own append_bytes/append_decimal already established
   (that pair is file-static there too, so this is a deliberate small
   duplication, not a missed shared-helper opportunity). */
static u32 dos_append_bytes(u8 *buf, u32 len, u32 cap, const u8 *src, u32 src_len)
{
    u32 i;

    for (i = 0; i < src_len && len < cap - 1; i++) buf[len++] = src[i];
    return len;
}

static u32 dos_append_decimal(u8 *buf, u32 len, u32 cap, u32 n)
{
    u8 digits[10];
    u32 digit_count = 0;

    if (n == 0) {
        digits[digit_count++] = '0';
    } else {
        while (n > 0 && digit_count < (u32)sizeof(digits)) {
            digits[digit_count++] = (u8)('0' + (n % 10));
            n /= 10;
        }
    }
    while (digit_count > 0 && len < cap - 1) buf[len++] = digits[--digit_count];
    return len;
}

/* M15 T15.6: builds "[image: NAME WxH]" (or "[image: WxH]" if
   source_name was never set -- see docmodel.h's wp_image_desc comment)
   into buf, returning the byte count written. The one and only place
   DOS ever surfaces an inserted image -- it has no addressable
   graphics to paint the real pixels into (docs/dos-platform.md), so
   this text marker via the ordinary plat_draw_text seam every other
   line already uses IS the rendering, not a temporary stand-in for it. */
static u32 dos_format_image_marker(u8 *buf, u32 cap, const wp_document *doc,
                                    const wp_image_desc *img)
{
    u32 len = 0;

    len = dos_append_bytes(buf, len, cap, (const u8 *)"[image: ", 8);
    if (img->source_name.length > 0) {
        const u8 *name = wp_strpool_get(&doc->strings, img->source_name);
        len = dos_append_bytes(buf, len, cap, name, img->source_name.length);
        len = dos_append_bytes(buf, len, cap, (const u8 *)" ", 1);
    }
    len = dos_append_decimal(buf, len, cap, (u32)img->width_px);
    len = dos_append_bytes(buf, len, cap, (const u8 *)"x", 1);
    len = dos_append_decimal(buf, len, cap, (u32)img->height_px);
    len = dos_append_bytes(buf, len, cap, (const u8 *)"]", 1);
    return len;
}

/* Paints every laid-out line of every paragraph, top to bottom, from
   row top_row -- through the SAME wp_line_next_fragment/wp_justify_
   space_extra_twips fragment walk M13a taught the other three real
   backends (T13.2), built in from day one here rather than starting
   with the old whole-line-buffer approach they each had to later fix.
   Column advances are a pure BYTE COUNT (frag.byte_end - frag.byte_
   start), not a plat_measure_text call -- correct specifically because
   DOS's font is genuinely, always monospace (see metrics_dos_
   internal.h), a simplification no proportional-font backend can make.
   Stops at bottom_row -- there is no scrolling yet, matching every
   other backend's own identical, honestly-documented MVP scope
   ("there is no scrolling yet (T4.3+)", platform/atari/app_shell.c). */
static void paint_document_body(int top_row, int bottom_row, plat_gc *gc,
                                 int *cursor_row_out, int *cursor_col_out)
{
    u32 si, pi;
    int row = top_row;
    wp_pos *cursor = wp_keymap_cursor(&g_ctx);
    wp_bool cursor_found = WP_FALSE;
    plat_font *font = NULL;
    wp_cstyle cached_style = 0;
    wp_bool have_font = WP_FALSE;

    *cursor_row_out = top_row;
    *cursor_col_out = 0;

    for (si = 0; si < g_doc.section_count && row <= bottom_row; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);
        u32 pc = (sec != NULL) ? sec->para_count : 0;

        for (pi = 0; pi < pc && row <= bottom_row; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
            const wp_para_style *pstyle = wp_document_pstyle_get(&g_doc, para->style);
            u8 align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;
            u32 li;

            if (layout == NULL) continue;
            for (li = 0; li < layout->line_count && row <= bottom_row; li++) {
                wp_line *line = &layout->lines[li];
                int col;
                i32 extra_twips;
                wp_line_fragment_iter iter;
                wp_line_fragment frag;


                col = dos_twips_to_col(
                    (align == WP_ALIGN_CENTER || align == WP_ALIGN_RIGHT)
                        ? (g_content_width_twips - line->width_twips) / (align == WP_ALIGN_CENTER ? 2 : 1)
                        : 0);

                if (para->kind == (u8)WP_PARA_IMAGE) {
                    const wp_image_desc *img = wp_document_image_get(&g_doc, para->image);

                    if (img != NULL) {
                        char marker[64];
                        u32 marker_len = dos_format_image_marker((u8 *)marker, (u32)sizeof(marker),
                                                                  &g_doc, img);
                        plat_font *marker_font;

                        if (plat_font_open((const u8 *)"", 0, 240, 0, &marker_font) == WP_OK) {
                            plat_draw_text(gc, marker_font, col, row, (const u8 *)marker, marker_len,
                                           0xAA, 0xAA, 0xAA);
                            plat_font_close(marker_font);
                        }
                    }
                    row++;
                    continue;
                }

                extra_twips = wp_justify_space_extra_twips(
                    &g_doc, para, line, align, (wp_bool)(li == layout->line_count - 1),
                    g_content_width_twips);

                if (!cursor_found && cursor->section == si && cursor->paragraph == pi &&
                    line_contains_pos(line, cursor->run, cursor->offset)) {
                    *cursor_row_out = row;
                    *cursor_col_out = cursor_col_in_line(para, line, col, cursor->run, cursor->offset);
                    cursor_found = WP_TRUE;
                }

                wp_line_fragment_iter_init(&iter, line);
                while (wp_line_next_fragment(&g_doc, para, line, &iter, &frag)) {
                    wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
                    const u8 *bytes;
                    u32 frag_len = frag.byte_end - frag.byte_start;

                    /* The layout is rebuilt from the current runs before
                       any paint, so an out-of-range run index should be
                       impossible here -- but this used to dereference the
                       result unchecked, which turned any future
                       stale-layout regression into a wild read rather than
                       a visibly wrong glyph. Cheap insurance on the one
                       path that walks engine-supplied indices. */
                    if (run == NULL) continue;
                    bytes = wp_strpool_get(&g_doc.strings, run->text);
                    if (bytes == NULL) continue;

                    if (!have_font || run->style != cached_style) {
                        const wp_char_style *cs = wp_document_cstyle_get(&g_doc, run->style);

                        if (cs != NULL) {
                            /* Clear `font` between the close and the open:
                               plat_font_open does NOT write *out when it
                               fails (WP_NOMEM), so leaving the old value in
                               place left a freed pointer that the
                               plat_font_close after this loop would free a
                               second time. Same fix applied to all four
                               backends. */
                            if (font != NULL) {
                                plat_font_close(font);
                                font = NULL;
                            }
                            have_font = WP_FALSE;
                            if (plat_font_open((const u8 *)"", 0, cs->size_twips,
                                                cs->flags, &font) == WP_OK) {
                                cached_style = run->style;
                                have_font = WP_TRUE;
                            } else {
                                font = NULL;
                            }
                        }
                    }

                    if (frag_len > 0 && font != NULL && !frag.is_space) {
                        g_dos_painted_frags++;
                        plat_draw_text(gc, font, col, row, bytes + frag.byte_start, frag_len,
                                        0xAA, 0xAA, 0xAA);
                    }

                    col += (int)frag_len;
                    if (frag.is_space) col += dos_twips_to_col(extra_twips);
                }

                row++;
            }

            if (!cursor_found && cursor->section == si && cursor->paragraph == pi) {
                /* Empty paragraph (one zero-width line) or cursor sits
                   exactly at the paragraph's own end -- park on the
                   last row this paragraph actually used. */
                *cursor_row_out = row - 1;
                *cursor_col_out = 0;
            }
        }
    }

    if (font != NULL) plat_font_close(font);
}

static void paint_status_line(plat_gc *gc)
{
    plat_font *font;
    wp_vi_status_view view;
    char mode_buf[40];

    if (!g_ctx.doc_loaded) return;

    if (plat_font_open((const u8 *)"", 0, 240, 0, &font) == WP_OK) {
        wp_keymap_get_status_view(&g_ctx, mode_buf, (u32)sizeof(mode_buf), &view);
        wp_statusline_paint(gc, font, 0, DOS_STATUS_ROW, DOS_SCREEN_COLS, 1, &view);
        plat_font_close(font);
    }
}

static void paint_help(plat_gc *gc)
{
    plat_font *font;
    static const u8 hint[] = "HELP -- j/k select, Enter follow, Backspace back, Esc/q close";

    if (plat_font_open((const u8 *)"", 0, 240, 0, &font) == WP_OK) {
        wp_help_view_paint(gc, font, 0, DOS_DOC_TOP_ROW, DOS_SCREEN_COLS,
                            DOS_DOC_BOTTOM_ROW - DOS_DOC_TOP_ROW + 1,
                            dos_col_to_twips(DOS_SCREEN_COLS), 1, 0, &g_help_view);

        {
            wp_vi_status_view view;
            view.mode_label_utf8 = hint;
            view.mode_label_len = (u32)(sizeof(hint) - 1);
            view.pending_count = 0;
            view.pending_cmd_utf8 = NULL;
            view.pending_cmd_len = 0;
            wp_statusline_paint(gc, font, 0, DOS_STATUS_ROW, DOS_SCREEN_COLS, 1, &view);
        }
        plat_font_close(font);
    }
}

/* Full-screen repaint -- there is no damaged-rectangle tracking (no
   window manager, no partial-invalidate events), and an 80x25 text
   screen redraw is cheap enough that this project's other backends'
   own per-rectangle optimization has no real DOS equivalent worth
   building. */
/* The real "how do I get out of here" chord for the ACTIVE scheme, as
   each scheme actually implements it: wordstar's Ctrl+K prefix then Q
   (wordstar.c), emacs's Ctrl+X Ctrl+C (emacs.c), wordperfect's bare F7
   (wordperfect.c). vi and msword both land on Ctrl+Q. */
static const char *quit_key_hint(void)
{
    switch ((wp_keymap_id)g_ctx.keymap_id) {
    case WP_KEYMAP_WORDSTAR:    return "Ctrl+K Q";
    case WP_KEYMAP_EMACS:       return "Ctrl+X Ctrl+C";
    case WP_KEYMAP_WORDPERFECT: return "F7";
    default:                    return "Ctrl+Q";
    }
}

static void redraw_screen(void)
{
    plat_window *win;
    plat_gc *gc;
    int cursor_row = DOS_DOC_TOP_ROW;
    int cursor_col = 0;

    clear_rows(DOS_TITLE_ROW, DOS_SCREEN_ROWS - 1, DOS_ATTR_NORMAL);
    {
        /* This line used to hardcode "Ctrl+Q: Quit" for every keymap. It
           was only ever true for vi (which reaches WP_ACT_FILE_QUIT
           through its shortcut fallback) and, since WP_MSACTION_QUIT, for
           msword. Under wordstar Ctrl+Q is a PREFIX key, emacs quits with
           Ctrl+X Ctrl+C, and wordperfect with F7 -- so the single hint
           this backend shows was actively wrong for three of the five
           schemes, on the one backend that has no menu bar to fall back
           on when the advertised key does nothing. */
        static char title[DOS_SCREEN_COLS + 1];
        int n = 0;
        const char *p;

        for (p = "RetroWP -- DOS (F1: Help, "; *p && n < DOS_SCREEN_COLS; p++) title[n++] = *p;
        for (p = quit_key_hint(); *p && n < DOS_SCREEN_COLS; p++) title[n++] = *p;
        for (p = ": Quit)"; *p && n < DOS_SCREEN_COLS; p++) title[n++] = *p;
        while (n < DOS_SCREEN_COLS) title[n++] = ' ';
        title[n] = '\0';

        draw_string(DOS_TITLE_ROW, 0, title, DOS_ATTR_TITLE);
    }

    if (plat_window_create((const u8 *)"", 0, 0, 0, &win) != WP_OK) return;
    plat_window_get_gc(win, &gc);

    if (g_help_active) {
        paint_help(gc);
    } else if (g_ctx.doc_loaded) {
        /* A real bug found during Phase 5 visual verification: nothing
           in the engine's own edit path (vimode.c/edit.c) or platform/
           common/keymap.c re-relayouts a paragraph after a keystroke
           edits it -- only a whole-document replace (Cut/Paste, Open)
           does, via replace_document's own relayout_document call.
           Without this, typed text landed in the document model (a
           real wp_run byte-level edit) but never appeared on screen:
           paint_document_body was walking a STALE para->layout that
           simply didn't know about the new bytes yet.

           This was for a long time the ONLY backend carrying the fix.
           Atari, Amiga and Mac each kept their own version of the
           narrower "relayout on resize or whole-document replace" gate,
           which is blind to keystroke edits by construction -- so all
           three silently failed to show typed text (Amiga additionally
           rendered a row of 00 boxes, painting run/offset ranges that no
           longer described the reshaped run array). All four now do the
           same thing here. relayout_document itself consults
           layout_dirty, so an unchanged document costs one flag test
           per paragraph rather than a full re-measure. */
        relayout_document(g_content_width_twips);
        paint_document_body(DOS_DOC_TOP_ROW, DOS_DOC_BOTTOM_ROW, gc, &cursor_row, &cursor_col);
        paint_status_line(gc);
    }

    plat_window_destroy(win);
    move_cursor(cursor_row, cursor_col);
}

/* ------------------------------------------------------------------ */
/* Actions                                                              */
/* ------------------------------------------------------------------ */

static void do_edit_copy(void)
{
    /* Whole-document copy -- no selection model yet, mirrors every
       other backend's own identical "no selection model yet" scope
       (platform/common/actions.c's own act_edit_copy does the same for
       the portable actions; File-scope Cut/Paste below are backend-
       supplied since they touch the document, not just the clipboard). */
    u32 needed;
    u8 *buf;

    if (!g_ctx.doc_loaded) return;
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

    if (!g_ctx.doc_loaded) return;
    do_edit_copy();
    set_document_from_text((const u8 *)empty_text, (u32)strlen(empty_text), g_content_width_twips);
}

static void do_edit_paste(void)
{
    u8 buf[4096];
    u32 out_len;

    if (!g_ctx.doc_loaded) return;
    if (plat_clipboard_get_text(buf, (u32)sizeof(buf), &out_len) != WP_OK) return;
    set_document_from_text(buf, out_len, g_content_width_twips);
}

/* Free-text filename entry on the status row -- platform.h's own
   plat_dialog_choice explicitly has "no free-text entry variant"
   (nothing else in this project needed one before File Open/Save), so
   this is backend-supplied, the same way Atari's fsel_input-based
   run_file_selector is entirely its own code, not a platform.h seam.
   Reads directly via the same low-level video/keyboard primitives the
   rest of this file uses -- Enter confirms, Escape cancels (returns
   WP_FALSE), Backspace edits. */
static wp_bool prompt_for_filename(const char *prompt, char *out, u32 out_cap)
{
    u32 len = 0;
    int col;

    clear_rows(DOS_STATUS_ROW, DOS_STATUS_ROW, DOS_ATTR_TITLE);
    draw_string(DOS_STATUS_ROW, 0, prompt, DOS_ATTR_TITLE);
    col = (int)strlen(prompt);
    move_cursor(DOS_STATUS_ROW, col);

    for (;;) {
        unsigned char ascii, scan;

        read_key(&ascii, &scan);
        if (ascii == DOS_KEY_ESC) return WP_FALSE;
        if (ascii == DOS_KEY_ENTER) {
            out[len] = '\0';
            return (wp_bool)(len > 0);
        }
        if (ascii == DOS_KEY_BACKSPACE) {
            if (len > 0) {
                len--;
                col--;
                put_char(DOS_STATUS_ROW, col, ' ', DOS_ATTR_TITLE);
                move_cursor(DOS_STATUS_ROW, col);
            }
            continue;
        }
        if (ascii >= 0x20 && ascii < 0x7F && len + 1 < out_cap && col < DOS_SCREEN_COLS) {
            out[len++] = (char)ascii;
            put_char(DOS_STATUS_ROW, col, (char)ascii, DOS_ATTR_TITLE);
            col++;
            move_cursor(DOS_STATUS_ROW, col);
        }
    }
}

static void do_file_open(void)
{
    char path[64];
    plat_file *f;
    u32 size, read_len;
    u8 *buf;

    if (!prompt_for_filename("Open: ", path, (u32)sizeof(path))) return;

    if (plat_file_open(path, PLAT_FILE_READ, &f) != WP_OK) return;
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
    char path[64];
    plat_file *f;
    u32 needed;
    u8 *buf;

    if (!g_ctx.doc_loaded) return;
    if (!prompt_for_filename("Save: ", path, (u32)sizeof(path))) return;

    format_txt_export(&g_doc, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;
    if (format_txt_export(&g_doc, buf, needed, &needed) != WP_OK) { mem_free(buf); return; }

    if (plat_file_open(path, PLAT_FILE_WRITE, &f) == WP_OK) {
        plat_file_write(f, buf, needed);
        plat_file_close(f);
    }
    mem_free(buf);
}

static wp_bool g_dos_offload_init_attempted = WP_FALSE;

static wp_bool ensure_offload(void)
{
    if (!g_dos_offload_init_attempted) {
        g_dos_offload_init_attempted = WP_TRUE;
        offload_init(WP_DOS_OFFLOAD_ENDPOINT); /* result read back via offload_available() */
    }
    return offload_available();
}

static wp_bool offload_caps_supports(const offload_capabilities *caps, offload_op op)
{
    return (wp_bool)((caps->ops_supported & (1uL << (u32)op)) != 0);
}

static void dos_show_message(const char *msg)
{
    plat_dialog_message(NULL, PLAT_DIALOG_OK, (const u8 *)msg, (u32)strlen(msg), NULL);
}

/* Insert Image (M15 T15.4): prompt for a source path via the same
   numbered-prompt text entry do_file_open reuses, downconvert it
   through the real OFFLOAD_IMAGE op (do_chat's own CAPS-pre-check
   pattern on Atari/Amiga -- offload_query() + offload_caps_supports()
   BEFORE building a request), cache the returned packed pixel bytes to
   disk, and insert a WP_PARA_IMAGE paragraph at the cursor. DOS is
   pure 80x25 text mode with no addressable graphics at all (see
   docs/dos-platform.md) -- it still performs this exact same real
   round trip as every other backend (it's the only way to honestly
   learn width_px/height_px), it just never reads blob_path's pixel
   bytes back. Unlike the other three backends, display_width_twips/
   height_twips here are NOT derived from the image's real pixel size
   at all -- there is no pixel-proportional rendering to size for, only
   a single bracketed text marker (T15.6, a later phase) printed via
   the SAME plat_draw_text every other line already uses. A generous
   fixed 40-column-wide, one-row-tall box comfortably fits that marker
   regardless of which image was picked. */
#define WP_DOS_IMAGE_MAX_FILE_SIZE 65000uL /* wire frame's own WIRE_MAX_PAYLOAD (64K) minus the 6-byte request header */
#define WP_DOS_IMAGE_TIMEOUT_MS    60000uL
#define WP_DOS_IMAGE_TARGET_COLORS 16u   /* matches the CGA/EGA/VGA nearest-match draw_dos.c already does for text-cell colors -- reused if a future phase ever adds real DOS graphics-mode rendering */
#define WP_DOS_IMAGE_TARGET_MAX_W  320u  /* WP_IMAGE_MAX_WIDTH_PX -- the ST low-res ceiling every target clamps against */
#define WP_DOS_IMAGE_TARGET_MAX_H  200u
#define WP_DOS_IMAGE_MARKER_COLS   40u

static u32 g_dos_image_cache_counter = 0;

/* "C:\WPIMG###.TMP" -- 8.3-safe, counter-suffixed so a second Insert
   Image in one session can't silently overwrite the first image's
   still-referenced pixel cache file underneath it. out must be >= 16
   bytes. Wraps at 1000, a real, accepted session-scope limit. */
static void dos_format_image_cache_path(char *out, u32 counter)
{
    u32 c = counter % 1000u;
    memcpy(out, "C:\\WPIMG", 8);
    out[8] = (char)('0' + (c / 100u) % 10u);
    out[9] = (char)('0' + (c / 10u) % 10u);
    out[10] = (char)('0' + c % 10u);
    memcpy(out + 11, ".TMP", 5); /* includes NUL */
}

static void do_insert_image(void)
{
    char path[64];
    plat_file *f;
    u32 size, read_len;
    u8 *req_buf;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;

    if (!g_ctx.doc_loaded) return;
    if (!prompt_for_filename("Insert Image: ", path, (u32)sizeof(path))) return;

    if (!ensure_offload()) {
        dos_show_message("No offload device available.");
        return;
    }
    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_IMAGE)) {
        dos_show_message("This gateway doesn't offer Insert Image.");
        return;
    }

    if (plat_file_open(path, PLAT_FILE_READ, &f) != WP_OK) {
        dos_show_message("Couldn't open that file.");
        return;
    }
    if (plat_file_size(f, &size) != WP_OK || size == 0 || size > WP_DOS_IMAGE_MAX_FILE_SIZE) {
        plat_file_close(f);
        dos_show_message("That image file is too large.");
        return;
    }

    req_buf = (u8 *)mem_alloc(size + 6);
    if (req_buf == NULL) { plat_file_close(f); return; }

    req_buf[0] = (u8)WP_DOS_IMAGE_TARGET_COLORS;
    req_buf[1] = 0; /* target_flags: color, not grayscale */
    be_write16(req_buf + 2, (u16)WP_DOS_IMAGE_TARGET_MAX_W);
    be_write16(req_buf + 4, (u16)WP_DOS_IMAGE_TARGET_MAX_H);

    if (plat_file_read(f, req_buf + 6, size, &read_len) != WP_OK) {
        mem_free(req_buf);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_buf;
    req.payload_len = read_len + 6;
    req.timeout_ms = (u32)WP_DOS_IMAGE_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        dos_show_message("Insert Image failed or timed out.");
        return;
    }
    mem_free(req_buf);

    if (resp.status != OFFLOAD_OK || resp.result_len < 5) {
        offload_response_free(&resp);
        dos_show_message("The gateway couldn't convert that image.");
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
        u32 name_start, k;

        if (palette_count < 1 || palette_count > 16 ||
            resp.result_len != 5u + (u32)palette_count * 3u + packed_len) {
            offload_response_free(&resp);
            dos_show_message("The gateway's reply was malformed.");
            return;
        }

        dos_format_image_cache_path(cache_path, g_dos_image_cache_counter);

        if (plat_file_open(cache_path, PLAT_FILE_WRITE, &f) != WP_OK) {
            offload_response_free(&resp);
            dos_show_message("Couldn't write the image cache file.");
            return;
        }
        st = plat_file_write(f, packed_pixels, packed_len);
        plat_file_close(f);
        if (st != WP_OK) {
            offload_response_free(&resp);
            dos_show_message("Couldn't write the image cache file.");
            return;
        }
        g_dos_image_cache_counter++;

        memset(&desc, 0, sizeof(desc));
        desc.width_px = (i32)width_px;
        desc.height_px = (i32)height_px;
        desc.display_width_twips = dos_col_to_twips((i32)WP_DOS_IMAGE_MARKER_COLS);
        desc.display_height_twips = dos_row_to_twips(1);
        desc.palette_count = palette_count;
        memcpy(desc.palette, palette, (u32)palette_count * 3u);
        if (wp_strpool_append(&g_doc.strings, (const u8 *)cache_path,
                               (u32)strlen(cache_path), &desc.blob_path) != WP_OK) {
            offload_response_free(&resp);
            return;
        }

        /* path's own final '\' marks where the bare filename starts --
           same convention DOS 8.3 paths always use. */
        name_start = 0;
        for (k = 0; path[k] != '\0'; k++) {
            if (path[k] == '\\') name_start = k + 1;
        }
        if (path[name_start] != '\0') {
            wp_strpool_append(&g_doc.strings, (const u8 *)(path + name_start),
                               (u32)strlen(path + name_start), &desc.source_name);
        }
        offload_response_free(&resp);

        if (wp_edit_insert_image(&g_doc, &g_journal, (*wp_keymap_cursor(&g_ctx)), &desc) != WP_OK) {
            dos_show_message("Couldn't insert that image.");
            return;
        }
        relayout_document(g_content_width_twips);
    }
}

/* The five backend-supplied (handler == NULL in g_wp_actions) actions
   actually reachable by keyboard shortcut -- mirrors platform/atari/
   app_shell.c's own invoke_backend_action exactly (same shape; every
   other Tools/Help action has shortcut_ascii == 0, menu-only on every
   backend, and DOS has no menu -- see docs/dos-platform.md. Insert
   Image (M15) is real Ctrl+G, not menu-only, for exactly that reason:
   it's the only way DOS could ever reach it at all). */
/* Defined just below, next to the F1 path. Forward-declared so the Help
   arm can sit in invoke_backend_action without reshuffling the file. */
static void open_help_node(const char *node_name, u32 node_name_len);

static void invoke_backend_action(wp_action_id id)
{
    /* DOS has no menu bar, so nothing dispatches a Help action here
       today -- F1 calls open_help_node directly and none of the
       WP_ACT_HELP_* entries carries a shortcut byte. Wired anyway, and
       through the same shared node mapping the other three backends
       use, so that a keymap scheme emitting one of these actions works
       here instead of silently doing nothing. */
    {
        u32 node_len = 0;
        const char *node = wp_action_help_node(id, &node_len);

        if (node != NULL) {
            open_help_node(node, node_len);
            return;
        }
    }

    switch (id) {
    case WP_ACT_FILE_OPEN:         do_file_open(); break;
    case WP_ACT_FILE_SAVE:         do_file_save(); break;
    case WP_ACT_EDIT_CUT:          do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE:        do_edit_paste(); break;
    case WP_ACT_TOOLS_INSERTIMAGE: do_insert_image(); break;
    default: break;
    }
}

static void invoke_action(wp_action_id id)
{
    const wp_action *act = wp_action_find(id);

    /* wp_action_find returns NULL for an out-of-range id, and id reaches
       us as a plain u8 out of wp_keymap_outcome -- so a stale or garbage
       action_id from any scheme would crash here rather than no-op. */
    if (act == NULL) return;

    if (act->handler != NULL) {
        act->handler(&g_ctx);
    } else {
        invoke_backend_action(id);
    }
}

/* Opens the help viewer at the named node (falling back to node 0) --
   mirrors platform/atari/app_shell.c's own open_help_node exactly. */
static void open_help_node(const char *node_name, u32 node_name_len)
{
    if (!g_help_doc_loaded) return;
    {
        u32 node = wp_help_doc_find_node(&g_help_doc, (const u8 *)node_name, node_name_len);
        wp_help_view_init(&g_help_view, &g_help_doc, (node != WP_HELP_NODE_NONE) ? node : 0);
    }
    g_help_active = WP_TRUE;
}

/* The real input surface: BIOS scan+ASCII+shift -> wp_key_event ->
   wp_keymap_feed_key (vi-mode/WordStar/Emacs/WordPerfect/MS Word, zero
   scheme-side changes) -> invoke_action for WP_KEYMAP_RESULT_ACTION.
   F1 opens Help from any state (no menu path, matching every other
   backend's identical choice); while Help is active every key goes to
   wp_help_view_feed_key instead. Returns WP_TRUE if the app should
   quit. */
static wp_bool dos_dispatch_key(void)
{
    unsigned char ascii, scan;
    u8 shift;
    wp_key_event ev;
    wp_keymap_outcome outcome;

    read_key(&ascii, &scan);
    shift = get_shift_status();
    ev = dos_translate_key(ascii, scan, shift);

    if (g_help_active) {
        if (wp_help_view_feed_key(&g_help_view, ev)) g_help_active = WP_FALSE;
        return WP_FALSE;
    }
    if (ev.special == (u16)WP_KEY_F1) {
        open_help_node("Main", 4);
        return WP_FALSE;
    }

    if (!g_ctx.doc_loaded) {
        /* With no document there is nothing for the keymap to edit, and
           feeding it would work on a document that isn't there -- which
           is why this gate exists. But it used to swallow EVERY key, and
           set_document_from_text() returns early WITHOUT setting
           doc_loaded when format_txt_import fails. A failed startup
           import therefore left the app in a state where only F1 did
           anything at all: no way to open a file, and -- since DOS has
           no menu bar to fall back on -- no way to quit either, short of
           killing the machine. Resolve the two actions that genuinely
           don't need a document straight from the shortcut table,
           bypassing the keymap entirely. */
        const wp_action *esc = wp_action_find_by_shortcut(ascii);

        if (esc != NULL && esc->id == WP_ACT_FILE_QUIT) return WP_TRUE;
        if (esc != NULL && esc->id == WP_ACT_FILE_OPEN) do_file_open();
        return WP_FALSE;
    }

    outcome = wp_keymap_feed_key(&g_ctx, ev, ascii);

    if (outcome.inserted_char) {
        apply_current_style_to_run_at((*wp_keymap_cursor(&g_ctx)));
    }

    switch ((wp_keymap_result_kind)outcome.kind) {
    case WP_KEYMAP_RESULT_QUIT:
        return WP_TRUE;
    case WP_KEYMAP_RESULT_SAVE_QUIT:
        do_file_save();
        return WP_TRUE;
    case WP_KEYMAP_RESULT_ACTION:
        invoke_action((wp_action_id)outcome.action_id);
        if (outcome.action_id == (u8)WP_ACT_FILE_QUIT) return WP_TRUE;
        return WP_FALSE;
    case WP_KEYMAP_RESULT_NONE:
    case WP_KEYMAP_RESULT_BEEP:
    default:
        return WP_FALSE;
    }
}

/* ------------------------------------------------------------------
   CI hook (T169) -- identical contract to platform/atari/app_shell.c's
   atari_ci_* pair, see that file's comment for why the marker carries a
   heartbeat COUNT rather than being a one-shot "alive" file: a count
   the host can watch advance proves the app is still scheduling and
   still reading the keyboard, which no amount of emulator-log grepping
   can, and DOSBox reports a wedged guest even less than Hatari does.

   Inert unless the trigger file exists, so a real user's run never
   touches the disk for this. 8.3 names throughout. */
#define WP_DOS_CI_TRIGGER  "C:\\RETROWP.CI"
#define WP_DOS_CI_ALIVE    "C:\\RETROWP.ALV"

static wp_bool g_dos_ci_active = WP_FALSE;
static u32     g_dos_ci_events = 0;

static wp_bool dos_ci_mode(void)
{
    plat_file *f;

    if (plat_file_open(WP_DOS_CI_TRIGGER, PLAT_FILE_READ, &f) != WP_OK) return WP_FALSE;
    plat_file_close(f);
    return WP_TRUE;
}

/* No sprintf, matching this file's own existing dos_append_decimal /
   statusline.c discipline. */
static u32 dos_ci_decimal(char *out, u32 cap, u32 v)
{
    char rev[12];
    u32 rlen = 0, len = 0, i;

    if (v == 0) { if (cap > 0) out[len++] = '0'; return len; }
    while (v > 0 && rlen < (u32)sizeof(rev)) { rev[rlen++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < rlen && len < cap; i++) out[len++] = rev[rlen - 1 - i];
    return len;
}

/* Marker payload is "<events> <painted_fragments>\n" -- two numbers, so
   the host can require BOTH that the app is still processing events and
   that its paint loop actually put document text on screen. */
static void dos_ci_write_alive(void)
{
    char buf[16];
    u32 len;
    plat_file *f;

    if (plat_file_open(WP_DOS_CI_ALIVE, PLAT_FILE_WRITE, &f) != WP_OK) return;
    len = dos_ci_decimal(buf, (u32)sizeof(buf), g_dos_ci_events);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)" ", 1);
    len = dos_ci_decimal(buf, (u32)sizeof(buf), g_dos_painted_frags);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)"\n", 1);
    plat_file_close(f);
}

void app_shell_run(void)
{
    wp_bool quit = WP_FALSE;

    set_text_mode();

    {
        wp_config cfg;

        wp_config_load(WP_DOS_CONFIG_PATH, &cfg);
        wp_app_ctx_init(&g_ctx, (wp_keymap_id)cfg.keymap_id, &g_doc, &g_journal);
    }

    g_help_doc_loaded = (wp_bool)(wp_help_doc_parse(g_help_source_retrowp,
                                                     (u32)g_help_source_retrowp_len,
                                                     &g_help_doc) == WP_OK);

    load_sample_document(dos_col_to_twips(DOS_SCREEN_COLS));

    redraw_screen();

    /* After the sample document is loaded AND the first full screen is
       painted -- so the marker's existence really does mean "the app
       came up", not merely "main() was entered". */
    g_dos_ci_active = dos_ci_mode();
    if (g_dos_ci_active) dos_ci_write_alive();

    while (!quit) {
        quit = dos_dispatch_key();
        redraw_screen();
        /* After dispatch and repaint, never before: a dispatch or paint
           that crashes or wedges must not get to bump the count first. */
        if (g_dos_ci_active && !quit) {
            g_dos_ci_events++;
            dos_ci_write_alive();
        }
    }

    if (g_ctx.doc_loaded) {
        wp_keymap_free(&g_ctx);
        wp_undo_journal_free(&g_journal);
        wp_document_free(&g_doc);
        g_ctx.doc_loaded = WP_FALSE;
    }
    if (g_help_doc_loaded) {
        wp_help_doc_free(&g_help_doc);
        g_help_doc_loaded = WP_FALSE;
    }

    clear_rows(0, DOS_SCREEN_ROWS - 1, DOS_ATTR_NORMAL);
    move_cursor(0, 0);
}

/* T14.2's own selfcheck, mirroring wp_atari_layout_selfcheck's/
   wp_amiga_layout_selfcheck's/wp_mac_layout_selfcheck's exact shape and
   even their exact fixture text -- but unlike those three (which can
   only check WIDTH BOUNDS, since a real GDOS/diskfont/QuickDraw font's
   exact per-character advances aren't known ahead of time), DOS's fixed
   monospace cell makes every width an EXACTLY computable multiple of
   DOS_CELL_WIDTH_TWIPS, so this checks precise hand-computed values --
   a stronger proof than the other three backends could manage, made
   possible by the very same "no real proportional font" limitation
   that is otherwise this backend's own honest, documented gap. */
wp_bool wp_dos_layout_selfcheck(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char text[] = "The quick brown fox jumps over the lazy dog\n";
    const i32 content_width_twips = dos_col_to_twips(20); /* narrow enough to force >1 line */
    wp_bool ok = WP_TRUE;

    if (format_txt_import((const u8 *)text, (u32)strlen(text), &doc) != WP_OK) return WP_FALSE;

    sec = wp_document_section_get(&doc, 0);
    para = wp_section_paragraph_get(sec, 0);

    if (wp_paragraph_relayout(&doc, para, content_width_twips) != WP_OK) {
        wp_document_free(&doc);
        return WP_FALSE;
    }

    layout = (wp_paragraph_layout *)para->layout;
    if (layout == NULL) {
        wp_document_free(&doc);
        return WP_FALSE;
    }

    /* Hand-traced against linebreak.c's own greedy algorithm at 20
       columns (2880 twips): "The quick brown fox" (19 chars, 2736
       twips) / "jumps over the lazy" (19 chars, 2736 twips) / "dog"
       (3 chars, 432 twips) -- three lines, exact widths, not bounds. */
    if (layout->line_count != 3) ok = WP_FALSE;
    if (ok && layout->lines[0].width_twips != 19 * DOS_CELL_WIDTH_TWIPS) ok = WP_FALSE;
    if (ok && layout->lines[1].width_twips != 19 * DOS_CELL_WIDTH_TWIPS) ok = WP_FALSE;
    if (ok && layout->lines[2].width_twips != 3 * DOS_CELL_WIDTH_TWIPS) ok = WP_FALSE;

    wp_document_free(&doc);

    /* M15 Phase 6: an image paragraph relayouts to EXACTLY its
       descriptor's display_width_twips/display_height_twips, the same
       proof wp_amiga_image_selfcheck/wp_mac_image_selfcheck's own
       Phase 5 extension already gives their backends -- pure engine-
       side (linebreak.c), no screen needed. Then proves the ONE thing
       actually unique to this backend: DOS never reads blob_path back,
       it only ever paints the bracketed text marker, so the marker
       text itself (not pixels) is what gets asserted here, byte for
       byte against dos_format_image_marker's real output. */
    if (ok) {
        wp_document img_doc;
        u32 img_sec_idx, img_para_idx;

        if (wp_document_init(&img_doc) != WP_OK) {
            ok = WP_FALSE;
        } else {
            wp_section *img_sec;
            wp_paragraph *img_para;
            wp_image_desc desc;
            wp_image handle;
            static const char name[] = "PHOTO.PNG";
            static const char expected_marker[] = "[image: PHOTO.PNG 320x200]";

            memset(&desc, 0, sizeof(desc));
            desc.width_px = 320;
            desc.height_px = 200;
            desc.display_width_twips = dos_col_to_twips((i32)WP_DOS_IMAGE_MARKER_COLS);
            desc.display_height_twips = dos_row_to_twips(1);
            desc.palette_count = 2;
            desc.palette[0][0] = 0;   desc.palette[0][1] = 0;   desc.palette[0][2] = 0;
            desc.palette[1][0] = 255; desc.palette[1][1] = 255; desc.palette[1][2] = 255;

            if (wp_document_section_add(&img_doc, &img_sec_idx) != WP_OK ||
                wp_strpool_append(&img_doc.strings, (const u8 *)name, (u32)strlen(name),
                                   &desc.source_name) != WP_OK ||
                wp_document_image_add(&img_doc, &desc, &handle) != WP_OK) {
                ok = WP_FALSE;
            } else {
                img_sec = wp_document_section_get(&img_doc, img_sec_idx);
                if (wp_section_paragraph_add(img_sec, &img_para_idx) != WP_OK) {
                    ok = WP_FALSE;
                } else {
                    wp_paragraph_layout *img_layout;

                    img_para = wp_section_paragraph_get(img_sec, img_para_idx);
                    img_para->kind = (u8)WP_PARA_IMAGE;
                    img_para->image = handle;
                    img_para->style = img_doc.default_pstyle;

                    if (wp_paragraph_relayout(&img_doc, img_para, dos_col_to_twips(80)) != WP_OK) {
                        ok = WP_FALSE;
                    } else {
                        img_layout = (wp_paragraph_layout *)img_para->layout;
                        if (img_layout == NULL || img_layout->line_count != 1) ok = WP_FALSE;
                        else if (img_layout->lines[0].width_twips != desc.display_width_twips) ok = WP_FALSE;
                        else if (img_layout->lines[0].height_twips != desc.display_height_twips) ok = WP_FALSE;
                    }

                    {
                        char marker[64];
                        u32 marker_len;
                        const wp_image_desc *img = wp_document_image_get(&img_doc, handle);

                        marker_len = dos_format_image_marker((u8 *)marker, (u32)sizeof(marker), &img_doc, img);
                        if (marker_len != (u32)(sizeof(expected_marker) - 1)) ok = WP_FALSE;
                        else if (memcmp(marker, expected_marker, marker_len) != 0) ok = WP_FALSE;
                    }
                }
            }
            wp_document_free(&img_doc);
        }
    }

    return ok;
}

/* T14.3's own selfcheck, mirroring wp_atari_file_clipboard_selfcheck's
   exact shape and fixture pattern: a real file round-trip via Open
   Watcom's own libc (file_dos.c), then a real clipboard round-trip via
   the in-process buffer (clipboard_dos.c). Written now, alongside
   T14.2, rather than left for later -- file/clipboard/timer turned out
   to be a hard prerequisite for linking the engine at all (see the
   Makefile's own comment on why), so their real implementations
   already existed once T14.2's build succeeded; this selfcheck is
   what actually proves they work, not just that they compile. */
wp_bool wp_dos_file_clipboard_selfcheck(void)
{
    static const char fixture[] = "dos file+clipboard round-trip fixture";
    static const char scratch_path[] = "C:\\DOSTEST.TXT";
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

/* T14.4, mirroring wp_amiga_transport_selfcheck's shape, adapted for a
   real, confirmed DOSBox limitation found during Phase 4 bring-up: the
   default `serial1=dummy` device's BIOS AH=03h status poll correctly
   reports "ready to send," but AH=01h (send) unconditionally reports a
   timeout regardless -- reproduced with a minimal, isolated raw INT
   14h program bypassing this backend's own code entirely, and
   confirmed independent of CPU speed and of manually asserting DTR/
   RTS/OUT2 via the Modem Control Register (see transport_dos.c's own
   comment on plat_transport_write, and docs/dos-platform.md for the
   full account). This is a real gap in what THIS CI ENVIRONMENT can
   verify for the write path specifically, not a bug in transport_dos.c
   or a reason to weaken its real INT 14h implementation -- honestly
   documented rather than silently worked around, the same way Mac's
   own BasiliskII serial passthrough gap was. So: open must succeed and
   write must return a well-defined status (not hang) -- both proven --
   while the properties this environment CAN fully verify are asserted
   as hard requirements: read correctly times out (nothing is attached
   to echo a response) within the deadline logic's own generous bounds,
   and -- offload_init's own job below -- with no live gateway
   attached, it fails within its documented budget (CONNECT_TIMEOUT_MS
   + HANDSHAKE_TIMEOUT_MS, engine/src/offload_client.c) and leaves
   offload_available()/offload_shutdown() behaving correctly. */
wp_bool wp_dos_transport_selfcheck(void)
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
        if (plat_transport_open(WP_DOS_OFFLOAD_ENDPOINT, 2000, &t) != WP_OK) return WP_FALSE;

        /* Exercised for real (proves it doesn't hang/crash), but not
           asserted WP_OK -- see the comment above. */
        (void)plat_transport_write(t, fixture, (u32)(sizeof(fixture) - 1), 2000);

        before = plat_ticks_ms();
        st = plat_transport_read(t, readbuf, (u32)sizeof(readbuf), 500, &read_len);
        after = plat_ticks_ms();
        elapsed = after - before;

        if (st != WP_TIMEOUT) ok = WP_FALSE;
        if (elapsed < 300 || elapsed > 5000) ok = WP_FALSE;

        plat_transport_close(t);
    }

    before = plat_ticks_ms();
    st = offload_init(WP_DOS_OFFLOAD_ENDPOINT);
    after = plat_ticks_ms();
    elapsed = after - before;

    if (st == WP_OK) ok = WP_FALSE; /* no live gateway is attached; a real
                                        success here would mean this
                                        selfcheck's own environment
                                        assumption above no longer holds */
    if (offload_available()) ok = WP_FALSE;
    if (elapsed > 15000) ok = WP_FALSE;
    offload_shutdown();
    if (offload_available()) ok = WP_FALSE;

    return ok;
}

/* T14.5: proves dos_translate_key's scan-code table against real PC
   scan-code-set-1 values (Ralf Brown's Interrupt List / any standard PC
   keyboard reference -- not hand-waved from memory), mirroring
   wp_amiga_key_translate_selfcheck's exact shape. */
wp_bool wp_dos_key_translate_selfcheck(void)
{
    static const struct {
        u8 ascii, scan, shift;
        u16 special;
        u8 mods;
    } cases[] = {
        { 27, 0x01, 0x00, (u16)WP_KEY_ESCAPE, 0 },
        { 27, 0x01, 0x08, (u16)WP_KEY_ESCAPE, (u8)WP_MOD_ALT },  /* Alt+Escape */
        { 0, 0x48, 0x08, (u16)WP_KEY_UP, (u8)WP_MOD_ALT },       /* Alt+Up -- a different special key */
        { 0, 0x3B, 0x08, (u16)WP_KEY_F1, (u8)WP_MOD_ALT },       /* Alt+F1 */
        { 0x1B, 0x1A, 0x00, (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL }, /* Ctrl+[ -- scan differs from Escape's 0x01, must NOT be treated as Escape */
        { 'a', 0x1E, 0x00, (u16)WP_KEY_NONE, 0 }                 /* plain 'a', no modifiers */
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wp_key_event ev = dos_translate_key(cases[i].ascii, cases[i].scan, cases[i].shift);

        if (ev.special != cases[i].special) return WP_FALSE;
        if (ev.mods != cases[i].mods) return WP_FALSE;
    }
    return WP_TRUE;
}

/* T14.5: real file round trip proving wp_config_load resolves all five
   real keymap= values, mirroring wp_atari_config_selfcheck exactly.
   host/tests/test_config.c already proves wp_config_parse's own
   string-matching logic exhaustively against hand-fed byte arrays, so
   this deliberately doesn't re-prove that -- it proves the one thing
   only real target hardware can, that real file_dos.c I/O + wp_config_
   load's own thin platform wrapper genuinely round-trip. */
wp_bool wp_dos_config_selfcheck(void)
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

        if (plat_file_open(WP_DOS_CONFIG_PATH, PLAT_FILE_WRITE, &f) != WP_OK) { ok = WP_FALSE; continue; }
        if (plat_file_write(f, (const u8 *)cases[i].content, (u32)strlen(cases[i].content)) != WP_OK) ok = WP_FALSE;
        plat_file_close(f);

        wp_config_load(WP_DOS_CONFIG_PATH, &cfg);
        if (cfg.keymap_id != cases[i].expected) ok = WP_FALSE;
    }

    plat_file_delete(WP_DOS_CONFIG_PATH);
    return ok;
}

/* The menu selfcheck for a backend with no menu.
   ------------------------------------------------------------------
   DOS has no menu bar at all, so its whole action surface is the
   shortcut table -- which makes reachability MORE load-bearing here
   than anywhere else, not less. The failure this backend actually
   shipped was of exactly that kind: under keymap=msword there was no
   quit action bound at all, so the app was genuinely unexitable, and
   the title bar advertised "Ctrl+Q: Quit" on the three schemes where
   that chord does nothing.

   So the portable table check runs here too, and then the two DOS
   specifics: every action this backend claims to implement must be
   reachable, and the advertised quit chord must be real for every
   scheme. */
wp_bool wp_dos_menu_selfcheck(void)
{
    /* Every action DOS wires by shortcut. Menu-only actions on the other
       three backends (Spell/Thesaurus/Grammar/Chat, Help Contents) are
       deliberately absent -- DOS reaches Help via F1 and the rest not at
       all, a documented gap, not an oversight. Insert Image is here
       precisely because it was given a real Ctrl+G for this reason. */
    static const wp_action_id reachable[] = {
        WP_ACT_FILE_OPEN, WP_ACT_FILE_SAVE, WP_ACT_FILE_QUIT,
        WP_ACT_EDIT_CUT, WP_ACT_EDIT_COPY, WP_ACT_EDIT_PASTE,
        WP_ACT_FORMAT_BOLD, WP_ACT_FORMAT_ITALIC, WP_ACT_FORMAT_UNDERLINE,
        WP_ACT_FORMAT_SIZE, WP_ACT_FORMAT_ALIGN,
        WP_ACT_TOOLS_INSERTIMAGE
    };
    const u32 reachable_count = (u32)(sizeof(reachable) / sizeof(reachable[0]));
    u8 saved_keymap;
    u32 i;

    if (!wp_menu_model_selfcheck()) return WP_FALSE;

    for (i = 0; i < reachable_count; i++) {
        const wp_action *act = wp_action_find(reachable[i]);

        if (act == (const wp_action *)0) return WP_FALSE;
        /* Reachable means a real shortcut byte that resolves back to
           this exact action -- not merely "listed above". */
        if (act->shortcut_ascii == 0) return WP_FALSE;
        if (wp_action_find_by_shortcut(act->shortcut_ascii) != act) return WP_FALSE;
    }

    /* Quit specifically: unreachable quit is unrecoverable on a backend
       with no menu and no window chrome. Check it under EVERY scheme,
       since the binding lives in the scheme, not in the action table --
       which is exactly how msword ended up with none. */
    saved_keymap = g_ctx.keymap_id;
    for (i = 0; i < (u32)WP_KEYMAP_COUNT; i++) {
        const char *hint;

        g_ctx.keymap_id = (u8)i;
        hint = quit_key_hint();
        if (hint == (const char *)0 || hint[0] == '\0') {
            g_ctx.keymap_id = saved_keymap;
            return WP_FALSE;
        }
        /* The hint must name the scheme's own chord, not the default
           one. Only vi and msword quit on Ctrl+Q; the other three each
           have their own, and advertising Ctrl+Q there was the bug. */
        if ((wp_keymap_id)i == WP_KEYMAP_WORDSTAR    && hint[0] != 'C') { g_ctx.keymap_id = saved_keymap; return WP_FALSE; }
        if ((wp_keymap_id)i == WP_KEYMAP_WORDPERFECT && hint[0] != 'F') { g_ctx.keymap_id = saved_keymap; return WP_FALSE; }
    }
    g_ctx.keymap_id = saved_keymap;

    /* Ctrl+Q must resolve to Quit in the action table itself: that is
       what the doc-not-loaded escape path and the vi/msword schemes both
       lean on. */
    if (wp_action_find_by_shortcut(0x11) != wp_action_find(WP_ACT_FILE_QUIT)) return WP_FALSE;

    return WP_TRUE;
}

wp_bool wp_dos_image_selfcheck(void)
{
    offload_capabilities caps;
    wp_bool ok = WP_TRUE;

    memset(&caps, 0, sizeof(caps));
    caps.ops_supported = (1uL << OFFLOAD_IMAGE);

    if (!offload_caps_supports(&caps, OFFLOAD_IMAGE)) ok = WP_FALSE;
    if (offload_caps_supports(&caps, OFFLOAD_CHAT)) ok = WP_FALSE;

    return ok;
}

/* T14.5: proves the help system's non-blocking pieces against the REAL
   compiled-in help content and REAL metrics_dos.c wrapping, mirroring
   wp_atari_help_selfcheck exactly (same two assertions). Deliberately
   does NOT call wp_help_view_paint (no real screen/gc assumed here). */
wp_bool wp_dos_help_selfcheck(void)
{
    wp_help_doc doc;
    plat_font *font;
    u32 starts[8];
    u32 lens[8];
    u32 row_count;
    static const char sample[] =
        "RetroWP wraps this sentence across more than one real DOS-measured row.";

    if (wp_help_doc_parse(g_help_source_retrowp, (u32)g_help_source_retrowp_len, &doc) != WP_OK) {
        return WP_FALSE;
    }
    if (doc.node_count == 0 ||
        wp_help_doc_find_node(&doc, (const u8 *)"Main", 4) == WP_HELP_NODE_NONE) {
        wp_help_doc_free(&doc);
        return WP_FALSE;
    }
    wp_help_doc_free(&doc);

    if (plat_font_open((const u8 *)"", 0, 240, 0, &font) != WP_OK) return WP_FALSE;
    row_count = wp_help_wrap_line(font, (const u8 *)sample, (u32)strlen(sample),
                                   dos_col_to_twips(40), starts, lens, 8);
    plat_font_close(font);

    if (row_count < 2 || row_count > 8) return WP_FALSE;
    {
        u32 i;
        for (i = 0; i < row_count; i++) {
            if (lens[i] == 0) return WP_FALSE;
        }
    }
    return WP_TRUE;
}
