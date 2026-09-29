/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "platform.h"
#include "mem_host.h"

void test_mem_leak_counters(void)
{
    void *a, *b;

    mem_host_reset_counters();
    a = mem_alloc(16);
    b = mem_alloc(32);
    ASSERT_EQ_U32(mem_host_live_count(), 2);

    mem_free(a);
    ASSERT_EQ_U32(mem_host_live_count(), 1);

    b = mem_realloc(b, 32, 64); /* grow: no change in live block count */
    ASSERT_EQ_U32(mem_host_live_count(), 1);

    mem_free(b);
    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

void test_file_roundtrip(void)
{
    plat_file *f;
    u8 out[64];
    u32 n;
    static const char *path = "/tmp/wp_test_file_roundtrip.txt";

    ASSERT_TRUE(plat_file_open(path, PLAT_FILE_WRITE, &f) == WP_OK);
    ASSERT_TRUE(plat_file_write(f, (const u8 *)"hello", 5) == WP_OK);
    plat_file_close(f);

    ASSERT_TRUE(plat_file_open(path, PLAT_FILE_READ, &f) == WP_OK);
    ASSERT_TRUE(plat_file_read(f, out, sizeof(out), &n) == WP_OK);
    ASSERT_EQ_U32(n, 5);
    ASSERT_MEM_EQ(out, "hello", 5);
    plat_file_close(f);

    plat_file_delete(path);
}

void test_pipe_transport_loopback_and_timeout(void)
{
    plat_transport *t;
    u8 out[16];
    u32 n;

    ASSERT_TRUE(plat_transport_open("pipe:/tmp/wp_test_transport.fifo", 1000, &t) == WP_OK);

    ASSERT_TRUE(plat_transport_write(t, (const u8 *)"ping!", 5, 1000) == WP_OK);
    ASSERT_TRUE(plat_transport_read(t, out, sizeof(out), 1000, &n) == WP_OK);
    ASSERT_EQ_U32(n, 5);
    ASSERT_MEM_EQ(out, "ping!", 5);

    /* Nothing left to read: must time out, not block or error. */
    ASSERT_TRUE(plat_transport_read(t, out, sizeof(out), 200, &n) == WP_TIMEOUT);

    plat_transport_close(t);
}

void test_fake_monospace_metrics(void)
{
    plat_font *f;
    plat_font_metrics m;
    i32 w;

    ASSERT_TRUE(plat_font_open((const u8 *)"mono", 4, 240, 0, &f) == WP_OK);
    ASSERT_TRUE(plat_font_metrics_get(f, &m) == WP_OK);
    ASSERT_EQ_U32(m.ascent, 240);
    ASSERT_EQ_U32(m.descent, 60);

    ASSERT_TRUE(plat_measure_text(f, (const u8 *)"AAAA", 4, &w) == WP_OK);
    ASSERT_EQ_U32(w, 4 * 120);

    plat_font_close(f);
}
