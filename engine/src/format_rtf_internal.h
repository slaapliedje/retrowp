/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_RTF_INTERNAL_H
#define WP_FORMAT_RTF_INTERNAL_H

#include "types.h"

/* Private to engine/src -- shared between format_rtf_import.c and
   format_rtf_export.c, not installed under engine/include. Defined
   `static` (growth_internal.h's pattern): each including translation
   unit gets its own private copy, no separate .c file needed. */

#define RTF_MAX_COLORS 32

/* Code pages, cp1252 included, are engine/include/codepage.h now. */

#endif /* WP_FORMAT_RTF_INTERNAL_H */
