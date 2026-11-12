/* SPDX-License-Identifier: GPL-2.0-or-later */
/* stw2txt: an Atari Works document through RetroWP's own importer
   (engine/src/format_stw.c) and plain-text exporter, written back in the
   Atari character set -- so it compares byte for byte with the .TXT
   Atari Works itself exported. host/tools/stw/stw_score.py runs it over
   the specimen corpus (docs/formats/atariworks.md). With --ps, the
   document's pages as PostScript instead (platform/common/psdev.c),
   to look at with Ghostscript: where its pictures landed. */
#include <stdio.h>
#include <stdlib.h>
#include "format_stw.h"
#include "format_txt.h"
#include "codepage.h"
#include "utf8.h"
#include "psdev.h"

static wp_status put_ps(void *user, const u8 *b, u32 n)
{
    (void)user;
    fwrite(b, 1, n, stdout);
    return WP_OK;
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    u8 *buf, *out;
    u32 need = 0, i, cp, k;
    wp_document doc;

    int ps = argc == 3 && argv[1][0] == '-';   /* --ps: the pages, as PostScript */

    if ((argc != 2 && !ps) || (f = fopen(argv[argc - 1], "rb")) == NULL) {
        fprintf(stderr, "usage: stw2txt [--ps] FILE.STW\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (u8 *)malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (format_stw_import(buf, (u32)n, &doc) != WP_OK) {
        fprintf(stderr, "stw2txt: %s does not open\n", argv[argc - 1]);
        return 1;
    }
    if (ps) {
        u32 pages = 0;
        return wp_ps_print(&doc, put_ps, NULL, 0, &pages) == WP_OK ? 0 : 1;
    }
    format_txt_export(&doc, NULL, 0, &need);
    out = (u8 *)malloc(need + 1);
    if (out == NULL || format_txt_export(&doc, out, need, &need) != WP_OK) return 2;
    for (i = 0; i < need; i += k) {
        u8 b;
        k = utf8_decode(out + i, need - i, &cp);
        if (k == 0) { k = 1; cp = '?'; }
        if (!wp_codepage_encode(WP_CODEPAGE_ATARI_ST, cp, &b)) b = '?';
        putchar(b);
    }
    wp_document_free(&doc);
    free(out);
    free(buf);
    return 0;
}
