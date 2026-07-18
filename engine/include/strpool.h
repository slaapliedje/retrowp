#ifndef WP_STRPOOL_H
#define WP_STRPOOL_H

#include "types.h"

/* A single per-document UTF-8 string pool. Runs and other document
   structures store a wp_span (offset + length) into this pool rather than
   an owned buffer, per document-model.md Design Principle 3: handles and
   offsets, not raw pointers, for anything that must be serialized.
   Offsets remain valid across pool growth because they are relative to
   the buffer start, never to a particular memory address. */

typedef u32 wp_str; /* byte offset into a wp_strpool; 0 is the permanent
                        none/invalid sentinel, never a real span's offset */

typedef struct {
    wp_str offset;
    u32    length;
} wp_span;

typedef struct {
    u8  *bytes;     /* UTF-8, not NUL-terminated internally */
    u32  length;    /* bytes used */
    u32  capacity;  /* bytes allocated */
} wp_strpool;

/* Initializes an empty pool. The pool lazily reserves 1 byte at offset 0
   on the first append, so offset 0 can never be returned for real
   content -- this keeps wp_str's "0 means none" sentinel branch-free for
   callers. */
wp_status wp_strpool_init(wp_strpool *pool);
void      wp_strpool_free(wp_strpool *pool);

/* Appends bytes to the pool (growing it if needed) and returns a span
   referencing them. The pool never rewrites or reclaims previously
   issued spans -- growth here is strictly append-only (M1 scope: import
   and build only, no deletion; a future editing milestone will need a
   compaction or tombstoning strategy on top of this). */
wp_status wp_strpool_append(wp_strpool *pool, const u8 *bytes, u32 len,
                             wp_span *out_span);

/* Returns a direct pointer to the bytes at span (not NUL-terminated;
   length-bounded only via span.length). The pointer is invalidated by any
   subsequent wp_strpool_append that grows the pool -- callers must not
   hold it across a mutation. */
const u8 *wp_strpool_get(const wp_strpool *pool, wp_span span);

#endif /* WP_STRPOOL_H */
