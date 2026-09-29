/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "endian.h"

void test_endian_read_write16(void)
{
    u8 buf[2];

    be_write16(buf, 0x1234u);
    ASSERT_TRUE(buf[0] == 0x12u);
    ASSERT_TRUE(buf[1] == 0x34u);
    ASSERT_EQ_U32(be_read16(buf), 0x1234u);
}

void test_endian_read_write32(void)
{
    u8 buf[4];

    be_write32(buf, 0xDEADBEEFuL);
    ASSERT_TRUE(buf[0] == 0xDEu);
    ASSERT_TRUE(buf[1] == 0xADu);
    ASSERT_TRUE(buf[2] == 0xBEu);
    ASSERT_TRUE(buf[3] == 0xEFu);
    ASSERT_EQ_U32(be_read32(buf), 0xDEADBEEFuL);
}

void test_endian_roundtrip_values(void)
{
    /* be_read/be_write are shift-based, not memory reinterpretation, so
       these round-trip correctly regardless of host endianness -- there
       is no separate "LE code path" to force here, the same code runs on
       every host by construction (see endian.h). */
    static const u32 values[] = {
        0uL, 1uL, 0xFFuL, 0x100uL, 0xFFFFuL, 0x10000uL,
        0x7FFFFFFFuL, 0xFFFFFFFFuL
    };
    u32 i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        u8 buf[4];
        be_write32(buf, values[i]);
        ASSERT_EQ_U32(be_read32(buf), values[i]);
    }
}

void test_endian_swap(void)
{
    ASSERT_EQ_U32(be_swap16(0x1234u), 0x3412u);
    ASSERT_EQ_U32(be_swap32(0x12345678uL), 0x78563412uL);
}

void test_endian_host_is_little_endian_consistent(void)
{
    wp_bool a = host_is_little_endian();
    wp_bool b = host_is_little_endian();

    ASSERT_TRUE(a == b);
    ASSERT_TRUE(a == WP_TRUE || a == WP_FALSE);
}
