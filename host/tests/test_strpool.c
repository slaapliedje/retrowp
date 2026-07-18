#include "test.h"
#include "strpool.h"

void test_strpool_reserves_offset_zero(void)
{
    wp_strpool pool;
    wp_span span;

    ASSERT_TRUE(wp_strpool_init(&pool) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&pool, (const u8 *)"hi", 2, &span) == WP_OK);
    ASSERT_TRUE(span.offset != 0); /* offset 0 is reserved, never real content */
    ASSERT_EQ_U32(span.length, 2);
    ASSERT_MEM_EQ(wp_strpool_get(&pool, span), "hi", 2);

    wp_strpool_free(&pool);
}

void test_strpool_multiple_appends_survive_growth(void)
{
    wp_strpool pool;
    wp_span spans[8];
    char buf[8];
    int i;

    ASSERT_TRUE(wp_strpool_init(&pool) == WP_OK);

    /* Force several reallocations by appending many small strings past
       the initial minimum capacity. */
    for (i = 0; i < 8; i++) {
        buf[0] = (char)('a' + i);
        ASSERT_TRUE(wp_strpool_append(&pool, (const u8 *)buf, 1, &spans[i]) == WP_OK);
    }

    /* Every earlier span must still resolve to its original byte after
       later appends forced the pool to grow and relocate -- spans are
       offsets, not pointers, so they must survive relocation. */
    for (i = 0; i < 8; i++) {
        char expected = (char)('a' + i);
        ASSERT_EQ_U32(spans[i].length, 1);
        ASSERT_MEM_EQ(wp_strpool_get(&pool, spans[i]), &expected, 1);
    }

    wp_strpool_free(&pool);
}

void test_strpool_empty_append(void)
{
    wp_strpool pool;
    wp_span span;

    ASSERT_TRUE(wp_strpool_init(&pool) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&pool, (const u8 *)"", 0, &span) == WP_OK);
    ASSERT_EQ_U32(span.length, 0);

    wp_strpool_free(&pool);
}
