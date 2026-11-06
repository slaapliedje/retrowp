#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# A stand-in for CUPS's lp, for test_convert.c's OFFLOAD_PRINT tests:
# keeps its arguments and the PDF it is piped, and answers as lp does.
printf '%s\n' "$@" > build/test_print/lp.args
cat > build/test_print/lp.pdf
echo "request id is Office-7 (1 file(s))"
