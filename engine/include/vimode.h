#ifndef WP_VIMODE_H
#define WP_VIMODE_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keyevent.h"

/* Portable, UI-agnostic vi-style modal editor (M8.5). Built entirely on
   the general editing primitives in editing.h (paragraph split/merge,
   wp_edit_delete_selection, undo grouping, word/line/paragraph motions)
   -- this module is a thin compositional layer, not a reimplementation
   of tree mutation. Zero platform calls anywhere in this module except
   mem_alloc/mem_free for the yank register (already permitted -- see
   docs/vimode.md), which is what makes it fully host-testable with no
   real platform backend (host/tests/test_vimode.c). Full design
   rationale (logical-paragraph-not-wrapped-line motions, the
   insertion-point caret model, the dw-doesn't-eat-the-newline clip, the
   :w/:q action hand-off seam, undo-grouping strategy for Insert-mode
   sessions and compound operators like cw) is in docs/vimode.md -- read
   it before changing this file. */

typedef enum {
    WP_VIMODE_NORMAL = 0,
    WP_VIMODE_INSERT,
    WP_VIMODE_VISUAL,
    WP_VIMODE_VISUAL_LINE,
    WP_VIMODE_COMMAND,          /* ':' ex-command, '/' forward search, or '?' backward
                                    search entry, distinguished by cmdline_prefix */
    WP_VIMODE_SUBSTITUTE_CONFIRM /* M11: ':s///c' paused at a match awaiting
                                     y/n/a/q -- see execute_substitute in vimode.c */
} wp_vimode_mode;

typedef enum {
    WP_VIOP_NONE = 0, WP_VIOP_DELETE, WP_VIOP_CHANGE, WP_VIOP_YANK
} wp_vimode_operator;

#define WP_VIMODE_CMDLINE_MAX    128
#define WP_VIMODE_SEARCH_MAX     128
#define WP_VIMODE_ACTION_ARG_MAX 8

/* Fully transparent struct (direct field access), matching this
   codebase's existing convention throughout (wp_document/
   wp_undo_journal are never opaque handles either) -- the get_mode/
   get_status_text functions below exist for status-line display
   convenience, not an encapsulation policy this project otherwise
   follows. doc/journal are borrowed references, not owned. */
typedef struct {
    wp_document     *doc;
    wp_undo_journal *journal;

    u8     mode;    /* wp_vimode_mode */
    wp_pos cursor;
    wp_selection visual; /* valid only while mode is VISUAL/VISUAL_LINE --
                             wp_selection's first real consumer alongside
                             wp_edit_delete_selection itself, see docs/editing.md */

    u32 count_pending;     /* the "3" in "3dw" (motion's own count) */
    u32 op_count_pending;  /* the "2" in "2d3w" (operator's count, multiplies with count_pending) */
    u8  pending_operator;  /* wp_vimode_operator */
    u8  pending_prefix;    /* 0, or the ASCII byte of a pending multi-key prefix (currently only 'g', for gg) */

    u8      *register_bytes; /* mem_alloc'd; NULL if empty. Single unnamed register only -- named
                                 registers ("a-"z) are deferred, see docs/vimode.md */
    u32      register_len;
    u32      register_cap;
    wp_bool  register_linewise; /* charwise (yw, x) vs linewise (dd, yy) -- changes how p/P places it */

    u8  cmdline_buf[WP_VIMODE_CMDLINE_MAX];
    u32 cmdline_len;
    u8  cmdline_prefix;    /* ':' or '/' while mode == WP_VIMODE_COMMAND, 0 otherwise */

    u8      last_search[WP_VIMODE_SEARCH_MAX];
    u32     last_search_len;
    wp_bool last_search_forward; /* the direction '/' (WP_TRUE) or '?' (WP_FALSE) last searched --
                                     'n' repeats this direction, 'N' repeats the opposite (M11) */

    /* M11: ':s/old/new/flags' and ':%s/old/new/flags' substitute state.
       old/new are copied out of cmdline_buf once the command is parsed
       (cmdline_buf itself is cleared on leaving COMMAND mode) -- same
       "own copy, not a borrowed slice" precedent as last_search above.
       The subst_paragraph/scan_offset/match fields are live ONLY while
       mode == WP_VIMODE_SUBSTITUTE_CONFIRM (the 'c' flag's per-match
       y/n/a/q pause); a plain ':s///' with no 'c' runs to completion
       synchronously inside execute_ex_command and never touches them. */
    u8      subst_old[WP_VIMODE_SEARCH_MAX];
    u32     subst_old_len;
    u8      subst_new[WP_VIMODE_SEARCH_MAX];
    u32     subst_new_len;
    wp_bool subst_global;         /* 'g' flag: every match per paragraph, not just the first */
    wp_bool subst_ignore_case;    /* 'i' flag */
    u32     subst_paragraph;      /* paragraph currently being scanned (confirm mode only) */
    u32     subst_last_paragraph; /* inclusive upper bound paragraph index for this command's scope */
    u32     subst_scan_offset;    /* byte offset within subst_paragraph to resume scanning from */
    wp_pos  subst_match;          /* pending match start, awaiting a confirm keystroke */

    wp_status last_error;

    wp_bool insert_group_open; /* tracks whether Insert mode (plain entry, or a
                                   change-operator's outer wrap) currently owns
                                   an open undo group -- see docs/vimode.md */
} wp_vimode_state;

typedef enum {
    WP_VIACTION_NONE = 0,
    WP_VIACTION_SAVE,       /* ':w' -- caller saves via its own path/format/Save-As logic */
    WP_VIACTION_QUIT,       /* ':q' -- caller closes; this module has no dirty/unsaved-changes concept */
    WP_VIACTION_SAVE_QUIT,  /* ':wq' or ':x' */
    WP_VIACTION_BEEP        /* invalid ex-command, or an unrecognized Normal-mode key -- cosmetic */
} wp_vimode_action_kind;

typedef struct {
    u8  kind;                          /* wp_vimode_action_kind */
    u8  arg[WP_VIMODE_ACTION_ARG_MAX]; /* reserved, unused this pass -- no ex-command arguments
                                           (e.g. ":w filename") are parsed yet, see docs/vimode.md */
    u32 arg_len;
} wp_vimode_action;

void wp_vimode_init(wp_vimode_state *vs, wp_document *doc, wp_undo_journal *journal);
/* Frees register_bytes. Does not touch doc/journal (borrowed). */
void wp_vimode_free(wp_vimode_state *vs);

/* The entire input surface -- every mode transition, motion, operator,
   count, Insert-mode keystroke, ex-command, and search character goes
   through repeated calls to this single function. This single-entry-
   point shape is what makes host/tests/test_vimode.c's synthetic-
   key-script testing approach possible. */
wp_vimode_action wp_vimode_feed_key(wp_vimode_state *vs, wp_key_event key);

wp_vimode_mode wp_vimode_get_mode(const wp_vimode_state *vs);
/* Writes a NUL-terminated status string (e.g. "NORMAL", "INSERT",
   "COMMAND :wq") into out, truncating to fit out_cap. Intended for a
   platform's status/mode line (see the parallel UI-scaffolding work) --
   pure data, no drawing. */
void wp_vimode_get_status_text(const wp_vimode_state *vs, char *out, u32 out_cap);

/* Byte-literal substring search (no regex, no case-folding), forward or
   backward per `forward`, wrapping around the document boundary exactly
   once. WP_ERR if not found. A thin wrapper over the shared
   wp_document_find_text (editing.h, M11) -- see that function's own doc
   comment for the exact search/wrap/scratch-buffer semantics this
   inherits unchanged. Kept as a public vimode.h entry point (rather than
   having every vimode.c call site call wp_document_find_text directly)
   purely for call-site brevity and API stability. */
wp_status wp_vimode_search(const wp_document *doc, wp_pos from,
                            const u8 *needle, u32 needle_len,
                            wp_bool forward, wp_pos *out_pos);

#endif /* WP_VIMODE_H */
