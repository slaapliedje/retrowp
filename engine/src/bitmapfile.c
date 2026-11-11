/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "bitmapfile.h"
#include "platform.h"

/* ---- Reading a row at a time -----------------------------------------
 *
 * Each format opens to a width, a height and a palette of up to 256 (a
 * direct-colour file's rows are put on a 6 x 6 x 6 cube instead), then
 * gives its rows top-down as palette indices, and can start again for
 * the second pass. */

enum { BM_IMG, BM_ILBM, BM_PBM, BM_MACPAINT, BM_PCX, BM_BMP };

typedef struct {
    int kind;
    const u8 *d;
    u32 len;
    i32 w, h;
    u32 ncol;
    u8  pal[256][3];
    u32 start, pos;
    i32 y;
    u32 planes, rowbytes;      /* raw bytes a row: planes x rowbytes */
    u8 *buf;
    u32 patlen, rep;           /* IMG: pattern length; rows still to repeat */
    u8  compress, masking;     /* ILBM/PBM */
    u32 bpp, bpl;              /* PCX: bits a pixel a plane, bytes a plane line */
    u32 stride;                /* BMP */
    wp_bool topdown;
} bm_src;

static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 le16(const u8 *p) { return (u16)((u16)p[0] | ((u16)p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* The 6 x 6 x 6 cube a direct-colour pixel is put on. */
static u8 cube(u8 r, u8 g, u8 b)
{
    return (u8)((r * 6 / 256) * 36 + (g * 6 / 256) * 6 + (b * 6 / 256));
}

static void cube_palette(bm_src *s)
{
    u32 i;
    for (i = 0; i < 216; i++) {
        s->pal[i][0] = (u8)((i / 36) * 51);
        s->pal[i][1] = (u8)(((i / 6) % 6) * 51);
        s->pal[i][2] = (u8)((i % 6) * 51);
    }
    s->ncol = 216;
}

static void grey_palette(bm_src *s, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) s->pal[i][0] = s->pal[i][1] = s->pal[i][2] = (u8)(n > 1 ? (i * 255) / (n - 1) : 0);
    s->ncol = n;
}

/* The VDI's first sixteen: an IMG of several planes without a palette. */
static const u8 g_vdi16[16][3] = {
    {255,255,255}, {0,0,0}, {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255},
    {255,255,0}, {255,0,255}, {192,192,192}, {128,128,128}, {128,0,0},
    {0,128,0}, {0,0,128}, {0,128,128}, {128,128,0}, {128,0,128}
};

static wp_bool name_ends(const char *name, const char *ext)
{
    u32 n, e, i;
    if (name == NULL) return WP_FALSE;
    n = (u32)strlen(name);
    e = (u32)strlen(ext);
    if (n < e) return WP_FALSE;
    for (i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return WP_FALSE;
    }
    return WP_TRUE;
}

static wp_bool macbinary_pntg(const u8 *d, u32 len)
{
    return (wp_bool)(len >= 128 && d[0] == 0 && d[1] >= 1 && d[1] <= 63 &&
                     memcmp(d + 65, "PNTG", 4) == 0);
}

static int bm_kind(const u8 *d, u32 len, const char *name)
{
    if (len >= 12 && memcmp(d, "FORM", 4) == 0 && memcmp(d + 8, "ILBM", 4) == 0) return BM_ILBM;
    if (len >= 12 && memcmp(d, "FORM", 4) == 0 && memcmp(d + 8, "PBM ", 4) == 0) return BM_PBM;
    if (len >= 54 && d[0] == 'B' && d[1] == 'M' && le32(d + 14) >= 12u && le32(d + 14) <= 124u) return BM_BMP;
    if (len >= 128 && d[0] == 0x0A && d[1] <= 5 && d[2] == 1 &&
        (d[3] == 1 || d[3] == 2 || d[3] == 4 || d[3] == 8)) return BM_PCX;
    if (len >= 16 && (be16(d) == 1 || be16(d) == 2) && be16(d + 2) >= 8 && be16(d + 2) <= 200 &&
        (be16(d + 4) == 1 || be16(d + 4) == 2 || be16(d + 4) == 4 || be16(d + 4) == 8) &&
        be16(d + 12) > 0 && be16(d + 14) > 0) return BM_IMG;
    if (macbinary_pntg(d, len) || name_ends(name, ".MAC") || name_ends(name, ".PNTG")) return BM_MACPAINT;
    return -1;
}

wp_bool bitmapfile_detect(const u8 *bytes, u32 len, const char *name)
{
    return (wp_bool)(bm_kind(bytes, len, name) >= 0);
}

/* ---- Opening ---------------------------------------------------------- */

static wp_status open_img(bm_src *s)
{
    const u8 *d = s->d;
    u32 hl = be16(d + 2), k;

    s->planes = be16(d + 4);
    s->patlen = be16(d + 6);
    s->w = be16(d + 12);
    s->h = be16(d + 14);
    s->start = hl * 2u;
    if (s->start > s->len) return WP_BADFMT;
    s->rowbytes = ((u32)s->w + 7u) / 8u;
    if (s->planes == 1) {
        s->ncol = 2;
        s->pal[0][0] = s->pal[0][1] = s->pal[0][2] = 255;
        s->pal[1][0] = s->pal[1][1] = s->pal[1][2] = 0;
    } else if (hl >= 11u + 3u * (1u << s->planes) && memcmp(d + 16, "XIMG", 4) == 0 && be16(d + 20) == 0) {
        s->ncol = 1u << s->planes;           /* RGB in thousandths */
        for (k = 0; k < s->ncol; k++) {
            u32 c;
            for (c = 0; c < 3; c++) {
                u32 v = be16(d + 22 + (k * 3u + c) * 2u);
                s->pal[k][c] = (u8)(v >= 1000u ? 255u : (v * 255u + 500u) / 1000u);
            }
        }
    } else if (s->planes <= 4) {
        s->ncol = 1u << s->planes;
        memcpy(s->pal, g_vdi16, s->ncol * 3u);
    } else {
        grey_palette(s, 256);
    }
    return WP_OK;
}

/* An IFF chunk walk: BMHD, CMAP, CAMG, BODY. */
static wp_status open_iff(bm_src *s)
{
    const u8 *d = s->d;
    u32 pos = 12, end = 8u + be32(d + 4), camg = 0, nplanes = 0;
    wp_bool have_bmhd = WP_FALSE, have_cmap = WP_FALSE;

    if (end > s->len) end = s->len;
    while (pos + 8u <= end) {
        u32 size = be32(d + pos + 4);
        const u8 *c = d + pos + 8;
        if (pos + 8u + size > s->len) return WP_BADFMT;
        if (memcmp(d + pos, "BMHD", 4) == 0 && size >= 20) {
            s->w = be16(c);
            s->h = be16(c + 2);
            nplanes = c[8];
            s->masking = c[9];
            s->compress = c[10];
            have_bmhd = WP_TRUE;
        } else if (memcmp(d + pos, "CMAP", 4) == 0) {
            u32 n = size / 3u, k;
            if (n > 256) n = 256;
            for (k = 0; k < n; k++) memcpy(s->pal[k], c + k * 3u, 3);
            s->ncol = n;
            have_cmap = WP_TRUE;
        } else if (memcmp(d + pos, "CAMG", 4) == 0 && size >= 4) {
            camg = be32(c);
        } else if (memcmp(d + pos, "BODY", 4) == 0) {
            s->start = pos + 8u;
            break;
        }
        pos += 8u + size + (size & 1u);
    }
    if (!have_bmhd || s->start == 0 || s->w <= 0 || s->h <= 0) return WP_BADFMT;
    if (s->compress > 1) return WP_UNSUPPORTED;
    if (s->kind == BM_PBM) {
        if (nplanes != 8) return WP_UNSUPPORTED;
        s->planes = 1;
        s->rowbytes = ((u32)s->w + 1u) & ~1u;
    } else {
        if ((camg & 0x800u) || nplanes == 0 || nplanes > 8) return WP_UNSUPPORTED;   /* HAM, deep */
        s->rowbytes = (((u32)s->w + 15u) / 16u) * 2u;
        s->planes = nplanes + (s->masking == 1 ? 1u : 0u);
        s->bpp = nplanes;                    /* the colour planes; a mask plane after them */
        if ((camg & 0x80u) && nplanes == 6 && have_cmap) {   /* Extra-Half-Brite */
            u32 k;
            for (k = 0; k < 32; k++) {
                s->pal[k + 32][0] = (u8)(s->pal[k][0] / 2);
                s->pal[k + 32][1] = (u8)(s->pal[k][1] / 2);
                s->pal[k + 32][2] = (u8)(s->pal[k][2] / 2);
            }
            s->ncol = 64;
        }
    }
    if (!have_cmap) grey_palette(s, s->kind == BM_PBM ? 256u : 1u << nplanes);
    return WP_OK;
}

static wp_status open_pcx(bm_src *s)
{
    const u8 *d = s->d;
    u32 k;

    s->bpp = d[3];
    s->planes = d[65];
    s->bpl = le16(d + 66);
    s->w = (i32)le16(d + 8) - (i32)le16(d + 4) + 1;
    s->h = (i32)le16(d + 10) - (i32)le16(d + 6) + 1;
    s->start = 128;
    if (s->w <= 0 || s->h <= 0 || s->planes == 0 || s->bpl == 0) return WP_BADFMT;
    s->rowbytes = s->bpl;
    if (s->bpp == 8 && s->planes == 3) {
        cube_palette(s);
    } else if (s->bpp == 8 && s->planes == 1) {
        if (s->len < 769u + 128u || d[s->len - 769u] != 0x0C) {
            grey_palette(s, 256);
        } else {
            for (k = 0; k < 256; k++) memcpy(s->pal[k], d + s->len - 768u + k * 3u, 3);
            s->ncol = 256;
        }
    } else if (s->bpp == 1 && s->planes == 1) {
        s->ncol = 2;                          /* 0 black, 1 white */
        s->pal[0][0] = s->pal[0][1] = s->pal[0][2] = 0;
        s->pal[1][0] = s->pal[1][1] = s->pal[1][2] = 255;
    } else if (s->bpp * s->planes <= 4) {
        s->ncol = 1u << (s->bpp * s->planes);
        for (k = 0; k < s->ncol; k++) memcpy(s->pal[k], d + 16 + k * 3u, 3);
    } else {
        return WP_UNSUPPORTED;
    }
    return WP_OK;
}

static wp_status open_bmp(bm_src *s)
{
    const u8 *d = s->d;
    u32 hdr = le32(d + 14), off = le32(d + 10), comp = 0, used, k, entry;
    i32 h;

    if (hdr == 12u) {                         /* OS/2 */
        s->w = le16(d + 18);
        h = (i32)(i16)le16(d + 20);
        s->bpp = le16(d + 24);
        entry = 3;
        used = 0;
    } else {
        s->w = (i32)le32(d + 18);
        h = (i32)le32(d + 22);
        s->bpp = le16(d + 28);
        comp = le32(d + 30);
        used = le32(d + 46);
        entry = 4;
    }
    if (comp != 0 && !(comp == 3 && s->bpp == 32)) return WP_UNSUPPORTED;   /* RLE, bitfields */
    s->topdown = (wp_bool)(h < 0);
    s->h = h < 0 ? -h : h;
    if (s->w <= 0 || s->h <= 0) return WP_BADFMT;
    s->start = off;
    s->stride = (((u32)s->w * s->bpp + 31u) / 32u) * 4u;
    if (s->bpp <= 8) {
        u32 n = used != 0 && used <= (1u << s->bpp) ? used : (1u << s->bpp);
        u32 at = 14u + hdr;
        if (at + n * entry > s->len) return WP_BADFMT;
        for (k = 0; k < n; k++) {             /* BGR(x) */
            s->pal[k][0] = d[at + k * entry + 2];
            s->pal[k][1] = d[at + k * entry + 1];
            s->pal[k][2] = d[at + k * entry];
        }
        s->ncol = n;
    } else if (s->bpp == 24 || s->bpp == 32) {
        cube_palette(s);
    } else {
        return WP_UNSUPPORTED;
    }
    if (off + s->stride * (u32)s->h > s->len) return WP_BADFMT;
    return WP_OK;
}

static wp_status bm_open(bm_src *s, const u8 *d, u32 len, const char *name)
{
    wp_status st;

    memset(s, 0, sizeof(*s));
    s->d = d;
    s->len = len;
    s->kind = bm_kind(d, len, name);
    switch (s->kind) {
    case BM_IMG: st = open_img(s); break;
    case BM_ILBM:
    case BM_PBM: st = open_iff(s); break;
    case BM_PCX: st = open_pcx(s); break;
    case BM_BMP: st = open_bmp(s); break;
    case BM_MACPAINT:
        s->w = 576; s->h = 720; s->planes = 1; s->rowbytes = 72;
        s->start = macbinary_pntg(d, len) ? 128u + 512u : 512u;
        s->ncol = 2;
        s->pal[0][0] = s->pal[0][1] = s->pal[0][2] = 255;
        s->pal[1][0] = s->pal[1][1] = s->pal[1][2] = 0;
        st = s->start < len ? WP_OK : WP_BADFMT;
        break;
    default:
        return WP_UNSUPPORTED;
    }
    if (st != WP_OK) return st;
    if (s->w > 4096 || s->h > 4096) return WP_UNSUPPORTED;
    if (s->kind != BM_BMP) {                  /* a row's raw bytes: planes x a plane's line */
        u32 n = s->planes * s->rowbytes;
        s->buf = (u8 *)mem_alloc(n ? n : 1u);
        if (s->buf == NULL) return WP_NOMEM;
    }
    s->pos = s->start;
    return WP_OK;
}

static void bm_rewind(bm_src *s)
{
    s->pos = s->start;
    s->y = 0;
    s->rep = 0;
}

static void bm_close(bm_src *s)
{
    if (s->buf != NULL) mem_free(s->buf);
    s->buf = NULL;
}

/* ---- Rows ------------------------------------------------------------ */

/* ByteRun1 / PackBits into out[n]: n-byte literal runs (0..127 is n+1
   bytes), repeats (-1..-127 is 1-n copies), -128 nothing. */
static wp_status unpack_bits(bm_src *s, u8 *out, u32 n)
{
    u32 k = 0;
    while (k < n) {
        i32 c;
        if (s->pos >= s->len) return WP_BADFMT;
        c = (i32)(signed char)s->d[s->pos++];
        if (c >= 0) {
            u32 cnt = (u32)c + 1u;
            if (s->pos + cnt > s->len) return WP_BADFMT;
            if (cnt > n - k) cnt = n - k;
            memcpy(out + k, s->d + s->pos, cnt);
            s->pos += (u32)c + 1u;
            k += cnt;
        } else if (c != -128) {
            u32 cnt = (u32)(1 - c);
            if (s->pos >= s->len) return WP_BADFMT;
            if (cnt > n - k) cnt = n - k;
            memset(out + k, s->d[s->pos++], cnt);
            k += cnt;
        }
    }
    return WP_OK;
}

static wp_status raw_bytes(bm_src *s, u8 *out, u32 n)
{
    if (s->pos + n > s->len) return WP_BADFMT;
    memcpy(out, s->d + s->pos, n);
    s->pos += n;
    return WP_OK;
}

/* One IMG scanline into buf: every plane's bytes, after any vertical
   replication count, which repeats it. */
static wp_status img_line(bm_src *s)
{
    u32 p;

    if (s->rep > 0) { s->rep--; return WP_OK; }
    if (s->pos + 4u <= s->len && s->d[s->pos] == 0 && s->d[s->pos + 1] == 0 && s->d[s->pos + 2] == 0xFF) {
        u32 n = s->d[s->pos + 3];
        s->rep = n > 0 ? n - 1u : 0;
        s->pos += 4u;
    }
    for (p = 0; p < s->planes; p++) {
        u8 *out = s->buf + p * s->rowbytes;
        u32 k = 0;
        while (k < s->rowbytes) {
            u8 b;
            if (s->pos >= s->len) return WP_BADFMT;
            b = s->d[s->pos++];
            if (b == 0x00) {                  /* a pattern, repeated */
                u32 cnt, i, j;
                if (s->pos >= s->len) return WP_BADFMT;
                cnt = s->d[s->pos++];
                if (s->pos + s->patlen > s->len) return WP_BADFMT;
                for (i = 0; i < cnt; i++) {
                    for (j = 0; j < s->patlen && k < s->rowbytes; j++) out[k++] = s->d[s->pos + j];
                }
                s->pos += s->patlen;
            } else if (b == 0x80) {           /* literal bytes */
                u32 cnt;
                if (s->pos >= s->len) return WP_BADFMT;
                cnt = s->d[s->pos++];
                if (s->pos + cnt > s->len) return WP_BADFMT;
                while (cnt-- > 0) { if (k < s->rowbytes) out[k++] = s->d[s->pos]; s->pos++; }
            } else {                          /* a solid run of set or clear bytes */
                u32 cnt = b & 0x7Fu;
                u8 v = (u8)((b & 0x80) ? 0xFF : 0x00);
                while (cnt-- > 0 && k < s->rowbytes) out[k++] = v;
            }
        }
    }
    return WP_OK;
}

/* PCX: one scanline's run-length bytes, all planes, continuously. */
static wp_status pcx_line(bm_src *s)
{
    u32 n = s->planes * s->bpl, k = 0;
    while (k < n) {
        u8 b;
        if (s->pos >= s->len) return WP_BADFMT;
        b = s->d[s->pos++];
        if ((b & 0xC0) == 0xC0) {
            u32 cnt = b & 0x3Fu;
            if (s->pos >= s->len) return WP_BADFMT;
            while (cnt-- > 0 && k < n) s->buf[k++] = s->d[s->pos];
            s->pos++;
        } else {
            s->buf[k++] = b;
        }
    }
    return WP_OK;
}

/* Planar bytes (plane p at buf + p * rowbytes, bit 7 leftmost) to
   indices: the first `nplanes` planes. */
static void planar(const bm_src *s, u32 nplanes, u8 *out)
{
    i32 x;
    for (x = 0; x < s->w; x++) {
        u32 p, v = 0, byte = (u32)x >> 3;
        u8 bit = (u8)(0x80u >> (x & 7));
        for (p = 0; p < nplanes; p++) if (s->buf[p * s->rowbytes + byte] & bit) v |= 1u << p;
        out[x] = (u8)v;
    }
}

static wp_status bm_row(bm_src *s, u8 *out)
{
    wp_status st = WP_OK;
    i32 x;

    switch (s->kind) {
    case BM_IMG:
        st = img_line(s);
        if (st == WP_OK) planar(s, s->planes, out);
        break;
    case BM_MACPAINT:
        st = unpack_bits(s, s->buf, 72);
        if (st == WP_OK) planar(s, 1, out);
        break;
    case BM_ILBM: {
        u32 p;
        for (p = 0; p < s->planes && st == WP_OK; p++) {
            u8 *dst = s->buf + p * s->rowbytes;
            st = s->compress ? unpack_bits(s, dst, s->rowbytes) : raw_bytes(s, dst, s->rowbytes);
        }
        if (st == WP_OK) planar(s, s->bpp, out);
        break;
    }
    case BM_PBM:
        st = s->compress ? unpack_bits(s, s->buf, s->rowbytes) : raw_bytes(s, s->buf, s->rowbytes);
        if (st == WP_OK) memcpy(out, s->buf, (u32)s->w);
        break;
    case BM_PCX:
        st = pcx_line(s);
        if (st != WP_OK) break;
        if (s->bpp == 8 && s->planes == 3) {
            for (x = 0; x < s->w; x++) out[x] = cube(s->buf[x], s->buf[s->bpl + x], s->buf[2 * s->bpl + x]);
        } else if (s->bpp == 8) {
            memcpy(out, s->buf, (u32)s->w);
        } else if (s->planes == 1) {          /* 1, 2 or 4 bits a pixel, packed */
            for (x = 0; x < s->w; x++) {
                u32 bit = (u32)x * s->bpp;
                out[x] = (u8)((s->buf[bit >> 3] >> (8u - s->bpp - (bit & 7u))) & ((1u << s->bpp) - 1u));
            }
        } else {                               /* 1 bit a plane, several planes */
            planar(s, s->planes, out);
        }
        break;
    case BM_BMP: {
        u32 row = s->topdown ? (u32)s->y : (u32)(s->h - 1 - s->y);
        const u8 *p = s->d + s->start + row * s->stride;
        for (x = 0; x < s->w; x++) {
            switch (s->bpp) {
            case 1: out[x] = (u8)((p[x >> 3] >> (7 - (x & 7))) & 1u); break;
            case 4: out[x] = (u8)((x & 1) ? (p[x >> 1] & 0x0F) : (p[x >> 1] >> 4)); break;
            case 8: out[x] = p[x]; break;
            case 24: out[x] = cube(p[x * 3 + 2], p[x * 3 + 1], p[x * 3]); break;
            default: out[x] = cube(p[x * 4 + 2], p[x * 4 + 1], p[x * 4]); break;
            }
        }
        break;
    }
    default:
        st = WP_UNSUPPORTED;
    }
    if (st == WP_OK) {
        for (x = 0; x < s->w; x++) if ((u32)out[x] >= s->ncol) out[x] = 0;
        s->y++;
    }
    return st;
}

/* ---- Fitting it to the machine --------------------------------------- */

static i32 luma(const u8 *c) { return (i32)c[0] * 30 + (i32)c[1] * 59 + (i32)c[2] * 11; }

static i32 dist2(const u8 *a, const u8 *b)
{
    i32 dr = (i32)a[0] - b[0], dg = (i32)a[1] - b[1], db = (i32)a[2] - b[2];
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

typedef struct {
    u32 count[256];
    u8  map[256];            /* source index -> output index */
    u8  out_pal[16][3];
    u32 out_n;
    i32 out_luma[16];
} bm_fit;

/* The most used colours, as many as the machine shows, with the darkest
   and the lightest kept whatever their count -- line art's ink and its
   paper -- and every colour mapped to the nearest of them. */
static void choose_palette(const bm_src *s, bm_fit *f, u32 max_colors)
{
    u32 chosen[16], n = 0, i, k, dark = 0, light = 0;
    wp_bool any = WP_FALSE;

    for (i = 0; i < s->ncol; i++) {
        if (f->count[i] == 0) continue;
        if (!any || luma(s->pal[i]) < luma(s->pal[dark])) dark = i;
        if (!any || luma(s->pal[i]) > luma(s->pal[light])) light = i;
        any = WP_TRUE;
    }
    if (!any) { dark = 0; light = 0; }
    chosen[n++] = dark;
    if (light != dark && n < max_colors) chosen[n++] = light;
    while (n < max_colors) {
        u32 best = 0;
        wp_bool found = WP_FALSE;
        for (i = 0; i < s->ncol; i++) {
            wp_bool taken = WP_FALSE;
            if (f->count[i] == 0) continue;
            for (k = 0; k < n; k++) if (chosen[k] == i) taken = WP_TRUE;
            if (taken) continue;
            if (!found || f->count[i] > f->count[best]) { best = i; found = WP_TRUE; }
        }
        if (!found) break;
        chosen[n++] = best;
    }
    for (k = 0; k < n; k++) {
        memcpy(f->out_pal[k], s->pal[chosen[k]], 3);
        f->out_luma[k] = luma(f->out_pal[k]);
    }
    f->out_n = n;
    for (i = 0; i < s->ncol; i++) {
        i32 bd = 0;
        u8 bk = 0;
        for (k = 0; k < n; k++) {
            i32 dd = dist2(s->pal[i], f->out_pal[k]);
            if (k == 0 || dd < bd) { bd = dd; bk = (u8)k; }
        }
        f->map[i] = bk;
    }
}

wp_status bitmapfile_read(const u8 *bytes, u32 len, const char *name,
                          u32 max_colors, i32 max_w, i32 max_h,
                          const wp_image_sink *sink, wp_document *doc,
                          wp_image_desc *desc)
{
    bm_src *s;
    bm_fit *f;
    u8 *row = NULL, *acc = NULL, *packed = NULL;
    i32 ow, oh, y, oy;
    char path[128];
    u32 path_len = 0, packed_len;
    wp_status st;

    if (sink == NULL || sink->begin == NULL || sink->write == NULL) return WP_ERR;
    if (max_colors < 2) max_colors = 2;
    if (max_colors > 16) max_colors = 16;
    s = (bm_src *)mem_alloc((u32)sizeof(bm_src));
    f = (bm_fit *)mem_alloc((u32)sizeof(bm_fit));
    if (s == NULL || f == NULL) {
        if (s != NULL) mem_free(s);
        if (f != NULL) mem_free(f);
        return WP_NOMEM;
    }
    st = bm_open(s, bytes, len, name);
    if (st != WP_OK) goto done;

    /* The machine's largest picture, the shape kept. */
    ow = s->w;
    oh = s->h;
    if (max_w > 0 && ow > max_w) { oh = (i32)(((long)oh * max_w) / ow); ow = max_w; }
    if (max_h > 0 && oh > max_h) { ow = (i32)(((long)ow * max_h) / oh); oh = max_h; }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;

    row = (u8 *)mem_alloc((u32)s->w);
    acc = (u8 *)mem_alloc((u32)ow);
    packed_len = ((u32)ow + 1u) / 2u;
    packed = (u8 *)mem_alloc(packed_len);
    if (row == NULL || acc == NULL || packed == NULL) { st = WP_NOMEM; goto done; }

    /* First pass: the colours, and how much of each. */
    memset(f->count, 0, sizeof(f->count));
    for (y = 0; y < s->h && st == WP_OK; y++) {
        i32 x;
        st = bm_row(s, row);
        if (st == WP_OK) for (x = 0; x < s->w; x++) f->count[row[x]]++;
    }
    if (st != WP_OK) goto done;
    choose_palette(s, f, max_colors);

    /* Second pass: each output row the darkest of the source block it
       stands for, column by column. */
    st = sink->begin(sink->user, ow, oh, path, (u32)sizeof(path), &path_len);
    if (st != WP_OK || path_len == 0 || path_len >= sizeof(path)) { st = WP_ERR; goto done; }
    bm_rewind(s);
    oy = 0;
    memset(acc, 0xFF, (u32)ow);
    for (y = 0; y < s->h && st == WP_OK; y++) {
        i32 x, last = (i32)((((long)oy + 1) * s->h + oh - 1) / oh) - 1;   /* the block's last source row */
        st = bm_row(s, row);
        if (st != WP_OK) break;
        for (x = 0; x < s->w; x++) {
            i32 ox = (i32)(((long)x * ow) / s->w);
            u8 c = f->map[row[x]];
            if (acc[ox] == 0xFF || f->out_luma[c] < f->out_luma[acc[ox]]) acc[ox] = c;
        }
        if (y >= last || y == s->h - 1) {
            i32 k;
            memset(packed, 0, packed_len);
            for (k = 0; k < ow; k++) {
                u8 c = acc[k] == 0xFF ? 0 : acc[k];
                packed[k >> 1] |= (u8)((k & 1) ? c : (u8)(c << 4));
            }
            st = sink->write(sink->user, packed, packed_len);
            memset(acc, 0xFF, (u32)ow);
            oy++;
            if (oy >= oh) break;
        }
    }
    if (sink->end != NULL) sink->end(sink->user);
    if (st != WP_OK) goto done;

    memset(desc, 0, sizeof(*desc));
    desc->width_px = ow;
    desc->height_px = oh;
    desc->palette_count = (u8)f->out_n;
    memcpy(desc->palette, f->out_pal, f->out_n * 3u);
    if (wp_strpool_append(&doc->strings, (const u8 *)path, path_len, &desc->blob_path) != WP_OK) st = WP_NOMEM;

done:
    if (row != NULL) mem_free(row);
    if (acc != NULL) mem_free(acc);
    if (packed != NULL) mem_free(packed);
    bm_close(s);
    mem_free(s);
    mem_free(f);
    return st;
}
