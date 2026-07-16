#ifndef TP_CORE_TP_NAMES_H
#define TP_CORE_TP_NAMES_H

/* Canonical sprite-name / ordering helpers -- the single source of truth for
 * the name policy the GUI, the CLI, and tp_normalize all share (arch review
 * §3.1: four divergent copies collapsed here). Pure string functions, no
 * allocation, no engine types. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Export key for one sprite: strip a trailing ".ext" from the BASENAME only,
 * keeping any folder prefix (so a folder child's key stays atlas-relative, e.g.
 * "tank/walk_01.png" -> "tank/walk_01"). A leading-dot basename is a dotfile
 * and is kept intact (".gitkeep", "tank/.png" -> "tank/.png"). Copies into `out`
 * (snprintf-capped). File sources must pass a basename (path_last) first -- the
 * folder is not this fn's to strip. Matches tp_normalize's final-name step
 * byte-for-byte (existing goldens pin it). */
void tp_sprite_export_key(const char *raw, char *out, size_t cap);

/* Natural order: digit runs compare numerically (walk_2 before walk_10), the
 * rest byte-wise. Leading zeros within a run are ignored. */
int tp_nat_cmp(const char *a, const char *b);

/* Longest common prefix of `names`, trimmed of trailing digits/separators so
 * walk_01/walk_02 -> "walk". Writes "" when there is none. */
void tp_names_common_prefix(const char *const *names, int count, char *out,
                            size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_NAMES_H */
