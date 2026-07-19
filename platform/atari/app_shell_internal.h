#ifndef WP_ATARI_APP_SHELL_INTERNAL_H
#define WP_ATARI_APP_SHELL_INTERNAL_H

#include <gem.h>
#include "types.h"

/* Private to platform/atari -- shared between app_shell.c (the real AES
   event loop, via app_shell.h's app_shell_run()) and
   host/tools/atari_smoke.c (the Hatari-based CI smoke test). Not
   installed; never included from engine/ or services/.

   Why a smoke test needs this at all: in this project's Hatari-based CI
   environment, synthetic mouse events cannot be reliably injected into
   the emulator (SDL relative-mouse steering was not controllable
   headlessly -- see docs/atari-platform.md). So the smoke test proves
   the real AES calls succeed (appl_init, menu_bar, wind_create/open all
   return success codes) and separately proves the message-dispatch
   logic is correct by calling wp_dispatch_message() directly with
   synthesized message arrays, instead of driving it end-to-end through
   a real evnt_multi() click. That gap is real and documented, not
   silently worked around. */

/* T4.3 grew this from a single "File > Quit" title to two titles: File
   (Open/Save/Quit) and Edit (Cut/Copy/Paste). Same index/linkage
   convention as before, just more of it -- see app_shell.c. */
#define WP_MENU_OBJ_ROOT        0
#define WP_MENU_OBJ_FILE_TITLE  1
#define WP_MENU_OBJ_EDIT_TITLE  2
#define WP_MENU_OBJ_FILE_BOX    3
#define WP_MENU_OBJ_FILE_OPEN   4
#define WP_MENU_OBJ_FILE_SAVE   5
#define WP_MENU_OBJ_FILE_QUIT   6
#define WP_MENU_OBJ_EDIT_BOX    7
#define WP_MENU_OBJ_EDIT_CUT    8
#define WP_MENU_OBJ_EDIT_COPY   9
#define WP_MENU_OBJ_EDIT_PASTE  10
/* M5 UI wiring: a third title, Tools (Spell Check), appended after Edit
   rather than inserted between File/Edit -- keeps every existing index
   stable so nothing above this line needed to change. */
#define WP_MENU_OBJ_TOOLS_TITLE      11
#define WP_MENU_OBJ_TOOLS_BOX        12
#define WP_MENU_OBJ_TOOLS_SPELLCHECK 13
/* post-M6: a second Tools item, Thesaurus, appended after Spell Check. */
#define WP_MENU_OBJ_TOOLS_THESAURUS  14
/* M8 T8.1: a third Tools item, Grammar Check, appended after Thesaurus. */
#define WP_MENU_OBJ_TOOLS_GRAMMARCHECK 15
#define WP_MENU_OBJ_COUNT       16

extern OBJECT wp_menu_tree[WP_MENU_OBJ_COUNT];

/* Patches ob_spec.free_string on the title/item objects. Must run before
   menu_bar() -- C89 has no designated initializers, so the OBSPEC union
   in the static array literal is left {0} and patched at runtime. */
void wp_menu_tree_init(void);

/* Walks ob_next/ob_head/ob_tail and asserts the sibling-chain convention
   holds (see app_shell.c for the convention itself). A hand-built GEM
   OBJECT tree with a wrong index silently breaks menu_bar() with no
   error return in some AES implementations, so this is checked
   explicitly rather than trusted. */
wp_bool wp_menu_tree_selfcheck(void);

/* Handles one AES message as delivered in evnt_multi's MesagBuf (msg[0]
   is the message type; the rest depend on type -- see the AES message
   layout in gem.h/mt_gem.h). Returns WP_TRUE if the app should quit
   (Quit menu item selected, or the window was closed). vdi is needed
   because WM_REDRAW repaints through the VDI clip/fill seam. */
wp_bool wp_dispatch_message(short window_handle, VdiHdl vdi, short msg[8]);

/* T4.2: imports a short multi-word paragraph, relayouts it at a column
   width deliberately narrow enough to force wrapping, and checks two
   properties quantitatively through the REAL VDI metrics seam
   (metrics_atari.c) rather than by eyeballing a screenshot: more than
   one line results, and every line's measured width stays within the
   requested column (none of this fixture's words are wide enough to
   need the single-overlong-word overflow allowance -- see
   docs/layout.md). This is the on-target equivalent of the host suite's
   test_layout_greedy_wrap_hand_computed_fixture, minus the exact
   hand-computed widths (VDI's bitmap system font metrics aren't known
   ahead of time the way the host stub's fake monospace metrics are). */
wp_bool wp_atari_layout_selfcheck(void);

/* Handles one MU_KEYBD event (evnt_multi's Key output: ASCII in the low
   byte, scan code in the high byte). Backspace (0x08) deletes the
   character before the cursor; printable ASCII (0x20-0x7E) inserts at
   the cursor; everything else (Enter included -- WP_OP_SPLIT_PARA is
   still unimplemented per docs/editing.md, so this app stays a single-
   paragraph editor) is a no-op. Forces an immediate repaint since AES
   has no way to know typing changed the window's content. A no-op if no
   document is loaded (smoke test path). */
void wp_dispatch_key(short window_handle, VdiHdl vdi, short key);

/* T4.3: proves plat_file_* and plat_clipboard_* work end to end WITHOUT
   needing the file selector or a real menu click (both GUI-dependent
   and outside this project's scripted-Hatari-testing reach -- see the
   "no real mouse-driven end-to-end test" gap above, which applies
   equally here). Round-trips a fixture string through plat_file_write/
   plat_file_read at a fixed scratch path, and separately through
   plat_clipboard_set_text/get_text, and checks both come back
   byte-identical. */
wp_bool wp_atari_file_clipboard_selfcheck(void);

/* T4.4: builds a genuinely multi-page document (real US Letter page
   dimensions through wp_document_paginate -- the first time this app
   exercises pagination at all; T4.2/T4.3 only ever called
   wp_paragraph_relayout directly for the single-window on-screen flow),
   edits it, saves it, and frees everything. Two independent memory
   signals: GEMDOS Malloc(-1L) (largest free block) at each step proves
   real headroom within the 1MB config, and mem_atari_live_count()
   (mem_atari.h) after everything is freed is the actual leak check --
   Malloc(-1L) turned out NOT to move for allocation traffic this small
   (mintlib claims its heap from GEMDOS once at startup and manages it
   internally; confirmed empirically, see docs/atari-platform.md), so it
   is headroom evidence only, not a leak proxy. The three *_out params
   (any may be NULL) report the Malloc(-1L) readings for the caller to
   log; the return value is the pass/fail verdict: page_count > 1,
   comfortable headroom after loading, and mem_atari_live_count() == 0
   after everything is freed. */
wp_bool wp_atari_memory_budget_selfcheck(long *free_before_out,
                                          long *free_after_load_out,
                                          long *free_after_free_out);

/* M5 UI wiring: proves the RTF/spell code paths platform/atari's menu
   actions now use actually work through REAL plat_file_* calls on this
   target, the same way wp_atari_file_clipboard_selfcheck already does
   for plaintext -- self-contained scratch fixtures (a temp .RTF file, a
   temp 4-word dictionary file), no shipped dictionary asset required.
   Deliberately does NOT call do_spell_check() itself: that function's
   form_alert() blocks waiting for a button click, which would hang this
   headless CI run forever (see docs/atari-platform.md) -- only the
   non-blocking building blocks (extension sniffing, format_rtf_import/
   export, spell_dict_open/spell_check_word/spell_suggest) are exercised
   here. */
wp_bool wp_atari_rtf_spell_selfcheck(void);

/* post-M6: proves extract_word_before_cursor() -- the new pure-logic
   piece behind the Thesaurus menu action -- against a handful of fixture
   documents/cursor positions (a word right at the cursor, no word at
   all, cursor at document start). Deliberately does NOT call
   do_thesaurus() itself (form_alert() again) or offload_init() against a
   real gateway: the offload_client.c <-> real-gateway round trip over
   real emulated RS-232 is already proven end to end by T6.3's
   atari_serial_smoke.c/ci_atari_serial.sh, reusing the exact same
   client API this menu action calls -- re-proving that here would just
   add ~2s of guaranteed-to-fail connect/handshake timeout to every
   ci_atari.sh run (no gateway is attached in that harness) for no new
   coverage. */
wp_bool wp_atari_thesaurus_selfcheck(void);

/* M8 T8.1/T8.2: proves the non-blocking pieces behind the Grammar Check
   menu action -- extracting the current paragraph's plain text, mapping
   a paragraph-relative byte range back to a (run_index, run-local
   offset) pair (declining when a range crosses a run boundary),
   decoding the OFFLOAD_GRAMMAR_CHECK wire response shape, and (T8.2) a
   representative real call into services/grammar's grammar_lite_check()
   -- against fixture documents/cursor positions and a hand-built canned
   response buffer. The grammar_lite_check() call is real cross-target
   verification, not redundant with host/tests/test_grammar.c: that
   suite proves the RULE LOGIC exhaustively but only as compiled by the
   host cc, while this proves the exact same source still behaves
   correctly once cross-compiled for m68k and actually run under real
   CPU emulation -- the same reasoning wp_atari_rtf_spell_selfcheck()
   already applies to spell_check_word/spell_suggest. Deliberately does
   NOT call do_grammar_check() itself (form_alert() blocks) or
   offload_init() against a real gateway: the offload_client.c round
   trip is already proven generic (works for any op, arbitrary payload
   sizes) over real emulated RS-232 by T6.3's atari_serial_smoke.c and
   T7.4's atari_convert_smoke.c, so re-proving it here for this one
   specific op would add nothing new -- same reasoning
   wp_atari_thesaurus_selfcheck's own comment already gives. */
wp_bool wp_atari_grammar_check_selfcheck(void);

#endif /* WP_ATARI_APP_SHELL_INTERNAL_H */
