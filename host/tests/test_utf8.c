#include "test.h"
#include "utf8.h"

void test_utf8_ascii_roundtrip(void)
{
    u8 buf[4];
    u32 n, cp;

    n = utf8_encode('A', buf);
    ASSERT_EQ_U32(n, 1);
    ASSERT_TRUE(buf[0] == 'A');

    n = utf8_decode(buf, 1, &cp);
    ASSERT_EQ_U32(n, 1);
    ASSERT_EQ_U32(cp, 'A');
}

void test_utf8_2byte_roundtrip(void)
{
    /* U+00E9 LATIN SMALL LETTER E WITH ACUTE */
    u8 buf[4];
    u32 n, cp;

    n = utf8_encode(0xE9uL, buf);
    ASSERT_EQ_U32(n, 2);
    n = utf8_decode(buf, 2, &cp);
    ASSERT_EQ_U32(n, 2);
    ASSERT_EQ_U32(cp, 0xE9uL);
}

void test_utf8_3byte_roundtrip(void)
{
    /* U+20AC EURO SIGN */
    u8 buf[4];
    u32 n, cp;

    n = utf8_encode(0x20ACuL, buf);
    ASSERT_EQ_U32(n, 3);
    n = utf8_decode(buf, 3, &cp);
    ASSERT_EQ_U32(n, 3);
    ASSERT_EQ_U32(cp, 0x20ACuL);
}

void test_utf8_4byte_roundtrip(void)
{
    /* U+1F600 GRINNING FACE */
    u8 buf[4];
    u32 n, cp;

    n = utf8_encode(0x1F600uL, buf);
    ASSERT_EQ_U32(n, 4);
    n = utf8_decode(buf, 4, &cp);
    ASSERT_EQ_U32(n, 4);
    ASSERT_EQ_U32(cp, 0x1F600uL);
}

void test_utf8_rejects_surrogate(void)
{
    u8 buf[4];

    ASSERT_EQ_U32(utf8_encode(0xD800uL, buf), 0);
    ASSERT_EQ_U32(utf8_encode(0xDFFFuL, buf), 0);
}

void test_utf8_rejects_overlong(void)
{
    /* 0xC1 0x81 is an overlong 2-byte encoding of 'A' (U+0041) -- must
       be rejected even though the bit pattern is structurally well
       formed. */
    u8 buf[2];
    u32 cp;

    buf[0] = 0xC1u; buf[1] = 0x81u;
    ASSERT_EQ_U32(utf8_decode(buf, 2, &cp), 0);
}

void test_utf8_rejects_truncated(void)
{
    /* A 3-byte lead with only one continuation byte present. */
    u8 buf[2];
    u32 cp;

    buf[0] = 0xE2u; buf[1] = 0x82u;
    ASSERT_EQ_U32(utf8_decode(buf, 2, &cp), 0);
}

void test_utf8_seq_len_table(void)
{
    ASSERT_EQ_U32(utf8_seq_len(0x41u), 1); /* 'A' */
    ASSERT_EQ_U32(utf8_seq_len(0xC3u), 2);
    ASSERT_EQ_U32(utf8_seq_len(0xE2u), 3);
    ASSERT_EQ_U32(utf8_seq_len(0xF0u), 4);
    ASSERT_EQ_U32(utf8_seq_len(0x80u), 0); /* continuation byte */
    ASSERT_EQ_U32(utf8_seq_len(0xFFu), 0); /* invalid lead */
}

void test_utf8_next_prev_and_count(void)
{
    /* "Aé€" == 'A' (1 byte) + U+00E9 (2 bytes) + U+20AC (3 bytes) */
    u8 s[6];
    u32 o1, o2, o3, back;

    s[0] = 'A';
    s[1] = 0xC3u; s[2] = 0xA9u;
    s[3] = 0xE2u; s[4] = 0x82u; s[5] = 0xACu;

    o1 = utf8_next(s, 6, 0);  ASSERT_EQ_U32(o1, 1);
    o2 = utf8_next(s, 6, o1); ASSERT_EQ_U32(o2, 3);
    o3 = utf8_next(s, 6, o2); ASSERT_EQ_U32(o3, 6);
    ASSERT_EQ_U32(utf8_cp_count(s, 6), 3);

    back = utf8_prev(s, 6, o3); ASSERT_EQ_U32(back, o2);
    back = utf8_prev(s, 6, back); ASSERT_EQ_U32(back, o1);
    back = utf8_prev(s, 6, back); ASSERT_EQ_U32(back, 0);
}

void test_utf8_validate_good_and_bad(void)
{
    u8 good[] = { 'h', 'i', 0xC3u, 0xA9u };
    u8 bad[]  = { 'h', 0x80u, 'i' };
    u32 bad_offset = 999;

    ASSERT_TRUE(utf8_validate(good, 4, &bad_offset) == WP_OK);

    bad_offset = 999;
    ASSERT_TRUE(utf8_validate(bad, 3, &bad_offset) == WP_BADFMT);
    ASSERT_EQ_U32(bad_offset, 1);
}
