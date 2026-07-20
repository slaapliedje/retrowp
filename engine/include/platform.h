#ifndef WP_PLATFORM_H
#define WP_PLATFORM_H

#include "types.h"

/* The platform interface (CLAUDE.md rule 1: engine/ talks to the outside
   world ONLY through these declarations -- no GEM, no MUI, no Toolbox, no
   DOS, no POSIX header may ever be included under engine/ or services/).
   Each backend under platform/<target>/ implements every function below.
   This header declares the interface only; it contains no implementation
   and includes no platform headers of its own. */


/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/* All engine allocation goes through these three functions (CLAUDE.md
   "Memory Rules"). mem_alloc(0) may return NULL or a distinct non-NULL
   value that is safe to pass to mem_free; callers must not dereference
   a zero-size allocation either way.
   mem_realloc(NULL, 0, new_size) behaves as mem_alloc(new_size).
   mem_realloc(ptr, old_size, 0) behaves as mem_free(ptr) and returns NULL.
   old_size must be the size the block was originally allocated or last
   reallocated with -- some backends (bare TOS) cannot query a live
   block's size, so callers are required to track it themselves. */
void *mem_alloc(u32 size);
void *mem_realloc(void *ptr, u32 old_size, u32 new_size);
void  mem_free(void *ptr);


/* ------------------------------------------------------------------ */
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

typedef void plat_file;

typedef enum {
    PLAT_FILE_READ   = 0,
    PLAT_FILE_WRITE  = 1,  /* create/truncate */
    PLAT_FILE_APPEND = 2
} plat_file_mode;

typedef enum {
    PLAT_SEEK_SET = 0,
    PLAT_SEEK_CUR = 1,
    PLAT_SEEK_END = 2
} plat_seek_whence;

/* native_path is an opaque, native byte string (whatever the host
   filesystem expects). Filesystem paths are not document text and are
   therefore not subject to the UTF-8-internal rule in skills/encoding.md;
   callers are responsible for producing a path the native filesystem
   accepts. */
wp_status plat_file_open(const char *native_path, plat_file_mode mode,
                          plat_file **out);
wp_status plat_file_read(plat_file *f, u8 *buf, u32 cap, u32 *out_len);
wp_status plat_file_write(plat_file *f, const u8 *buf, u32 len);
wp_status plat_file_seek(plat_file *f, i32 offset, plat_seek_whence whence);
wp_status plat_file_tell(plat_file *f, u32 *pos_out);
wp_status plat_file_size(plat_file *f, u32 *size_out);
void      plat_file_close(plat_file *f);
wp_status plat_file_delete(const char *native_path);
wp_status plat_file_exists(const char *native_path, wp_bool *exists_out);


/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */

/* Clipboard text crosses the UTF-8-internal boundary the same way file
   I/O and display text do: the engine hands UTF-8 in and out, and the
   backend converts to/from the native clipboard format internally. */
wp_status plat_clipboard_set_text(const u8 *utf8, u32 len);
wp_status plat_clipboard_get_text(u8 *out, u32 out_cap, u32 *out_len);
wp_bool   plat_clipboard_has_text(void);


/* ------------------------------------------------------------------ */
/* Timer / ticks                                                       */
/* ------------------------------------------------------------------ */

u32  plat_ticks_ms(void);
void plat_sleep_ms(u32 ms);


/* ------------------------------------------------------------------ */
/* Text metrics (CLAUDE.md rule 4: layout code reaches font metrics     */
/* ONLY through this seam -- never a platform font API directly)       */
/* ------------------------------------------------------------------ */

typedef void plat_font;

typedef struct {
    i32 ascent;   /* twips */
    i32 descent;  /* twips */
    i32 leading;  /* twips */
} plat_font_metrics;

wp_status plat_font_open(const u8 *utf8_font_name, u32 name_len,
                          i32 size_twips, u16 style_flags, plat_font **out);
void      plat_font_close(plat_font *f);
wp_status plat_font_metrics_get(plat_font *f, plat_font_metrics *out);

/* Measures a UTF-8 run under font f. width_twips_out is set to the run's
   advance width in twips. Layout depends on this being exact and
   deterministic for a given (font, text) pair. */
wp_status plat_measure_text(plat_font *f, const u8 *utf8, u32 len,
                             i32 *width_twips_out);


/* ------------------------------------------------------------------ */
/* Glyph draw                                                           */
/* ------------------------------------------------------------------ */

typedef void plat_gc;

wp_status plat_draw_text(plat_gc *gc, plat_font *f, i32 x, i32 y,
                          const u8 *utf8, u32 len, u8 r, u8 g, u8 b);
wp_status plat_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h,
                          u8 r, u8 g, u8 b);
void      plat_gc_flush(plat_gc *gc);


/* ------------------------------------------------------------------ */
/* Window / dialog primitives                                          */
/* ------------------------------------------------------------------ */

/* Deliberately minimal at this stage: enough surface for a stub/host
   backend to link and for a test harness to exercise the seam. The full
   event-loop shape (menus, redraw events, resize) is designed per
   platform starting at the first real GUI backend and is intentionally
   not locked in here. */

typedef void plat_window;

wp_status plat_window_create(const u8 *title_utf8, u32 title_len,
                              i32 w, i32 h, plat_window **out);
void      plat_window_destroy(plat_window *w);
wp_status plat_window_get_gc(plat_window *w, plat_gc **out);

typedef enum {
    PLAT_DIALOG_OK       = 0,
    PLAT_DIALOG_OKCANCEL = 1,
    PLAT_DIALOG_YESNO    = 2
} plat_dialog_kind;

typedef enum {
    PLAT_DR_OK     = 0,
    PLAT_DR_CANCEL = 1,
    PLAT_DR_YES    = 2,
    PLAT_DR_NO     = 3
} plat_dialog_result;

wp_status plat_dialog_message(plat_window *parent, plat_dialog_kind kind,
                               const u8 *utf8_msg, u32 len,
                               plat_dialog_result *out);

/* Native "choose one of N" dialog (M8.5) -- same philosophy as
   plat_dialog_message: the portable side says WHAT (a title and a list
   of option labels), the backend says HOW (GEM form_do() with
   OF_RBUTTON-grouped radio items on Atari; a real dialog-manager list/
   radio widget on Amiga/Mac; a numbered text prompt on DOS -- see
   docs/atari-platform.md). option_labels_utf8[i]/option_label_lens[i]
   give each option's label; default_index is pre-selected. *out_index
   is only meaningful when *out_cancelled is WP_FALSE. Required of every
   backend (WP_UNSUPPORTED is the documented legal escape hatch, not a
   reason to skip a target when it's actually being built). No free-text
   entry variant exists -- nothing in this project's current feature set
   needs one yet (font size and alignment are both small, fixed,
   pre-enumerated lists). */
wp_status plat_dialog_choice(plat_window *parent,
                              const u8 *title_utf8, u32 title_len,
                              const u8 * const *option_labels_utf8,
                              const u32 *option_label_lens,
                              u32 option_count, u32 default_index,
                              wp_bool *out_cancelled, u32 *out_index);


/* ------------------------------------------------------------------ */
/* Transport (offload link: serial or network)                         */
/* ------------------------------------------------------------------ */

/* The only seam that moves bytes for the offload link. offload/transport
   (framing, CRC, resync) is platform-free and calls these four functions
   through a thin adapter in engine/src/offload_client.c; it never opens a
   socket, pipe, or serial port itself. Each backend interprets `endpoint`
   by prefix and owns everything past that:
     platform/host:  "pipe:<path>", "tcp:<host>:<port>"
     platform/atari (future): "serial:<device>:<baud>", "tcp:<host>:<port>"
   Swapping transports for a new target means implementing these four
   functions again -- no change to offload.h, offload_client.c, or
   offload/transport is required. */

typedef void plat_transport;

wp_status plat_transport_open(const char *endpoint, u32 connect_timeout_ms,
                               plat_transport **out);

/* Reads up to out_cap bytes, waiting up to timeout_ms for at least one
   byte to arrive. Returns WP_OK with *out_len set (possibly less than
   out_cap) on any data, WP_TIMEOUT if none arrived in time, or WP_ERR if
   the connection is closed/broken (distinct from timeout, since a caller
   should reconnect rather than retry on WP_ERR). */
wp_status plat_transport_read(plat_transport *t, u8 *out, u32 out_cap,
                               u32 timeout_ms, u32 *out_len);
wp_status plat_transport_write(plat_transport *t, const u8 *data, u32 len,
                                u32 timeout_ms);
void      plat_transport_close(plat_transport *t);


/* ------------------------------------------------------------------ */
/* Encoding edge: native charset <-> UTF-8 (skills/encoding.md, Edge A) */
/* ------------------------------------------------------------------ */

/* Convert a UTF-8 run to the native charset for display/clipboard.
   Unmappable codepoints become a fallback byte (e.g. '?') AND are counted
   in *unmapped_count so callers can warn. */
wp_status plat_utf8_to_native(const u8 *utf8, u32 len,
                               u8 *out, u32 out_cap, u32 *out_len,
                               u32 *unmapped_count);

/* Convert native input (keyboard/clipboard/file) to UTF-8 for the engine. */
wp_status plat_native_to_utf8(const u8 *native, u32 len,
                               u8 *out, u32 out_cap, u32 *out_len);

#endif /* WP_PLATFORM_H */
