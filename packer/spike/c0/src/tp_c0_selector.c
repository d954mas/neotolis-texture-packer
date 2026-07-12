#include "tp_c0/tp_c0_op.h"

#include <stdio.h>
#include <string.h>

/* Resolve a request-edge selector to exactly one existing entity ID. Create ops
 * mint a fresh ID and do NOT pass through here; this resolves the target of ops
 * that address an entity that must already exist (spec §5.4/§5.6). */

/* Append up to 4 candidate IDs of `kind` matching `name` into err prose. */
static void list_candidates(const tp_c0_entity_ref *entities, int n, tp_c0_id_kind kind, const char *name,
                            tp_error *err) {
    if (!err) {
        return;
    }
    char buf[256];
    size_t used = 0;
    int shown = 0;
    int wrote = snprintf(buf, sizeof buf, "ambiguous selector '%s' matches:", name ? name : "");
    if (wrote > 0 && (size_t)wrote < sizeof buf) {
        used = (size_t)wrote;
    }
    for (int i = 0; i < n && shown < 4; i++) {
        if (entities[i].kind != kind || !entities[i].name || !name || strcmp(entities[i].name, name) != 0) {
            continue;
        }
        char id_text[TP_C0_ID_TEXT_CAP];
        if (tp_c0_id_format(kind, entities[i].id, id_text, sizeof id_text, NULL) != TP_C0_OK) {
            continue;
        }
        int w = snprintf(buf + used, sizeof buf - used, " %s", id_text);
        if (w < 0 || (size_t)w >= sizeof buf - used) {
            break;
        }
        used += (size_t)w;
        shown++;
    }
    (void)tp_c0_fail(err, TP_C0_ERR_SELECTOR_AMBIGUOUS, "%s", buf);
}

tp_c0_detail tp_c0_selector_resolve(const tp_c0_selector *sel, const tp_c0_entity_ref *entities, int n,
                                    tp_c0_id128 *out_id, tp_error *err) {
    if (!sel || (n > 0 && !entities) || !out_id) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null selector/entities/out");
    }
    *out_id = tp_c0_id128_nil();

    if (sel->kind == TP_C0_SEL_ID) {
        for (int i = 0; i < n; i++) {
            if (entities[i].kind == sel->target_kind && tp_c0_id128_eq(entities[i].id, sel->id)) {
                *out_id = sel->id;
                return TP_C0_OK;
            }
        }
        return tp_c0_fail(err, TP_C0_ERR_SELECTOR_UNRESOLVED, "id selector matches no live entity");
    }

    int matches = 0;
    tp_c0_id128 first = tp_c0_id128_nil();
    for (int i = 0; i < n; i++) {
        if (entities[i].kind != sel->target_kind) {
            continue;
        }
        bool hit = false;
        if (sel->kind == TP_C0_SEL_NAME) {
            hit = entities[i].name && strcmp(entities[i].name, sel->name) == 0;
        } else { /* TP_C0_SEL_INDEX */
            hit = entities[i].index == sel->index;
        }
        if (hit) {
            if (matches == 0) {
                first = entities[i].id;
            }
            matches++;
        }
    }

    if (matches == 1) {
        *out_id = first;
        return TP_C0_OK;
    }
    if (matches == 0) {
        if (sel->kind == TP_C0_SEL_INDEX) {
            return tp_c0_fail(err, TP_C0_ERR_SELECTOR_UNRESOLVED, "index selector [%d] out of range", sel->index);
        }
        return tp_c0_fail(err, TP_C0_ERR_SELECTOR_UNRESOLVED, "selector '%s' matches no entity", sel->name);
    }
    /* matches > 1 */
    if (sel->kind == TP_C0_SEL_NAME) {
        list_candidates(entities, n, sel->target_kind, sel->name, err);
        return TP_C0_ERR_SELECTOR_AMBIGUOUS;
    }
    return tp_c0_fail(err, TP_C0_ERR_SELECTOR_AMBIGUOUS, "index selector [%d] is ambiguous", sel->index);
}
