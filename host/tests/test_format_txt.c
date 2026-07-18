#include "test.h"
#include "format_txt.h"
#include "platform.h"

#define FIXTURE_CAP 4096

static wp_status read_fixture(const char *path, u8 *buf, u32 cap, u32 *out_len)
{
    plat_file *f;
    wp_status st;

    st = plat_file_open(path, PLAT_FILE_READ, &f);
    if (st != WP_OK) return st;
    st = plat_file_read(f, buf, cap, out_len);
    plat_file_close(f);
    return st;
}

/* Fixtures already in canonical form (LF-only, every line including the
   last terminated by '\n') must round-trip byte-for-byte through
   export -> import -> export. See docs/formats/txt.md. */
static void assert_roundtrips(const char *path)
{
    u8 original[FIXTURE_CAP];
    u32 original_len;
    wp_document doc;
    u8 exported[FIXTURE_CAP];
    u32 exported_len;

    ASSERT_TRUE(read_fixture(path, original, FIXTURE_CAP, &original_len) == WP_OK);
    ASSERT_TRUE(format_txt_import(original, original_len, &doc) == WP_OK);
    ASSERT_TRUE(format_txt_export(&doc, exported, FIXTURE_CAP, &exported_len) == WP_OK);

    ASSERT_EQ_U32(exported_len, original_len);
    ASSERT_MEM_EQ(exported, original, original_len);

    wp_document_free(&doc);
}

void test_format_txt_roundtrip_simple(void)
{
    assert_roundtrips("host/tests/fixtures/simple.txt");
}

void test_format_txt_roundtrip_empty(void)
{
    assert_roundtrips("host/tests/fixtures/empty.txt");
}

void test_format_txt_roundtrip_multi_para(void)
{
    assert_roundtrips("host/tests/fixtures/multi_para.txt");
}

void test_format_txt_roundtrip_blank_lines(void)
{
    assert_roundtrips("host/tests/fixtures/blank_lines.txt");
}

void test_format_txt_roundtrip_unicode(void)
{
    assert_roundtrips("host/tests/fixtures/unicode.txt");
}

void test_format_txt_import_no_trailing_newline(void)
{
    u8 original[FIXTURE_CAP];
    u32 original_len;
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;

    ASSERT_TRUE(read_fixture("host/tests/fixtures/no_trailing_newline.txt",
                             original, FIXTURE_CAP, &original_len) == WP_OK);
    ASSERT_TRUE(format_txt_import(original, original_len, &doc) == WP_OK);

    ASSERT_EQ_U32(doc.section_count, 1);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 1);

    para = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(para->run_count, 1);
    run = wp_paragraph_run_get(para, 0);
    ASSERT_EQ_U32(run->text.length, 17);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "No newline at end", 17);

    wp_document_free(&doc);
}

void test_format_txt_import_crlf(void)
{
    u8 original[FIXTURE_CAP];
    u32 original_len;
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;

    ASSERT_TRUE(read_fixture("host/tests/fixtures/crlf.txt",
                             original, FIXTURE_CAP, &original_len) == WP_OK);
    ASSERT_TRUE(format_txt_import(original, original_len, &doc) == WP_OK);

    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    para = wp_section_paragraph_get(sec, 0);
    run = wp_paragraph_run_get(para, 0);
    ASSERT_EQ_U32(run->text.length, 13); /* "CRLF line one" -- no \r included */
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "CRLF line one", 13);

    para = wp_section_paragraph_get(sec, 1);
    run = wp_paragraph_run_get(para, 0);
    ASSERT_EQ_U32(run->text.length, 13);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "CRLF line two", 13);

    wp_document_free(&doc);
}

void test_format_txt_import_empty_creates_zero_paragraphs(void)
{
    wp_document doc;
    wp_section *sec;

    ASSERT_TRUE(format_txt_import((const u8 *)"", 0, &doc) == WP_OK);
    ASSERT_EQ_U32(doc.section_count, 1);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 0);

    wp_document_free(&doc);
}

void test_format_txt_export_reports_size_on_overflow(void)
{
    wp_document doc;
    u8 tiny[1];
    u32 needed;

    ASSERT_TRUE(format_txt_import((const u8 *)"hi\n", 3, &doc) == WP_OK);

    ASSERT_TRUE(format_txt_export(&doc, tiny, 0, &needed) == WP_RANGE);
    ASSERT_EQ_U32(needed, 3);

    ASSERT_TRUE(format_txt_export(&doc, tiny, 1, &needed) == WP_RANGE);
    ASSERT_EQ_U32(needed, 3);

    wp_document_free(&doc);
}
