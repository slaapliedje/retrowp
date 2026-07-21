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

/* post-M8 (2nd revisit): a real 3-level SCREEN->BAR->titles restructuring
   was attempted here (to fix a real, source-verified bug where only the
   first menu title ever painted). It passed this project's OWN
   single-image CI (EmuTOS 0.9.12) cleanly, including a new
   cycle-detection self-check specifically added to catch a bad
   renumbering. But the multi-TOS matrix added in the SAME session
   (host/tools/ci_atari_matrix.sh) caught what single-image CI never
   could: the restructured tree causes a real bus-error crash under a
   newer EmuTOS build (1.4) and a real hang under real TOS 1.04 --
   confirmed via an A/B test against the pre-restructuring binary, which
   passes cleanly on BOTH. Reverted for that reason. (3rd revisit, same
   session): moving Format out of ROOT's ob_tail position was tried next,
   staying on the flat 2-level shape -- not a fix, just a safe place to
   land the new Format title without reintroducing risk.

   post-M8.5 (4th revisit): a real fix, this time grounded in the AES's
   OWN documented public contract rather than reverse-engineered EmuTOS
   source -- the Atari Compendium (Scott Sanders), queried directly via
   the atari-compendium MCP server, documents the canonical GEM menu-bar
   OBJECT shape in "The Menu Bar" (Chapter 6, pp. 6.25-6.26): ROOT is a
   G_IBOX with exactly two children, BAR and DROPDOWNS. BAR (a G_BOX) has
   exactly one child, ACTIVE, which holds every G_TITLE as ITS children.
   DROPDOWNS (a G_IBOX) holds one G_BOX per title -- positioned to fall
   underneath its corresponding title -- NOT nested under the title
   itself. This is why both prior bugs existed: menu_bar(MENU_INSTALL)'s
   internal ob_draw(tree, 1, MAX_DEPTH) call only ever walks DESCENDANTS
   of array index 1 (confirmed against objc_draw()'s public documented
   behavior, p.425 -- "renders an AES object tree on screen" from `obj`
   down `depth` levels, never siblings). With titles as index 1's
   siblings (every shape tried before this), only whichever title sat at
   index 1 ever painted. With dropdown boxes nested under their titles
   (the same past shapes), ob_draw's install-time walk reached them too,
   painting closed dropdowns as "glyph noise" (see git history) unless
   patched with OF_HIDETREE. Under the correct shape, BAR/ACTIVE/titles
   ARE index 1's whole subtree (so all titles paint), and DROPDOWNS is a
   separate subtree under ROOT that ob_draw(tree, 1, ...) never reaches
   at all (so dropdowns are structurally never install-time-painted,
   independent of OF_HIDETREE -- kept anyway, defensively, below).

   The Desk dropdown box has a separately documented shape (same pages):
   exactly 8 children -- an app-defined first item (here, "About
   RetroWP...", wired to the help viewer's About node), a disabled
   separator, then SIX dummy G_STRING objects the AES manages at
   MENU_INSTALL time to list running desk accessories/applications. This
   is now confirmed exactly, not just documented: a local EmuTOS source
   checkout's aes/gemmnlib.c defines THESCREEN=0, THEBAR=1, THEACTIVE=2
   (matching WP_MENU_OBJ_ROOT/BAR/ACTIVE exactly) and gemctrl.c's THEDESK=3
   (matching WP_MENU_OBJ_DESK_TITLE exactly) -- these are not this
   project's own numbering choice lining up by luck, they're the real AES's
   own hardcoded constants. menu_fixup() computes themenus =
   tree[THESCREEN].ob_tail (DROPDOWNS, by construction: DROPDOWNS must be
   ROOT's ob_tail) then dabox = tree[themenus].ob_head (the Desk box, by
   construction: it must be DROPDOWNS's ob_head), rebuilds dabox's child
   chain from scratch via ob_add() for dabox+1..dabox+cnt where
   cnt = D.g_accreg ? (2 + D.g_accreg) : 1 -- and include/sysconf.h defines
   NUM_ACCS as 6, so cnt tops out at 8, requiring dabox+1..dabox+8 (this
   tree's DESK_ABOUT..DESK_DUMMY6) to be valid, contiguous slots. Also
   confirmed by reading the source rather than assumed: menu_fixup()
   repoints each slot's ob_spec directly at EmuTOS's own internal
   accessory-title string (tree[ob].ob_spec = (LONG)D.g_acctitle[...]) --
   it does not write through the pointer already there, so a real TOS that
   behaves the same way would never touch the bytes of
   g_text_desk_dummyN[] itself. They're kept as writable static buffers
   anyway, defensively, since this was only verified for EmuTOS, not real
   TOS's own implementation.

   menu_sub() (same file) confirms the DROPDOWNS ordering requirement too:
   given a title's object index ititle, it walks
   tree[tree[THESCREEN].ob_tail].ob_head forward via ob_next exactly
   (ititle - THEACTIVE - 1) times to find that title's dropdown box -- so
   DROPDOWNS's child chain order must match ACTIVE's title order exactly,
   which is what this tree builds.

   One more residual, honestly-flagged gap: the Compendium's prose names
   BAR's one child "ACTIVE" and gives its size but never states its
   ob_type explicitly. G_BOX is used here (matching BAR, its parent, and
   every other hand-built container in this tree) as the most consistent
   reading, not a directly-cited fact.

   A real, reproducible timing artifact was found and fixed during this
   revisit, unrelated to the tree shape's correctness: host/tools/
   atari_smoke.c's very first synthetic MN_SELECTED dispatch (called
   directly, immediately after wind_open_grect(), bypassing the real
   event loop entirely -- see this header's own top-of-file comment on
   why) was flaky specifically against this bigger tree. See that file's
   own comment at the evnt_timer() call added to fix it.

   A second, independent, previously-undiscovered bug was found only by
   actually taking a real Hatari screenshot of the fixed tree (something
   none of CI checks): all five titles' text was painting on top of each
   other in the leftmost ~25 pixels, illegible. `graf_handle()`'s wcell/
   hcell out-params (system font character-cell size in real pixels) were
   being fetched and discarded (`&dummy` x4) since T4.1 -- a hand-built
   OBJECT tree, unlike one loaded via rsrc_load()/rsrc_obfix(), is never
   auto-scaled from character-cell units to pixels, so this tree's
   authored x-positions (0, 6, 12, 18, 25 -- character counts) were being
   used as raw, unscaled pixel coordinates. Invisible before this session
   because only one title ever painted at all, so relative spacing was
   never visually exercised. wp_menu_tree_init() now takes (wcell, hcell)
   and scales every ob_x/ob_width/ob_y/ob_height in place as its final
   step; both real call sites now pass graf_handle()'s real values
   through instead of discarding them.

   This must clear the FULL multi-TOS matrix (ci_atari_matrix.sh), not
   just single-image CI, before being trusted -- exactly the lesson the
   2nd revisit above already paid for once. See docs/atari-platform.md
   for the verification record. */
#define WP_MENU_OBJ_ROOT               0
#define WP_MENU_OBJ_BAR                1
#define WP_MENU_OBJ_ACTIVE             2
#define WP_MENU_OBJ_DESK_TITLE         3
#define WP_MENU_OBJ_FILE_TITLE         4
#define WP_MENU_OBJ_EDIT_TITLE         5
#define WP_MENU_OBJ_TOOLS_TITLE        6
#define WP_MENU_OBJ_FORMAT_TITLE       7
#define WP_MENU_OBJ_DROPDOWNS          8
#define WP_MENU_OBJ_DESK_BOX           9
#define WP_MENU_OBJ_DESK_ABOUT         10
#define WP_MENU_OBJ_DESK_SEP           11
#define WP_MENU_OBJ_DESK_DUMMY1        12
#define WP_MENU_OBJ_DESK_DUMMY2        13
#define WP_MENU_OBJ_DESK_DUMMY3        14
#define WP_MENU_OBJ_DESK_DUMMY4        15
#define WP_MENU_OBJ_DESK_DUMMY5        16
#define WP_MENU_OBJ_DESK_DUMMY6        17
#define WP_MENU_OBJ_FILE_BOX           18
#define WP_MENU_OBJ_FILE_OPEN          19
#define WP_MENU_OBJ_FILE_SAVE          20
#define WP_MENU_OBJ_FILE_QUIT          21
#define WP_MENU_OBJ_EDIT_BOX           22
#define WP_MENU_OBJ_EDIT_CUT           23
#define WP_MENU_OBJ_EDIT_COPY          24
#define WP_MENU_OBJ_EDIT_PASTE         25
#define WP_MENU_OBJ_TOOLS_BOX          26
#define WP_MENU_OBJ_TOOLS_SPELLCHECK   27
#define WP_MENU_OBJ_TOOLS_THESAURUS    28
#define WP_MENU_OBJ_TOOLS_GRAMMARCHECK 29
#define WP_MENU_OBJ_TOOLS_CHAT         30
#define WP_MENU_OBJ_FORMAT_BOX         31
#define WP_MENU_OBJ_FORMAT_BOLD        32
#define WP_MENU_OBJ_FORMAT_ITALIC      33
#define WP_MENU_OBJ_FORMAT_UNDERLINE   34
#define WP_MENU_OBJ_FORMAT_SIZE_UP     35
#define WP_MENU_OBJ_FORMAT_SIZE_DOWN   36
#define WP_MENU_OBJ_FORMAT_ALIGN_LEFT    37
#define WP_MENU_OBJ_FORMAT_ALIGN_CENTER  38
#define WP_MENU_OBJ_FORMAT_ALIGN_RIGHT   39
#define WP_MENU_OBJ_COUNT        40

extern OBJECT wp_menu_tree[WP_MENU_OBJ_COUNT];

/* Patches ob_spec.free_string on the title/item objects, then scales
   every populated ob_x/ob_y/ob_width/ob_height in wp_menu_tree[] from
   the array literal's logical character-cell units into real pixels
   (ob_x/ob_width *= wcell, ob_y/ob_height *= hcell). Hand-built OBJECT
   trees are never passed through rsrc_load()/rsrc_obfix() -- the one
   real mechanism that would otherwise do this scaling for a
   resource-file-loaded tree -- so this project's own code must do it.
   wcell/hcell are graf_handle()'s first two out-params (system default
   text font's character cell width/height in pixels, per the Atari
   Compendium's own graf_handle() reference); the caller must call
   graf_handle() itself and pass those through, not query font metrics
   directly (CLAUDE.md's font-metrics-abstraction rule). Must run before
   menu_bar() -- both for the string patching (C89 has no designated
   initializers, so the OBSPEC union in the static array literal is left
   {0}) and the scaling. Call exactly once per process -- calling twice
   double-scales every coordinate. */
void wp_menu_tree_init(short wcell, short hcell);

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
   byte, scan code in the high byte -- see atari_translate_key's own
   comment in app_shell.c for the real IKBD scan-code table this is
   built from). M8.5: translates the key to a portable wp_key_event and
   feeds it to vi-mode (engine/src/vimode.c) first; a key vi-mode itself
   doesn't consume falls through to the action registry's Ctrl+<letter>
   shortcuts (platform/common/actions.c). Forces an immediate repaint
   since AES has no way to know typing changed the window's content.
   Returns WP_TRUE if the app should quit (':q'/':wq' or the Ctrl+Q
   shortcut), matching wp_dispatch_message's own convention. A no-op
   (returns WP_FALSE) if no document is loaded (smoke test path).
   kstate is evnt_multi's own kstate output (M10 Phase 2), passed
   straight through to atari_translate_key -- see that function's own
   comment. */
wp_bool wp_dispatch_key(short window_handle, VdiHdl vdi, short key, short kstate);

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

/* M8 T8.3: proves offload_caps_supports() -- the new pure bit-check
   behind Chat's "consult CAPS before submitting" behavior -- against a
   hand-built offload_capabilities value with a known bitmask (CHAT bit
   set, IMAGE bit clear). There is little else new and non-blocking to
   test here: Chat's prompt extraction reuses extract_current_paragraph_text()
   (already proven by wp_atari_grammar_check_selfcheck()), and its
   response has no structure to decode (docs/formats/offload-protocol.md
   -- the reply is shipped back verbatim, unlike GRAMMAR_CHECK's issue
   list). Deliberately does NOT call do_chat() itself (form_alert()
   blocks) or offload_init()/offload_query() against a real gateway --
   same reasoning every other selfcheck in this file already gives. */
wp_bool wp_atari_chat_selfcheck(void);

/* Proves needs_80_column_warning()'s threshold logic against the three
   real resolutions this project's target hardware actually has (ST
   low/medium-res, plus a wide/overscan case) rather than the real
   form_alert() dialog itself, which -- like every other blocking
   dialog in this file -- isn't exercised by CI. See
   docs/atari-platform.md for how the 640px threshold was verified
   against the real SpeedoGDOS INSTALL.PRG. */
wp_bool wp_atari_resolution_selfcheck(void);

/* post-M8 (3rd revisit): proves the non-blocking pieces behind the
   Format menu/keyboard-shortcut actions (Bold/Italic/Underline, Font
   Size, Alignment) -- cstyle/pstyle find_or_add dedup, the paint-time
   alignment offset arithmetic against hand-computed values, and that
   every Ctrl+letter shortcut code is distinct from the others and from
   backspace/printable-insert. Does not call toggle_style_flag/
   change_font_size/apply_alignment themselves (those operate on this
   file's global g_doc/g_doc_loaded editor state, which this smoke test
   never populates -- same boundary every other selfcheck here
   respects) or wp_dispatch_key end-to-end for the same reason. See
   docs/atari-platform.md. */
wp_bool wp_atari_format_selfcheck(void);

/* M8.5: proves plat_dialog_choice's OBJECT-tree construction
   (platform/atari/dialog_atari.c) for a representative option_count --
   index arithmetic, ob_next/head/tail linkage, exactly-one-default-
   selected -- without ever calling the blocking, interactive form_do(),
   the same boundary every other selfcheck here respects. See
   docs/atari-platform.md's Format Size/Alignment dialogs section. */
wp_bool wp_atari_dialog_selfcheck(void);

/* M8.5 Phase 9: proves atari_translate_key (app_shell.c)'s IKBD scan-
   code table against real (scan, ascii) values from the Atari
   Compendium, including the Ctrl+[-vs-Escape disambiguation that only
   works because scan code is checked before ASCII -- see the selfcheck's
   own comment and docs/vimode.md. A real interactive Hatari verification
   was attempted for this phase and did not reliably demonstrate a
   keystroke reaching RETROWP.PRG end-to-end in this environment (see
   docs/atari-platform.md); this selfcheck is the rigorous substitute,
   same boundary every other wp_atari_*_selfcheck here already respects. */
wp_bool wp_atari_key_translate_selfcheck(void);

/* M8.5 Phase 10: proves the compiled-in help content (docs/help/
   retrowp.help via the generated header) parses cleanly with every
   link resolving, and that the pager's greedy word-wrap
   (wp_help_wrap_line, platform/common/help_view.c) runs correctly
   against REAL Atari VDI-measured font metrics, not just host/tests'
   simplified fake-monospace ones. Does not open the help viewer itself
   or call wp_help_view_paint -- same no-real-window/no-blocking-call
   boundary every other selfcheck in this file respects. See
   docs/help.md. */
wp_bool wp_atari_help_selfcheck(void);

/* M10 Phase 6: proves wp_config_load resolves all five real keymap=
   values against a real file at WP_ATARI_CONFIG_PATH -- see
   app_shell.c's own comment on wp_atari_config_selfcheck for what it
   checks and why. */
wp_bool wp_atari_config_selfcheck(void);

#endif /* WP_ATARI_APP_SHELL_INTERNAL_H */
