/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include "app_shell.h"

/* RetroWP's own stack. A program RUN from a script gets 4096 bytes,
   and this toolchain ignores the __stack cookie (measured on target,
   2026-08-09): MUI's draw callback has used 1.3 KB of it before the
   word processor's code begins, and page view's walk -- page, line,
   word, then diskfont opening a face -- ran off the end of the rest.
   exec's StackSwap (V37; every Kickstart RetroWP supports is 2.04 or
   later) moves the task onto a block of its own for the whole run.
   Everything main touches after the swap is static: a local addressed
   from the stack pointer would be looked for on the new stack. */
#define RWP_STACK_BYTES 16384UL

static struct StackSwapStruct g_swap;
static APTR g_stack;

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);

    app_shell_set_start_file(argc > 1 ? argv[1] : (const char *)0);
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower < RWP_STACK_BYTES)
        g_stack = AllocVec(RWP_STACK_BYTES, MEMF_ANY);
    if (g_stack == NULL) {
        app_shell_run();   /* big enough already, or no memory: as it is */
        return 0;
    }
    g_swap.stk_Lower = g_stack;
    g_swap.stk_Upper = (ULONG)g_stack + RWP_STACK_BYTES;
    g_swap.stk_Pointer = (APTR)g_swap.stk_Upper;
    StackSwap(&g_swap);
    app_shell_run();
    StackSwap(&g_swap);
    FreeVec(g_stack);
    return 0;
}
