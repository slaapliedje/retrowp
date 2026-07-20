#ifndef WP_KEYEVENT_H
#define WP_KEYEVENT_H

#include "types.h"

/* Portable key-event representation (M8.5). Pure value type, zero
   platform calls, zero behavior -- same category as wp_pos/wp_span
   already living in docmodel.h. Each platform backend translates its
   own native input (Atari AES evnt_multi's packed key field, Amiga
   IDCMP_RAWKEY, Mac KeyDown, DOS INT 16h, a host test harness's
   synthetic script) into this before any engine-adjacent code
   (wp_vimode_feed_key and friends) ever sees it. See docs/vimode.md. */

typedef enum {
    WP_KEY_NONE = 0,   /* not a special key -- consult codepoint instead */
    WP_KEY_ESCAPE,
    WP_KEY_ENTER,
    WP_KEY_TAB,
    WP_KEY_BACKSPACE,
    WP_KEY_DELETE,
    WP_KEY_UP,
    WP_KEY_DOWN,
    WP_KEY_LEFT,
    WP_KEY_RIGHT,
    WP_KEY_HOME,
    WP_KEY_END,
    WP_KEY_PAGE_UP,
    WP_KEY_PAGE_DOWN,
    WP_KEY_F1,
    WP_KEY_F2,
    WP_KEY_F3,
    WP_KEY_F4,
    WP_KEY_F5,
    WP_KEY_F6,
    WP_KEY_F7,
    WP_KEY_F8,
    WP_KEY_F9,
    WP_KEY_F10
    /* F11/F12 deliberately omitted: real ST/Amiga/classic-Mac keyboards
       commonly stop at F10 -- add later if a target genuinely needs
       more. */
} wp_special_key;

typedef enum {
    WP_MOD_SHIFT = 1 << 0,
    WP_MOD_CTRL  = 1 << 1,
    WP_MOD_ALT   = 1 << 2
} wp_key_mod;

typedef struct {
    u32 codepoint; /* decoded Unicode scalar for printable input; 0 when special != WP_KEY_NONE */
    u16 special;   /* wp_special_key */
    u8  mods;      /* wp_key_mod bitfield */
    u8  _pad;
} wp_key_event;

#endif /* WP_KEYEVENT_H */
