#include <gem.h>
#include <osbind.h>
#include <string.h>
#include "types.h"
#include "platform.h"
#include "docmodel.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "format_ws.h"
#include "layout.h"
#include "editing.h"
#include "app_shell.h"
#include "app_shell_internal.h"
#include "metrics_atari_internal.h"
#include "mem_atari.h"
#include "spell.h"
#include "grammar.h"
#include "offload.h"
#include "endian.h"
#include "keyevent.h"
#include "vimode.h"
#include "keymap.h"
#include "config.h"
#include "app_ctx.h"
#include "actions.h"
#include "menucheck.h"
#include "statusline.h"
#include "help_doc.h"
#include "help_view.h"
#include "help_retrowp.h"

/* Hand-built menu bar OBJECT tree. There is no resource editor or .rsc
   file anywhere in this project's toolchain (no ResourceMaster/
   Human68k-style tool, no packaged .rsc -- see docs/atari-platform.md),
   so a static hand-built OBJECT[] is the practical choice; .rsc loading
   (rsrc_load) remains the better tool for anything with real dialog
   layout (multi-field forms), which this project doesn't need yet.

   ob_next/ob_head/ob_tail are OBJECT-ARRAY INDICES, not pointers.
   Convention (verified against a running AES under Hatari):
     - ob_head/ob_tail on a parent bound its child sibling chain
       (first/last child index).
     - ob_next on a non-last sibling is the next sibling's index.
     - ob_next on the LAST sibling in a chain wraps back to its OWN
       PARENT's index (there is no -1 terminator inside a chain).
     - Only the true root (no parent) uses ob_next = -1.
   menu_bar() does NOT compute ob_x/ob_y/ob_width/ob_height from the
   string content -- those must be set by hand, in character-cell units
   relative to the parent (screen-relative only for the root).

   4-LEVEL shape, grounded in the Atari Compendium's documented menu-bar
   contract (see app_shell_internal.h's block comment for the full
   derivation and the two reverted flatter attempts this replaces):

     ROOT (G_IBOX)
      |-- BAR (G_BOX) -- ROOT's ob_head
      |    `-- ACTIVE (G_BOX) -- BAR's only child
      |         `-- Desk/File/Edit/Tools/Format G_TITLEs, in on-screen order
      `-- DROPDOWNS (G_IBOX) -- ROOT's ob_tail
           |-- Desk's dropdown G_BOX (8 children -- see below) -- DROPDOWNS's ob_head
           |-- File's dropdown G_BOX (Open/Save/Quit)
           |-- Edit's dropdown G_BOX (Cut/Copy/Paste)
           |-- Tools's dropdown G_BOX (Spell/Thesaurus/Grammar/Chat)
           `-- Format's dropdown G_BOX (Bold/.../Align Right) -- DROPDOWNS's ob_tail

   Each dropdown G_BOX's ob_x matches its own title's ob_x (screen-relative,
   since DROPDOWNS itself starts at x=0) so it falls directly underneath.
   menu_bar(MENU_INSTALL)'s internal ob_draw(tree, 1, MAX_DEPTH) walks only
   BAR's subtree (BAR->ACTIVE->titles) -- DROPDOWNS is a sibling of BAR
   under ROOT, so it is never install-time-painted by that call, which is
   what fixes both the "only one title renders" bug (all titles are now
   descendants of index 1) and the old dropdown "glyph noise" bug (dropdowns
   are no longer descendants of index 1 at all). OF_HIDETREE used to be
   kept on every dropdown box anyway, "defensively -- cheap, and not proven
   unnecessary". It was neither cheap nor defensive: it is what stopped the
   menus working at all. The AES does NOT clear OF_HIDETREE when it drops a
   menu down -- objc_draw skips a hidden subtree unconditionally, so a
   pre-hidden box can never paint. Confirmed empirically under Hatari: with
   the flag set, hovering File/Edit/Tools/Format highlighted the title and
   dropped nothing; clearing it made all four render correctly, with no
   return of the install-time "glyph noise" the flag was there to guard
   against -- because the 4-level shape above already makes that
   structurally impossible (ob_draw(tree, 1, ...) cannot reach DROPDOWNS).
   The Desk menu was the one exception that DID work with the flag set,
   which is what made this so easy to miss: the AES rebuilds the Desk box
   itself at MENU_INSTALL time (menu_fixup) and resets its flags.

   Desk's dropdown box is deliberately real (the old flat trees shipped
   with none at all): exactly 8 children per the Compendium -- an
   app-defined "About RetroWP..." item (opens the help viewer's About
   node), a disabled separator, and 6 AES-reserved dummy slots the AES
   overwrites at MENU_INSTALL time to list running accessories/apps. */
/* ob_spec on a container G_BOX/G_IBOX is NOT a string pointer -- it is a
   packed color/border word. Atari Compendium, "The Object-Specific Field"
   (p.6.18): G_BOX and G_IBOX both hold "a WORD containing color
   information" in the low 16 bits, with "a signed BYTE representing the
   border thickness of the box" in bits 23-16. The color word's own layout
   (same page, OBJC_COLORWORD) is:

     bits 15-12 borderc | 11-8 textc | 7 opaque | 6-4 pattern | 3-0 fillc

   and the color indices (p.6.19) put WHITE at 0, BLACK at 1.

   These were left at the array literal's {0} until now: wp_menu_tree_init
   only ever patched ob_spec.free_string on the G_TITLE/G_STRING objects,
   so every container kept ob_spec == 0. Decoded through the layout above
   that is border thickness 0 (no frame at all), bit 7 clear (TRANSPARENT,
   so the interior is never filled), and white-on-white. A dropdown box
   drawn that way paints no background and no frame -- its item strings
   land directly on top of whatever the window was already showing, and
   nothing erases the document underneath. That is the real cause of the
   "menus do nothing but corrupt the screen" report: the AES was dropping
   each menu down correctly the whole time, into an invisible box. */
#define WP_MENU_BOX_OBSPEC  0x00FF1180L /* thickness -1, black border, black
                                            text, opaque, solid white fill */
#define WP_MENU_BAR_OBSPEC  0x00000180L /* thickness 0 (the bar has no frame
                                            of its own), black text, opaque,
                                            solid white fill */

OBJECT wp_menu_tree[WP_MENU_OBJ_COUNT] = {
    /* next,                    head,                   tail,                   type,     flags,       state,       spec, x,  y, w,  h */
    { -1,                        WP_MENU_OBJ_BAR,         WP_MENU_OBJ_DROPDOWNS,  G_IBOX,   OF_NONE,     OS_NORMAL,   {0}, 0,  0, 40, 1 },
    { WP_MENU_OBJ_DROPDOWNS,     WP_MENU_OBJ_ACTIVE,      WP_MENU_OBJ_ACTIVE,     G_BOX,    OF_NONE,     OS_NORMAL,   {0}, 0,  0, 40, 1 },
    { WP_MENU_OBJ_BAR,           WP_MENU_OBJ_DESK_TITLE,  WP_MENU_OBJ_HELP_TITLE,   G_BOX,  OF_NONE,     OS_NORMAL,   {0}, 0,  0, 40, 1 },
    { WP_MENU_OBJ_FILE_TITLE,    -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 0,  0, 6,  1 },
    { WP_MENU_OBJ_EDIT_TITLE,    -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 6,  0, 6,  1 },
    { WP_MENU_OBJ_TOOLS_TITLE,   -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 12, 0, 6,  1 },
    { WP_MENU_OBJ_FORMAT_TITLE,  -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 18, 0, 7,  1 },
    { WP_MENU_OBJ_HELP_TITLE,    -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 25, 0, 8,  1 },
    { WP_MENU_OBJ_ACTIVE,        -1,                      -1,                     G_TITLE,  OF_NONE,     OS_NORMAL,   {0}, 33, 0, 6,  1 },
    { WP_MENU_OBJ_ROOT,          WP_MENU_OBJ_DESK_BOX,    WP_MENU_OBJ_HELP_BOX,   G_IBOX,   OF_NONE,     OS_NORMAL,   {0}, 0,  1, 40, 8 },
    { WP_MENU_OBJ_FILE_BOX,      WP_MENU_OBJ_DESK_ABOUT,  WP_MENU_OBJ_DESK_DUMMY6, G_BOX,   OF_NONE    , OS_NORMAL,   {0}, 0,  0, 20, 8 },
    { WP_MENU_OBJ_DESK_SEP,      -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  0, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY1,   -1,                      -1,                     G_STRING, OF_NONE,     OS_DISABLED, {0}, 0,  1, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY2,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY3,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  3, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY4,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  4, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY5,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  5, 20, 1 },
    { WP_MENU_OBJ_DESK_DUMMY6,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  6, 20, 1 },
    { WP_MENU_OBJ_DESK_BOX,      -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  7, 20, 1 },
    { WP_MENU_OBJ_EDIT_BOX,      WP_MENU_OBJ_FILE_NEW,    WP_MENU_OBJ_FILE_QUIT,  G_BOX,    OF_NONE    , OS_NORMAL,   {0}, 6,  0, 8,  4 },
    { WP_MENU_OBJ_FILE_SAVE,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  1, 8,  1 },
    { WP_MENU_OBJ_FILE_QUIT,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 8,  1 },
    { WP_MENU_OBJ_FILE_BOX,      -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  3, 8,  1 },
    { WP_MENU_OBJ_TOOLS_BOX,     WP_MENU_OBJ_EDIT_UNDO,   WP_MENU_OBJ_EDIT_PASTE, G_BOX,    OF_NONE    , OS_NORMAL,   {0}, 12, 0, 8,  4 },
    { WP_MENU_OBJ_EDIT_COPY,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  1, 8,  1 },
    { WP_MENU_OBJ_EDIT_PASTE,    -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 8,  1 },
    { WP_MENU_OBJ_EDIT_BOX,      -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  3, 8,  1 },
    { WP_MENU_OBJ_FORMAT_BOX,    WP_MENU_OBJ_TOOLS_SPELLCHECK, WP_MENU_OBJ_TOOLS_CHAT, G_BOX, OF_NONE    , OS_NORMAL, {0}, 18, 0, 16, 4 },
    { WP_MENU_OBJ_TOOLS_THESAURUS, -1,                    -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  0, 16, 1 },
    { WP_MENU_OBJ_TOOLS_GRAMMARCHECK, -1,                 -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  1, 16, 1 },
    { WP_MENU_OBJ_TOOLS_CHAT,    -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 16, 1 },
    { WP_MENU_OBJ_TOOLS_BOX,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  3, 16, 1 },
    { WP_MENU_OBJ_HELP_BOX,      WP_MENU_OBJ_FORMAT_BOLD, WP_MENU_OBJ_FORMAT_ALIGN_RIGHT, G_BOX, OF_NONE    , OS_NORMAL, {0}, 25, 0, 16, 8 },
    { WP_MENU_OBJ_FORMAT_ITALIC, -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  0, 16, 1 },
    { WP_MENU_OBJ_FORMAT_UNDERLINE, -1,                   -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  1, 16, 1 },
    { WP_MENU_OBJ_FORMAT_SIZE_UP, -1,                     -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 16, 1 },
    { WP_MENU_OBJ_FORMAT_SIZE_DOWN, -1,                   -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  3, 16, 1 },
    { WP_MENU_OBJ_FORMAT_ALIGN_LEFT, -1,                  -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  4, 16, 1 },
    { WP_MENU_OBJ_FORMAT_ALIGN_CENTER, -1,                -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  5, 16, 1 },
    { WP_MENU_OBJ_FORMAT_ALIGN_RIGHT, -1,                 -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  6, 16, 1 },
    { WP_MENU_OBJ_FORMAT_BOX,    -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  7, 16, 1 },
    /* Help's dropdown box and items. The TITLE is NOT here -- it sits up
       with the other titles at array index 8, and it has to: see
       WP_MENU_OBJ_HELP_TITLE's own comment for the index arithmetic the
       AES does. Box x=33 repeats its title's x so it falls directly
       underneath, the same rule every other box follows. OF_LASTOB lives
       on the final array entry and nowhere else -- it moved off Align
       Right when these were added. */
    { WP_MENU_OBJ_DROPDOWNS,     WP_MENU_OBJ_HELP_CONTENTS, WP_MENU_OBJ_HELP_FORMAT, G_BOX, OF_NONE,     OS_NORMAL,   {0}, 33, 0, 15, 3 },
    { WP_MENU_OBJ_HELP_KEYS,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  0, 15, 1 },
    { WP_MENU_OBJ_HELP_FORMAT,   -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  1, 15, 1 },
    { WP_MENU_OBJ_HELP_BOX,      -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  2, 15, 1 },
    /* File > New, appended (see WP_MENU_OBJ_FILE_NEW). It is the FIRST
       child of the File box, so its ob_next is Open, not the box -- only
       the LAST sibling in a chain wraps back to its parent. OF_LASTOB
       lives here now, on the final array entry, and nowhere else. */
    { WP_MENU_OBJ_FILE_OPEN,     -1,                      -1,                     G_STRING, OF_NONE,     OS_NORMAL,   {0}, 0,  0, 8,  1 },
    /* Edit > Undo, appended for the same reason File > New above was:
       array position is nothing to the AES, and renumbering is what has
       broken this tree twice. First child of the Edit box, so its
       ob_next is Cut. OF_LASTOB lives here now. */
    { WP_MENU_OBJ_EDIT_CUT,      -1,                      -1,                     G_STRING, OF_LASTOB,   OS_NORMAL,   {0}, 0,  0, 8,  1 }
};

/* T169: how many text fragments the paint loop has actually drawn since
   startup. Reported through the CI marker so the gate can require the
   app to have rendered REAL DOCUMENT CONTENT, not merely to be alive --
   the signal that catches an app which comes up, paints its chrome and
   then shows an empty page. That is not hypothetical: the DOS backend
   was found doing exactly this while this gate was being built. */
static u32 g_atari_painted_frags;

static char g_text_desk[]  = " Desk ";
static char g_text_file[]  = " File ";
static char g_text_edit[]  = " Edit ";
static char g_text_tools[] = " Tools ";
static char g_text_new[]   = "  New";
static char g_text_open[]  = "  Open";
static char g_text_save[]  = "  Save";
static char g_text_quit[]  = "  Quit";
static char g_text_undo[]  = "  Undo";
static char g_text_cut[]   = "  Cut";
static char g_text_copy[]  = "  Copy";
static char g_text_paste[] = "  Paste";
static char g_text_spellcheck[] = "  Spell Check";
static char g_text_thesaurus[] = "  Thesaurus";
static char g_text_grammarcheck[] = "  Grammar Check";
static char g_text_chat[] = "  Chat";
static char g_text_format[] = " Format ";
static char g_text_bold[] = "  Bold";
static char g_text_italic[] = "  Italic";
static char g_text_underline[] = "  Underline";
static char g_text_size_up[] = "  Size +";
static char g_text_size_down[] = "  Size -";
static char g_text_align_left[] = "  Align Left";
static char g_text_align_center[] = "  Align Center";
static char g_text_align_right[] = "  Align Right";
static char g_text_help[] = " Help ";
/* Labels come from the shared registry (g_wp_actions[].label_utf8), not
   from these buffers -- these carry this backend's own two-space menu-
   item indent, which is a GEM presentation convention the portable table
   has no business knowing about. wp_menu_tree_selfcheck asserts the
   registry label appears inside each one, so a label renamed in
   actions.c cannot leave a stale string sitting in the ST's menu. */
static char g_text_help_contents[] = "  Contents";
static char g_text_help_keys[] = "  Editing Keys";
static char g_text_help_format[] = "  Format Keys";

/* Desk dropdown: an app-defined item, a disabled separator, then 6
   AES-reserved dummy slots (see app_shell_internal.h's block comment --
   EmuTOS's real menu_fixup() rebuilds this box's 8-child chain and
   REPOINTS each dummy's ob_spec at its own internal accessory-title
   string, confirmed by reading aes/gemmnlib.c directly; it does not
   write through the pointer already there). Kept as writable static
   buffers anyway rather than string literals -- that repoint-not-write
   behavior was only confirmed for EmuTOS, not real TOS's own AES. */
static char g_text_desk_about[] = "  About RetroWP...";
static char g_text_desk_sep[]   = "  ----------------";
static char g_text_desk_dummy1[32];
static char g_text_desk_dummy2[32];
static char g_text_desk_dummy3[32];
static char g_text_desk_dummy4[32];
static char g_text_desk_dummy5[32];
static char g_text_desk_dummy6[32];

/* Where a dropdown box actually goes, clamped so its right edge never
   leaves the screen.
 *
   This is Atari's OWN rule, from the Resource Construction Set that
   generated essentially every GEM menu ever shipped -- RCSMENU.C's
   fix_menu_bar():

       x1 = x + 2 * gl_wchar;
       x2 = full.g_w - tree[menu_obj].ob_width;
       tree[menu_obj].ob_x = min(x1, x2);

   It is not cosmetic, and skipping it is not a layout bug -- it CRASHES
   the machine. TOS's own menu handler saves the screen under a dropdown
   before drawing it (aes/gemmnlib.c, menu_sr -> bb_save) and does NOT
   clip that rectangle to the screen first; it trusts the tree to have
   been built by a tool that already did. A box hanging off the right
   edge therefore makes the AES copy memory past the end of the
   framebuffer: screen corruption, then bombs, on a real Falcon, while
   merely traversing the menu bar rightwards.

   This tree is authored in CHARACTER CELLS and scaled by wcell at
   runtime, which is exactly how it gets into trouble: the Help title is
   at column 33 with a 15-wide box, so the menu bar needs 48 columns to
   contain its own dropdowns. At 640x480 with an 8px font that is 80
   columns and everything fits; halve the column count and Format and
   Help both hang off the edge.

   Kept as a separate function, taking its inputs rather than reading the
   tree, so wp_atari_menu_selfcheck can prove it on the real failing
   geometry without needing a screen (SMOKE.PRG never opens one).

   The 2-cell indent is RCS's too, and it is NOT cosmetic -- it was
   dropped once here on the grounds that the Compendium's prose says a
   box falls "underneath its corresponding G_TITLE", and that was wrong.
   menu_sr() subtracts MTH (GEMLIB.H: 1 pixel) from the rect's x before
   handing it to bb_save, so a box at x=0 -- which is exactly where Desk
   sits when boxes are flush with their titles -- produces x = -1 and
   reads off the left end of the framebuffer. Atari's indent is what
   keeps the leftmost dropdown clear of that edge. Both halves of
   min(x + 2*wcell, screen_w - box_w) guard an edge; take either away and
   one edge is unguarded. */
short wp_menu_dropdown_x(short title_x, short box_w, short screen_w,
                         short wcell)
{
    short want  = (short)(title_x + 2 * wcell);
    short limit = (short)(screen_w - box_w);

    if (limit < 0) limit = 0;           /* box wider than the screen */
    return (want < limit) ? want : limit;
}

/* What wp_menu_tree_init was last given. The placement selfcheck below
   re-derives every box position from these rather than hardcoding an
   alignment rule, which is what let a stale "flush with its title"
   assertion outlive the rule it was asserting. Zero until init runs. */
static short g_menu_screen_w;
static short g_menu_wcell;

void wp_menu_tree_init(short wcell, short hcell,
                        short screen_w_px, short screen_h_px)
{
    short i;

    g_menu_screen_w = screen_w_px;
    g_menu_wcell    = wcell;

    wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_spec.free_string = g_text_desk;
    wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_spec.free_string = g_text_file;
    wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_spec.free_string = g_text_edit;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_spec.free_string = g_text_tools;
    wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_spec.free_string = g_text_new;
    wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_spec.free_string = g_text_open;
    wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_spec.free_string = g_text_save;
    wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string = g_text_quit;
    wp_menu_tree[WP_MENU_OBJ_EDIT_UNDO].ob_spec.free_string = g_text_undo;
    wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_spec.free_string = g_text_cut;
    wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_spec.free_string = g_text_copy;
    wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string = g_text_paste;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_spec.free_string = g_text_spellcheck;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_spec.free_string = g_text_thesaurus;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_spec.free_string = g_text_grammarcheck;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_CHAT].ob_spec.free_string = g_text_chat;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_spec.free_string = g_text_format;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_BOLD].ob_spec.free_string = g_text_bold;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_ITALIC].ob_spec.free_string = g_text_italic;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_UNDERLINE].ob_spec.free_string = g_text_underline;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_UP].ob_spec.free_string = g_text_size_up;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_DOWN].ob_spec.free_string = g_text_size_down;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_LEFT].ob_spec.free_string = g_text_align_left;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_CENTER].ob_spec.free_string = g_text_align_center;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_RIGHT].ob_spec.free_string = g_text_align_right;
    wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_spec.free_string = g_text_help;
    wp_menu_tree[WP_MENU_OBJ_HELP_CONTENTS].ob_spec.free_string = g_text_help_contents;
    wp_menu_tree[WP_MENU_OBJ_HELP_KEYS].ob_spec.free_string = g_text_help_keys;
    wp_menu_tree[WP_MENU_OBJ_HELP_FORMAT].ob_spec.free_string = g_text_help_format;
    wp_menu_tree[WP_MENU_OBJ_DESK_ABOUT].ob_spec.free_string = g_text_desk_about;
    wp_menu_tree[WP_MENU_OBJ_DESK_SEP].ob_spec.free_string = g_text_desk_sep;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY1].ob_spec.free_string = g_text_desk_dummy1;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY2].ob_spec.free_string = g_text_desk_dummy2;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY3].ob_spec.free_string = g_text_desk_dummy3;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY4].ob_spec.free_string = g_text_desk_dummy4;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY5].ob_spec.free_string = g_text_desk_dummy5;
    wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY6].ob_spec.free_string = g_text_desk_dummy6;

    /* Containers take a color/border WORD, not a string pointer -- see
       WP_MENU_BOX_OBSPEC's own comment above for the Compendium-documented
       layout and for why leaving these at {0} made every dropdown paint as
       an invisible, non-erasing box. ROOT and DROPDOWNS stay 0: they are
       G_IBOXes that exist only to group, and are meant to draw nothing.
       ACTIVE stays 0 too -- it exactly overlays BAR, which now fills. */
    wp_menu_tree[WP_MENU_OBJ_BAR].ob_spec.index        = WP_MENU_BAR_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_spec.index   = WP_MENU_BOX_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_spec.index   = WP_MENU_BOX_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_spec.index   = WP_MENU_BOX_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_spec.index  = WP_MENU_BOX_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_spec.index = WP_MENU_BOX_OBSPEC;
    wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_spec.index    = WP_MENU_BOX_OBSPEC;

    /* Scale the whole tree from the array literal's logical
       character-cell units into real pixels -- see this function's own
       declaration comment (app_shell_internal.h) for why a hand-built
       tree needs this done explicitly. Every object needs it (titles,
       boxes, and items alike), not just titles -- ROOT's own
       screen-sized w/h included. */
    for (i = 0; i < WP_MENU_OBJ_COUNT; i++) {
        wp_menu_tree[i].ob_x = (short)(wp_menu_tree[i].ob_x * wcell);
        wp_menu_tree[i].ob_width = (short)(wp_menu_tree[i].ob_width * wcell);
        wp_menu_tree[i].ob_y = (short)(wp_menu_tree[i].ob_y * hcell);
        wp_menu_tree[i].ob_height = (short)(wp_menu_tree[i].ob_height * hcell);
    }

    /* The array literal authors every container at a nominal 40 cells
       wide and ROOT at 1 cell tall. Both are wrong on a real ST screen,
       and the height one is why dropdowns never appeared at all: at
       640x200/640x400 the bar spans only HALF the display, and ROOT --
       1 cell tall -- does not contain DROPDOWNS, which starts at y=1
       and runs 8 cells further down. A child that lies outside its own
       parent's rectangle has nothing to draw into. The Compendium's
       menu-bar contract (Ch.6, "The Menu Bar") wants ROOT screen-sized,
       BAR/ACTIVE spanning the full width, and DROPDOWNS covering
       everything below the bar.

       Done here in real pixels, AFTER the cell scaling above, because a
       runtime screen size cannot be expressed in the compile-time array
       literal -- the same reason the string pointers are patched in
       rather than initialized. */
    wp_menu_tree[WP_MENU_OBJ_ROOT].ob_x = 0;
    wp_menu_tree[WP_MENU_OBJ_ROOT].ob_y = 0;
    wp_menu_tree[WP_MENU_OBJ_ROOT].ob_width = screen_w_px;
    wp_menu_tree[WP_MENU_OBJ_ROOT].ob_height = screen_h_px;

    wp_menu_tree[WP_MENU_OBJ_BAR].ob_width = screen_w_px;
    wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_width = screen_w_px;

    wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_width = screen_w_px;
    wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_height = (short)(screen_h_px - hcell);

    /* Bar and active-area heights, straight from RCSMENU.C's
       fix_menu_bar() -- the "+2 pixels for a border line" the
       Compendium's menu-bar contract also asks for (Ch.6, "The Menu
       Bar"), plus the extra row the active area gets. Both were exactly
       hcell here, which is a cell short of what the AES lays out for. */
    wp_menu_tree[WP_MENU_OBJ_BAR].ob_height    = (short)(hcell + 2);
    wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_height = (short)(hcell + 3);

    /* Now clamp every dropdown onto the screen. Titles and boxes are
       paired in menu-bar order; see wp_menu_dropdown_x above for why
       this is load-bearing rather than tidy. */
    {
        static const short pairs[] = {
            WP_MENU_OBJ_DESK_TITLE,   WP_MENU_OBJ_DESK_BOX,
            WP_MENU_OBJ_FILE_TITLE,   WP_MENU_OBJ_FILE_BOX,
            WP_MENU_OBJ_EDIT_TITLE,   WP_MENU_OBJ_EDIT_BOX,
            WP_MENU_OBJ_TOOLS_TITLE,  WP_MENU_OBJ_TOOLS_BOX,
            WP_MENU_OBJ_FORMAT_TITLE, WP_MENU_OBJ_FORMAT_BOX,
            WP_MENU_OBJ_HELP_TITLE,   WP_MENU_OBJ_HELP_BOX
        };
        short n = (short)(sizeof(pairs) / sizeof(pairs[0]));
        short p;

        for (p = 0; p < n; p += 2) {
            short title = pairs[p];
            short box   = pairs[p + 1];

            wp_menu_tree[box].ob_x =
                wp_menu_dropdown_x(wp_menu_tree[title].ob_x,
                                   wp_menu_tree[box].ob_width,
                                   screen_w_px, wcell);
        }
    }
}

/* Walks every parent's child chain (ob_head -> ob_next -> ... ->
   ob_tail) with a step budget bounded by WP_MENU_OBJ_COUNT and fails if
   the chain doesn't reach ob_tail within budget -- catches an
   accidental ob_next loop or a chain that never reaches its declared
   tail, either of which a hand-edited renumbering can introduce
   silently (no compiler error, just a hang the moment AES tries to
   walk it for real -- exactly the failure class the prior 3-level
   restructuring attempt is suspected to have hit). Host-testable, no
   Hatari needed. */
static wp_bool child_chain_ok(short parent)
{
    short head = wp_menu_tree[parent].ob_head;
    short tail = wp_menu_tree[parent].ob_tail;
    short cur = head;
    int steps;

    if (head == -1 && tail == -1) return WP_TRUE; /* no children, fine */
    if (head == -1 || tail == -1) return WP_FALSE; /* one set, not both */

    for (steps = 0; steps <= WP_MENU_OBJ_COUNT; steps++) {
        if (cur == tail) return WP_TRUE;
        if (cur < 0 || cur >= WP_MENU_OBJ_COUNT) return WP_FALSE; /* out of bounds */
        cur = wp_menu_tree[cur].ob_next;
    }
    return WP_FALSE; /* budget exhausted -- cycle or chain never reaches tail */
}

/* Number of selectable leaf items across all six dropdowns. The Desk
   separator and the six AES-reserved dummy slots are leaves too but are
   never dispatched, so they are excluded here and counted separately.

   Still a hard-coded constant, but it now guards less than it used to:
   dispatch is a table (g_menu_actions) rather than a hand-written
   switch, and menu_action_map_selfcheck walks that table against both
   the tree and the registry. What this count still catches is the case
   neither of those can -- a leaf added to the tree that is in NEITHER
   the table nor the local-item list, i.e. drawn, highlightable, and
   silently inert. Bumping it remains the prompt to go wire the item up. */
#define WP_ATARI_MENU_TITLES              6   /* Desk File Edit Tools Format Help */
#define WP_ATARI_MENU_DISPATCHED_LEAVES  24   /* About + 4 File + 4 Edit + 4 Tools + 8 Format + 3 Help */
#define WP_ATARI_MENU_INERT_LEAVES        7   /* separator + 6 AES-reserved Desk slots */

/* Defined next to the dispatch table it validates, far below -- this
   selfcheck runs it as its last step so a tree that is structurally
   perfect but wired to nothing still fails. */
static wp_bool menu_action_map_selfcheck(void);

/* A dropdown is correctly placed if it is exactly where
   wp_menu_dropdown_x would put it AND it lies wholly on screen. The
   second half is the one that matters: TOS's menu_sr() hands this rect
   to bb_save() after growing it by MTH on each side, without clipping,
   so a box off either edge makes the AES copy memory outside the
   framebuffer. Before init has run there is no screen to check against,
   so the geometry is not yet meaningful and the check passes. */
static wp_bool menu_box_placed_ok(short title, short box)
{
    short x, w;

    if (g_menu_screen_w <= 0 || g_menu_wcell <= 0) return WP_TRUE;

    x = wp_menu_tree[box].ob_x;
    w = wp_menu_tree[box].ob_width;

    if (x != wp_menu_dropdown_x(wp_menu_tree[title].ob_x, w,
                                g_menu_screen_w, g_menu_wcell))
        return WP_FALSE;

    if (x < 0) return WP_FALSE;                       /* off the left  */
    if ((short)(x + w) > g_menu_screen_w) return WP_FALSE;  /* off the right */

    return WP_TRUE;
}

wp_bool wp_menu_tree_selfcheck(void)
{
    /* OF_LASTOB means "last object in the TREE", singular -- Atari
       Compendium p.6.16 states it outright: "This flag signals to the
       AES that the current OBJECT is the last in the object tree.
       (Required!)". This tree used to set it on the last CHILD of every
       dropdown box (5 objects), and this selfcheck asserted that as
       correct, so it could never catch it. The AES stops its linear
       scan of the array at the first OF_LASTOB it finds, which sat at
       WP_MENU_OBJ_DESK_DUMMY6 (17) -- everything after it, i.e. the
       File/Edit/Tools/Format dropdown boxes at 18..39, was invisible to
       the AES. Verified on a real Hatari boot: only the Desk menu
       dropped down; every other title highlighted on hover and then did
       nothing. Checked here as a whole-array count rather than as
       per-object assertions so the "exactly one, and it is the final
       object" invariant cannot drift again. */
    {
        short i;
        short lastob_count = 0;

        for (i = 0; i < WP_MENU_OBJ_COUNT; i++) {
            if (wp_menu_tree[i].ob_flags & OF_LASTOB) lastob_count++;
        }
        if (lastob_count != 1) return WP_FALSE;
        if ((wp_menu_tree[WP_MENU_OBJ_COUNT - 1].ob_flags & OF_LASTOB) == 0) return WP_FALSE;
    }

    if (!child_chain_ok(WP_MENU_OBJ_ROOT)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_BAR)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_ACTIVE)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_DROPDOWNS)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_DESK_BOX)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_FILE_BOX)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_EDIT_BOX)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_TOOLS_BOX)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_FORMAT_BOX)) return WP_FALSE;
    if (!child_chain_ok(WP_MENU_OBJ_HELP_BOX)) return WP_FALSE;

    /* ROOT -> BAR (ob_head), DROPDOWNS (ob_tail). */
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_next != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_head != WP_MENU_OBJ_BAR) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_tail != WP_MENU_OBJ_DROPDOWNS) return WP_FALSE;
    if (WP_MENU_OBJ_ROOT != 0) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ROOT].ob_type != G_IBOX) return WP_FALSE;

    /* BAR -> ACTIVE, its only child. menu_bar() draws index 1's subtree,
       so BAR must be array index 1 -- this is the whole fix; assert it
       explicitly rather than trusting the enum ordering above. */
    if (WP_MENU_OBJ_BAR != 1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_BAR].ob_next != WP_MENU_OBJ_DROPDOWNS) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_BAR].ob_head != WP_MENU_OBJ_ACTIVE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_BAR].ob_tail != WP_MENU_OBJ_ACTIVE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_BAR].ob_type != G_BOX) return WP_FALSE;

    /* ACTIVE -> Desk/File/Edit/Tools/Format titles, in on-screen order. */
    if (wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_next != WP_MENU_OBJ_BAR) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_head != WP_MENU_OBJ_DESK_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_tail != WP_MENU_OBJ_HELP_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_ACTIVE].ob_type != G_BOX) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_next != WP_MENU_OBJ_FILE_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_head != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_tail != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_TITLE].ob_spec.free_string != g_text_desk) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_next != WP_MENU_OBJ_EDIT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_TITLE].ob_spec.free_string != g_text_file) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_next != WP_MENU_OBJ_TOOLS_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_TITLE].ob_spec.free_string != g_text_edit) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_next != WP_MENU_OBJ_FORMAT_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_TITLE].ob_spec.free_string != g_text_tools) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_next != WP_MENU_OBJ_HELP_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_spec.free_string != g_text_format) return WP_FALSE;

    /* Help is the last title, so its ob_next closes the ring back to the
       parent -- the same "last sibling points at its parent" rule every
       other chain here ends with. Getting this wrong is not a visual
       glitch: the AES walks siblings until it sees the parent index, so
       a wrong value here is an unterminated walk. */
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_next != WP_MENU_OBJ_ACTIVE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_head != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_tail != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_type != G_TITLE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_spec.free_string != g_text_help) return WP_FALSE;

    /* THE TITLES MUST BE CONTIGUOUS ARRAY INDICES FROM DESK_TITLE, in
       menu-bar order. The AES locates a title's dropdown box by walking
       DROPDOWNS's child chain (ititle - THEACTIVE - 1) times -- array
       index arithmetic on the title itself, with no link involved (see
       WP_MENU_OBJ_HELP_TITLE's comment for how this was learned). A
       title placed anywhere else keeps every chain valid and every other
       assertion here green, while silently addressing the wrong box or
       running off the end of the chain. Asserted as arithmetic on the
       whole run so inserting a seventh title in the wrong place fails
       here instead of on someone's screen. */
    if (WP_MENU_OBJ_DESK_TITLE != WP_MENU_OBJ_ACTIVE + 1) return WP_FALSE;
    if (WP_MENU_OBJ_FILE_TITLE != WP_MENU_OBJ_DESK_TITLE + 1) return WP_FALSE;
    if (WP_MENU_OBJ_EDIT_TITLE != WP_MENU_OBJ_FILE_TITLE + 1) return WP_FALSE;
    if (WP_MENU_OBJ_TOOLS_TITLE != WP_MENU_OBJ_EDIT_TITLE + 1) return WP_FALSE;
    if (WP_MENU_OBJ_FORMAT_TITLE != WP_MENU_OBJ_TOOLS_TITLE + 1) return WP_FALSE;
    if (WP_MENU_OBJ_HELP_TITLE != WP_MENU_OBJ_FORMAT_TITLE + 1) return WP_FALSE;

    /* And the same count of boxes, in the same order, since that walk
       pairs the Nth title with the Nth box positionally. */
    {
        short t = WP_MENU_OBJ_DESK_TITLE;
        short b = wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_head;
        short n = 0;

        while (t != WP_MENU_OBJ_ACTIVE && n <= WP_MENU_OBJ_COUNT) {
            if (b < 0 || b >= WP_MENU_OBJ_COUNT) return WP_FALSE;
            /* Each box must be placed by the documented rule, and must
               fit on screen -- see menu_box_placed_ok. */
            if (!menu_box_placed_ok(t, b)) return WP_FALSE;
            t = wp_menu_tree[t].ob_next;
            b = wp_menu_tree[b].ob_next;
            n++;
        }
        if (n != WP_ATARI_MENU_TITLES) return WP_FALSE;
        /* Both chains must close on their own parent at the same step. */
        if (b != WP_MENU_OBJ_DROPDOWNS) return WP_FALSE;
    }

    /* Titles must not overlap, or two of them highlight as one region.
       Checked as a chain rather than against hardcoded x values so it
       keeps holding at any cell width. */
    if (wp_menu_tree[WP_MENU_OBJ_HELP_TITLE].ob_x <
        wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_x +
        wp_menu_tree[WP_MENU_OBJ_FORMAT_TITLE].ob_width) return WP_FALSE;

    /* DROPDOWNS -> one G_BOX per title, same left-to-right order, each
       structurally unreachable from BAR's subtree (see the tree's own
       top-of-file comment for why that's the actual fix). */
    if (wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_next != WP_MENU_OBJ_ROOT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_head != WP_MENU_OBJ_DESK_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_tail != WP_MENU_OBJ_HELP_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DROPDOWNS].ob_type != G_IBOX) return WP_FALSE;

    /* Desk's dropdown: exactly 8 children (About, disabled separator, 6
       AES-reserved dummy slots), contiguous in the array -- menu_fixup()
       is understood to patch these by raw index arithmetic from
       tree[DROPDOWNS].ob_head, not by chain traversal, so contiguity is
       load-bearing, not cosmetic. */
    if (wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_next != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_head != WP_MENU_OBJ_DESK_ABOUT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_tail != WP_MENU_OBJ_DESK_DUMMY6) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_DESK_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (WP_MENU_OBJ_DESK_DUMMY6 != WP_MENU_OBJ_DESK_ABOUT + 7) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_DESK_ABOUT].ob_next != WP_MENU_OBJ_DESK_SEP) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_ABOUT].ob_spec.free_string != g_text_desk_about) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_SEP].ob_next != WP_MENU_OBJ_DESK_DUMMY1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_SEP].ob_state != OS_DISABLED) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_SEP].ob_spec.free_string != g_text_desk_sep) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY1].ob_next != WP_MENU_OBJ_DESK_DUMMY2) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY1].ob_spec.free_string != g_text_desk_dummy1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY2].ob_next != WP_MENU_OBJ_DESK_DUMMY3) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY3].ob_next != WP_MENU_OBJ_DESK_DUMMY4) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY4].ob_next != WP_MENU_OBJ_DESK_DUMMY5) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY5].ob_next != WP_MENU_OBJ_DESK_DUMMY6) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY6].ob_next != WP_MENU_OBJ_DESK_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY6].ob_flags & OF_LASTOB) != 0) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_DESK_DUMMY6].ob_spec.free_string != g_text_desk_dummy6) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_next != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_head != WP_MENU_OBJ_FILE_NEW) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_next != WP_MENU_OBJ_FILE_OPEN) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_spec.free_string != g_text_new) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_tail != WP_MENU_OBJ_FILE_QUIT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FILE_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (!menu_box_placed_ok(WP_MENU_OBJ_FILE_TITLE, WP_MENU_OBJ_FILE_BOX)) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_next != WP_MENU_OBJ_FILE_SAVE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_OPEN].ob_spec.free_string != g_text_open) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_next != WP_MENU_OBJ_FILE_QUIT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_SAVE].ob_spec.free_string != g_text_save) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_next != WP_MENU_OBJ_FILE_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_flags & OF_LASTOB) != 0) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_QUIT].ob_spec.free_string != g_text_quit) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_next != WP_MENU_OBJ_TOOLS_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_head != WP_MENU_OBJ_EDIT_UNDO) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_UNDO].ob_next != WP_MENU_OBJ_EDIT_CUT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_UNDO].ob_spec.free_string != g_text_undo) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_tail != WP_MENU_OBJ_EDIT_PASTE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_EDIT_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (!menu_box_placed_ok(WP_MENU_OBJ_EDIT_TITLE, WP_MENU_OBJ_EDIT_BOX)) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_next != WP_MENU_OBJ_EDIT_COPY) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_CUT].ob_spec.free_string != g_text_cut) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_next != WP_MENU_OBJ_EDIT_PASTE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_COPY].ob_spec.free_string != g_text_copy) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_next != WP_MENU_OBJ_EDIT_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_flags & OF_LASTOB) != 0) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_PASTE].ob_spec.free_string != g_text_paste) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_next != WP_MENU_OBJ_FORMAT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_head != WP_MENU_OBJ_TOOLS_SPELLCHECK) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_tail != WP_MENU_OBJ_TOOLS_CHAT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (!menu_box_placed_ok(WP_MENU_OBJ_TOOLS_TITLE, WP_MENU_OBJ_TOOLS_BOX)) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_next != WP_MENU_OBJ_TOOLS_THESAURUS) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_spec.free_string != g_text_spellcheck) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_SPELLCHECK].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_next != WP_MENU_OBJ_TOOLS_GRAMMARCHECK) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_spec.free_string != g_text_thesaurus) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_THESAURUS].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_next != WP_MENU_OBJ_TOOLS_CHAT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_spec.free_string != g_text_grammarcheck) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_GRAMMARCHECK].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_CHAT].ob_next != WP_MENU_OBJ_TOOLS_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_TOOLS_CHAT].ob_spec.free_string != g_text_chat) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_TOOLS_CHAT].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_next != WP_MENU_OBJ_HELP_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_head != WP_MENU_OBJ_FORMAT_BOLD) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_tail != WP_MENU_OBJ_FORMAT_ALIGN_RIGHT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FORMAT_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (!menu_box_placed_ok(WP_MENU_OBJ_FORMAT_TITLE, WP_MENU_OBJ_FORMAT_BOX)) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOLD].ob_next != WP_MENU_OBJ_FORMAT_ITALIC) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_BOLD].ob_spec.free_string != g_text_bold) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ITALIC].ob_next != WP_MENU_OBJ_FORMAT_UNDERLINE) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ITALIC].ob_spec.free_string != g_text_italic) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_UNDERLINE].ob_next != WP_MENU_OBJ_FORMAT_SIZE_UP) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_UNDERLINE].ob_spec.free_string != g_text_underline) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_UP].ob_next != WP_MENU_OBJ_FORMAT_SIZE_DOWN) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_UP].ob_spec.free_string != g_text_size_up) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_DOWN].ob_next != WP_MENU_OBJ_FORMAT_ALIGN_LEFT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_SIZE_DOWN].ob_spec.free_string != g_text_size_down) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_LEFT].ob_next != WP_MENU_OBJ_FORMAT_ALIGN_CENTER) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_LEFT].ob_spec.free_string != g_text_align_left) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_CENTER].ob_next != WP_MENU_OBJ_FORMAT_ALIGN_RIGHT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_CENTER].ob_spec.free_string != g_text_align_center) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_RIGHT].ob_next != WP_MENU_OBJ_FORMAT_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_RIGHT].ob_spec.free_string != g_text_align_right) return WP_FALSE;
    /* Align Right used to carry OF_LASTOB as the final array entry. The
       Help objects are now the final entries, so it must NOT -- the
       whole-array count above already proves there is exactly one, but
       this pins WHICH object lost it, since a stray extra here is the
       exact shape of the bug that once hid every dropdown from the AES. */
    if ((wp_menu_tree[WP_MENU_OBJ_FORMAT_ALIGN_RIGHT].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_next != WP_MENU_OBJ_DROPDOWNS) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_head != WP_MENU_OBJ_HELP_CONTENTS) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_tail != WP_MENU_OBJ_HELP_FORMAT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_type != G_BOX) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_HELP_BOX].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    if (!menu_box_placed_ok(WP_MENU_OBJ_HELP_TITLE, WP_MENU_OBJ_HELP_BOX)) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_HELP_CONTENTS].ob_next != WP_MENU_OBJ_HELP_KEYS) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_CONTENTS].ob_spec.free_string != g_text_help_contents) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_KEYS].ob_next != WP_MENU_OBJ_HELP_FORMAT) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_KEYS].ob_spec.free_string != g_text_help_keys) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_FORMAT].ob_next != WP_MENU_OBJ_HELP_BOX) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_HELP_FORMAT].ob_spec.free_string != g_text_help_format) return WP_FALSE;
    /* Help > Format Keys used to be the final array entry and so carried
       OF_LASTOB. File > New was appended after it (T170), so the flag
       moved on; this now asserts it moved OFF here, which is the half a
       whole-array count cannot express -- the count above proves there
       is exactly one and that it sits on the last object, but not that
       an intermediate object was cleared rather than the count being
       satisfied some other way. This assertion is what caught the flag
       being left in two places during that very edit. */
    if ((wp_menu_tree[WP_MENU_OBJ_HELP_FORMAT].ob_flags & OF_LASTOB) != 0) return WP_FALSE;

    if (wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_type != G_STRING) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_head != -1) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_tail != -1) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_flags & OF_LASTOB) != 0) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_EDIT_UNDO].ob_flags & OF_LASTOB) == 0) return WP_FALSE;
    if (wp_menu_tree[WP_MENU_OBJ_EDIT_UNDO].ob_type != G_STRING) return WP_FALSE;
    if ((wp_menu_tree[WP_MENU_OBJ_FILE_NEW].ob_flags & OF_HIDETREE) != 0) return WP_FALSE;

    /* Every item this menu bar shows must resolve to a real action, and
       every action the shared registry files under a group this backend
       builds must appear somewhere in the tree. Proving both directions
       is the point of routing dispatch through the registry at all --
       one direction alone still permits a menu item that dispatches
       nothing, or a registered action no ST user can ever reach. */
    if (!menu_action_map_selfcheck()) return WP_FALSE;

    return WP_TRUE;
}


/* Proves wp_menu_dropdown_x against the geometry that actually broke,
   without needing a screen -- SMOKE.PRG never opens one.

   The clamp is the whole point, so the test is: on a screen too narrow
   for the tree's nominal layout, does every box still end on-screen? */
wp_bool wp_menu_dropdown_geometry_selfcheck(void)
{
    /* Wide screen: nothing is clamped; the box sits 2 cells right of
       its title, per RCS. */
    if (wp_menu_dropdown_x(264, 120, 640, 8) != 280) return WP_FALSE;

    /* The LEFT edge, which is the half this originally got wrong. Desk's
       title is at x=0, and menu_sr() subtracts MTH=1 before bb_save --
       so a flush box would hand the AES x = -1. The indent must put it
       clear of zero. */
    if (wp_menu_dropdown_x(0, 160, 640, 8) != 16) return WP_FALSE;

    /* Help's real geometry at 8px cells on a 40-column screen: title at
       column 33 (264px), 15-wide box (120px), 320px of screen. Nominal
       right edge 384 -- 64px past the end. Must be pulled back to
       320-120 = 200. This is the exact case that made TOS's own
       bb_save() read past the framebuffer. */
    if (wp_menu_dropdown_x(264, 120, 320, 8) != 200) return WP_FALSE;

    /* Format's box, same screen: 25*8=200, 16 cells wide = 128px,
       nominal right edge 328, clamped to 320-128 = 192. */
    if (wp_menu_dropdown_x(200, 128, 320, 8) != 192) return WP_FALSE;

    /* Exactly-fits must NOT be moved: an off-by-one here would shift
       every dropdown left by a pixel on the common 640px screen. */
    if (wp_menu_dropdown_x(520, 120, 640, 8) != 520) return WP_FALSE;

    /* Degenerate: a box wider than the whole screen clamps to 0 rather
       than going negative, which would put ob_x off the LEFT edge and
       reintroduce the same out-of-bounds save at the other end. */
    if (wp_menu_dropdown_x(264, 700, 640, 8) != 0) return WP_FALSE;

    return WP_TRUE;
}

wp_bool wp_atari_menu_selfcheck(void)
{
    short i;
    short titles = 0;
    short items = 0;
    short dispatched = 0;
    short inert = 0;

    /* The portable half first: row order, labels, accelerator bytes,
       group coverage -- everything this backend's menu depends on that
       is not GEM-specific. Runs here as well as on the host so the SAME
       table is proven under the m68k cross-compiler's own word sizes. */
    if (!wp_menu_model_selfcheck()) return WP_FALSE;

    /* Then the GEM OBJECT tree itself. */
    if (!wp_menu_tree_selfcheck()) return WP_FALSE;

    /* And the geometry rule that costs a crash rather than a glitch.
       Exercised on the REAL numbers this tree uses, not on abstract
       ones: Help's title is at column 33 with a 15-wide box, so it needs
       48 columns, and the case that took down a real Falcon is a screen
       with fewer than that. */
    if (!wp_menu_dropdown_geometry_selfcheck()) return WP_FALSE;

    for (i = 0; i < WP_MENU_OBJ_COUNT; i++) {
        const OBJECT *ob = &wp_menu_tree[i];

        if (ob->ob_head != -1) continue; /* a container, not a leaf */

        /* Menu-bar titles are leaves too (no children) but are G_TITLE,
           not selectable items -- count them apart rather than letting
           them fall into the item arithmetic below. */
        if (ob->ob_type == G_TITLE) {
            titles++;
        } else if (ob->ob_type == G_STRING) {
            items++;

            /* The AES-reserved Desk slots and the separator are the only
               leaves with no dispatch case, and the tree's own comment
               already establishes that they are CONTIGUOUS (menu_fixup
               patches them by index arithmetic, not chain traversal), so
               an index range is the honest way to identify them -- not
               ob_state, which marks only the separator. */
            if (i >= WP_MENU_OBJ_DESK_SEP && i <= WP_MENU_OBJ_DESK_DUMMY6) inert++;
            else dispatched++;
        } else {
            return WP_FALSE; /* a leaf that is neither a title nor an item */
        }

        /* Every leaf must point at a real buffer. A NULL ob_spec is what
           a half-finished menu edit looks like: the AES walks it and
           draws garbage or faults. (ob_spec for a G_STRING/G_TITLE IS a
           pointer, unlike the containers' packed colour word.) */
        if (ob->ob_spec.free_string == (char *)0) return WP_FALSE;

        /* ...but only the items this app owns must be non-EMPTY. The six
           AES-reserved Desk slots are deliberately blank char[32]
           buffers: menu_register()/menu_fixup() write installed desk
           accessory names into them at runtime, so an empty string there
           is the correct initial state, not a missing label. Asserting
           otherwise is what made this check fail on its first real
           boot. */
        if (!(i >= WP_MENU_OBJ_DESK_DUMMY1 && i <= WP_MENU_OBJ_DESK_DUMMY6)) {
            if (ob->ob_spec.free_string[0] == '\0') return WP_FALSE;
        }

        /* No leaf may be hidden. OF_HIDETREE on a dropdown BOX was the
           bug that made four of the five menus structurally undrawable
           (the AES never clears it, so objc_draw skips the subtree); the
           same flag on an individual item hides just that item, equally
           silently. */
        if ((ob->ob_flags & OF_HIDETREE) != 0) return WP_FALSE;
    }

    if (titles != WP_ATARI_MENU_TITLES) return WP_FALSE;
    if (items != WP_ATARI_MENU_DISPATCHED_LEAVES + WP_ATARI_MENU_INERT_LEAVES) return WP_FALSE;
    if (dispatched != WP_ATARI_MENU_DISPATCHED_LEAVES) return WP_FALSE;
    if (inert != WP_ATARI_MENU_INERT_LEAVES) return WP_FALSE;

    /* The separator is the one item that must be greyed: it is drawn as
       a rule between About and the desk-accessory slots, and an enabled
       separator is selectable and dispatches nothing. */
    if (wp_menu_tree[WP_MENU_OBJ_DESK_SEP].ob_state != OS_DISABLED) return WP_FALSE;

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
static wp_undo_journal g_journal;
static i32 g_content_width_twips = 0;

/* M8.5/M10: the active keybinding scheme's own state (platform/common/
   keymap.h) and the rest of the shared cross-platform app state
   (platform/common/app_ctx.h) -- see wp_app_ctx_init's call in
   app_shell_run(). wp_keymap_cursor(&g_ctx) is now the
   SINGLE source of truth for "where editing currently applies" (it
   replaces the pre-M8.5 g_cursor global), g_ctx.doc_loaded replaces
   g_doc_loaded, and g_ctx.style_flags/current_size_twips replace
   g_current_style_flags/g_current_size_twips below -- all four were
   plain file-scope statics before M8.5's action registry (platform/
   common/actions.c) needed a single struct its portable handlers could
   take a pointer to. g_doc/g_journal themselves stay as direct globals
   (g_ctx.doc/g_ctx.journal just point at them) rather than being folded
   in too -- every pre-existing call site in this file already spells
   them `&g_doc`/`&g_journal`, and there is no real duplication to remove
   by renaming those specifically (unlike cursor/style/size, which the
   registry and vi-mode both need to be ONE thing, not two kept in sync
   by hand). Kept as file-scope statics rather than threaded through
   wp_dispatch_message's signature so the smoke test
   (host/tools/atari_smoke.c) -- which never loads a document -- keeps
   working unchanged: g_ctx.doc_loaded stays WP_FALSE there, and
   paint_document() is a no-op in that case. */
static wp_app_ctx g_ctx;

/* M8.5 Phase 10: the help system. g_help_doc is parsed ONCE at startup
   from the generated g_help_source_retrowp array (build/generated/
   help_retrowp.h, produced from docs/help/retrowp.help by host/tools/
   gen_help_header.c -- see docs/help.md) -- help content ships compiled
   into RETROWP.PRG, no runtime file dependency the way the optional
   spell dictionary has. g_help_active is a whole separate UI mode from
   normal editing: while it's set, wp_dispatch_key routes keys to
   wp_help_view_feed_key instead of vi-mode, and redraw_window paints
   the help pager instead of the document -- see paint_help() and the
   g_help_active checks in both. Reachable via F1 only this pass (no
   Help menu item -- see docs/atari-platform.md's "Atari retrofit"
   section for why this project treats wp_menu_tree[]'s OBJECT array as
   something to avoid restructuring without a strong reason). */
static wp_help_doc g_help_doc;
static wp_bool g_help_doc_loaded = WP_FALSE;
static wp_help_view_state g_help_view;
static wp_bool g_help_active = WP_FALSE;

/* "Current input style" -- the classic toggle-before-typing model (Ctrl+B,
   then subsequently typed characters come out bold) rather than restyling
   a selection, since this app still has no selection-scoped restyling for
   plain typing (Visual mode's d/y/c operate on selections; Format toggles
   still only ever affect what's typed NEXT). Reset to the document's own
   default on every load/replace (see replace_document) so a freshly
   opened file doesn't inherit whatever was toggled on for the previous
   one. Lives in g_ctx.style_flags / g_ctx.current_size_twips now (see
   above) -- this comment stays with the concept, not a specific global. */

/* Fixed cycle Font Size steps through, in twips (1 point = 20 twips):
   8/10/12/14/18/24/36pt -- the same kind of small fixed point-size list
   1980s word processors offered rather than a free-entry field (no
   numeric-entry dialog exists in this app). */
static const i32 g_font_sizes_twips[] = { 160, 200, 240, 280, 360, 480, 720 };
#define WP_FONT_SIZE_COUNT (sizeof(g_font_sizes_twips) / sizeof(g_font_sizes_twips[0]))

static const char g_sample_text[] =
    "This is RetroWP, the portable word processor engine, running on "
    "a real Atari ST under Hatari. Word wrap and on-screen rendering "
    "are driven entirely through the platform.h VDI metrics seam -- "
    "the engine itself has never heard of GEM.\n"
    "\n"
    "Press F1 at any time for the built-in manual -- it covers the "
    "editing keys, formatting, and everything else. The Help menu "
    "opens the same manual at Editing Keys or Format Keys directly. "
    "File > New (Ctrl+N) starts a fresh, empty document.\n";

/* Relayouts only what actually needs it: a paragraph whose layout was
   never built (layout == NULL), one the engine marked layout_dirty (any
   content edit -- edit.c sets it on every insert/delete), or every
   paragraph when the column itself changed (a real window resize, where
   nothing is dirty but every line must re-break).

   The dirty check is what makes calling this before EVERY paint cheap
   enough to be the correct default: wp_paragraph_relayout has no
   early-out of its own (it opens fonts, tokenizes and measures
   unconditionally -- see linebreak.c), so relayouting a clean document
   on each redraw would mean a full re-measure per keystroke on an 8MHz
   68000. Skipping clean paragraphs reduces the steady-state cost to one
   flag test each. */
static void relayout_document(i32 content_width_twips)
{
    wp_bool width_changed = (wp_bool)(content_width_twips != g_content_width_twips);
    u32 si, pi;

    g_content_width_twips = content_width_twips;
    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);
        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            if (!width_changed && para->layout != NULL && !para->layout_dirty) continue;
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

    /* M8.5/M10: a whole-document swap invalidates any Insert/Visual/
       pending-operator state the active keymap scheme was holding for
       the PREVIOUS document (its cursor, register contents, etc. --
       none of it means anything once the tree underneath has been freed
       and replaced), so the scheme is reset the same way the undo
       journal above already is, not just resynced. wp_keymap_init's own
       cursor default (doc start) is overridden right after to match
       this app's existing "cursor sits at the end of a freshly loaded
       document" behavior. Safe to call unconditionally: wp_app_ctx_init
       (app_shell_run) already ran wp_keymap_init once before the first
       possible replace_document call, so the scheme's own owned memory
       is always a valid wp_keymap_free target here, populated or not. */
    wp_keymap_free(&g_ctx);
    wp_keymap_init(&g_ctx, (wp_keymap_id)g_ctx.keymap_id, &g_doc, &g_journal);
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_end(&g_doc);

    def_cs = wp_document_cstyle_get(&g_doc, g_doc.default_cstyle);
    g_ctx.style_flags = def_cs->flags;
    g_ctx.current_size_twips = def_cs->size_twips;

    def_ps = wp_document_pstyle_get(&g_doc, g_doc.default_pstyle);
    g_ctx.align = def_ps->align;

    /* Every load path -- Open, the startup sample, Paste -- lands here,
       and none of them leaves unsaved WORK behind: the document now
       matches the thing it was built from. Marking clean here rather
       than in each caller means a new load path cannot forget. */
    wp_document_mark_saved(&g_doc);
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
/* The pure arithmetic behind paint_document's per-line alignment
   offset, pulled out into its own function so wp_atari_format_selfcheck
   can verify it against hand-computed values without needing a real
   window/VDI handle -- same reasoning as every other selfcheck's
   "hand-computed" bar (see wp_atari_layout_selfcheck). Returns a TWIPS
   offset to add to the line's normal (left-aligned) x origin; callers
   convert to pixels themselves since the twips<->px ratio is a metrics
   concern, not an alignment one. LEFT (and any unrecognized value)
   always returns 0. Never negative -- a line wider than the column
   (shouldn't happen; relayout guarantees lines fit) just draws at the
   left edge rather than off the window's left side. */
static i32 atari_align_offset_twips(u8 align, i32 content_width_twips, i32 line_width_twips)
{
    i32 extra_twips = content_width_twips - line_width_twips;

    if (extra_twips <= 0) return 0;
    if (align == WP_ALIGN_CENTER) return extra_twips / 2;
    if (align == WP_ALIGN_RIGHT) return extra_twips;
    return 0;
}

/* Caret width. 2px, not Mac's 3: ST Low is only 320px wide, so a caret
   is a visibly larger share of a character cell here than on a 512px
   Mac screen, and 2px still reads clearly at ST High's 640x400. */
#define WP_ATARI_CARET_W_PX 2

/* Byte-identical to platform/mac/app_shell.c's, platform/amiga/
   app_shell.c's and platform/dos/app_shell.c's -- a line owns a
   half-open (run, offset) range, and the cursor is on it when it falls
   inside. Four copies of eight lines is deliberate: it is the only
   piece of the caret logic that is NOT shared through
   wp_line_cursor_x_twips, because it needs no measuring at all. */
static wp_bool line_contains_pos(const wp_line *line, u32 run, u32 offset)
{
    if (run < line->start_run || run > line->end_run) return WP_FALSE;
    if (run == line->start_run && offset < line->start_offset) return WP_FALSE;
    if (run == line->end_run && offset > line->end_offset) return WP_FALSE;
    return WP_TRUE;
}

/* Paints a white strip, clipped by whatever vs_clip the caller set.
   Factored out because the repaint now clears in several small places
   instead of one big one -- see paint_document's own comment. */
static void fill_white_strip(VdiHdl vdi, i32 x, i32 y, i32 w, i32 h)
{
    short pxy[4];

    if (w <= 0 || h <= 0) return;
    pxy[0] = (short)x;
    pxy[1] = (short)y;
    pxy[2] = (short)(x + w - 1);
    pxy[3] = (short)(y + h - 1);
    vsf_interior(vdi, FIS_SOLID);
    vsf_color(vdi, G_WHITE);
    v_bar(vdi, pxy);
}

/* CLEARS AS IT DRAWS, rather than being handed an already-blanked area.
   This is why: redraw_window used to blank the entire work area with one
   v_bar and then repaint every visible line on top. That is right for a
   real WM_REDRAW, but it also ran on EVERY KEYSTROKE, so deleting a
   single character blanked and repainted the whole window. On real
   hardware the result is a full-screen white flash per keypress --
   reported from a Falcon030 as "the update refresh when deleting
   characters... it is awful", and it is: the cost is not redrawing the
   text (a two-line document is nothing to redraw) but the whole-window
   white frame the eye sees in between.

   Clearing each line's own strip immediately before drawing that line
   keeps every white interval down to one line's height and one line's worth
   of time, so there is never a frame in which the document is missing.
   The strip spans the full width on purpose: it has to erase whatever
   the PREVIOUS contents of that line were, which after a delete is
   wider than what is about to be drawn.

   Everything below the last line still needs erasing in one go -- that
   is where lines that no longer exist used to be, and nothing draws
   over them. */
static void paint_document(short window_handle, VdiHdl vdi, GRECT area)
{
    atari_gc gc;
    u32 si, pi, li;
    i32 y_px = area.g_y;
    plat_font *font = NULL;
    wp_cstyle cached_style = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent_px = 0;

    if (!g_ctx.doc_loaded) {
        fill_white_strip(vdi, area.g_x, area.g_y, area.g_w, area.g_h);
        return;
    }

    gc.vdi = vdi;
    gc.window_handle = window_handle;

    for (si = 0; si < g_doc.section_count; si++) {
        wp_section *sec = wp_document_section_get(&g_doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
            const wp_para_style *pstyle = wp_document_pstyle_get(&g_doc, para->style);
            u8 align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;
            wp_bool is_cursor_para =
                ((*wp_keymap_cursor(&g_ctx)).section == si &&
                 (*wp_keymap_cursor(&g_ctx)).paragraph == pi) ? WP_TRUE : WP_FALSE;
            u32 cursor_line_index = 0;

            if (layout == NULL) continue;

            /* Which of this paragraph's display lines the cursor sits on.
               The last line is the fallback rather than the first: a
               cursor one past the final character (where it lands after
               every insertion) is outside every line's half-open range,
               and end-of-document is where it spends most of its time. */
            if (is_cursor_para && layout->line_count > 0) {
                cursor_line_index = layout->line_count - 1;
                for (li = 0; li < layout->line_count; li++) {
                    if (line_contains_pos(&layout->lines[li],
                                           (*wp_keymap_cursor(&g_ctx)).run,
                                           (*wp_keymap_cursor(&g_ctx)).offset)) {
                        cursor_line_index = li;
                        break;
                    }
                }
            }

            for (li = 0; li < layout->line_count; li++) {
                wp_line *line = &layout->lines[li];
                i32 x_px = area.g_x;
                i32 line_origin_px;
                i32 extra_twips;
                wp_line_fragment_iter iter;
                wp_line_fragment frag;

                if (y_px > area.g_y + area.g_h) break;

                /* Erase this line's strip right before drawing it (see
                   this function's own comment). Full width, because the
                   text being replaced may have been longer than the text
                   about to be drawn -- a delete is exactly that case. */
                fill_white_strip(vdi, area.g_x, y_px, area.g_w,
                                  atari_twips_to_px_y(line->height_twips));

                /* Alignment never changes where lines BREAK (both engine
                   relayout and this paint pass use the same
                   g_content_width_twips column), only where each already-
                   broken line starts drawing -- a pure paint-time x
                   offset, no engine/layout change needed. JUSTIFY
                   resolves to 0 here (left start) same as LEFT -- correct,
                   since justify only redistributes slack BETWEEN words,
                   via the per-fragment walk below (T13.2). */
                x_px += atari_twips_to_px_x(
                    atari_align_offset_twips(align, g_content_width_twips, line->width_twips));
                line_origin_px = x_px;   /* the fragment walk below advances x_px past it */

                /* M15 T15.6: an image paragraph's single line has no run
                   bytes to fragment (wp_line_next_fragment already returns
                   WP_FALSE for it, per justify.c's own kind-guard) -- paint
                   its real indexed-bitmap content (read back from blob_path
                   on demand, per services/spell.c's own "stream from disk,
                   don't load the model whole" precedent) and move straight
                   to the next line, skipping the fragment-walk/font-cache
                   logic below entirely. Falls back to a solid palette[0]
                   rect if the cache file can't be read, or if this VDI
                   device has no spare palette register for image content
                   (see plat_draw_image's own comment in draw_atari.c),
                   rather than drawing nothing at all. */
                if (para->kind == (u8)WP_PARA_IMAGE) {
                    const wp_image_desc *img = wp_document_image_get(&g_doc, para->image);

                    if (img != NULL && img->palette_count > 0) {
                        i32 w_px = atari_twips_to_px_x(line->width_twips);
                        i32 h_px = atari_twips_to_px_y(line->height_twips);
                        wp_bool drew_real = WP_FALSE;

                        if (img->blob_path.length > 0 && img->blob_path.length < 256) {
                            char ipath[256];
                            const u8 *span_bytes = wp_strpool_get(&g_doc.strings, img->blob_path);
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
                                        if (plat_draw_image((plat_gc *)&gc, x_px, y_px, w_px, h_px,
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
                            plat_fill_rect((plat_gc *)&gc, x_px, y_px, w_px, h_px,
                                           img->palette[0][0], img->palette[0][1], img->palette[0][2]);
                        }
                    }
                    y_px += atari_twips_to_px_y(line->height_twips);
                    continue;
                }

                extra_twips = wp_justify_space_extra_twips(
                    &g_doc, para, line, align, (wp_bool)(li == layout->line_count - 1),
                    g_content_width_twips);

                wp_line_fragment_iter_init(&iter, line);
                while (wp_line_next_fragment(&g_doc, para, line, &iter, &frag)) {
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
                    bytes = wp_strpool_get(&g_doc.strings, run->text);
                    if (bytes == NULL) continue;

                    if (!have_font || run->style != cached_style) {
                        const wp_char_style *cs = wp_document_cstyle_get(&g_doc, run->style);

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
                                ascent_px = atari_twips_to_px_y(fm.ascent);
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
                        g_atari_painted_frags++;
                        plat_draw_text((plat_gc *)&gc, font, x_px, y_px + ascent_px,
                                        bytes + frag.byte_start, frag_len, 0, 0, 0);
                    }

                    x_px += atari_twips_to_px_x(frag_width_twips);
                    if (frag.is_space) x_px += atari_twips_to_px_x(extra_twips);
                }

                /* T162: this backend drew NO caret at all -- it rendered
                   the document and the vi-mode status bar and left the
                   cursor entirely invisible, on the one target that gets
                   the most real interactive testing (and the 1MB
                   baseline, CLAUDE.md rule 5). Beyond the obvious "you
                   cannot see where typed text will land", it meant every
                   cursor-related defect here was structurally invisible
                   to screenshot verification, which is how T144's menu
                   bug and T157's caret bug were both actually caught.

                   Column via the shared, host-tested wp_line_cursor_x_
                   twips (engine/src/justify.c), fed the same extra_twips
                   the fragment walk above used, so a JUSTIFY line's
                   stretched gaps count identically in both.

                   Height comes from the LINE's own height_twips, not
                   ascent_px: ascent_px is carried across loop iterations
                   from whichever run last opened a font, so on an empty
                   paragraph (a blank line, or a brand-new document)
                   nothing in the fragment walk ever sets it and the
                   caret would collapse to a stub. The line box is also
                   simply the more honest thing for a caret to span.

                   Drawn through plat_fill_rect rather than a raw
                   vsf_color/v_bar pair so it goes through the same
                   colour abstraction as every other fill (the image
                   fallback above uses it too); black resolves via
                   rgb_to_vdi_color's own 384 threshold. Nothing extra is
                   needed for the AES: redraw_window already brackets
                   this whole call in wind_update/graf_mouse(M_OFF) -- a
                   caret drawn outside that bracket would corrupt the
                   save-under buffer (see redraw_window's own comment). */
                if (is_cursor_para && li == cursor_line_index) {
                    i32 caret_x_px = line_origin_px + atari_twips_to_px_x(
                        wp_line_cursor_x_twips(&g_doc, para, line,
                                                (*wp_keymap_cursor(&g_ctx)).run,
                                                (*wp_keymap_cursor(&g_ctx)).offset,
                                                extra_twips));
                    i32 caret_h_px = atari_twips_to_px_y(line->height_twips);

                    if (caret_h_px < 1) caret_h_px = 1;
                    plat_fill_rect((plat_gc *)&gc, caret_x_px, y_px,
                                    WP_ATARI_CARET_W_PX, caret_h_px, 0, 0, 0);
                }

                y_px += atari_twips_to_px_y(line->height_twips);
            }
        }
    }

    /* Below the last line: where deleted lines used to be. Nothing in
       the loop above draws here, so without this a document that got
       shorter would keep showing its own ghost. */
    fill_white_strip(vdi, area.g_x, y_px, area.g_w,
                      (area.g_y + area.g_h) - y_px);

    if (font != NULL) plat_font_close(font);
}

/* M8.5: actual text insertion/deletion now happens inside vi-mode's own
   Insert-mode handling (engine/src/vimode.c's feed_insert, driven by
   wp_dispatch_key below) rather than through app-level do_insert_char/
   do_backspace functions -- those pre-vimode single-paragraph-editor
   functions are gone; wp_edit_insert_text/wp_edit_delete_range are still
   the same primitives, just called from the portable engine layer now.

   What vi-mode's Insert mode does NOT know about is this app's "current
   input style" toggle state (g_ctx.style_flags/current_size_twips are
   platform/common concepts vimode.c has no access to, and rightly so --
   see CLAUDE.md's engine-has-zero-platform-calls rule, which extends to
   not depending on platform/common either). wp_edit_insert_text's own
   style-inheritance rule (docs/editing.md) always wins first when vi-
   mode inserts a character -- there's no way to hand it an override
   style directly. So this runs AFTER wp_dispatch_key sees a successful
   Insert-mode keystroke and overwrites the just-created run's style
   (identified via insert_span_at_pos's own documented contract: pos.run
   is ALWAYS the newly-inserted run's index, in every case -- empty
   paragraph, start, run boundary, or a mid-run split) with whatever the
   current toggle state says it should be. find_or_add means repeated
   typing with the same toggle state keeps reusing one cstyle handle
   rather than growing the table per keystroke. */
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

/* Format menu actions (Font Size +/-, Alignment Left/Center/Right).
   Neither touches document content directly -- each only changes "what
   happens to text typed/laid out from here on" (size) or restyles the
   paragraph at the cursor (alignment), so neither needs an undo-journal
   entry (consistent with this app's existing scope: undo covers content
   edits, not view/formatting state -- see docs/editing.md). A repaint is
   the caller's job (matches every other action in this file).

   M8.5: Bold/Italic/Underline moved to platform/common/actions.c's
   registry (act_format_bold/italic/underline) -- both the Format menu
   items and the Ctrl+B/I/U keyboard shortcuts now call
   wp_action_find(...)->handler(&g_ctx) instead of a local
   toggle_style_flag, so there is exactly one place, not two, that knows
   "toggling a style flag is ctx->style_flags ^= flag". Font Size and
   Alignment stay as these local direct-set functions for the MENU path
   specifically (Size +/-, Align Left/Center/Right) -- the registry's own
   WP_ACT_FORMAT_SIZE/FORMAT_ALIGN actions open a real plat_dialog_choice
   picker instead (see actions.c), a real, deliberate UX difference from
   "set directly" that this pass intentionally does NOT also carry over
   to the existing five Format menu items: doing so would mean removing
   items from wp_menu_tree's hand-built OBJECT array, exactly the kind
   of menu-tree index-arithmetic surgery that has twice caused real,
   silent regressions on other TOS/EmuTOS versions (see
   app_shell_internal.h's own account, and docs/atari-platform.md) --
   not worth the risk for a cosmetic menu simplification. The two
   keyboard shortcuts (Ctrl+]/Ctrl+L) DO get the new dialog-based
   behavior (see wp_dispatch_key below), since that only changes a
   dispatch table, not the OBJECT tree. */

static void change_font_size(int delta_steps)
{
    unsigned int i;

    if (!g_ctx.doc_loaded) return;

    for (i = 0; i < WP_FONT_SIZE_COUNT; i++) {
        if (g_font_sizes_twips[i] == g_ctx.current_size_twips) {
            int next = (int)i + delta_steps;
            if (next < 0) next = 0;
            if (next >= (int)WP_FONT_SIZE_COUNT) next = (int)WP_FONT_SIZE_COUNT - 1;
            g_ctx.current_size_twips = g_font_sizes_twips[next];
            return;
        }
    }
    /* Current size isn't one of the fixed steps (e.g. a size set by an
       imported RTF's own \fs value) -- snap to the nearest step rather
       than doing nothing. */
    g_ctx.current_size_twips = g_font_sizes_twips[delta_steps > 0 ? WP_FONT_SIZE_COUNT - 1 : 0];
}

/* Applies to the paragraph (*wp_keymap_cursor(&g_ctx)) is currently in -- vi-mode
   motions move that cursor now (M8.5), but "alignment always applies to
   the CURRENT paragraph, not a selection" is unchanged from before vi-
   mode existed (Visual mode's own selection doesn't have an alignment
   operator -- see docs/vimode.md's "Explicitly Deferred" section).
   Unlike character style, this doesn't need find_or_add's
   "seed new text" trick -- it directly restyles the EXISTING paragraph,
   which is what "align this paragraph" means. Alignment never changes
   line-break points (same content width either way), so no relayout is
   needed -- only a repaint, done by the caller, picks it up. */
static void apply_alignment(u8 align)
{
    wp_section *sec;
    wp_paragraph *para;
    const wp_para_style *cur;
    wp_para_style style;
    wp_pstyle handle;

    if (!g_ctx.doc_loaded) return;

    sec = wp_document_section_get(&g_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);
    cur = wp_document_pstyle_get(&g_doc, para->style);
    style = *cur;
    style.align = align;

    if (wp_document_pstyle_find_or_add(&g_doc, &style, &handle) == WP_OK) {
        wp_paragraph_pstyle_set(para, handle);
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

/* 9600 with no flow control is the deliberately conservative default: it
   is what the 1MB plain-TOS baseline (CLAUDE.md rule 5) can keep up with
   through a polled BIOS AUX device, and it assumes nothing about the
   cable beyond TX/RX/GND. It is a DEFAULT, not a ceiling -- Rsconf()
   itself tops out at 19200 (transport_atari.c documents why), and a
   machine with a properly wired RTS/CTS cable can ask for handshaking:

       -DWP_ATARI_OFFLOAD_ENDPOINT='"serial:aux:19200:hard"'

   Both ends must agree out of band; there is no baud negotiation in the
   protocol (docs/atari-platform.md). The gateway is told the same thing
   with --serial-baud and --serial-flow, so the line above pairs with:

       whisper_gateway.py --serial-rx=/dev/ttyAMA0 --serial-tx=/dev/ttyAMA0
                          --serial-baud=19200 --serial-flow=rtscts

   A flow-control mismatch is the one to watch for: asking the ST for
   `hard` while the gateway leaves RTS/CTS off means nothing ever asserts
   RTS, Bcostat() never reports ready, and every write times out. Neither
   end can tell that apart from a dead cable. */
#ifndef WP_ATARI_OFFLOAD_ENDPOINT
#define WP_ATARI_OFFLOAD_ENDPOINT "serial:aux:9600"
#endif

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

    if (!g_ctx.doc_loaded) return;
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

    /* Opening replaces the whole document, so it asks before discarding
       unsaved work -- BEFORE the file picker, so the writer is not made
       to choose a file and only then told they cannot have it. */
    if (!wp_action_confirm_discard(&g_ctx)) return;

    /* T172: an ex command may already have named the file (":w foo" /
       ":e foo"), in which case the native picker is skipped entirely --
       that is the whole point: never leave the keyboard to name a file.
       Everything below this line is unchanged and already path-driven,
       including format selection by extension, so ":w notes.rtf" saves
       RTF exactly as picking that name in the requester would. */
    if (!wp_keymap_take_pending_path(fullpath, (u32)sizeof(fullpath))) {
        /* "*.*" (not "*.TXT") so RTF/ODT files are visible too -- see
           str_ends_with_ci above for how the format is actually picked. */
        strcpy(path, "C:\\*.*");
        fname[0] = '\0';
        if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;
    }

    if (str_ends_with_ci(fullpath, ".ODT")) {
        do_odt_open(fullpath);
        return;
    }

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) return;
    if (plat_file_size(f, &size) != WP_OK) { plat_file_close(f); return; }

    buf = (u8 *)mem_alloc(size + 1);
    if (buf != NULL) {
        if (plat_file_read(f, buf, size, &read_len) == WP_OK) {
                /* WordStar is detected by CONTENT, not by name: those
                   files carry no magic number and no reserved extension
                   -- commonly .WS, .DOC (colliding with Word), or none
                   at all. The sniff is conservative and never claims
                   plain ASCII, which is what WordStar's own
                   non-document mode produced. See
                   docs/formats/wordstar.md. */
            if (format_ws_looks_like(buf, read_len)) {
                wp_document ws_doc;
                if (format_ws_import(buf, read_len, &ws_doc) == WP_OK) {
                    replace_document(&ws_doc, g_content_width_twips);
                }
            } else if (str_ends_with_ci(fullpath, ".RTF")) {
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

    if (!g_ctx.doc_loaded) return;

    /* T172: an ex command may already have named the file (":w foo" /
       ":e foo"), in which case the native picker is skipped entirely --
       that is the whole point: never leave the keyboard to name a file.
       Everything below this line is unchanged and already path-driven,
       including format selection by extension, so ":w notes.rtf" saves
       RTF exactly as picking that name in the requester would. */
    if (!wp_keymap_take_pending_path(fullpath, (u32)sizeof(fullpath))) {
        strcpy(path, "C:\\*.*");
        strcpy(fname, "UNTITLED.TXT");
        if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;
    }

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
            /* Only now is the document really on disk. Marking it clean
               any earlier -- on entry, or after export but before the
               write -- would have a failed save quietly convince the app
               there was nothing left to lose. */
            wp_document_mark_saved(&g_doc);
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

/* Spell Check: like everything else that touches the whole document
   (Cut/Copy/Paste), scoped to "no selection model yet". The dictionary
   is queried from disk, never loaded whole (services/spell/spell.c,
   docs/services/spell.md) -- a user who wants real spell-check on
   RETROWP.PRG drops a sorted word-list file at this fixed path (see
   docs/atari-platform.md's deployment note; there is no shipped
   dictionary asset, and none is needed for this feature to build/link/
   run -- a missing file is reported through the same form_alert path as
   a real miss, not a crash). */
#define WP_ATARI_SPELL_DICT_PATH "C:\\WPSPELL.DIC"
#define WP_ATARI_SPELL_MAX_SUGGESTIONS 3

/* M10: same fixed-path convention as WP_ATARI_SPELL_DICT_PATH above --
   a user who wants a non-default keybinding layout drops a plain-text
   `keymap=<name>` config file at this path (docs/keymaps.md). A missing
   file is a normal, silent, zero-config case (wp_config_load's own
   documented default), not an error -- unlike the spell dictionary,
   there is no user-visible alert for a missing config file, since "no
   config" is the expected common case, not a feature the user opted
   into and might be missing an asset for. */
#define WP_ATARI_CONFIG_PATH "C:\\RETROWP.CFG"

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

    if (!g_ctx.doc_loaded) return;

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
   boundary vi-mode's own insert_backspace (engine/src/vimode.c) has --
   there is still no mouse-driven word selection (no selection model at
   all outside Visual mode, per docs/editing.md), and
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

    if (!g_ctx.doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);
    if ((*wp_keymap_cursor(&g_ctx)).run >= para->run_count) return WP_FALSE;

    run = wp_paragraph_run_get(para, (u16)(*wp_keymap_cursor(&g_ctx)).run);
    bytes = wp_strpool_get(&g_doc.strings, run->text);

    offset = (*wp_keymap_cursor(&g_ctx)).offset;
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

/* Concatenates the plain UTF-8 text of the CURRENT paragraph ((*wp_keymap_cursor(&g_ctx))'s
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

    if (!g_ctx.doc_loaded) return WP_FALSE;
    sec = wp_document_section_get(&g_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);

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

    sec = wp_document_section_get(&g_doc, (*wp_keymap_cursor(&g_ctx)).section);
    para = wp_section_paragraph_get(sec, (*wp_keymap_cursor(&g_ctx)).paragraph);

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

/* message/suggestion are fixed, owned buffers (not pointers into a
   response that might outlive its owner) specifically so this struct
   is source-agnostic: populated either from decode_grammar_issue_at()
   (the wire response) or, M8 T8.2, directly from grammar_lite_check()
   (services/grammar) when no offload device is attached -- the rest of
   do_grammar_check() (sort, dialog loop, apply) doesn't need to know or
   care which source an issue came from. Sized larger than
   WP_ATARI_GRAMMAR_DISPLAY_LEN deliberately: the SUGGESTION here is
   what actually gets inserted into the document if the user picks
   Apply, so it must not be truncated to dialog-display width -- only
   append_str_n (below) truncates, and only for what's shown on screen. */
typedef struct {
    u16  start_offset;
    u16  span_len;
    char message[80];
    char suggestion[80];
} wp_atari_grammar_issue;

static void copy_bounded(char *out, u32 out_cap, const u8 *s, u32 s_len)
{
    u32 len = (s_len < out_cap - 1) ? s_len : out_cap - 1;

    memcpy(out, s, len);
    out[len] = '\0';
}

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
        if (i == index) copy_bounded(out->message, (u32)sizeof(out->message), buf + pos, mlen);
        pos += mlen;

        slen = be_read16(buf + pos); pos += 2;
        if (pos + slen > len) return WP_FALSE;
        if (i == index) copy_bounded(out->suggestion, (u32)sizeof(out->suggestion), buf + pos, slen);
        pos += slen;
    }
    return WP_TRUE;
}

/* Appends up to max_len bytes of s into buf via append_str -- keeps one
   long field from crowding the rest of a form_alert dialog off the
   tiny ST screen. Truncates the DIALOG TEXT only; the wire/local values
   actually applied to the document (wp_atari_grammar_issue's own
   fields, above) are never touched by this. Grammar Check's three
   fields (message/quote/suggestion) share one dialog and so each get
   the tighter WP_ATARI_GRAMMAR_DISPLAY_LEN; Chat's reply is the ONLY
   field in its own dialog and is prose, not a short suggestion, so it
   gets its own, more generous WP_ATARI_CHAT_DISPLAY_LEN -- max_len is a
   parameter, not a shared hardcoded macro, specifically so callers
   aren't forced to agree on one truncation width. */
static void append_str_n(char *buf, u32 cap, u32 *pos, const u8 *s, u32 s_len, u32 max_len)
{
    char tmp[192]; /* generous fixed cap: the largest max_len any caller passes today
                       (WP_ATARI_CHAT_DISPLAY_LEN) plus headroom for a future caller */
    u32 tlen = (s_len < max_len) ? s_len : max_len;

    if (tlen >= (u32)sizeof(tmp)) tlen = (u32)sizeof(tmp) - 1;
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
    wp_bool used_offload;
    u32 n, i, j;

    if (!g_ctx.doc_loaded) return;

    paragraph = (u8 *)mem_alloc(WP_ATARI_GRAMMAR_MAX_PARAGRAPH_LEN);
    if (paragraph == NULL) return;

    if (!extract_current_paragraph_text(paragraph, (u32)WP_ATARI_GRAMMAR_MAX_PARAGRAPH_LEN, &paragraph_len)) {
        mem_free(paragraph);
        form_alert(1, FA_INFO "[Place the cursor in a paragraph|with some text, then choose|Grammar Check again.][ OK ]");
        return;
    }

    issue_count = 0;
    used_offload = ensure_offload();

    if (used_offload) {
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

        for (i = 0; i < n; i++) {
            if (!decode_grammar_issue_at(resp.result, resp.result_len, i, &issues[issue_count])) break;
            issue_count++;
        }
    } else {
        /* No Pi/gateway attached -- fall back to the on-device rule-
           based checker (M8 T8.2) rather than failing outright, per
           CLAUDE.md's Offload Rules ("the engine must be fully
           functional WITHOUT an offload device attached"). Told to the
           user up front, not silently swapped in: this catches a much
           narrower, purely mechanical set of issues than the real AI-
           backed path above (docs/services/grammar.md) -- a connected
           gateway that then fails or times out mid-request is a
           different, rarer failure NOT covered by this fallback (still
           a hard failure above), since T8.2's own scope is specifically
           "no device attached at all," not "every possible offload
           failure." */
        grammar_issue local_issues[WP_ATARI_GRAMMAR_MAX_ISSUES];

        form_alert(1, FA_INFO "[No offload device attached.|Using basic on-device grammar|checks instead.][ OK ]");

        n = grammar_lite_check(paragraph, paragraph_len, local_issues, (u32)WP_ATARI_GRAMMAR_MAX_ISSUES);
        for (j = 0; j < n; j++) {
            issues[issue_count].start_offset = (u16)local_issues[j].start_offset;
            issues[issue_count].span_len = (u16)local_issues[j].span_len;
            copy_bounded(issues[issue_count].message, (u32)sizeof(issues[issue_count].message),
                         (const u8 *)local_issues[j].message, (u32)strlen(local_issues[j].message));
            copy_bounded(issues[issue_count].suggestion, (u32)sizeof(issues[issue_count].suggestion),
                         (const u8 *)local_issues[j].suggestion, (u32)strlen(local_issues[j].suggestion));
            issue_count++;
        }
    }

    if (issue_count == 0) {
        if (used_offload) offload_response_free(&resp);
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
        wp_bool has_suggestion = (issue->suggestion[0] != '\0');
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
        append_str_n(msg, (u32)sizeof(msg), &pos, (const u8 *)issue->message, (u32)strlen(issue->message),
                     (u32)WP_ATARI_GRAMMAR_DISPLAY_LEN);
        append_str(msg, (u32)sizeof(msg), &pos, "|Text: \"");
        if ((u32)issue->start_offset + (u32)issue->span_len <= paragraph_len) {
            append_str_n(msg, (u32)sizeof(msg), &pos, paragraph + issue->start_offset, issue->span_len,
                         (u32)WP_ATARI_GRAMMAR_DISPLAY_LEN);
        }
        append_str(msg, (u32)sizeof(msg), &pos, "\"|Suggestion: ");
        if (has_suggestion) {
            append_str(msg, (u32)sizeof(msg), &pos, "\"");
            append_str_n(msg, (u32)sizeof(msg), &pos, (const u8 *)issue->suggestion, (u32)strlen(issue->suggestion),
                         (u32)WP_ATARI_GRAMMAR_DISPLAY_LEN);
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

            if (wp_edit_delete_range(&g_doc, &g_journal, (*wp_keymap_cursor(&g_ctx)).section, (*wp_keymap_cursor(&g_ctx)).paragraph,
                                      run_index, run_start, run_end) == WP_OK) {
                wp_pos insert_pos;
                wp_pos out_pos;

                insert_pos.section = (*wp_keymap_cursor(&g_ctx)).section;
                insert_pos.paragraph = (*wp_keymap_cursor(&g_ctx)).paragraph;
                insert_pos.run = (u32)run_index;
                insert_pos.offset = run_start;

                if (wp_edit_insert_text(&g_doc, &g_journal, insert_pos, (const u8 *)issue->suggestion,
                                         (u32)strlen(issue->suggestion), &out_pos) == WP_OK) {
                    delta += (i32)strlen(issue->suggestion) - (i32)issue->span_len;
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

    if (used_offload) offload_response_free(&resp);
    mem_free(paragraph);

    /* At least one edit landed somewhere in this paragraph -- (*wp_keymap_cursor(&g_ctx))'s
       old run/offset may now be a stale index into a run array that was
       resized by those edits (run_remove_at/run_insert_at can shift or
       shrink it). Resetting to this paragraph's start is always valid:
       every delete-then-insert pair here leaves the paragraph with at
       least one run (insert_span_at_pos's para->run_count == 0 case
       creates one), so run 0 / offset 0 is guaranteed to exist. */
    if (any_applied) {
        (*wp_keymap_cursor(&g_ctx)).run = 0;
        (*wp_keymap_cursor(&g_ctx)).offset = 0;
        relayout_document(g_content_width_twips);
    }
}

/* Chat (M8 T8.3): "freeform assist" -- sends the current paragraph
   (reusing extract_current_paragraph_text(), the same piece Grammar
   Check already built) as a prompt and shows the reply read-only via
   form_alert(), the same "look something up, show it, done" pattern
   Thesaurus already established. Never mutates the document -- unlike
   Grammar Check's Apply/Skip/Stop loop, inventing an insert-the-reply
   flow is out of scope for what T8.3's own AC actually asks for
   (capability gating, not a new editing flow).

   This is the first op whose client actually CONSULTS the CAPS
   handshake's ops_supported bitmask before submitting, rather than
   just handling OFFLOAD_UNSUPPORTED reactively after a full round trip
   the way Thesaurus/Grammar Check/ODT-PDF all do -- see
   docs/formats/offload-protocol.md's OFFLOAD_CHAT section for why this
   is the actual point of "present only when the gateway advertises it,
   absent gracefully otherwise": a gateway that doesn't advertise CHAT
   gets an immediate, honest answer with no wasted network round trip. */
#define WP_ATARI_CHAT_MAX_PARAGRAPH_LEN 2000uL /* matches ollama_gateway.c's CHAT_PROMPT_MAX_LEN */
#define WP_ATARI_CHAT_TIMEOUT_MS        90000uL /* same cold-model-load rationale as Thesaurus/Grammar Check */
#define WP_ATARI_CHAT_DISPLAY_LEN       180 /* the reply is the ONLY field in its dialog, unlike Grammar Check's three */

static wp_bool offload_caps_supports(const offload_capabilities *caps, offload_op op)
{
    return (wp_bool)((caps->ops_supported & (1uL << (u32)op)) != 0);
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

    paragraph = (u8 *)mem_alloc(WP_ATARI_CHAT_MAX_PARAGRAPH_LEN);
    if (paragraph == NULL) return;

    if (!extract_current_paragraph_text(paragraph, (u32)WP_ATARI_CHAT_MAX_PARAGRAPH_LEN, &paragraph_len)) {
        mem_free(paragraph);
        form_alert(1, FA_INFO "[Place the cursor in a paragraph|with some text, then choose|Chat again.][ OK ]");
        return;
    }

    if (!ensure_offload()) {
        mem_free(paragraph);
        form_alert(1, FA_ERROR "[No offload device available.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        return;
    }

    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_CHAT)) {
        mem_free(paragraph);
        form_alert(1, FA_INFO "[This gateway doesn't offer Chat.][ OK ]");
        return;
    }

    req.op = OFFLOAD_CHAT;
    req.payload = paragraph;
    req.payload_len = paragraph_len;
    req.timeout_ms = (u32)WP_ATARI_CHAT_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(paragraph);
        form_alert(1, FA_ERROR "[Chat request failed or timed out.][ OK ]");
        return;
    }
    mem_free(paragraph);

    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        form_alert(1, FA_INFO "[The connected gateway couldn't|answer that.][ OK ]");
        return;
    }

    append_str(msg, (u32)sizeof(msg), &pos, FA_INFO "[");
    append_str_n(msg, (u32)sizeof(msg), &pos, resp.result, resp.result_len, (u32)WP_ATARI_CHAT_DISPLAY_LEN);
    append_str(msg, (u32)sizeof(msg), &pos, "][ OK ]");

    offload_response_free(&resp);
    form_alert(1, msg);
}

/* Insert Image (M15 T15.4): pick a source file, downconvert it through
   the real OFFLOAD_IMAGE op, cache the returned packed pixel bytes to
   disk, and insert a WP_PARA_IMAGE paragraph at the cursor. Mirrors
   do_odt_open's own file-read/offload-submit/error-handling shape and
   do_chat's own CAPS-pre-check pattern (offload_query() +
   offload_caps_supports() BEFORE building a request, not submit-and-
   hope like do_odt_open/do_thesaurus). Real pixel rendering (reading
   blob_path back and blitting it, via plat_draw_image) happens later,
   in paint_document -- this function only needs the paragraph to exist
   at the right size, which is already true once wp_edit_insert_image
   succeeds. */
#define WP_ATARI_IMAGE_MAX_FILE_SIZE 65000uL /* wire frame's own WIRE_MAX_PAYLOAD (64K) minus the 6-byte request header, the same client-side cap docs/formats/offload-protocol.md's OFFLOAD_IMAGE section documents */
#define WP_ATARI_IMAGE_TIMEOUT_MS    60000uL
#define WP_ATARI_IMAGE_TARGET_MAX_W  320u  /* WP_IMAGE_MAX_WIDTH_PX -- the ST low-res ceiling every target clamps against */
#define WP_ATARI_IMAGE_TARGET_MAX_H  200u

/* Real, runtime-queried color budget for an inserted image -- NOT a
   fixed 16, since this project's own real boot resolution is Medium
   (4 colors total, see docs/atari-platform.md's "Boot resolution:
   Medium, not Low"), not every ST necessarily runs Low. Two colors
   (pens 0/1, G_WHITE/G_BLACK) are permanently reserved for the existing
   text/UI -- see plat_draw_image's own comment in draw_atari.c for why
   the ST's palette registers can't be shared the way Amiga's per-
   request ObtainBestPen pens can. Clamped to a minimum of 1 so the
   request itself always stays in OFFLOAD_IMAGE's valid 1..16 range,
   even on a genuinely mono (2-color) screen where plat_draw_image will
   honestly fail and fall back to a solid rect (no spare pen exists at
   all once 0/1 are reserved). */
static u32 atari_image_color_budget(void)
{
    i32 avail = atari_screen_max_colors() - 2;
    if (avail < 1) avail = 1;
    if (avail > 16) avail = 16;
    return (u32)avail;
}

static u32 g_atari_image_cache_counter = 0;

/* "C:\WPIMG###.TMP" -- 8.3-safe, same fixed-scratch-path convention as
   WP_ATARI_SPELL_DICT_PATH/the *_selfcheck scratch files above, except
   counter-suffixed: a single fixed name would let a SECOND Insert
   Image in one session silently overwrite the first image's still-
   referenced pixel cache file underneath it. out must be >= 16 bytes.
   Wraps at 1000 -- a real, accepted session-scope limit far beyond
   anything a real editing session would reach. */
static void atari_format_image_cache_path(char *out, u32 counter)
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
    char path[128];
    char fname[16];
    char fullpath[144];
    plat_file *f;
    u32 size, read_len;
    u8 *req_buf;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;

    if (!g_ctx.doc_loaded) return;

    strcpy(path, "C:\\*.*");
    fname[0] = '\0';
    if (!run_file_selector(path, fname, fullpath, (u32)sizeof(fullpath))) return;

    if (!ensure_offload()) {
        form_alert(1, FA_ERROR "[No offload device available.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        return;
    }
    if (offload_query(&caps) != WP_OK || !offload_caps_supports(&caps, OFFLOAD_IMAGE)) {
        form_alert(1, FA_INFO "[This gateway doesn't offer|Insert Image.][ OK ]");
        return;
    }

    if (plat_file_open(fullpath, PLAT_FILE_READ, &f) != WP_OK) {
        form_alert(1, FA_ERROR "[Couldn't open that file.][ OK ]");
        return;
    }
    if (plat_file_size(f, &size) != WP_OK || size == 0 || size > WP_ATARI_IMAGE_MAX_FILE_SIZE) {
        plat_file_close(f);
        form_alert(1, FA_ERROR "[That image file is too large|for this app to send.][ OK ]");
        return;
    }

    req_buf = (u8 *)mem_alloc(size + 6);
    if (req_buf == NULL) { plat_file_close(f); return; }

    req_buf[0] = (u8)atari_image_color_budget();
    req_buf[1] = 0; /* target_flags: color, not grayscale -- real VDI palette */
    be_write16(req_buf + 2, (u16)WP_ATARI_IMAGE_TARGET_MAX_W);
    be_write16(req_buf + 4, (u16)WP_ATARI_IMAGE_TARGET_MAX_H);

    if (plat_file_read(f, req_buf + 6, size, &read_len) != WP_OK) {
        mem_free(req_buf);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_buf;
    req.payload_len = read_len + 6;
    req.timeout_ms = (u32)WP_ATARI_IMAGE_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        mem_free(req_buf);
        form_alert(1, FA_ERROR "[Insert Image failed or timed out.][ OK ]");
        return;
    }
    mem_free(req_buf);

    if (resp.status != OFFLOAD_OK || resp.result_len < 5) {
        offload_response_free(&resp);
        form_alert(1, FA_ERROR "[The connected gateway couldn't|convert that image.][ OK ]");
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

        if (palette_count < 1 || palette_count > 16 ||
            resp.result_len != 5u + (u32)palette_count * 3u + packed_len) {
            offload_response_free(&resp);
            form_alert(1, FA_ERROR "[The gateway's reply was malformed.][ OK ]");
            return;
        }

        atari_format_image_cache_path(cache_path, g_atari_image_cache_counter);

        if (plat_file_open(cache_path, PLAT_FILE_WRITE, &f) != WP_OK) {
            offload_response_free(&resp);
            form_alert(1, FA_ERROR "[Couldn't write the image cache|file to disk.][ OK ]");
            return;
        }
        st = plat_file_write(f, packed_pixels, packed_len);
        plat_file_close(f);
        if (st != WP_OK) {
            offload_response_free(&resp);
            form_alert(1, FA_ERROR "[Couldn't write the image cache|file to disk.][ OK ]");
            return;
        }
        g_atari_image_cache_counter++;

        memset(&desc, 0, sizeof(desc));
        desc.width_px = (i32)width_px;
        desc.height_px = (i32)height_px;
        desc.display_width_twips = atari_px_to_twips_x((i32)width_px);
        desc.display_height_twips = atari_px_to_twips_y((i32)height_px);
        desc.palette_count = palette_count;
        memcpy(desc.palette, palette, (u32)palette_count * 3u);
        if (wp_strpool_append(&g_doc.strings, (const u8 *)cache_path,
                               (u32)strlen(cache_path), &desc.blob_path) != WP_OK) {
            offload_response_free(&resp);
            return;
        }
        if (fname[0] != '\0') {
            wp_strpool_append(&g_doc.strings, (const u8 *)fname, (u32)strlen(fname), &desc.source_name);
        }
        offload_response_free(&resp);

        if (wp_edit_insert_image(&g_doc, &g_journal, (*wp_keymap_cursor(&g_ctx)), &desc) != WP_OK) {
            form_alert(1, FA_ERROR "[Couldn't insert that image|into the document.][ OK ]");
            return;
        }
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

    /* T162: the caret column, measured with this machine's REAL font.
       Bound by the same limit the JUSTIFY bar below has -- exact advance
       widths can't be hand-computed here -- so this asserts the
       relations paint_document actually depends on instead of pixel
       values: the caret is at 0 on a line's first byte, at exactly that
       line's measured width on its last, never leaves that range, and
       advances strictly monotonically in between.

       The monotonic bar is the one that matters. A caret pinned to the
       left margin -- what this backend shipped before T162, and what Mac
       (T157) and Amiga (T161) each shipped -- returns 0 for every offset
       and fails on the very first step. Deliberately checks line 0 of a
       paragraph that wrapped, so "0 at the line's start" is a real claim
       about the LINE's origin, not one that a paragraph-relative
       measurement would satisfy by accident. */
    if (ok && layout->lines[0].start_run == layout->lines[0].end_run) {
        const wp_line *l0 = &layout->lines[0];
        i32 prev_x = -1;
        u32 off;

        if (wp_line_cursor_x_twips(&doc, para, l0, l0->start_run, l0->start_offset, 0) != 0) {
            ok = WP_FALSE;
        }
        if (ok && wp_line_cursor_x_twips(&doc, para, l0, l0->end_run, l0->end_offset, 0)
                   != l0->width_twips) {
            ok = WP_FALSE;
        }
        for (off = l0->start_offset; ok && off <= l0->end_offset; off++) {
            i32 x = wp_line_cursor_x_twips(&doc, para, l0, l0->start_run, off, 0);

            if (x < 0 || x > l0->width_twips) ok = WP_FALSE;
            if (x <= prev_x) ok = WP_FALSE;
            prev_x = x;
        }
    }

    /* T13.2: real per-word JUSTIFY needs a real font's exact advance
       widths to draw correctly, but this selfcheck can't hand-compute
       those on real hardware (see the comment above wp_amiga_layout_
       selfcheck) -- so it only checks the bounds every real backend's
       paint loop actually depends on: every interior line's slack is
       non-negative (each line's natural width is <= the column by
       construction, so this can never go negative), and the paragraph's
       last line always stays exactly 0 regardless of alignment, per
       PDF export's own established convention. */
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
    const u32 para_count = 200; /* ~24KB of source text. Was 60 (~7.5KB),
                                    which stopped being multi-page the
                                    moment body text got smaller: picking
                                    the 8x8 cell instead of 8x16 in ST
                                    Medium roughly doubled the lines that
                                    fit on an 11in page (about 47 -> 76),
                                    and 60 paragraphs then landed on ONE
                                    page, failing this check's own
                                    page_count >= 2 assertion.

                                    That was a stale fixture, not a
                                    regression -- but it shows the old
                                    margin was thin enough that a font
                                    change could cross it silently. 200
                                    paragraphs is ~3 pages even at the
                                    smallest cell this backend will
                                    choose, and still trivial next to
                                    1MB. */
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
/* WordStar import, on real 68000 hardware.
 *
 * Worth running on target rather than trusting the host tests alone,
 * because this parser's one platform-sensitive operation is exactly the
 * thing that differs here: it masks bit 7 off every text byte. m68k GCC
 * makes `char` SIGNED by default, so a high-bit byte is negative, and
 * any place the format touched a plain char instead of u8 would sign-
 * extend and corrupt text that the host build handles perfectly. This
 * proves the whole path -- soft returns, the high-bit flag, style
 * toggles -- against the compiler that actually ships. */
wp_bool wp_atari_wordstar_selfcheck(void)
{
    static const u8 ws[] = {
        'W','r','a','p','p','e','d' | 0x80u,
        0x8Du, 0x0Au,                       /* soft return: same paragraph */
        'l','i','n','e',
        0x0Du, 0x0Au,                       /* hard return: real break */
        0x02u, 'b','o','l','d', 0x02u,
        0x0Du, 0x0Au
    };
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    const u8 *bytes;
    wp_bool ok = WP_TRUE;

    if (!format_ws_looks_like(ws, (u32)sizeof(ws))) return WP_FALSE;
    if (format_ws_import(ws, (u32)sizeof(ws), &doc) != WP_OK) return WP_FALSE;

    sec = wp_document_section_get(&doc, 0);
    if (sec == NULL || sec->para_count != 2) ok = WP_FALSE;

    if (ok) {
        para = wp_section_paragraph_get(sec, 0);
        /* "Wrapped line" -- the soft return joined, and the flagged 'd'
           survived as 'd' rather than as a sign-extended 0xE4. */
        if (para == NULL || para->run_count != 1) ok = WP_FALSE;
        else {
            bytes = wp_strpool_get(&doc.strings, para->runs[0].text);
            if (para->runs[0].text.length != 12u) ok = WP_FALSE;
            else if (memcmp(bytes, "Wrapped line", 12) != 0) ok = WP_FALSE;
        }
    }

    if (ok) {
        const wp_char_style *cs;
        para = wp_section_paragraph_get(sec, 1);
        if (para == NULL || para->run_count != 1) ok = WP_FALSE;
        else {
            cs = wp_document_cstyle_get(&doc, para->runs[0].style);
            if (cs == NULL || (cs->flags & WP_STYLE_BOLD) == 0) ok = WP_FALSE;
        }
    }

    wp_document_free(&doc);
    return ok;
}

wp_bool wp_atari_thesaurus_selfcheck(void)
{
    static const char text[] = "hello world\n";
    static const char blank_text[] = "   \n";
    u8 word[WP_ATARI_THESAURUS_MAX_WORD_LEN];
    u32 word_len;
    wp_bool ok = WP_TRUE;

    if (format_txt_import((const u8 *)text, (u32)strlen(text), &g_doc) != WP_OK) return WP_FALSE;
    g_ctx.doc_loaded = WP_TRUE;

    /* cursor at document end ("hello world|") -> "world" */
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_end(&g_doc);
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "world", 5) != 0) {
        ok = WP_FALSE;
    }

    /* cursor at the very start of the run -> nothing before it at all */
    (*wp_keymap_cursor(&g_ctx)).offset = 0;
    if (extract_word_before_cursor(word, &word_len)) ok = WP_FALSE;

    /* cursor right after "hello" ("hello| world") -> "hello" */
    (*wp_keymap_cursor(&g_ctx)).offset = 5;
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "hello", 5) != 0) {
        ok = WP_FALSE;
    }

    /* cursor after the space, before "world" ("hello |world") -- skips
       back over the space to find "hello", the nearest preceding word */
    (*wp_keymap_cursor(&g_ctx)).offset = 6;
    if (!extract_word_before_cursor(word, &word_len) || word_len != 5 ||
        memcmp(word, "hello", 5) != 0) {
        ok = WP_FALSE;
    }

    wp_document_free(&g_doc);
    g_ctx.doc_loaded = WP_FALSE;

    /* a run with no alpha bytes anywhere before the cursor -> no word,
       not just "no word immediately adjacent" */
    if (format_txt_import((const u8 *)blank_text, (u32)strlen(blank_text), &g_doc) != WP_OK) return WP_FALSE;
    g_ctx.doc_loaded = WP_TRUE;
    (*wp_keymap_cursor(&g_ctx)) = wp_pos_doc_end(&g_doc);
    if (extract_word_before_cursor(word, &word_len)) ok = WP_FALSE;

    wp_document_free(&g_doc);
    g_ctx.doc_loaded = WP_FALSE;

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
    g_ctx.doc_loaded = WP_TRUE;

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

    (*wp_keymap_cursor(&g_ctx)).section = sec_idx;
    (*wp_keymap_cursor(&g_ctx)).paragraph = para_idx;
    (*wp_keymap_cursor(&g_ctx)).run = 0;
    (*wp_keymap_cursor(&g_ctx)).offset = 0;

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
        strcmp(issue.message, "msg") != 0 || strcmp(issue.suggestion, "fix") != 0) {
        ok = WP_FALSE;
    }
    if (decode_grammar_issue_at(canned, canned_len, 1, &issue)) ok = WP_FALSE; /* out of range */

    /* M8 T8.2: grammar_lite_check() itself (services/grammar) is already
       covered thoroughly by host/tests/test_grammar.c, but ONLY as
       compiled by the host cc -- proving the exact same source actually
       behaves correctly once cross-compiled for m68k and run under real
       CPU emulation is exactly the kind of "don't assume host behavior
       transfers" check this project applies elsewhere (the same reason
       wp_atari_rtf_spell_selfcheck() re-exercises spell_check_word/
       spell_suggest here rather than trusting host/tests/test_spell.c
       alone). One representative issue is enough: the rule logic itself
       is already exhaustively proven on host. */
    {
        static const char gtext[] = "this the the works";
        grammar_issue gout[4];
        u32 gcount = grammar_lite_check((const u8 *)gtext, (u32)strlen(gtext), gout, 4);
        wp_bool saw_capital = WP_FALSE, saw_doubled = WP_FALSE;
        u32 gi;

        for (gi = 0; gi < gcount; gi++) {
            if (strcmp(gout[gi].message, "Missing capital letter") == 0) saw_capital = WP_TRUE;
            if (strcmp(gout[gi].message, "Repeated word") == 0) saw_doubled = WP_TRUE;
        }
        if (!saw_capital || !saw_doubled) ok = WP_FALSE;
    }

    wp_document_free(&g_doc);
    g_ctx.doc_loaded = WP_FALSE;

    return ok;
}

wp_bool wp_atari_chat_selfcheck(void)
{
    offload_capabilities caps;
    wp_bool ok = WP_TRUE;

    /* Pure bit-math over a hand-built caps struct, not a real gateway
       round trip -- proves offload_caps_supports() itself, the shared
       pre-check every offload-consuming action (Chat, and since M15,
       Insert Image) relies on before ever building a request. IMAGE is
       now included as a REAL supported bit (M15 T15.4) rather than the
       permanently-absent one it used to be -- do_insert_image()'s own
       real round trip against a real gateway is proven separately by
       host/tests/test_image_convert.c and the CI matrix's real
       screenshot verification, not by this function. */
    memset(&caps, 0, sizeof(caps));
    caps.ops_supported = (1uL << OFFLOAD_THESAURUS) | (1uL << OFFLOAD_CHAT) | (1uL << OFFLOAD_IMAGE) |
                         (1uL << OFFLOAD_DICTATE);

    if (!offload_caps_supports(&caps, OFFLOAD_CHAT)) ok = WP_FALSE;
    if (!offload_caps_supports(&caps, OFFLOAD_IMAGE)) ok = WP_FALSE;
    if (!offload_caps_supports(&caps, OFFLOAD_DICTATE)) ok = WP_FALSE;
    if (!offload_caps_supports(&caps, OFFLOAD_THESAURUS)) ok = WP_FALSE;
    if (offload_caps_supports(&caps, OFFLOAD_GRAMMAR_CHECK)) ok = WP_FALSE;

    return ok;
}

/* M8.5: a small, fixed-size system font used ONLY for the status line's
   own height/drawing -- deliberately independent of the document's
   current cstyle (which changes per run, per toggle) so the reserved
   strip's height never shifts underneath the document as the user
   types. 8pt (160 twips), the smallest step in g_font_sizes_twips. */
#define WP_ATARI_STATUSLINE_SIZE_TWIPS 160

/* Shared by the vi-mode status bar and the help hint bar/body text
   below -- both need "how tall is one line of some font, in pixels"
   from real plat_font_metrics_get, never a hardcoded guess (matters
   concretely given ST Low's own documented "notoriously tight" 40-
   column/200-row budget). */
static i32 font_line_height_px(i32 size_twips)
{
    plat_font *font;
    plat_font_metrics fm;
    i32 h_px;

    if (plat_font_open((const u8 *)"", 0, size_twips, 0, &font) != WP_OK) return 0;
    plat_font_metrics_get(font, &fm);
    h_px = atari_twips_to_px_y(fm.ascent + fm.descent + fm.leading);
    plat_font_close(font);
    return h_px;
}

static i32 statusline_height_px(void)
{
    return font_line_height_px(WP_ATARI_STATUSLINE_SIZE_TWIPS);
}

/* Paints the reserved bottom strip showing vi-mode's current mode (and,
   in Command-line mode, the ':'/'/' buffer being typed) -- see
   platform/common/statusline.h. Always repaints in full (the strip is
   small and its content can change on every keystroke, unlike the
   document body's damaged-rectangle tracking above) and sets its own
   clip rather than reusing the caller's, since it's called once after
   redraw_window's own per-rectangle document loop has already finished
   and reset clipping. A no-op if no document is loaded (nothing to show
   a vi-mode state for) or the window is too short to fit the bar. */
static void paint_status_line(short window_handle, VdiHdl vdi, GRECT full_work, i32 bar_h_px)
{
    atari_gc gc;
    plat_font *font;
    wp_vi_status_view view;
    char mode_buf[40];
    short pxy[4];

    if (!g_ctx.doc_loaded || bar_h_px <= 0 || bar_h_px > full_work.g_h) return;

    gc.vdi = vdi;
    gc.window_handle = window_handle;

    pxy[0] = full_work.g_x;
    pxy[1] = (short)(full_work.g_y + full_work.g_h - bar_h_px);
    pxy[2] = (short)(full_work.g_x + full_work.g_w - 1);
    pxy[3] = (short)(full_work.g_y + full_work.g_h - 1);
    vs_clip(vdi, 1, pxy);

    if (plat_font_open((const u8 *)"", 0, WP_ATARI_STATUSLINE_SIZE_TWIPS, 0, &font) == WP_OK) {
        /* wp_keymap_get_status_view already folds the Command-line
           buffer into its one composed mode string and leaves
           pending_count/pending_cmd at 0/NULL -- see its own header
           comment for why (no per-mode accessor exists yet that
           wouldn't be more confusing shown alone; a documented, minor
           gap, not fixed here). */
        wp_keymap_get_status_view(&g_ctx, mode_buf, (u32)sizeof(mode_buf), &view);

        wp_statusline_paint((plat_gc *)&gc, font, (i32)pxy[0], (i32)pxy[1],
                             (i32)(pxy[2] - pxy[0] + 1), bar_h_px, &view);
        plat_font_close(font);
    }

    vs_clip(vdi, 0, pxy);
}

/* 10pt -- comfortably readable for help prose, smaller than the
   document's own 12pt default so more of it fits on ST Low's tight
   40-column/200-row screen. */
#define WP_ATARI_HELP_BODY_SIZE_TWIPS 200

/* Paints the help pager full-screen (replacing the document body and
   the vi-mode status bar entirely while g_help_active is set -- see
   wp_dispatch_key/redraw_window) plus a small fixed hint bar along the
   bottom, reusing wp_statusline_paint for that hint the same way the
   vi-mode status bar does: platform/common/statusline.c is deliberately
   generic ("usable by anything that wants a labeled status strip, not
   hard-wired to vi-mode specifically", per its own header comment), and
   a static hint string is exactly that. */
static void paint_help(short window_handle, VdiHdl vdi)
{
    atari_gc gc;
    GRECT work;
    plat_font *body_font;
    i32 hint_bar_h_px, body_line_h_px, body_h_px;
    short pxy[4];

    wind_get_grect(window_handle, WF_WORKXYWH, &work);
    gc.vdi = vdi;
    gc.window_handle = window_handle;

    pxy[0] = work.g_x;
    pxy[1] = work.g_y;
    pxy[2] = (short)(work.g_x + work.g_w - 1);
    pxy[3] = (short)(work.g_y + work.g_h - 1);
    vs_clip(vdi, 1, pxy);
    vsf_interior(vdi, FIS_SOLID);
    vsf_color(vdi, G_WHITE);
    v_bar(vdi, pxy);
    vs_clip(vdi, 0, pxy);

    hint_bar_h_px = statusline_height_px();
    if (hint_bar_h_px > work.g_h) hint_bar_h_px = 0;
    body_line_h_px = font_line_height_px(WP_ATARI_HELP_BODY_SIZE_TWIPS);
    body_h_px = work.g_h - hint_bar_h_px;
    if (body_h_px < 0) body_h_px = 0;

    if (plat_font_open((const u8 *)"", 0, WP_ATARI_HELP_BODY_SIZE_TWIPS, 0, &body_font) == WP_OK) {
        /* wp_help_view_paint positions text by BASELINE (plat_draw_text's
           own convention, matching v_gtext) -- ascent_px tells it where
           row 0's baseline sits below the viewport's own top edge. A
           bare y_px with no ascent offset drew row 0's ascending strokes
           above the viewport entirely -- a real bug found and fixed via
           Amiga's own Phase 6 interactive verification (see
           help_view.h's own comment), latent here too since this pager
           was never interactively exercised end to end. */
        plat_font_metrics body_fm;
        i32 body_ascent_px = 0;
        if (plat_font_metrics_get(body_font, &body_fm) == WP_OK) {
            body_ascent_px = atari_twips_to_px_y(body_fm.ascent);
        }
        wp_help_view_paint((plat_gc *)&gc, body_font, work.g_x, work.g_y, work.g_w, body_h_px,
                            atari_px_to_twips_x((i32)work.g_w), body_line_h_px, body_ascent_px, &g_help_view);
        plat_font_close(body_font);
    }

    if (hint_bar_h_px > 0) {
        plat_font *hint_font;
        wp_vi_status_view view;
        static const u8 hint[] = "HELP -- j/k select, Enter follow, Backspace back, Esc/q close";

        pxy[1] = (short)(work.g_y + work.g_h - hint_bar_h_px);
        vs_clip(vdi, 1, pxy);
        if (plat_font_open((const u8 *)"", 0, WP_ATARI_STATUSLINE_SIZE_TWIPS, 0, &hint_font) == WP_OK) {
            view.mode_label_utf8 = hint;
            view.mode_label_len = (u32)(sizeof(hint) - 1);
            view.pending_count = 0;
            view.pending_cmd_utf8 = NULL;
            view.pending_cmd_len = 0;
            wp_statusline_paint((plat_gc *)&gc, hint_font, (i32)pxy[0], (i32)pxy[1],
                                 (i32)work.g_w, hint_bar_h_px, &view);
            plat_font_close(hint_font);
        }
        vs_clip(vdi, 0, pxy);
    }
}

/* Every repaint in this backend funnels through here, so this is the one
   place that has to observe GEM's two screen-ownership rules:

     wind_update(BEG_UPDATE/END_UPDATE) tells the AES "I am drawing, do
     not drop a menu or draw an alert on top of me right now", and
     graf_mouse(M_OFF/M_ON) takes the mouse cursor off the screen for the
     duration -- the VDI has no idea the cursor is there, so drawing
     under a visible cursor corrupts the AES's save-under buffer, and the
     next cursor move restores those stale bytes onto the screen.

   Neither was being done anywhere in this backend: graf_mouse(ARROW) at
   startup was the only mouse call in the entire file. The visible
   symptom was a permanent block of garbage at the screen centre -- the
   mouse's power-on position, painted over by the startup document draw
   and then "restored" on first movement. On a 4-plane Medium-res screen
   the stale planes decode to a solid green rectangle, which is what made
   it look like an image-drawing bug rather than a cursor bug. */
static void redraw_window(short window_handle, VdiHdl vdi)
{
    GRECT r, work, doc_area;
    short pxy[4];
    i32 bar_h_px;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, (const MFORM *)0);

    if (g_help_active) {
        paint_help(window_handle, vdi);
        graf_mouse(M_ON, (const MFORM *)0);
        wind_update(END_UPDATE);
        return;
    }

    wind_get_grect(window_handle, WF_WORKXYWH, &work);

    /* Nothing in the engine's edit path (vimode.c/edit.c) or platform/
       common/keymap.c relayouts a paragraph after a keystroke edits it
       -- it only sets para->layout_dirty and leaves the rebuild to the
       app. Before this call, only a whole-document replace (Open, Cut,
       Paste) ever triggered relayout_document, so typed text landed in
       the model as a real wp_run byte edit but paint_document kept
       walking a STALE para->layout that didn't know about the new
       bytes: on screen the document silently truncated mid-sentence and
       the typed characters never appeared at all. Verified interactively
       under Hatari/EmuTOS -- platform/dos/app_shell.c already carried
       exactly this fix and exactly this comment (found there during M14
       Phase 5 visual verification); Atari, Amiga and Mac never got it.
       relayout_document skips clean paragraphs, so this costs one flag
       test per paragraph on a redraw that changed nothing. */
    if (g_ctx.doc_loaded) {
        relayout_document(atari_px_to_twips_x((i32)work.g_w));
    }

    bar_h_px = statusline_height_px();
    if (bar_h_px > work.g_h) bar_h_px = 0; /* degenerate/tiny window -- skip the bar rather
                                               than hand paint_document a negative height */
    doc_area = work;
    doc_area.g_h = (short)(work.g_h - bar_h_px);

    wind_get_grect(window_handle, WF_FIRSTXYWH, &r);
    while (r.g_w > 0 && r.g_h > 0) {
        pxy[0] = r.g_x;
        pxy[1] = r.g_y;
        pxy[2] = (short)(r.g_x + r.g_w - 1);
        pxy[3] = (short)(r.g_y + r.g_h - 1);
        vs_clip(vdi, 1, pxy);
        /* No whole-area v_bar here any more: paint_document clears each
           line's strip as it draws it, which removes the full-screen
           white flash this used to produce on every keystroke while
           still erasing everything that needs erasing. The clip
           rectangle above still bounds all of it, so a partial AES
           redraw repaints exactly its own rectangle as before. */
        paint_document(window_handle, vdi, doc_area);
        wind_get_grect(window_handle, WF_NEXTXYWH, &r);
    }
    vs_clip(vdi, 0, pxy);

    paint_status_line(window_handle, vdi, work, bar_h_px);

    graf_mouse(M_ON, (const MFORM *)0);
    wind_update(END_UPDATE);
}

/* Translates one AES key into a portable wp_key_event (M8.5) --
   evnt_multi's Key output packs the IKBD scan code in the high byte and
   the ASCII code (0 if the key has none) in the low byte; verified
   against ~/Documents/Atari's Atari Compendium (Appendix F, IKBD Scan
   Codes), not just recalled from memory, per this project's own "verify
   against a real/authoritative source" discipline. Scan code is
   consulted FIRST for the handful of keys that need a dedicated
   wp_special_key (Escape, Enter -- main and numeric-keypad, Tab,
   Backspace, Delete, the four arrows, Home, F1-F10): those all return
   early below, before ASCII is even inspected, which is what correctly
   tells apart the physical Escape key (scan 0x01, ascii 0x1B) from
   Ctrl+[ (scan 0x1A, ALSO ascii 0x1B -- same ASCII control code, real
   physically different key). Anything else falls through to the ASCII
   byte: 0 means an unmapped special key this app has no use for (Help/
   Undo/Insert/numpad/mouse-equivalent keys -- a real, honest gap, not a
   crash), 0x01-0x1F means a Ctrl+<letter> chord (decoded the same way
   host/tests/test_vimode.c's own feed_keys script helper already does,
   e.g. 0x02 => {codepoint:'b', mods:CTRL}, i.e. Ctrl+B), anything else
   is a plain printable codepoint. The ST keyboard has no dedicated Page
   Up/Down or End key -- WP_KEY_PAGE_UP/PAGE_DOWN/END are simply never
   produced here, matching the Compendium's own table (no such rows
   exist), not an oversight.

   M10 Phase 2: kstate is evnt_multi's own kstate output (real AES
   bits: 0=Rshift, 1=Lshift, 2=Ctrl, 3=Alt -- Atari Compendium, same
   source as the scan-code table above) -- previously captured by every
   caller and silently discarded. Only Alt is consulted here: Ctrl stays
   entirely ASCII-derived, unchanged from before (kstate's own Ctrl bit
   is redundant with it and was never needed). Alt is ORed into ev.mods
   uniformly at the single exit point below, regardless of which case
   produced ev -- a special key, a Ctrl-chord, or a plain codepoint can
   each also be Alt-held (Alt+Escape, Alt+Ctrl+B, Alt+b are all real,
   distinct chords a future scheme may bind), so this is applied after
   the switch/if chain, not folded into any one branch. */
static wp_key_event atari_translate_key(short key, short kstate)
{
    u8 ascii = (u8)(key & 0xFF);
    u8 scan  = (u8)((key >> 8) & 0xFF);
    u8 alt   = (kstate & 0x08) ? (u8)WP_MOD_ALT : 0;
    wp_key_event ev;

    ev.codepoint = 0;
    ev.special = (u16)WP_KEY_NONE;
    ev.mods = 0;
    ev._pad = 0;

    switch (scan) {
    case 0x01: ev.special = (u16)WP_KEY_ESCAPE; goto done;
    case 0x1C: case 0x72: ev.special = (u16)WP_KEY_ENTER; goto done; /* main + numpad Enter */
    case 0x0F: ev.special = (u16)WP_KEY_TAB; goto done;
    case 0x0E: ev.special = (u16)WP_KEY_BACKSPACE; goto done;
    case 0x53: ev.special = (u16)WP_KEY_DELETE; goto done;
    case 0x48: ev.special = (u16)WP_KEY_UP; goto done;
    case 0x50: ev.special = (u16)WP_KEY_DOWN; goto done;
    case 0x4B: ev.special = (u16)WP_KEY_LEFT; goto done;
    case 0x4D: ev.special = (u16)WP_KEY_RIGHT; goto done;
    case 0x47: ev.special = (u16)WP_KEY_HOME; goto done;
    default: break;
    }
    if (scan >= 0x3B && scan <= 0x44) {
        ev.special = (u16)(WP_KEY_F1 + (scan - 0x3B));
        goto done;
    } else if (scan == 0x62) {
        /* The ST's own HELP key. Scan codes verified against the Atari
           Compendium's IKBD table (p.787: HELP 0x6200, UNDO 0x6100),
           not recalled -- the same discipline the table below already
           states for every other key here. Both keys were simply
           unhandled before now, on a keyboard that has them. */
        ev.special = (u16)WP_KEY_HELP;
        goto done;
    } else if (scan == 0x61) {
        ev.special = (u16)WP_KEY_UNDO;
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

/* M8.5: proves atari_translate_key's scan-code table against real IKBD
   (scan<<8 | ascii) values taken directly from ~/Documents/Atari's Atari
   Compendium (Appendix F), the same source atari_translate_key's own
   comment cites -- not hand-waved from memory. Exists because this
   function cannot be reached by any host test (host has no AES/IKBD),
   and because a real interactive verification attempt (RETROWP.PRG
   under headless Hatari, driven by synthetic xdotool key events into
   the emulator's SDL window) did not reliably demonstrate a visible
   keystroke end-to-end in this environment -- consistent with, and
   reinforcing, the xdotool-fidelity gap docs/atari-platform.md's T4.3
   section already documents for this exact Hatari/SDL/X11 stack. Same
   boundary every other wp_atari_*_selfcheck in this file already
   respects: no blocking call, no real event loop, just the pure
   translation function against hand-verified expected values.

   The Ctrl+[ vs Escape row is the one case that actually EXERCISES the
   scan-code-first design decision (see atari_translate_key's own
   comment): both produce ASCII 0x1B, and only the scan code (0x01 vs
   0x1A) tells them apart -- a table that only ever inspected the ASCII
   byte, like the pre-M8.5 wp_dispatch_key did, could not distinguish
   them at all.

   M10 Phase 2: every pre-existing row now carries an explicit kstate
   column, 0 in every one of them -- proves Alt plumbing changed
   NOTHING about any previously-passing case (the actual regression-
   safety guard, not just "this still compiles"). The new Alt-flavored
   rows at the end prove the bit is read correctly, combined correctly
   with an already-Ctrl'd chord, and left off when kstate's bit 3 is
   clear despite other kstate bits being set (Shift/Ctrl bits 0-2 are
   real AES kstate bits too; atari_translate_key must ignore them, per
   its own header comment that Ctrl stays purely ASCII-derived). */
wp_bool wp_atari_key_translate_selfcheck(void)
{
    static const struct {
        short key;          /* (scan << 8) | ascii, as evnt_multi returns it */
        short kstate;       /* AES kstate: bit0=Rshift,1=Lshift,2=Ctrl,3=Alt */
        u32   codepoint;
        u16   special;
        u8    mods;
    } cases[] = {
        { (short)((0x01 << 8) | 0x1B), 0, 0, (u16)WP_KEY_ESCAPE, 0 },
        { (short)((0x1C << 8) | 0x0D), 0, 0, (u16)WP_KEY_ENTER, 0 },
        { (short)((0x72 << 8) | 0x0D), 0, 0, (u16)WP_KEY_ENTER, 0 }, /* numpad Enter */
        { (short)((0x0F << 8) | 0x09), 0, 0, (u16)WP_KEY_TAB, 0 },
        { (short)((0x0E << 8) | 0x08), 0, 0, (u16)WP_KEY_BACKSPACE, 0 },
        { (short)((0x53 << 8) | 0x7F), 0, 0, (u16)WP_KEY_DELETE, 0 },
        { (short)((0x48 << 8) | 0x00), 0, 0, (u16)WP_KEY_UP, 0 },
        { (short)((0x50 << 8) | 0x00), 0, 0, (u16)WP_KEY_DOWN, 0 },
        { (short)((0x4B << 8) | 0x00), 0, 0, (u16)WP_KEY_LEFT, 0 },
        { (short)((0x4D << 8) | 0x00), 0, 0, (u16)WP_KEY_RIGHT, 0 },
        { (short)((0x47 << 8) | 0x00), 0, 0, (u16)WP_KEY_HOME, 0 },
        { (short)((0x3B << 8) | 0x00), 0, 0, (u16)WP_KEY_F1, 0 },
        { (short)((0x44 << 8) | 0x00), 0, 0, (u16)WP_KEY_F10, 0 },
        /* HELP and UNDO were recorded here for years as "real, unmapped,
           harmless" -- the ST has both keys and this app ignored them.
           Now mapped; scan codes from the Compendium's IKBD table
           (p.787: HELP 0x6200, UNDO 0x6100). */
        { (short)((0x62 << 8) | 0x00), 0, 0, (u16)WP_KEY_HELP, 0 },
        { (short)((0x61 << 8) | 0x00), 0, 0, (u16)WP_KEY_UNDO, 0 },
        { (short)((0x1E << 8) | 0x61), 0, 'a', (u16)WP_KEY_NONE, 0 },        /* plain 'a' */
        { (short)((0x39 << 8) | 0x20), 0, 0x20, (u16)WP_KEY_NONE, 0 },       /* Space -- printable-range boundary */
        { (short)((0x1E << 8) | 0x01), 0, 'a', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL }, /* Ctrl+A */
        { (short)((0x30 << 8) | 0x02), 0, 'b', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL }, /* Ctrl+B -- Bold shortcut */
        { (short)((0x1A << 8) | 0x1B), 0, '{', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL }, /* Ctrl+[ -- NOT Escape, despite matching ASCII */
        { (short)((0x1E << 8) | 0x61), 0x08, 'a', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT }, /* Alt+A -- kstate bit3 set */
        { (short)((0x30 << 8) | 0x02), 0x08, 'b', (u16)WP_KEY_NONE, (u8)(WP_MOD_CTRL | WP_MOD_ALT) }, /* Alt+Ctrl+B */
        { (short)((0x01 << 8) | 0x1B), 0x08, 0, (u16)WP_KEY_ESCAPE, (u8)WP_MOD_ALT }, /* Alt+Escape -- a special key can be Alt'd too */
        { (short)((0x1E << 8) | 0x61), 0x07, 'a', (u16)WP_KEY_NONE, 0 } /* Shift+Ctrl bits (0-2) set, Alt bit clear -- must NOT set WP_MOD_ALT */
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wp_key_event ev = atari_translate_key(cases[i].key, cases[i].kstate);
        if (ev.codepoint != cases[i].codepoint) return WP_FALSE;
        if (ev.special != cases[i].special) return WP_FALSE;
        if (ev.mods != cases[i].mods) return WP_FALSE;
    }
    return WP_TRUE;
}

/* M8.5 Phase 10: proves the help system's non-blocking pieces against
   the REAL compiled-in help content and REAL Atari VDI font metrics,
   not just host/tests' synthetic fixtures and fake monospace metrics
   (host/tests/test_help_view.c's own precedent). Deliberately does NOT
   call wp_help_view_paint or actually open the help viewer (F1) -- like
   every other selfcheck in this file, no real window/interactive event
   loop exists in this smoke-test path (see app_shell_internal.h's own
   header comment on why). What IS real and worth proving here: the
   parser accepts the shipped content and every link resolves (a build-
   time guarantee on host already, via test_help_content.c -- this is
   the same property holding after a genuine m68k cross-compile), and
   wp_help_wrap_line -- the greedy word-wrap algorithm the pager relies
   on -- runs correctly against a REAL VDI-measured font rather than
   host's simplified fake-monospace one, which could plausibly hide a
   real bug (e.g. a font whose measured widths aren't a clean multiple
   of anything, unlike host's deliberately round fake metrics). */
#define WP_HELP_SELFCHECK_MAX_ROWS 8

wp_bool wp_atari_help_selfcheck(void)
{
    wp_help_doc doc;
    plat_font *font;
    u32 starts[WP_HELP_SELFCHECK_MAX_ROWS];
    u32 lens[WP_HELP_SELFCHECK_MAX_ROWS];
    u32 row_count;
    static const char sample[] =
        "RetroWP wraps this sentence across more than one real Atari VDI-measured row.";

    if (wp_help_doc_parse(g_help_source_retrowp, (u32)g_help_source_retrowp_len, &doc) != WP_OK) {
        return WP_FALSE;
    }
    if (doc.node_count == 0 ||
        wp_help_doc_find_node(&doc, (const u8 *)"Main", 4) == WP_HELP_NODE_NONE) {
        wp_help_doc_free(&doc);
        return WP_FALSE;
    }
    wp_help_doc_free(&doc);

    if (plat_font_open((const u8 *)"", 0, WP_ATARI_HELP_BODY_SIZE_TWIPS, 0, &font) != WP_OK) {
        return WP_FALSE;
    }
    /* A narrow column forces real wrapping (not just a single row) --
       proves more than "it returned something" against real measurement. */
    row_count = wp_help_wrap_line(font, (const u8 *)sample, (u32)strlen(sample), 3000,
                                   starts, lens, WP_HELP_SELFCHECK_MAX_ROWS);
    plat_font_close(font);

    if (row_count < 2 || row_count > WP_HELP_SELFCHECK_MAX_ROWS) return WP_FALSE;
    {
        u32 i;
        for (i = 0; i < row_count; i++) {
            if (lens[i] == 0) return WP_FALSE; /* every row must carry real content */
        }
    }
    return WP_TRUE;
}

/* M10 Phase 6: proves wp_config_load resolves all five real keymap=
   values against a REAL file at WP_ATARI_CONFIG_PATH, written and read
   through real Atari file I/O -- host/tests/test_config.c already
   proves wp_config_parse's own string-matching logic exhaustively
   against hand-fed byte arrays, so this selfcheck deliberately doesn't
   re-prove that; it proves the one thing only real target hardware
   can: that a real plat_file_write/plat_file_open/plat_file_read round
   trip at the real configured path resolves every value correctly.
   Leaves the fixture exactly as clean as it found it (plat_file_delete
   at the end, unconditionally) -- a real user's own first run must see
   a config-file-free system, not a stray leftover from this check. */
wp_bool wp_atari_config_selfcheck(void)
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

        if (plat_file_open(WP_ATARI_CONFIG_PATH, PLAT_FILE_WRITE, &f) != WP_OK) { ok = WP_FALSE; continue; }
        if (plat_file_write(f, (const u8 *)cases[i].content, (u32)strlen(cases[i].content)) != WP_OK) ok = WP_FALSE;
        plat_file_close(f);

        wp_config_load(WP_ATARI_CONFIG_PATH, &cfg);
        if (cfg.keymap_id != cases[i].expected) ok = WP_FALSE;
    }

    plat_file_delete(WP_ATARI_CONFIG_PATH);
    return ok;
}

/* Reflects a Bold/Italic/Underline toggle's new state onto its menu
   item's checkmark, regardless of whether it was triggered by a menu
   click or a Ctrl+<letter> keyboard shortcut (both now call the exact
   same g_wp_actions handler -- see wp_dispatch_key/wp_dispatch_message
   below). A no-op for every other action id. */
static void sync_format_check(wp_action_id id)
{
    switch (id) {
    case WP_ACT_FORMAT_BOLD:
        menu_icheck(wp_menu_tree, WP_MENU_OBJ_FORMAT_BOLD,
                    (g_ctx.style_flags & WP_STYLE_BOLD) ? CHECK : UNCHECK);
        break;
    case WP_ACT_FORMAT_ITALIC:
        menu_icheck(wp_menu_tree, WP_MENU_OBJ_FORMAT_ITALIC,
                    (g_ctx.style_flags & WP_STYLE_ITALIC) ? CHECK : UNCHECK);
        break;
    case WP_ACT_FORMAT_UNDERLINE:
        menu_icheck(wp_menu_tree, WP_MENU_OBJ_FORMAT_UNDERLINE,
                    (g_ctx.style_flags & WP_STYLE_UNDERLINE) ? CHECK : UNCHECK);
        break;
    default:
        break;
    }
}

/* The backend-supplied (handler == NULL in g_wp_actions) actions that
   are actually reachable by keyboard shortcut today -- every other
   Tools action and Help have shortcut_ascii == 0 (see actions.c), so
   wp_action_find_by_shortcut can never return them from a real
   keystroke; this switch only needs to cover what can really arrive.
   WP_ACT_TOOLS_INSERTIMAGE (M15) is deliberately Ctrl+G-only, NOT
   added to wp_menu_tree[]'s own hand-built Tools OBJECT chain --
   touching that structure is exactly the class of change that caused
   two real regressions earlier in this project's history (see
   docs/atari-platform.md), and this action doesn't need it: a real
   keyboard shortcut reaches it just as well, the same mitigation
   T8.5.7's own dialog actions already used for the same reason. */
/* Defined below, next to the F1 path that was its only caller before the
   Help menu existed. Forward-declared rather than moved so this change
   does not reshuffle unrelated code. */
static void open_help_node(short window_handle, VdiHdl vdi,
                            const char *node_name, u32 node_name_len);

static void invoke_backend_action(short window_handle, VdiHdl vdi, wp_action_id id)
{
    const char *node;
    u32 node_len = 0;

    /* The three Help entries differ only by which node of the one help
       document they open, so they share a single arm rather than three
       near-identical ones. The node name comes from the shared registry
       (wp_action_help_node) so all four backends open the same node for
       the same action -- see actions.h for why that mapping is not
       repeated per backend. */
    node = wp_action_help_node(id, &node_len);
    if (node != NULL) {
        open_help_node(window_handle, vdi, node, node_len);
        return;
    }

    switch (id) {
    case WP_ACT_FILE_OPEN:         do_file_open(); break;
    case WP_ACT_FILE_SAVE:         do_file_save(); break;
    case WP_ACT_EDIT_CUT:          do_edit_cut(); break;
    case WP_ACT_EDIT_PASTE:        do_edit_paste(); break;
    case WP_ACT_TOOLS_SPELLCHECK:  do_spell_check(); break;
    case WP_ACT_TOOLS_THESAURUS:   do_thesaurus(); break;
    case WP_ACT_TOOLS_GRAMMARCHECK: do_grammar_check(); break;
    case WP_ACT_TOOLS_CHAT:        do_chat(); break;
    case WP_ACT_TOOLS_INSERTIMAGE: do_insert_image(); break;
    case WP_ACT_TOOLS_DICTATE:
        /* Two lines, because everything after bringing the link up is
           portable -- see wp_action_dictate's own comment in
           platform/common/actions.h. */
        if (!ensure_offload()) {
            form_alert(1, FA_ERROR "[No offload device available.|(tried " WP_ATARI_OFFLOAD_ENDPOINT ")][ OK ]");
        } else {
            (void)wp_action_dictate(&g_ctx);
        }
        break;
    default: break;
    }
}

/* M8.5: every keystroke is translated to a portable wp_key_event and fed
   to vi-mode first (engine/src/vimode.c) -- Normal-mode motions/
   operators, Insert-mode text entry, Visual/Command modes. A key vi-mode
   itself doesn't claim falls through to wp_action_find_by_shortcut,
   which covers every app-level Ctrl+<letter> shortcut (Bold/Italic/
   Underline/Size/Align/Open/Save/Quit/Cut/Paste -- see platform/common/
   actions.c's g_wp_actions table), EXCEPT while composing a ':' or '/'
   command line, where every non-Escape/Enter/Backspace key belongs to
   vi-mode's own cmdline buffer, not an app shortcut. This split is safe
   by construction, not just convention: vimode.c's Normal/Insert/Visual
   dispatch now requires an unmodified key for every plain-letter
   command (the one deliberate exception, Ctrl+R for redo, isn't
   registered as any g_wp_action shortcut -- see the mods-guard fix in
   engine/src/vimode.c and docs/vimode.md), so a Ctrl-chord is guaranteed
   to reach here un-consumed by vi-mode. */
/* Opens the help viewer at the named node (falling back to node 0 if the
   name doesn't resolve). Shared by F1 (opens at "Main") and the Desk
   menu's "About RetroWP..." item (opens at "About") so both routes
   through the one help system this project already built (M8.5 Phase
   10) rather than a second, ad hoc dialog. */
static void open_help_node(short window_handle, VdiHdl vdi,
                            const char *node_name, u32 node_name_len)
{
    if (!g_help_doc_loaded) return;
    {
        u32 node = wp_help_doc_find_node(&g_help_doc, (const u8 *)node_name, node_name_len);
        wp_help_view_init(&g_help_view, &g_help_doc,
                           (node != WP_HELP_NODE_NONE) ? node : 0);
    }
    g_help_active = WP_TRUE;
    redraw_window(window_handle, vdi);
}

/* Dispatches a wp_action_id exactly the way the pre-M10 vi-mode
   shortcut-fallback path always has (handler-or-backend, plus the
   Atari-specific menu-checkmark sync) -- now the one place that logic
   lives, shared between that fallback (inside wp_keymap_feed_key's own
   VI arm) and any other scheme's WP_KEYMAP_RESULT_ACTION outcome. */
static void invoke_action(short window_handle, VdiHdl vdi, wp_action_id id)
{
    const wp_action *act = wp_action_find(id);

    /* wp_action_find returns NULL for an out-of-range id, and id reaches
       us as a plain u8 out of wp_keymap_outcome -- so a stale or garbage
       action_id from any scheme would crash here rather than no-op. */
    if (act == NULL) return;

    if (act->handler != NULL) {
        act->handler(&g_ctx);
        sync_format_check(id);
    } else {
        invoke_backend_action(window_handle, vdi, id);
    }
}

wp_bool wp_dispatch_key(short window_handle, VdiHdl vdi, short key, short kstate)
{
    wp_key_event ev;
    wp_keymap_outcome outcome;

    if (!g_ctx.doc_loaded) return WP_FALSE;

    ev = atari_translate_key(key, kstate);

    /* Help is a whole separate UI mode (M8.5 Phase 10): while active,
       every key goes to wp_help_view_feed_key instead of the active
       keymap scheme, full stop, until the viewer itself reports a close
       request (Escape or 'q' -- see help_view.h). F1 opens it from any
       scheme/mode/state; there is no menu path this pass (see
       g_help_active's own comment), and pressing F1 again while already
       showing help is a harmless no-op rather than resetting the user's
       position back to the index node. */
    if (g_help_active) {
        if (wp_help_view_feed_key(&g_help_view, ev)) g_help_active = WP_FALSE;
        redraw_window(window_handle, vdi);
        return WP_FALSE;
    }
    /* A dedicated HELP key beats everything: no scheme competes for it,
       which is exactly why it is the better door on the machines that
       have one. */
    if (ev.special == (u16)WP_KEY_HELP) {
        open_help_node(window_handle, vdi, "Main", 4);
        return WP_FALSE;
    }
    /* F1 only means Help when the active scheme does not bind it. Under
       WordPerfect it is Undelete, and swallowing it here is what made
       that unreachable on every backend. */
    if (ev.special == (u16)WP_KEY_F1 &&
        !wp_keymap_claims_f1((wp_keymap_id)g_ctx.keymap_id)) {
        open_help_node(window_handle, vdi, "Main", 4);
        return WP_FALSE;
    }
    if (ev.special == (u16)WP_KEY_UNDO) {
        invoke_action(window_handle, vdi, WP_ACT_EDIT_UNDO);
        redraw_window(window_handle, vdi);
        return WP_FALSE;
    }

    outcome = wp_keymap_feed_key(&g_ctx, ev, (u8)(key & 0xFF));

    /* The active scheme has no notion of this app's "current input
       style" toggle state (see apply_current_style_to_run_at's own
       comment) -- restamp the run it just created, but ONLY when this
       keystroke really was a character insertion (wp_keymap_outcome's
       own inserted_char contract), never on a bare cursor motion, or
       moving over already-styled text would silently overwrite it. */
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
        /* T172: an ex command may have named a file (":w foo", ":e foo").
           invoke_action carries no payload -- a menu pick has nothing to
           say beyond "the user chose Open" -- so the name is parked here
           and collected by do_file_open/do_file_save below. Set on every
           ACTION outcome, not just the file ones: the helper ignores an
           empty name, and take_ always clears, so a name can never leak
           into a later unrelated File > Open. */
        wp_keymap_set_pending_path(outcome.path, (u32)outcome.path_len);
        invoke_action(window_handle, vdi, (wp_action_id)outcome.action_id);
        break;
    default:
        break;
    }

    if (g_ctx.quit_requested) return WP_TRUE;

    redraw_window(window_handle, vdi);
    return WP_FALSE;
}

/* Menu item -> registry action. This replaced a hand-written switch that
   duplicated, case for case, the dispatch wp_dispatch_key already did
   through invoke_action() -- the exact duplication M8.5 introduced the
   action registry to end, which this backend (the registry's own
   birthplace) had never actually adopted for its menu path. The two
   copies had already drifted: the menu arm redrew after some actions and
   not others, and Spell/Thesaurus/Grammar/Chat were reachable ONLY from
   the menu, never from invoke_action, so no keymap scheme could ever
   expose them.

   A table, not a switch, so menu_action_map_selfcheck below can walk it
   and prove both directions of the mapping on target. */
typedef struct {
    short obj;
    wp_action_id action;
} wp_menu_action_pair;

static const wp_menu_action_pair g_menu_actions[] = {
    { WP_MENU_OBJ_FILE_NEW,           WP_ACT_FILE_NEW },
    { WP_MENU_OBJ_FILE_OPEN,          WP_ACT_FILE_OPEN },
    { WP_MENU_OBJ_FILE_SAVE,          WP_ACT_FILE_SAVE },
    { WP_MENU_OBJ_FILE_QUIT,          WP_ACT_FILE_QUIT },
    { WP_MENU_OBJ_EDIT_UNDO,          WP_ACT_EDIT_UNDO },
    { WP_MENU_OBJ_EDIT_CUT,           WP_ACT_EDIT_CUT },
    { WP_MENU_OBJ_EDIT_COPY,          WP_ACT_EDIT_COPY },
    { WP_MENU_OBJ_EDIT_PASTE,         WP_ACT_EDIT_PASTE },
    { WP_MENU_OBJ_TOOLS_SPELLCHECK,   WP_ACT_TOOLS_SPELLCHECK },
    { WP_MENU_OBJ_TOOLS_THESAURUS,    WP_ACT_TOOLS_THESAURUS },
    { WP_MENU_OBJ_TOOLS_GRAMMARCHECK, WP_ACT_TOOLS_GRAMMARCHECK },
    { WP_MENU_OBJ_TOOLS_CHAT,         WP_ACT_TOOLS_CHAT },
    { WP_MENU_OBJ_FORMAT_BOLD,        WP_ACT_FORMAT_BOLD },
    { WP_MENU_OBJ_FORMAT_ITALIC,      WP_ACT_FORMAT_ITALIC },
    { WP_MENU_OBJ_FORMAT_UNDERLINE,   WP_ACT_FORMAT_UNDERLINE },
    { WP_MENU_OBJ_HELP_CONTENTS,      WP_ACT_HELP_CONTENTS },
    { WP_MENU_OBJ_HELP_KEYS,          WP_ACT_HELP_KEYS },
    { WP_MENU_OBJ_HELP_FORMAT,        WP_ACT_HELP_FORMAT }
};

#define WP_MENU_ACTION_COUNT \
    ((short)(sizeof(g_menu_actions) / sizeof(g_menu_actions[0])))

/* Registry actions in a menu group this backend builds that deliberately
   have NO row above. Each is still reachable; an action that is neither
   mapped nor listed here is dead weight in a menu bar, which is what
   menu_action_map_selfcheck refuses to let happen silently. */
static wp_bool action_exempt_from_menu(wp_action_id id)
{
    switch (id) {
    /* This backend's Format menu is deliberately FINER-grained than the
       registry: the registry has one cycling "Size..." and one cycling
       "Alignment...", while the ST menu offers explicit Size +/- and
       Align Left/Center/Right, which is better for a mouse. Both
       registry actions remain reachable by their own Ctrl shortcuts, so
       nothing is lost -- there is simply no 1:1 item to map. */
    case WP_ACT_FORMAT_SIZE:
    case WP_ACT_FORMAT_ALIGN:
        return WP_TRUE;
    /* Ctrl+G only, by design -- see actions.c's own comment on why this
       one stays off the hand-built OBJECT tree. */
    case WP_ACT_TOOLS_INSERTIMAGE:
        return WP_TRUE;
    /* Ctrl+D only, same reasoning as Insert Image above: this backend's
       OBJECT tree is hand-built, and editing it has twice produced real
       regressions (see docs/atari-platform.md). Amiga and Mac build
       their Tools menus dynamically from the registry, so both show
       "Dictate" automatically -- only this backend needs the exemption,
       and the action stays fully reachable by its shortcut.

       For dictation specifically the keystroke IS the intended route:
       the feature exists so a writer can speak a sentence without
       leaving the keyboard, which a mouse-driven menu pick would
       defeat. */
    case WP_ACT_TOOLS_DICTATE:
        return WP_TRUE;
    default:
        return WP_FALSE;
    }
}

/* Menu items with no registry action at all, because the action they
   perform is finer-grained than (Size/Align) or conventionally outside
   (Desk's About) the shared registry. Returns WP_TRUE if it handled the
   item. Kept separate from the table so the table stays a pure mapping
   that the selfcheck can reason about. */
static wp_bool dispatch_local_menu_item(short window_handle, VdiHdl vdi, short obj)
{
    switch (obj) {
    case WP_MENU_OBJ_DESK_ABOUT:
        /* GEM convention puts About under Desk, not Help -- so this one
           item opens a help node without a registry action behind it. */
        open_help_node(window_handle, vdi, "About", 5);
        return WP_TRUE;
    case WP_MENU_OBJ_FORMAT_SIZE_UP:    change_font_size(1); return WP_TRUE;
    case WP_MENU_OBJ_FORMAT_SIZE_DOWN:  change_font_size(-1); return WP_TRUE;
    case WP_MENU_OBJ_FORMAT_ALIGN_LEFT:   apply_alignment(WP_ALIGN_LEFT); return WP_TRUE;
    case WP_MENU_OBJ_FORMAT_ALIGN_CENTER: apply_alignment(WP_ALIGN_CENTER); return WP_TRUE;
    case WP_MENU_OBJ_FORMAT_ALIGN_RIGHT:  apply_alignment(WP_ALIGN_RIGHT); return WP_TRUE;
    default:
        return WP_FALSE;
    }
}

/* Returns WP_TRUE to quit. Redraws unconditionally afterwards rather
   than per-action: the old switch redrew after some items and not
   others, which left alert-box residue on screen after Save, Thesaurus
   and Chat (each pops a form_alert that the AES does not restore behind)
   until something else happened to trigger a repaint. redraw_window is
   idempotent, so "always" is both simpler and more correct than a list
   of which actions dirty the window -- the same conclusion the Mac
   backend reached separately. */
static wp_bool dispatch_menu_item(short window_handle, VdiHdl vdi, short obj)
{
    short i;

    if (!dispatch_local_menu_item(window_handle, vdi, obj)) {
        for (i = 0; i < WP_MENU_ACTION_COUNT; i++) {
            if (g_menu_actions[i].obj != obj) continue;
            invoke_action(window_handle, vdi, g_menu_actions[i].action);
            break;
        }
    }

    if (g_ctx.quit_requested) return WP_TRUE;

    /* open_help_node already redrew to show the viewer; redrawing again
       is harmless but pointless, and skipping it keeps the help screen
       from being painted twice on a slow ST. */
    if (!g_help_active) redraw_window(window_handle, vdi);
    return WP_FALSE;
}

/* Proves the table above in BOTH directions on target, which is the
   whole reason dispatch is a table rather than a switch:

     forward  -- every mapped menu object is a real leaf of wp_menu_tree
                 and every mapped action id resolves in the registry, so
                 a renumbered WP_MENU_OBJ_* or a removed action fails
                 loudly instead of dispatching the wrong item;
     backward -- every registry action filed under a group this backend
                 builds a title for is either mapped or explicitly
                 exempt, so adding an action to actions.c without giving
                 ST users any way to reach it cannot pass unnoticed.

   The backward direction is the one that matters for a shared registry:
   Amiga and Mac build their menus by folding over g_wp_actions, so they
   pick up a new action automatically, and this hand-built tree would
   otherwise be the one backend that silently doesn't. */
static wp_bool menu_action_map_selfcheck(void)
{
    short i;
    u32 a;

    for (i = 0; i < WP_MENU_ACTION_COUNT; i++) {
        short obj = g_menu_actions[i].obj;
        short j;

        if (obj < 0 || obj >= WP_MENU_OBJ_COUNT) return WP_FALSE;
        if (wp_menu_tree[obj].ob_type != G_STRING) return WP_FALSE;
        if (wp_menu_tree[obj].ob_head != -1) return WP_FALSE; /* must be a leaf */
        if (wp_action_find(g_menu_actions[i].action) == NULL) return WP_FALSE;

        /* Two rows pointing at one object would make the second
           unreachable; two rows pointing at one action is legitimate
           nowhere in this table today and almost certainly a copy-paste
           slip if it appears. */
        for (j = 0; j < i; j++) {
            if (g_menu_actions[j].obj == obj) return WP_FALSE;
            if (g_menu_actions[j].action == g_menu_actions[i].action) return WP_FALSE;
        }
    }

    for (a = 0; a < (u32)WP_ACT_COUNT; a++) {
        const wp_action *act = &g_wp_actions[a];
        wp_bool mapped = WP_FALSE;

        if (act->group == WP_MENU_NONE) continue;
        if (action_exempt_from_menu(act->id)) continue;

        for (i = 0; i < WP_MENU_ACTION_COUNT; i++) {
            if (g_menu_actions[i].action == act->id) mapped = WP_TRUE;
        }
        if (!mapped) return WP_FALSE;
    }

    return WP_TRUE;
}

wp_bool wp_dispatch_message(short window_handle, VdiHdl vdi, short msg[8])
{
    switch (msg[0]) {
    case MN_SELECTED:
        menu_tnormal(wp_menu_tree, msg[3], UNHIGHLIGHT);
        return dispatch_menu_item(window_handle, vdi, msg[4]);

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
        if (g_ctx.doc_loaded) {
            GRECT work;
            wind_get_grect(window_handle, WF_WORKXYWH, &work);
            relayout_document(atari_px_to_twips_x((i32)work.g_w));
        }
        return WP_FALSE;

    default:
        return WP_FALSE;
    }
}

/* Pure decision logic behind the startup 80-column warning, factored
   out of app_shell_run() so it's testable without a real VDI handle --
   same reasoning offload_caps_supports() is its own function rather
   than inlined into do_chat(). screen_width_px is work_out[0]+1 (the
   real pixel width v_opnvwk reports); 640px is the real threshold
   SpeedoGDOS's own INSTALL.PRG uses (verified: refuses at 320x200,
   proceeds at 640x200 -- see docs/atari-platform.md). */
static wp_bool needs_80_column_warning(short screen_width_px, short wcell)
{
    if (wcell <= 0) return WP_TRUE;     /* nonsense metrics: warn, don't divide */
    return (wp_bool)((screen_width_px / wcell) < 80);
}

/* Divide-by-zero guard, proven rather than assumed: a backend that
   reports a zero cell width must make this warn, not crash. */
static wp_bool wcell_is_zero_safe(void)
{
    return needs_80_column_warning(640, 0);
}

wp_bool wp_atari_resolution_selfcheck(void)
{
    wp_bool ok = WP_TRUE;

    if (!needs_80_column_warning(320, 8)) ok = WP_FALSE;  /* ST low-res: 40 columns */
    if (needs_80_column_warning(640, 8))  ok = WP_FALSE;  /* ST medium-res: exactly 80 */
    if (needs_80_column_warning(640 * 2, 8)) ok = WP_FALSE; /* wide/overscan: well over */

    /* THE case the old pixels-only test waved through: 640 pixels of a
       16-pixel-wide system font is 40 columns, not 80. It passed
       "screen_width_px < 640" and put the app on a screen too narrow to
       hold its own menu bar. */
    if (!needs_80_column_warning(640, 16)) ok = WP_FALSE;
    if (wcell_is_zero_safe() != WP_TRUE) ok = WP_FALSE;

    return ok;
}

/* Proves the non-blocking pieces behind the Format menu/keyboard-
   shortcut actions (docs/atari-platform.md's "Format controls wired
   into the UI" section): cstyle/pstyle find_or_add dedup (real
   cross-compiled verification of the same engine logic
   test_docmodel.c already covers on host -- same reasoning
   wp_atari_grammar_check_selfcheck's own comment gives for
   grammar_lite_check()), the alignment offset arithmetic against hand-
   computed values, and that the Ctrl+letter shortcut codes wired into
   wp_dispatch_key are genuinely distinct from each other and from the
   existing backspace/printable-insert range. Deliberately builds its
   own local, throwaway wp_document rather than touching g_doc/
   g_ctx.doc_loaded -- toggle_style_flag/change_font_size/apply_alignment
   all operate on this file's global editor state, which a smoke test
   (never having called load_sample_document) shouldn't be poking at
   directly, the same boundary every other selfcheck here already
   respects. */
wp_bool wp_atari_format_selfcheck(void)
{
    wp_document doc;
    wp_char_style cs;
    wp_para_style ps;
    wp_cstyle cs1, cs2, cs3;
    wp_pstyle ps1, ps2;
    static const short shortcut_codes[] = {
        0x02, 0x09, 0x15, 0x1D, 0x1B, 0x0C, 0x05, 0x12
    };
    unsigned int i, j;

    if (wp_document_init(&doc) != WP_OK) return WP_FALSE;

    cs = *wp_document_cstyle_get(&doc, doc.default_cstyle);
    cs.flags |= WP_STYLE_BOLD;
    if (wp_document_cstyle_find_or_add(&doc, &cs, &cs1) != WP_OK) goto fail;
    if (wp_document_cstyle_find_or_add(&doc, &cs, &cs2) != WP_OK) goto fail;
    if (cs1 != cs2) goto fail; /* identical style must reuse the handle */
    cs.flags |= WP_STYLE_ITALIC;
    if (wp_document_cstyle_find_or_add(&doc, &cs, &cs3) != WP_OK) goto fail;
    if (cs3 == cs1) goto fail; /* genuinely different style must NOT reuse it */

    ps = *wp_document_pstyle_get(&doc, doc.default_pstyle);
    ps.align = WP_ALIGN_CENTER;
    if (wp_document_pstyle_find_or_add(&doc, &ps, &ps1) != WP_OK) goto fail;
    if (wp_document_pstyle_find_or_add(&doc, &ps, &ps2) != WP_OK) goto fail;
    if (ps1 != ps2) goto fail;

    wp_document_free(&doc);

    /* Hand-computed: a 700-twip-wide line in a 1000-twip column has
       300 twips of slack -- center splits it (150), right takes it all
       (300), left (and out-of-range values) take none. */
    if (atari_align_offset_twips(WP_ALIGN_LEFT, 1000, 700) != 0) return WP_FALSE;
    if (atari_align_offset_twips(WP_ALIGN_CENTER, 1000, 700) != 150) return WP_FALSE;
    if (atari_align_offset_twips(WP_ALIGN_RIGHT, 1000, 700) != 300) return WP_FALSE;
    /* A line that exactly fills (or, degenerately, exceeds) the column
       gets no offset either way -- never draws past the left edge. */
    if (atari_align_offset_twips(WP_ALIGN_CENTER, 1000, 1000) != 0) return WP_FALSE;
    if (atari_align_offset_twips(WP_ALIGN_RIGHT, 1000, 1200) != 0) return WP_FALSE;

    /* Every shortcut code must be distinct from the others and from
       backspace (0x08) and the printable-insert range (0x20-0x7E) --
       a real hand-editing mistake (typo'ing a duplicate control code)
       would otherwise silently make one shortcut invoke another's
       action instead of its own. */
    for (i = 0; i < sizeof(shortcut_codes) / sizeof(shortcut_codes[0]); i++) {
        if (shortcut_codes[i] == 0x08) return WP_FALSE;
        if (shortcut_codes[i] >= 0x20 && shortcut_codes[i] < 0x7F) return WP_FALSE;
        for (j = i + 1; j < sizeof(shortcut_codes) / sizeof(shortcut_codes[0]); j++) {
            if (shortcut_codes[i] == shortcut_codes[j]) return WP_FALSE;
        }
    }

    return WP_TRUE;

fail:
    wp_document_free(&doc);
    return WP_FALSE;
}

/* ------------------------------------------------------------------
   CI hook (T169). Mirrors platform/amiga/app_shell.c's own amiga_ci_*
   pair -- the gate that found T164 -- but with one deliberate upgrade.
   The Amiga version writes a one-shot "alive" marker and then leaves
   "did it survive?" to grepping amiberry's log for illegal-instruction
   lines. That works there only because amiberry happens to log CPU
   faults in a greppable form; Hatari, BasiliskII and DOSBox each report
   faults differently (or not at all), so log-grepping does not port.

   Instead the marker carries a DECIMAL EVENT COUNT that this app
   rewrites every time it completes an event-loop pass. The host gate
   reads it, injects a few harmless keystrokes, and requires the count
   to ADVANCE. That is a strictly stronger claim than "no fault was
   logged": it proves the app is still scheduling, still reading the
   keyboard, and still returning to its loop -- a hang inside a paint or
   a wedged event loop fails it, and neither of those raises a CPU
   exception for any log to record. It is also emulator-agnostic, so
   the identical contract ports to the Mac and DOS backends.

   Everything here is inert unless the trigger file exists, so a real
   user's boot never touches the disk for this. 8.3 names throughout
   (RETROWP.CI / RETROWP.ALV) -- a longer name is silently truncated by
   GEMDOS and the host would then poll for a file that never appears. */
#define WP_ATARI_CI_TRIGGER  "C:\\RETROWP.CI"
#define WP_ATARI_CI_ALIVE    "C:\\RETROWP.ALV"

static wp_bool g_atari_ci_active = WP_FALSE;
static u32     g_atari_ci_events = 0;

static wp_bool atari_ci_mode(void)
{
    plat_file *f;

    if (plat_file_open(WP_ATARI_CI_TRIGGER, PLAT_FILE_READ, &f) != WP_OK) return WP_FALSE;
    plat_file_close(f);
    return WP_TRUE;
}

/* Deliberately no sprintf: this backend already avoids it everywhere
   else (see platform/common/statusline.c's own no-sprintf discipline),
   and pulling in stdio for a CI marker would be a real size cost on a
   1MB target for something a user never runs. */
static u32 atari_ci_decimal(char *out, u32 cap, u32 v)
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
   that its paint loop actually put document text on screen.

   Deliberately no sprintf: this backend avoids it everywhere else (see
   platform/common/statusline.c's own no-sprintf discipline), and pulling
   stdio in for a CI marker would be a real size cost on a 1MB target for
   something a user never runs. */
static void atari_ci_write_alive(void)
{
    char buf[16];
    u32 len;
    plat_file *f;

    if (plat_file_open(WP_ATARI_CI_ALIVE, PLAT_FILE_WRITE, &f) != WP_OK) return;
    len = atari_ci_decimal(buf, (u32)sizeof(buf), g_atari_ci_events);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)" ", 1);
    len = atari_ci_decimal(buf, (u32)sizeof(buf), g_atari_painted_frags);
    plat_file_write(f, (const u8 *)buf, len);
    plat_file_write(f, (const u8 *)"\n", 1);
    plat_file_close(f);
}

void app_shell_run(void)
{
    short work_in[11];
    short work_out[57];
    short dummy;
    short wcell, hcell;
    VdiHdl vdi_handle;
    short window_handle;
    GRECT desk;
    short msg[8];
    short mx, my, mbtn, mkstate, mkey, mcount;
    short events;
    wp_bool quit;

    appl_init();

    /* M10: read once at startup, fixed for the process lifetime -- see
       WP_ATARI_CONFIG_PATH's own comment and docs/keymaps.md. Missing/
       unreadable/malformed all quietly yield the documented default
       (WP_KEYMAP_VI), so an existing zero-config install behaves
       exactly as it always has. */
    {
        wp_config cfg;
        wp_config_load(WP_ATARI_CONFIG_PATH, &cfg);

        /* M8.5/M10: wires the active scheme's own .doc/.journal to
           &g_doc/&g_journal and seeds everything else to sane defaults
           -- see g_ctx's own comment above. Safe to call before any
           document is loaded: it only stores pointers, never
           dereferences them (mirrors how replace_document itself
           doesn't run until load_sample_document below). */
        wp_app_ctx_init(&g_ctx, (wp_keymap_id)cfg.keymap_id, &g_doc, &g_journal);
    }

    /* M8.5 Phase 10: parses the compiled-in help content once at
       startup. WP_NOMEM is the only realistic failure (the source is
       fixed, already-tested content, not user input) -- if it somehow
       happens, g_help_doc_loaded stays WP_FALSE and F1 is a silent
       no-op rather than a crash; see wp_dispatch_key. */
    g_help_doc_loaded = (wp_bool)(wp_help_doc_parse(g_help_source_retrowp,
                                                     (u32)g_help_source_retrowp_len,
                                                     &g_help_doc) == WP_OK);

    vdi_handle = graf_handle(&wcell, &hcell, &dummy, &dummy);
    for (dummy = 0; dummy < 10; dummy++) work_in[dummy] = 1;
    work_in[10] = 2; /* raster coordinates */
    v_opnvwk(work_in, &vdi_handle, work_out);

    /* This window's own drawing handle needs the identical one-time
       GDOS registration ensure_metrics_vdi already does for the
       separate metrics-only handle -- otherwise plat_draw_text's own
       atari_select_font call (draw_atari.c) can only snap to the
       built-in bitmap font's coarse sizes on THIS handle even when
       g_metrics_vdi's measurements used the full scalable range,
       reintroducing the same measure-vs-draw mismatch fixing
       plat_draw_text alone doesn't fully close. */
    atari_ensure_gdos_fonts(vdi_handle);

    /* work_out[0] is the screen's max addressable X (width in pixels,
       minus one) -- a standard v_opnvwk output, same field the real
       SpeedoGDOS INSTALL.PRG checks before it will run at all (verified:
       it refuses at 320x200 low-res with "needs an 80 column screen",
       and proceeds at 640x200 medium-res -- see docs/atari-platform.md).
       This app doesn't need to refuse outright the way an installer
       does -- editing still works, just cramped -- so this warns once
       at startup with the same 640px/80-column threshold rather than
       blocking launch. */
    if (needs_80_column_warning((short)(work_out[0] + 1), wcell)) {
        form_alert(1, FA_ERROR "[Screen is under 80 columns.|Text and menus may be cramped|at this resolution.][ OK ]");
    }

    wp_menu_tree_init(wcell, hcell,
                      (short)(work_out[0] + 1), (short)(work_out[1] + 1));
    menu_bar(wp_menu_tree, MENU_INSTALL);

    /* Query the desktop work area AFTER installing the menu bar -- AES
       shrinks it to make room for the menu bar row. */
    wind_get_grect(0, WF_WORKXYWH, &desk);

    window_handle = wind_create_grect(NAME | MOVER | CLOSER | SIZER, &desk);
    wind_set_str(window_handle, WF_NAME, "RetroWP");
    wind_open_grect(window_handle, &desk);

    graf_mouse(ARROW, (const MFORM *)0);

    {
        GRECT work;
        wind_get_grect(window_handle, WF_WORKXYWH, &work);
        load_sample_document(atari_px_to_twips_x((i32)work.g_w));
    }

    /* Written only after the menu bar is installed, the window is open,
       and the sample document is laid out -- so its mere existence is
       the "the app really came up" signal, exactly what AMISMOKE-style
       smoke tests cannot attest. See atari_ci_mode's own comment. */
    g_atari_ci_active = atari_ci_mode();
    if (g_atari_ci_active) atari_ci_write_alive();

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
            quit = wp_dispatch_key(window_handle, vdi_handle, mkey, mkstate);
        }
        /* Heartbeat, after dispatch rather than before: a dispatch that
           crashes or wedges must NOT be able to bump the count first. */
        if (g_atari_ci_active && !quit) {
            g_atari_ci_events++;
            atari_ci_write_alive();
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
