#ifndef WP_SPELL_H
#define WP_SPELL_H

#include "types.h"

/* Compact on-disk-dictionary spell checker. Pure C, data-driven, no UI,
   no platform-specific calls beyond platform.h's file and memory seam
   (plat_file_* / mem_alloc). Full design (on-disk format, binary search
   over a line-oriented
   file, the affix rule table, suggestion generation, and the ASCII-only
   scope) is in docs/services/spell.md -- read it before changing this
   file. */

typedef struct spell_dict spell_dict;

#define SPELL_MAX_WORD_LEN 32

/* Opens a dictionary file (one sorted lowercase a-z word per line, LF-
   terminated -- docs/services/spell.md). The file is never read into
   memory as a whole; only a handle and its size are kept. */
wp_status spell_dict_open(const char *native_path, spell_dict **out);
void      spell_dict_close(spell_dict *dict);

/* Checks utf8_word (len bytes) against the dictionary, trying an exact
   match first and then each affix rule in turn. *out_known is set
   WP_TRUE for a match (exact or via a stripped affix), WP_FALSE for a
   genuine miss. Words containing any byte >= 0x80, or longer than
   SPELL_MAX_WORD_LEN-1, are outside this service's ASCII-only scope and
   are reported as WP_TRUE (assumed correct) -- failing open rather than
   flagging every accented/non-English word as a misspelling. */
wp_status spell_check_word(spell_dict *dict, const u8 *utf8_word, u32 len,
                            wp_bool *out_known);

/* Fills out[0..*out_count) with up to max_suggestions known dictionary
   words at edit-distance 1 from utf8_word (deletion/transposition/
   substitution/insertion), each entry NUL-terminated within its
   SPELL_MAX_WORD_LEN-byte slot. Bounded but not fast -- see
   docs/services/spell.md. Returns *out_count == 0 (not an error) if
   nothing was found, or if utf8_word is outside the ASCII-only scope. */
wp_status spell_suggest(spell_dict *dict, const u8 *utf8_word, u32 len,
                         char out[][SPELL_MAX_WORD_LEN], u32 max_suggestions,
                         u32 *out_count);

#endif /* WP_SPELL_H */
