#include <stddef.h>
#include "platform.h"

/* T9.2 stub, not a real implementation: engine/src/linebreak.c's
   wp_paragraph_relayout calls plat_font_open/plat_measure_text (and
   wp_document_paginate calls relayout in turn), so these must EXIST for
   linebreak.o to link at all -- but T9.1b never calls relayout or
   paginate anywhere (app_shell.c's editor_draw deliberately bypasses
   layout entirely, per its own comment), so this code is genuinely
   unreachable today, not silently wrong. wp_paragraph_layout_free
   (also in linebreak.c, needed by wp_document_free/edit.c) stays
   correct regardless, since it never touches these -- every real
   wp_paragraph.layout in this app is NULL. Real graphics.library/
   diskfont metrics land in T9.2, per the M9 plan's architecture table. */

wp_status plat_font_open(const u8 *utf8_font_name, u32 name_len,
                          i32 size_twips, u16 style_flags, plat_font **out)
{
    WP_UNUSED(utf8_font_name);
    WP_UNUSED(name_len);
    WP_UNUSED(size_twips);
    WP_UNUSED(style_flags);
    WP_UNUSED(out);
    return WP_UNSUPPORTED;
}

void plat_font_close(plat_font *f)
{
    WP_UNUSED(f);
}

wp_status plat_font_metrics_get(plat_font *f, plat_font_metrics *out)
{
    WP_UNUSED(f);
    WP_UNUSED(out);
    return WP_UNSUPPORTED;
}

wp_status plat_measure_text(plat_font *f, const u8 *utf8, u32 len,
                             i32 *width_twips_out)
{
    WP_UNUSED(f);
    WP_UNUSED(utf8);
    WP_UNUSED(len);
    WP_UNUSED(width_twips_out);
    return WP_UNSUPPORTED;
}
