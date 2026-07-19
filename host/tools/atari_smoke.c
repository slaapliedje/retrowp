/* Hatari-based CI smoke test for platform/atari's T4.1 AES shell.
   Cross-compiled with m68k-atari-mint-gcc into its own .prg (SMOKE.PRG)
   and autostarted under Hatari (docs/atari-platform.md documents the
   exact recipe). Uses the NF_STDERR/NF_EXIT Native Features trap (Hatari
   and ARAnyM only -- illegal on real hardware, test-only code) to print
   a log to the host's stderr and make the Hatari *process* exit with a
   pass/fail status code, so host/tools/ci_atari.sh can assert on $?
   exactly like host/tools/ci.sh does for the host test suite.

   Real mouse-driven verification isn't available in this environment
   (see app_shell_internal.h) -- this proves menu_bar()/wind_create()/
   wind_open() all succeed for real, and separately proves
   wp_dispatch_message()'s logic with synthesized AES messages. */

#include <gem.h>
#include "types.h"
#include "app_shell_internal.h"

/* GCC can prove the string-pointer argument is unused by the asm's
   output and eliminate the caller's push of it entirely (confirmed via
   disassembly during development) -- __attribute__((noipa)) plus
   listing the argument as a real input operand is required, not
   optional, for this trap to work reliably at -O2. */
__attribute__((noipa))
static long nf_get_id(const char *name)
{
    long id;
    __asm__ volatile(".word 0x7300\n\t" : "=d"(id) : "g"(name) : "cc", "memory");
    return id;
}

__attribute__((noipa))
static long nf_call1(long id, long arg1)
{
    long ret;
    __asm__ volatile(".word 0x7301\n\t" : "=d"(ret) : "g"(id), "g"(arg1) : "cc", "memory");
    return ret;
}

static long g_nf_stderr_id;
static long g_nf_exit_id;

static void nf_log(const char *msg)
{
    if (g_nf_stderr_id) nf_call1(g_nf_stderr_id, (long)msg);
}

static void nf_exit_now(long code)
{
    if (g_nf_exit_id) nf_call1(g_nf_exit_id, code);
    /* Unreachable if NF_EXIT is present (Hatari/ARAnyM); falls through
       to a normal return only if run without Native Features support,
       which this smoke test is not designed to do. */
}

/* Formats a non-negative long as decimal into buf (caller-sized, no
   stdio pulled in just for this) and logs it via NF_STDERR, prefixed by
   label. Used to report the T4.4 memory-budget probe's actual numbers,
   not just its pass/fail verdict -- so a human reading CI output can
   see real headroom, not just trust a threshold. */
static void nf_log_long(const char *label, long v)
{
    char buf[16];
    int i = 15;

    buf[i--] = '\0';
    if (v == 0) {
        buf[i--] = '0';
    } else {
        wp_bool neg = (v < 0);
        unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
        while (uv > 0 && i >= 0) {
            buf[i--] = (char)('0' + (uv % 10));
            uv /= 10;
        }
        if (neg && i >= 0) buf[i--] = '-';
    }
    nf_log(label);
    nf_log(&buf[i + 1]);
    nf_log("\r\n");
}

static int g_pass = 0;
static int g_fail = 0;

static void check(wp_bool cond, const char *what)
{
    if (cond) {
        g_pass++;
        nf_log("PASS: ");
    } else {
        g_fail++;
        nf_log("FAIL: ");
    }
    nf_log(what);
    nf_log("\r\n");
}

int main(void)
{
    short work_in[11];
    short work_out[57];
    short dummy;
    VdiHdl vdi_handle;
    short window_handle;
    GRECT desk;
    short r;
    short msg[8];

    g_nf_stderr_id = nf_get_id("NF_STDERR");
    g_nf_exit_id = nf_get_id("NF_EXIT");

    nf_log("ATARI SMOKE: starting\r\n");

    wp_menu_tree_init();
    check(wp_menu_tree_selfcheck(), "menu tree ob_next/head/tail linkage");

    check(appl_init() >= 0, "appl_init");

    vdi_handle = graf_handle(&dummy, &dummy, &dummy, &dummy);
    for (dummy = 0; dummy < 10; dummy++) work_in[dummy] = 1;
    work_in[10] = 2;
    v_opnvwk(work_in, &vdi_handle, work_out);
    check(vdi_handle != 0, "v_opnvwk assigned a vdi handle");

    r = menu_bar(wp_menu_tree, MENU_INSTALL);
    check(r != 0, "menu_bar(MENU_INSTALL) succeeded");

    wind_get_grect(0, WF_WORKXYWH, &desk);
    window_handle = wind_create_grect(NAME | MOVER | CLOSER | SIZER, &desk);
    check(window_handle >= 0, "wind_create_grect");

    r = wind_open_grect(window_handle, &desk);
    check(r != 0, "wind_open_grect");

    check(wp_atari_layout_selfcheck(),
          "VDI-fed relayout wraps to >1 line, no line exceeds its column");

    check(wp_atari_file_clipboard_selfcheck(),
          "plat_file_* and plat_clipboard_* round-trip a fixture byte-identical");

    check(wp_atari_rtf_spell_selfcheck(),
          "RTF file round-trip + spell dictionary lookups over real Atari file I/O");

    check(wp_atari_thesaurus_selfcheck(),
          "word-before-cursor extraction for the Thesaurus menu action");

    check(wp_atari_grammar_check_selfcheck(),
          "paragraph extraction, run-offset mapping, and issue decode for Grammar Check");

    check(wp_atari_chat_selfcheck(),
          "capability-bit check for the Chat menu action");

    {
        long free_before = 0, free_after_load = 0, free_after_free = 0;
        wp_bool budget_ok = wp_atari_memory_budget_selfcheck(&free_before, &free_after_load, &free_after_free);

        nf_log_long("  Malloc(-1) before load:      ", free_before);
        nf_log_long("  Malloc(-1) after load+edit:   ", free_after_load);
        nf_log_long("  Malloc(-1) after free:        ", free_after_free);
        check(budget_ok,
              "multi-page doc (paginate/edit/save) stays within 1MB budget, no gross leak");
    }

    /* Real mouse injection isn't available here -- exercise dispatch
       logic directly with synthesized AES messages instead. */
    msg[0] = WM_REDRAW; msg[1] = 0; msg[2] = 0; msg[3] = window_handle;
    msg[4] = desk.g_x; msg[5] = desk.g_y; msg[6] = desk.g_w; msg[7] = desk.g_h;
    check(wp_dispatch_message(window_handle, vdi_handle, msg) == WP_FALSE,
          "dispatch(WM_REDRAW) does not request quit");

    msg[0] = MN_SELECTED; msg[3] = WP_MENU_OBJ_FILE_TITLE; msg[4] = WP_MENU_OBJ_FILE_QUIT;
    check(wp_dispatch_message(window_handle, vdi_handle, msg) == WP_TRUE,
          "dispatch(MN_SELECTED Quit) requests quit");

    msg[0] = WM_CLOSED;
    check(wp_dispatch_message(window_handle, vdi_handle, msg) == WP_TRUE,
          "dispatch(WM_CLOSED) requests quit");

    menu_bar(wp_menu_tree, MENU_REMOVE);
    wind_close(window_handle);
    wind_delete(window_handle);
    v_clsvwk(vdi_handle);
    appl_exit();

    nf_log("ATARI SMOKE: done\r\n");
    if (g_fail == 0) {
        nf_log("ATARI SMOKE: ALL PASSED\r\n");
        nf_exit_now(0);
    } else {
        nf_log("ATARI SMOKE: FAILURES PRESENT\r\n");
        nf_exit_now(1);
    }

    return 0;
}
