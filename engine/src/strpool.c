#include <string.h>
#include "strpool.h"
#include "platform.h"
#include "growth_internal.h"

wp_status wp_strpool_init(wp_strpool *pool)
{
    pool->bytes = NULL;
    pool->length = 0;
    pool->capacity = 0;
    return WP_OK;
}

void wp_strpool_free(wp_strpool *pool)
{
    mem_free(pool->bytes);
    pool->bytes = NULL;
    pool->length = 0;
    pool->capacity = 0;
}

static wp_status strpool_grow(wp_strpool *pool, u32 needed_len)
{
    u32 new_cap;
    void *new_mem;

    if (needed_len <= pool->capacity) return WP_OK;

    new_cap = wp_grow_capacity(pool->capacity, needed_len, 128);

    new_mem = mem_realloc(pool->bytes, pool->capacity, new_cap);
    if (new_mem == NULL) return WP_NOMEM;

    pool->bytes = (u8 *)new_mem;
    pool->capacity = new_cap;
    return WP_OK;
}

wp_status wp_strpool_append(wp_strpool *pool, const u8 *bytes, u32 len,
                             wp_span *out_span)
{
    wp_status st;
    u32 offset;

    if (pool->length == 0) {
        st = strpool_grow(pool, 1);
        if (st != WP_OK) return st;
        pool->bytes[0] = 0;
        pool->length = 1;
    }

    st = strpool_grow(pool, pool->length + len);
    if (st != WP_OK) return st;

    offset = pool->length;
    if (len > 0) memcpy(pool->bytes + offset, bytes, len);
    pool->length += len;

    out_span->offset = offset;
    out_span->length = len;
    return WP_OK;
}

const u8 *wp_strpool_get(const wp_strpool *pool, wp_span span)
{
    return pool->bytes + span.offset;
}
