/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fuzz_formats -- every reader of bytes RetroWP takes from a file, fed
   mutated inputs (T20.8). `make fuzz` builds it under AddressSanitizer
   and UndefinedBehaviorSanitizer and runs it; any memory error,
   undefined operation, or a reader taking longer than FUZZ_SLOW_S on one
   input ends the run with the input saved to build/fuzz/crash-*.bin.

   Deterministic: the seed (FUZZ_SEED, default 1) fixes every input, so a
   run is repeatable, and `fuzz_formats replay <target> <file>` feeds one
   saved input back. Not libFuzzer: this builds with the same gcc as
   everything else, needs no coverage runtime, and a fixed seed belongs
   in a make target that is meant to give the same answer twice.

   Seeds: files in the repository (fixtures, clipart, docs, the help
   source) and small documents built in code -- for the formats the
   repository has no sample of, the same bytes the unit tests build
   (test_stw_seed, test_gem_seed). Atari Works specimens stay out of the
   repository, so STW starts from the unit test's document only.

   After a document reader succeeds, the document is written back out as
   text, RTF and Markdown: a strange document is the exporters' input
   too. */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include "platform.h"
#include "docmodel.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "format_md.h"
#include "format_stw.h"
#include "format_ws.h"
#include "format_wmf.h"
#include "bitmapfile.h"
#include "drawfile.h"
#include "drawing.h"
#include "merge.h"
#include "config.h"
#include "help_doc.h"

u32 test_stw_seed(u8 *out, u32 cap);   /* host/tests/test_format_stw.c */
u32 test_gem_seed(u8 *out);            /* host/tests/test_drawing.c */

#define FUZZ_MAX_INPUT  65536u
#define FUZZ_MAX_SEEDS  128u
#define FUZZ_SLOW_S     5u

/* ---- deterministic randomness (xorshift32) ------------------------ */

static u32 g_rng = 1;

static u32 rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static u32 rnd_below(u32 n) { return n == 0 ? 0 : rnd() % n; }

/* ---- seeds ---------------------------------------------------------- */

typedef struct {
    u8 *bytes;
    u32 len;
} seed;

typedef struct {
    seed s[FUZZ_MAX_SEEDS];
    u32 n;
} seed_set;

static void seed_add(seed_set *set, const u8 *bytes, u32 len)
{
    if (set->n >= FUZZ_MAX_SEEDS || len == 0) return;
    if (len > FUZZ_MAX_INPUT) len = FUZZ_MAX_INPUT;
    set->s[set->n].bytes = (u8 *)malloc(len);
    if (set->s[set->n].bytes == NULL) return;
    memcpy(set->s[set->n].bytes, bytes, len);
    set->s[set->n].len = len;
    set->n++;
}

static void seed_add_file(seed_set *set, const char *path)
{
    FILE *f = fopen(path, "rb");
    static u8 buf[FUZZ_MAX_INPUT];
    size_t n;

    if (f == NULL) return;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    seed_add(set, buf, (u32)n);
}

/* Every file in `dir` whose name ends in `ext` (case ignored). */
static void seed_add_dir(seed_set *set, const char *dir, const char *ext)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    size_t el = strlen(ext);

    if (d == NULL) return;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        char path[512];
        size_t i;
        int match = nl > el;

        for (i = 0; match && i < el; i++) {
            char a = e->d_name[nl - el + i], b = ext[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) match = 0;
        }
        if (!match) continue;
        if (strlen(dir) + nl + 2 > sizeof(path)) continue;
        sprintf(path, "%s/%s", dir, e->d_name);
        seed_add_file(set, path);
    }
    closedir(d);
}

/* ---- mutation ------------------------------------------------------- */

static u8  g_in[FUZZ_MAX_INPUT];
static u32 g_in_len;
static const char *g_target = "";
static u32 g_iter;

static void put16(u8 *p, u32 v, int le)
{
    if (le) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
    else    { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
}

static void put32(u8 *p, u32 v, int le)
{
    if (le) { put16(p, v & 0xFFFFu, 1); put16(p + 2, v >> 16, 1); }
    else    { put16(p, v >> 16, 0); put16(p + 2, v & 0xFFFFu, 0); }
}

static const u32 k_interesting[] = {
    0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
    0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 16, 32, 64, 4096, 65535, 65536
};

static void mutate(const seed_set *set)
{
    const seed *base = &set->s[rnd_below(set->n)];
    u32 ops = 1 + rnd_below(8), k;

    memcpy(g_in, base->bytes, base->len);
    g_in_len = base->len;

    for (k = 0; k < ops; k++) {
        u32 at = rnd_below(g_in_len ? g_in_len : 1);
        u32 iv = k_interesting[rnd_below((u32)(sizeof(k_interesting) / sizeof(k_interesting[0])))];

        switch (rnd_below(10)) {
        case 0:                                     /* flip a bit */
            if (g_in_len) g_in[at] ^= (u8)(1u << rnd_below(8));
            break;
        case 1:                                     /* a byte to anything */
            if (g_in_len) g_in[at] = (u8)rnd();
            break;
        case 2:                                     /* a byte to a boundary */
            if (g_in_len) g_in[at] = (u8)iv;
            break;
        case 3:                                     /* 16 bits to a boundary */
            if (at + 2 <= g_in_len) put16(g_in + at, iv, (int)(rnd() & 1));
            break;
        case 4:                                     /* 32 bits to a boundary */
            if (at + 4 <= g_in_len) put32(g_in + at, iv, (int)(rnd() & 1));
            break;
        case 5: {                                   /* cut a span out */
            u32 n = rnd_below(g_in_len - at + 1);
            memmove(g_in + at, g_in + at + n, g_in_len - at - n);
            g_in_len -= n;
            break;
        }
        case 6: {                                   /* repeat a span */
            u32 n = rnd_below(g_in_len - at + 1);
            if (n > 0 && g_in_len + n <= FUZZ_MAX_INPUT) {
                memmove(g_in + at + n, g_in + at, g_in_len - at);
                g_in_len += n;
            }
            break;
        }
        case 7: {                                   /* insert random bytes */
            u32 n = 1 + rnd_below(16), i;
            if (g_in_len + n <= FUZZ_MAX_INPUT) {
                memmove(g_in + at + n, g_in + at, g_in_len - at);
                for (i = 0; i < n; i++) g_in[at + i] = (u8)rnd();
                g_in_len += n;
            }
            break;
        }
        case 8:                                     /* cut short */
            g_in_len = at;
            break;
        default: {                                  /* splice another seed in */
            const seed *o = &set->s[rnd_below(set->n)];
            u32 from = rnd_below(o->len), n = rnd_below(o->len - from + 1);
            if (at + n > FUZZ_MAX_INPUT) n = FUZZ_MAX_INPUT - at;
            memcpy(g_in + at, o->bytes + from, n);
            if (at + n > g_in_len) g_in_len = at + n;
            break;
        }
        }
    }
}

/* ---- the readers ------------------------------------------------------ */

static wp_status sink_begin(void *u, i32 w, i32 h, char *path, u32 cap, u32 *n)
{
    (void)u; (void)w; (void)h;
    if (cap < 2) return WP_ERR;
    path[0] = 'x';
    *n = 1;
    return WP_OK;
}
static wp_status sink_write(void *u, const u8 *b, u32 n) { (void)u; (void)b; (void)n; return WP_OK; }
static wp_status sink_end(void *u) { (void)u; return WP_OK; }
static const wp_image_sink g_sink = { sink_begin, sink_write, sink_end, NULL };

/* A document read: written back out three ways, then freed. */
static void exercise(wp_document *doc)
{
    static u8 out[262144];
    u32 n;

    (void)format_txt_export(doc, out, sizeof(out), &n);
    (void)format_rtf_export(doc, out, sizeof(out), &n);
    (void)format_md_export(doc, out, sizeof(out), &n);
    wp_document_free(doc);
}

static void read_doc(wp_status (*fn)(const u8 *, u32, wp_document *))
{
    wp_document doc;
    if (fn(g_in, g_in_len, &doc) == WP_OK) exercise(&doc);
}

static void t_txt(void) { read_doc(format_txt_import); }
static void t_md(void)  { read_doc(format_md_import); }
static void t_stw(void) { (void)format_stw_looks_like(g_in, g_in_len); read_doc(format_stw_import); }
static void t_ws(void)  { (void)format_ws_looks_like(g_in, g_in_len); read_doc(format_ws_import); }

static void t_rtf(void)
{
    wp_document doc;
    if (format_rtf_import_images(g_in, g_in_len, &doc, &g_sink) == WP_OK) exercise(&doc);
}

static void t_bitmap(void)
{
    static const char *const names[] = { NULL, "A.IMG", "A.PCX", "A.BMP", "A.MAC", "A.IFF" };
    const char *name = names[g_iter % (sizeof(names) / sizeof(names[0]))];
    wp_document doc;
    wp_image_desc desc;

    (void)bitmapfile_detect(g_in, g_in_len, name);
    if (wp_document_init(&doc) != WP_OK) return;
    memset(&desc, 0, sizeof(desc));
    (void)bitmapfile_read(g_in, g_in_len, name, 2 + (g_iter % 15), 320, 200, &g_sink, &doc, &desc);
    wp_document_free(&doc);
}

static void t_gem(void)
{
    wp_drawing_builder b;
    wp_drawfile_info info;

    (void)drawfile_detect(g_in, g_in_len);
    wp_drawing_builder_init(&b);
    (void)drawfile_read(g_in, g_in_len, &b, &info);
    wp_drawing_builder_free(&b);
}

static void t_wmf(void)
{
    wp_drawing_builder b;
    i32 w = 0, h = 0;

    wp_drawing_builder_init(&b);
    (void)wmf_to_drawing(g_in, g_in_len, &b, &w, &h);
    wp_drawing_builder_free(&b);
}

static void t_merge(void)
{
    wp_merge_data d;
    if (wp_merge_data_read(g_in, g_in_len, &d) == WP_OK) wp_merge_data_free(&d);
}

static void t_config(void)
{
    wp_config c;
    (void)wp_config_parse(g_in, g_in_len, &c);
}

static void t_help(void)
{
    wp_help_doc h;
    if (wp_help_doc_parse(g_in, g_in_len, &h) == WP_OK) wp_help_doc_free(&h);
}

typedef struct {
    const char *name;
    void (*run)(void);
    seed_set seeds;
} target;

static target g_targets[] = {
    { "txt",    t_txt,    { { { 0, 0 } }, 0 } },
    { "md",     t_md,     { { { 0, 0 } }, 0 } },
    { "rtf",    t_rtf,    { { { 0, 0 } }, 0 } },
    { "stw",    t_stw,    { { { 0, 0 } }, 0 } },
    { "ws",     t_ws,     { { { 0, 0 } }, 0 } },
    { "bitmap", t_bitmap, { { { 0, 0 } }, 0 } },
    { "gem",    t_gem,    { { { 0, 0 } }, 0 } },
    { "wmf",    t_wmf,    { { { 0, 0 } }, 0 } },
    { "merge",  t_merge,  { { { 0, 0 } }, 0 } },
    { "config", t_config, { { { 0, 0 } }, 0 } },
    { "help",   t_help,   { { { 0, 0 } }, 0 } }
};
#define TARGET_COUNT ((u32)(sizeof(g_targets) / sizeof(g_targets[0])))

static target *find_target(const char *name)
{
    u32 i;
    for (i = 0; i < TARGET_COUNT; i++)
        if (strcmp(g_targets[i].name, name) == 0) return &g_targets[i];
    return NULL;
}

static void load_seeds(void)
{
    static u8 buf[8192];
    seed_set *s;
    u32 n;

    s = &find_target("txt")->seeds;
    seed_add_dir(s, "host/tests/fixtures", ".txt");

    s = &find_target("md")->seeds;
    seed_add_dir(s, "docs", ".md");

    s = &find_target("rtf")->seeds;
    seed_add_dir(s, "host/tests/fixtures", ".rtf");
    {
        /* A picture in RTF, so \pict reaches the sink: 1 x 1, 1 bit. */
        static const char pict[] =
            "{\\rtf1\\ansi{\\fonttbl{\\f0 Times;}}\\f0\\fs24 Hi {\\b there}\\par"
            "{\\pict\\dibitmap0\\picw1\\pich1\\picwgoal20\\pichgoal20 "
            "28000000010000000100000001000100000000000400000000000000000000000200000000000000"
            "00000000ffffff0080000000}\\par}";
        seed_add(s, (const u8 *)pict, (u32)strlen(pict));
    }

    s = &find_target("stw")->seeds;
    n = test_stw_seed(buf, sizeof(buf));
    seed_add(s, buf, n);

    s = &find_target("ws")->seeds;
    {
        static const u8 ws[] = {
            'T','h','e',' ','q','u','i','c','k', 0x8D, 0x0A,
            'b','r','o','w','n',' ','f','o','x', 0x0D, 0x0A,
            0x02, 'N','e','x','t', 0x02, ' ', 0x13, 'o','n','e', 0x13, 0x0D, 0x0A,
            '.', 'p', 'a', ' ', 0x0D, 0x0A, 0x1A
        };
        seed_add(s, ws, (u32)sizeof(ws));
    }

    s = &find_target("bitmap")->seeds;
    {
        static const u8 img[] = {
            0, 1, 0, 8, 0, 1, 0, 2, 0, 85, 0, 85, 0, 16, 0, 4,
            0x82, 0x80, 2, 0xF0, 0x0F, 0x00, 0x00, 0xFF, 0x02, 0x00, 0x01, 0xAA, 0x55
        };
        static const u8 ilbm[] = {
            'F','O','R','M', 0,0,0,70, 'I','L','B','M',
            'B','M','H','D', 0,0,0,20, 0,4, 0,2, 0,0, 0,0, 2, 0, 1, 0, 0,0, 10, 11, 0,4, 0,2,
            'C','M','A','P', 0,0,0,12, 255,255,255, 255,0,0, 0,255,0, 0,0,255,
            'B','O','D','Y', 0,0,0,10, 0x01,0x50,0x00, 0x01,0x30,0x00, 0xFF,0xF0, 0xFF,0xF0
        };
        static u8 pcx[128 + 6 + 769], bmp[54 + 16], mac[128 + 512 + 720 * 2];
        u32 i;

        seed_add(s, img, (u32)sizeof(img));
        seed_add(s, ilbm, (u32)sizeof(ilbm));
        pcx[0] = 0x0A; pcx[1] = 5; pcx[2] = 1; pcx[3] = 8; pcx[8] = 3; pcx[10] = 1;
        pcx[65] = 1; pcx[66] = 4; pcx[128] = 0xC4; pcx[129] = 5;
        pcx[130] = 1; pcx[131] = 2; pcx[132] = 3; pcx[133] = 4; pcx[134] = 0x0C;
        seed_add(s, pcx, (u32)sizeof(pcx));
        bmp[0] = 'B'; bmp[1] = 'M'; bmp[10] = 54; bmp[14] = 40;
        bmp[18] = 2; bmp[22] = 2; bmp[26] = 1; bmp[28] = 24;
        seed_add(s, bmp, (u32)sizeof(bmp));
        mac[1] = 4; memcpy(mac + 2, "TEST", 4); memcpy(mac + 65, "PNTG", 4);
        for (i = 0; i < 720; i++) { mac[640 + i * 2] = 0xB9; mac[640 + i * 2 + 1] = (u8)(i == 0 ? 0xFF : 0); }
        seed_add(s, mac, (u32)sizeof(mac));
    }

    s = &find_target("gem")->seeds;
    n = test_gem_seed(buf);
    seed_add(s, buf, n);

    s = &find_target("wmf")->seeds;
    seed_add_dir(s, "clipart", ".wmf");

    s = &find_target("merge")->seeds;
    {
        static const char csv[] = "Name,Town,Note\r\nAnn,\"Bath, Somerset\",\"says \"\"hi\"\"\nthen\"\r\n\r\nBob,Leeds\r\n";
        static const char tsv[] = "Name\tPrice, each\nWidget\t2,50\n";
        seed_add(s, (const u8 *)csv, (u32)strlen(csv));
        seed_add(s, (const u8 *)tsv, (u32)strlen(tsv));
    }

    s = &find_target("config")->seeds;
    {
        static const char cfg[] = "# a comment\n\n; another\nkeymap=vim\noffload=serial:aux:9600\n";
        seed_add(s, (const u8 *)cfg, (u32)strlen(cfg));
    }

    s = &find_target("help")->seeds;
    seed_add_file(s, "docs/help/retrowp.help");
    {
        static const char help[] = "@node Main \"Main\"\nSee also:\n{Editing Basics|Editing}\n@endnode\n"
                                   "@node Editing \"Editing Basics\"\nSome text.\n@endnode\n";
        seed_add(s, (const u8 *)help, (u32)strlen(help));
    }
}

/* ---- when something goes wrong --------------------------------------- */

static void save_input(const char *why)
{
    static int saved = 0;
    char path[256];
    FILE *f;

    if (saved) return;        /* the report and its abort: one save */
    saved = 1;

    sprintf(path, "build/fuzz/crash-%s-%lu.bin", g_target, (unsigned long)g_iter);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(g_in, 1, g_in_len, f);
        fclose(f);
    }
    fprintf(stderr, "fuzz: %s in %s at iteration %lu, %lu bytes -- saved %s\n"
                    "fuzz: replay with: bin/fuzz/fuzz_formats replay %s %s\n",
            why, g_target, (unsigned long)g_iter, (unsigned long)g_in_len, path, g_target, path);
}

/* ASan's report calls the death callback; UBSan's abort, and a plain
   crash, arrive as signals -- `make fuzz` sets abort_on_error for both. */
void __sanitizer_set_death_callback(void (*cb)(void));
static void on_death(void) { save_input("sanitizer report"); }

static void on_fault(int sig)
{
    save_input(sig == SIGABRT ? "an abort (a sanitizer report above)" : "a crash");
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_alarm(int sig)
{
    (void)sig;
    save_input("a reader still running after " "5" " s");
    _exit(2);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    u32 iters = 20000, i, t;
    const char *only = getenv("FUZZ_TARGET");
    const char *e;

    if ((e = getenv("FUZZ_ITERS")) != NULL) iters = (u32)strtoul(e, NULL, 10);
    if ((e = getenv("FUZZ_SEED")) != NULL) g_rng = (u32)strtoul(e, NULL, 10);
    if (g_rng == 0) g_rng = 1;
    __sanitizer_set_death_callback(on_death);
    signal(SIGALRM, on_alarm);
    signal(SIGABRT, on_fault);
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    signal(SIGFPE, on_fault);

    if (argc == 4 && strcmp(argv[1], "replay") == 0) {
        target *tg = find_target(argv[2]);
        FILE *f = fopen(argv[3], "rb");
        if (tg == NULL || f == NULL) { fprintf(stderr, "replay: no such target or file\n"); return 2; }
        g_in_len = (u32)fread(g_in, 1, sizeof(g_in), f);
        fclose(f);
        g_target = tg->name;
        /* every variant a run gives one input (the bitmap reader's name
           and colour count follow the iteration) */
        for (g_iter = 0; g_iter < 30; g_iter++) tg->run();
        printf("replay: %s, %lu bytes: no fault\n", tg->name, (unsigned long)g_in_len);
        return 0;
    }

    load_seeds();
    for (t = 0; t < TARGET_COUNT; t++) {
        target *tg = &g_targets[t];
        u32 ok = 0;

        if (only != NULL && strcmp(only, tg->name) != 0) continue;
        if (tg->seeds.n == 0) {
            fprintf(stderr, "fuzz: %s has no seeds -- run from the repository root\n", tg->name);
            return 2;
        }
        g_target = tg->name;
        /* The seeds themselves first, unmutated: they must read. */
        for (i = 0; i < tg->seeds.n; i++) {
            memcpy(g_in, tg->seeds.s[i].bytes, tg->seeds.s[i].len);
            g_in_len = tg->seeds.s[i].len;
            g_iter = i;
            alarm(FUZZ_SLOW_S);
            tg->run();
            alarm(0);
        }
        for (i = 0; i < iters; i++) {
            g_iter = i;
            mutate(&tg->seeds);
            alarm(FUZZ_SLOW_S);
            tg->run();
            alarm(0);
            ok++;
        }
        printf("fuzz: %-6s %lu seeds, %lu inputs, no fault\n",
               tg->name, (unsigned long)tg->seeds.n, (unsigned long)ok);
        fflush(stdout);
    }
    return 0;
}
