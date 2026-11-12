/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "psdev.h"
#include "devout.h"
#include "pageprint.h"
#include "strpool.h"
#include "utf8.h"

typedef struct {
    wp_ps_write_fn write;
    void *user;
    wp_status st;
    wp_bool page_open;
    i32 sent_w, sent_h;     /* the paper last asked for, twips */
    u32 rgb;                /* the colour set last, 0xRRGGBB; past 24 bits for none yet */
} ps_out;

static void put(ps_out *o, const char *s, u32 n)
{
    if (o->st != WP_OK || n == 0) return;
    o->st = o->write(o->user, (const u8 *)s, n);
}

static void puts_(ps_out *o, const char *s) { put(o, s, (u32)strlen(s)); }

static void putn(ps_out *o, i32 v)
{
    char d[12];
    u32 n = 0;
    u32 u = (u32)(v < 0 ? -v : v);
    if (v < 0) puts_(o, "-");
    do { d[n++] = (char)('0' + u % 10u); u /= 10u; } while (u > 0 && n < 11);
    while (n > 0) put(o, &d[--n], 1);
    put(o, " ", 1);
}

/* An 0..255 colour channel as 0..1 to three places. */
static void putc01(ps_out *o, u8 c)
{
    char b[8];
    u32 v = ((u32)c * 1000u + 127u) / 255u;
    b[0] = (char)('0' + v / 1000u);
    b[1] = '.';
    b[2] = (char)('0' + (v / 100u) % 10u);
    b[3] = (char)('0' + (v / 10u) % 10u);
    b[4] = (char)('0' + v % 10u);
    b[5] = ' ';
    put(o, b, 6);
}

static void put_rgb(ps_out *o, u8 r, u8 g, u8 b)
{
    u32 rgb = ((u32)r << 16) | ((u32)g << 8) | b;
    if (rgb == o->rgb) return;   /* already the colour */
    o->rgb = rgb;
    putc01(o, r); putc01(o, g); putc01(o, b);
    puts_(o, "C\n");
}

/* The prolog: the twelve standard faces re-encoded to Latin-1, and the
   few procedures the pages use. */
static const char *const g_prolog[] = {
    "%!PS-Adobe-3.0\n%%Creator: RetroWP\n%%Pages: (atend)\n%%EndComments\n",
    "%%BeginProlog\n",
    /* Latin-1 where the printer lacks it (an early level 1 one):
       StandardEncoding with its upper half */
    "/ISOLatin1Encoding where { pop } { /ISOLatin1Encoding StandardEncoding 256 array copy def\n",
    "ISOLatin1Encoding 160 [\n",
    "/space /exclamdown /cent /sterling /currency /yen /brokenbar /section /dieresis /copyright /ordfeminine /guillemotleft /logicalnot /hyphen /registered /macron\n",
    "/degree /plusminus /twosuperior /threesuperior /acute /mu /paragraph /periodcentered /cedilla /onesuperior /ordmasculine /guillemotright /onequarter /onehalf /threequarters /questiondown\n",
    "/Agrave /Aacute /Acircumflex /Atilde /Adieresis /Aring /AE /Ccedilla /Egrave /Eacute /Ecircumflex /Edieresis /Igrave /Iacute /Icircumflex /Idieresis\n",
    "/Eth /Ntilde /Ograve /Oacute /Ocircumflex /Otilde /Odieresis /multiply /Oslash /Ugrave /Uacute /Ucircumflex /Udieresis /Yacute /Thorn /germandbls\n",
    "/agrave /aacute /acircumflex /atilde /adieresis /aring /ae /ccedilla /egrave /eacute /ecircumflex /edieresis /igrave /iacute /icircumflex /idieresis\n",
    "/eth /ntilde /ograve /oacute /ocircumflex /otilde /odieresis /divide /oslash /ugrave /uacute /ucircumflex /udieresis /yacute /thorn /ydieresis\n",
    "] putinterval } ifelse\n",
    /* WPEnc: Latin-1, and in its empty 0x80-0x8F the characters every
       standard text face has besides (g_extra below) */
    "/WPEnc ISOLatin1Encoding 256 array copy def WPEnc 128 [\n",
    "/quoteleft /quoteright /quotedblleft /quotedblright /endash /emdash /ellipsis /bullet\n",
    "/dagger /daggerdbl /trademark /oe /OE /perthousand /fi /fl ] putinterval\n",
    "/RE { findfont dup length dict begin { 1 index /FID ne { def } { pop pop } ifelse } forall"
    " /Encoding WPEnc def currentdict end definefont pop } bind def\n",
    "/F0 /Times-Roman RE /F1 /Times-Bold RE /F2 /Times-Italic RE /F3 /Times-BoldItalic RE\n",
    "/F4 /Helvetica RE /F5 /Helvetica-Bold RE /F6 /Helvetica-Oblique RE /F7 /Helvetica-BoldOblique RE\n",
    "/F8 /Courier RE /F9 /Courier-Bold RE /F10 /Courier-Oblique RE /F11 /Courier-BoldOblique RE\n",
    "/C { setrgbcolor } bind def\n",
    /* w h PSZ (the paper) and tumble DPX (two-sided) ask setpagedevice
       where there is one (level 2); a level 1 printer takes the paper
       it has, one-sided */
    "/PSZ { /setpagedevice where { pop 2 array astore 1 dict dup /PageSize 4 -1 roll put"
    " setpagedevice } { pop pop } ifelse } bind def\n",
    "/DPX { /setpagedevice where { pop 2 dict dup /Tumble 4 -1 roll put dup /Duplex true put"
    " setpagedevice } { pop } ifelse } bind def\n",
    /* x y w h R: a filled rectangle */
    "/R { 4 dict begin /h exch def /w exch def /y exch def /x exch def newpath x y moveto w 0 rlineto"
    " 0 h rlineto w neg 0 rlineto closepath fill end } bind def\n",
    /* x y w (s) size /Face T: s at x on the baseline y, w wide */
    "/T { 6 dict begin /fn exch def /fs exch def /s exch def /w exch def /y exch def /x exch def"
    " gsave x y translate 1 -1 scale fn findfont fs scalefont setfont"
    " s stringwidth pop dup 0 gt { w exch div } { pop 1 } ifelse 1 scale"
    " 0 0 moveto s show grestore end } bind def\n",
    "%%EndProlog\n",
    NULL
};

/* The face a style asks for: Times for a roman family (or a name that
   says Times, serif or roman), Courier for a modern one (or Courier,
   mono, typewriter), Helvetica otherwise; then bold, italic. */
static u32 face_of(const wp_document *doc, const wp_char_style *cs)
{
    u32 base = 4;
    if (cs != NULL && cs->font != 0) {
        const wp_font_desc *fd = wp_document_font_get(doc, cs->font);
        if (fd != NULL) {
            const u8 *nm = wp_strpool_get(&doc->strings, fd->name);
            u32 i, n = fd->name.length;
            char low[48];
            for (i = 0; i < n && i < sizeof(low) - 1; i++) {
                char c = (char)nm[i];
                low[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            low[i] = '\0';
            if (fd->family == (u8)WP_FAMILY_ROMAN || strstr(low, "times") || strstr(low, "serif") ||
                strstr(low, "roman") || strstr(low, "dutch") || strstr(low, "georgia")) base = 0;
            if (fd->family == (u8)WP_FAMILY_MODERN || strstr(low, "courier") || strstr(low, "mono") ||
                strstr(low, "typewriter")) base = 8;
            if (strstr(low, "sans")) base = 4;
        }
    }
    if (cs != NULL && (cs->flags & WP_STYLE_BOLD)) base += 1;
    if (cs != NULL && (cs->flags & WP_STYLE_ITALIC)) base += 2;
    return base;
}

/* WPEnc's 0x80-0x8F, in order (the prolog names them). */
static const u16 g_extra[16] = {
    0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2026, 0x2022,
    0x2020, 0x2021, 0x2122, 0x0153, 0x0152, 0x2030, 0xFB01, 0xFB02
};

/* A string literal: the text's characters in WPEnc (others their
   nearest plain one), escaped. */
static void put_pstring(ps_out *o, const u8 *utf8, u32 len)
{
    u32 pos = 0;
    put(o, "(", 1);
    while (pos < len) {
        u32 cp = '?';
        u32 n = utf8_decode(utf8 + pos, len - pos, &cp);
        u8 c;
        if (n == 0) { n = 1; cp = '?'; }
        pos += n;
        if (cp > 0xFF || (cp >= 0x80 && cp < 0xA0)) {
            u32 k;
            for (k = 0; k < 16 && g_extra[k] != cp; k++) {}
            if (k < 16) c = (u8)(0x80 + k);
            else { c = utf8_ascii_fallback(cp); if (c == 0) c = '?'; }
        } else {
            c = (u8)cp;
        }
        if (c == '(' || c == ')' || c == '\\') {
            char e[2]; e[0] = '\\'; e[1] = (char)c; put(o, e, 2);
        } else if (c < 0x20 || c >= 0x7F) {
            char e[4];
            e[0] = '\\'; e[1] = (char)('0' + (c >> 6)); e[2] = (char)('0' + ((c >> 3) & 7)); e[3] = (char)('0' + (c & 7));
            put(o, e, 4);
        } else {
            put(o, (const char *)&c, 1);
        }
    }
    put(o, ")", 1);
}

static void ps_text(void *user, const wp_document *doc, const wp_char_style *cs,
                    i32 x, i32 y, i32 w, i32 h, const u8 *utf8, u32 len)
{
    ps_out *o = (ps_out *)user;
    /* The size whose ascent-to-descent is the font's the layout used --
       the standard faces span about 1.12 em -- so the text fills the
       line it was given, whatever this machine's fonts are; the style's
       own size when there is no height. Tenths of a point. */
    i32 size = h > 0 ? (h * 100) / 112 : cs != NULL && cs->size_twips > 0 ? cs->size_twips / 2 : 240;
    u32 face = face_of(doc, cs);
    char fname[8];

    if (w <= 0 || len == 0) return;   /* nothing to show; a zero scale would be singular */
    put_rgb(o, cs != NULL ? cs->color_r : 0, cs != NULL ? cs->color_g : 0, cs != NULL ? cs->color_b : 0);
    putn(o, x); putn(o, y); putn(o, w);
    put_pstring(o, utf8, len);
    put(o, " ", 1);
    putn(o, size);
    fname[0] = '/'; fname[1] = 'F';
    if (face >= 10) { fname[2] = '1'; fname[3] = (char)('0' + face - 10); fname[4] = '\0'; }
    else { fname[2] = (char)('0' + face); fname[3] = '\0'; }
    puts_(o, fname);
    puts_(o, " T\n");
    if (cs != NULL && (cs->flags & WP_STYLE_UNDERLINE)) {   /* a rule just under the baseline */
        i32 t = size / 16 > 1 ? size / 16 : 1;
        putn(o, x); putn(o, y + size / 9); putn(o, w); putn(o, t);
        puts_(o, "R\n");
    }
}

static void ps_rect(void *user, i32 x, i32 y, i32 w, i32 h, u8 r, u8 g, u8 b)
{
    ps_out *o = (ps_out *)user;
    if (w <= 0 || h <= 0) return;
    put_rgb(o, r, g, b);
    putn(o, x); putn(o, y); putn(o, w); putn(o, h);
    puts_(o, "R\n");
}

/* Shading: a real grey on paper, where the screens use a pattern. */
static void ps_shade(void *user, i32 x, i32 y, i32 w, i32 h, u8 percent)
{
    u8 level = (u8)(255 - ((u32)(percent > 100 ? 100 : percent) * 255u) / 100u);
    ps_rect(user, x, y, w, h, level, level, level);
}

static void put_path(ps_out *o, const i32 *xy, u16 n)
{
    u16 i;
    puts_(o, "newpath ");
    for (i = 0; i < n; i++) {
        putn(o, xy[i * 2]); putn(o, xy[i * 2 + 1]);
        puts_(o, i == 0 ? "moveto\n" : "lineto\n");
    }
}

static void ps_polygon(void *user, const i32 *xy, u16 n, u8 r, u8 g, u8 b)
{
    ps_out *o = (ps_out *)user;
    if (n < 3) return;
    put_rgb(o, r, g, b);
    put_path(o, xy, n);
    puts_(o, "closepath eofill\n");
}

static void ps_polyline(void *user, const i32 *xy, u16 n, i32 width, u8 r, u8 g, u8 b)
{
    ps_out *o = (ps_out *)user;
    if (n < 2) return;
    put_rgb(o, r, g, b);
    putn(o, width > 1 ? width : 1);
    puts_(o, "setlinewidth 1 setlinecap 1 setlinejoin\n");
    put_path(o, xy, n);
    puts_(o, "stroke\n");
}

/* A band of a picture's dithered pixels: set bits in the colour. */
static void ps_mono(void *user, i32 x, i32 y, i32 w, i32 h, const u16 *rows, u32 row_words,
                    u8 r, u8 g, u8 b)
{
    static const char hex[] = "0123456789abcdef";
    ps_out *o = (ps_out *)user;
    u32 bytes = ((u32)w + 7u) / 8u, row, k;

    if (w <= 0 || h <= 0) return;
    put_rgb(o, r, g, b);
    puts_(o, "gsave ");
    putn(o, x); putn(o, y); puts_(o, "translate ");
    putn(o, w); putn(o, h); puts_(o, "scale ");
    putn(o, w); putn(o, h); puts_(o, "true [");
    putn(o, w); puts_(o, "0 0 "); putn(o, h); puts_(o, "0 0] {<\n");
    for (row = 0; row < (u32)h; row++) {
        const u16 *wds = rows + row * row_words;
        for (k = 0; k < bytes; k++) {
            u16 word = wds[k / 2];
            u8 byte = (u8)((k & 1) ? (word & 0xFF) : (word >> 8));
            char pair[2];
            pair[0] = hex[byte >> 4]; pair[1] = hex[byte & 15];
            put(o, pair, 2);
        }
        put(o, "\n", 1);
    }
    puts_(o, ">} imagemask grestore\n");
}

static const wp_dev_ops g_ps_ops = { ps_text, ps_rect, ps_shade, ps_polygon, ps_polyline, ps_mono };

static ps_out g_ps;
static u32 g_page_no;

static wp_status ps_begin_page(void *user, const wp_section *sec, u32 page_no)
{
    ps_out *o = (ps_out *)user;

    g_page_no = page_no;
    puts_(o, "%%Page: "); putn(o, (i32)page_no); putn(o, (i32)page_no); puts_(o, "\n");
    if (sec->page_width != o->sent_w || sec->page_height != o->sent_h) {
        putn(o, sec->page_width / 20); putn(o, sec->page_height / 20);
        puts_(o, "PSZ\n");
        o->sent_w = sec->page_width;
        o->sent_h = sec->page_height;
    }
    /* Tenths of a point from the paper's top left, y down. */
    puts_(o, "gsave 0 ");
    putn(o, sec->page_height / 20);
    puts_(o, "translate 0.1 -0.1 scale\n");
    o->page_open = WP_TRUE;
    o->rgb = 0;   /* a page starts in black */
    return o->st;
}

static wp_status ps_end_page(void *user)
{
    ps_out *o = (ps_out *)user;
    if (o->page_open) puts_(o, "grestore showpage\n");
    o->page_open = WP_FALSE;
    return o->st;
}

static i32 tenths(i32 twips) { return twips / 2; }

wp_status wp_ps_print(wp_document *doc, wp_ps_write_fn write, void *user, u8 sides, u32 *pages_out)
{
    static int dummy_gc;   /* the gc this device is bound to: only its address matters */
    wp_page_device dev;
    u32 pages = 0;
    wp_status st;

    if (pages_out != NULL) *pages_out = 0;
    memset(&g_ps, 0, sizeof(g_ps));
    g_ps.write = write;
    g_ps.user = user;
    g_ps.st = WP_OK;
    g_ps.rgb = 0x1000000L;
    {
        u32 k;
        for (k = 0; g_prolog[k] != NULL; k++) puts_(&g_ps, g_prolog[k]);
    }
    if (sides == 1) puts_(&g_ps, "false DPX\n");
    else if (sides == 2) puts_(&g_ps, "true DPX\n");

    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dummy_gc;
    dev.x_px = tenths;
    dev.y_px = tenths;
    dev.begin_page = ps_begin_page;
    dev.end_page = ps_end_page;
    dev.user = &g_ps;
    wp_dev_bind(dev.gc, &g_ps_ops, &g_ps);
    st = wp_pageprint(doc, &dev, &pages);
    if (st == WP_OK && g_ps.page_open) st = ps_end_page(&g_ps);
    wp_dev_bind(NULL, NULL, NULL);
    puts_(&g_ps, "%%Trailer\n%%Pages: ");
    putn(&g_ps, (i32)pages);
    puts_(&g_ps, "\n%%EOF\n");
    if (st == WP_OK) st = g_ps.st;
    if (pages_out != NULL) *pages_out = pages;
    return st;
}
