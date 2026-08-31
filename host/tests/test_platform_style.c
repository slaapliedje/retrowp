#include "test.h"
#include "platform.h"
#include "docmodel.h"

/* platform.h's PLAT_STYLE_* and docmodel.h's wp_style_flags deliberately
   restate the same bit values in two places, so that platform/<target>/
   need not include an application header to interpret plat_font_open()'s
   style_flags argument (docs/platform-extraction.md's one real leak).
 *
 * Duplicated constants drift. These tests are what stops that being
 * discovered as "bold renders italic on the Atari" -- the engine passes
 * wp_style_flags straight into a seam that reads them as PLAT_STYLE_*, so
 * a divergence is silent everywhere except on real hardware. */

void test_platform_style_bits_match_document_model(void)
{
    ASSERT_EQ_U32(PLAT_STYLE_BOLD,      (unsigned)WP_STYLE_BOLD);
    ASSERT_EQ_U32(PLAT_STYLE_ITALIC,    (unsigned)WP_STYLE_ITALIC);
    ASSERT_EQ_U32(PLAT_STYLE_UNDERLINE, (unsigned)WP_STYLE_UNDERLINE);
    ASSERT_EQ_U32(PLAT_STYLE_STRIKE,    (unsigned)WP_STYLE_STRIKE);
}

/* The seam's four bits must not collide with the two document-only ones.
   If SUPER or SUB ever landed on a PLAT_ bit, every backend would silently
   start bolding superscripts. */
void test_platform_style_bits_do_not_collide_with_document_only_bits(void)
{
    unsigned seam = PLAT_STYLE_BOLD | PLAT_STYLE_ITALIC |
                    PLAT_STYLE_UNDERLINE | PLAT_STYLE_STRIKE;
    unsigned doc_only = (unsigned)WP_STYLE_SUPER | (unsigned)WP_STYLE_SUB;

    ASSERT_EQ_U32(seam & doc_only, 0u);

    /* All four distinct, i.e. nobody defined two of them as the same bit. */
    ASSERT_EQ_U32(PLAT_STYLE_BOLD | PLAT_STYLE_ITALIC |
                  PLAT_STYLE_UNDERLINE | PLAT_STYLE_STRIKE, 0x0Fu);
}
