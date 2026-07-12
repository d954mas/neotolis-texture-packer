#include "tp_c0/tp_c0_txn.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tp_c0_txn_priv.h" /* tpc0_is_hex32_lower (shared with the parsers) */

/* Full-batch validation: revision precondition first (short-circuit), then per-op
 * semantic checks collected in stable order (op_index asc, then field order),
 * before any application. No model mutation -- committed results are stubs. */

static void add_err(tp_c0_txn_result *out, int op_index, tp_c0_detail code, const char *fmt, ...) TP_PRINTF_ATTR(4, 5);

static void add_err(tp_c0_txn_result *out, int op_index, tp_c0_detail code, const char *fmt, ...) {
    if (out->error_count >= TP_C0_MAX_ERRORS) {
        out->errors_truncated = true; /* collect-all overflowed: mark, don't silently drop */
        return;
    }
    tp_c0_txn_error *e = &out->errors[out->error_count];
    e->op_index = op_index;
    e->code = code;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(e->message, sizeof e->message, fmt, ap);
        va_end(ap);
    } else {
        e->message[0] = '\0';
    }
    out->error_count++;
}

/* Map an "*_id" addressing key to the shape-ID kind it must carry. sprite_id is
 * bare 32-hex (no shape prefix, §5.2) -> returns INVALID (checked separately). */
static tp_c0_id_kind key_shape_kind(const char *key) {
    if (strcmp(key, "atlas_id") == 0) {
        return TP_C0_ID_KIND_ATLAS;
    }
    if (strcmp(key, "source_id") == 0) {
        return TP_C0_ID_KIND_SOURCE;
    }
    if (strcmp(key, "anim_id") == 0) {
        return TP_C0_ID_KIND_ANIM;
    }
    if (strcmp(key, "target_id") == 0) {
        return TP_C0_ID_KIND_TARGET;
    }
    return TP_C0_ID_KIND_INVALID;
}

static const char *kind_id_key(tp_c0_id_kind k) {
    switch (k) {
        case TP_C0_ID_KIND_ATLAS: return "atlas_id";
        case TP_C0_ID_KIND_SOURCE: return "source_id";
        case TP_C0_ID_KIND_ANIM: return "anim_id";
        case TP_C0_ID_KIND_TARGET: return "target_id";
        case TP_C0_ID_KIND_INVALID: return "";
    }
    return "";
}

/* An addressing/ID field is EXACTLY one of the real id keys -- the shape-prefixed
 * `*_id` (atlas/source/anim/target) or the bare 32-hex sprite_id -- NOT any key
 * that merely ends in "_id". `exporter_id` is a registry name (e.g. "defold"),
 * and `out_path`/`name`/`key`/`id` are plain strings; none is an id token. */
static bool is_id_addressing_key(const char *key) {
    return strcmp(key, "atlas_id") == 0 || strcmp(key, "source_id") == 0 || strcmp(key, "anim_id") == 0 ||
           strcmp(key, "target_id") == 0 || strcmp(key, "sprite_id") == 0;
}

static bool entity_exists(const tp_c0_entity_ref *entities, int n, tp_c0_id_kind kind, tp_c0_id128 id) {
    for (int i = 0; i < n; i++) {
        if (entities[i].kind == kind && tp_c0_id128_eq(entities[i].id, id)) {
            return true;
        }
    }
    return false;
}

/* Validate one addressing/id field; collect at most one error for it. */
static void check_id_field(tp_c0_txn_result *out, int idx, const tp_c0_op_info *info, const tp_c0_field *f,
                           const tp_c0_entity_ref *entities, int entity_count) {
    if (f->val.kind != TP_C0_VAL_STR) {
        add_err(out, idx, TP_C0_ERR_TXN_BAD_TYPE, "field '%s' must be a string id", f->key);
        return;
    }
    tp_c0_id_kind expected = key_shape_kind(f->key);
    if (expected == TP_C0_ID_KIND_INVALID) { /* sprite_id: bare 32-hex */
        if (!tpc0_is_hex32_lower(f->val.sval)) {
            add_err(out, idx, TP_C0_ERR_ID_BAD_LENGTH, "sprite id '%s' must be 32 lowercase hex", f->key);
        }
        return;
    }
    tp_c0_id_kind got = TP_C0_ID_KIND_INVALID;
    tp_c0_id128 id;
    tp_error e = {0};
    tp_c0_detail d = tp_c0_id_parse(f->val.sval, &got, &id, &e);
    if (d != TP_C0_OK) {
        add_err(out, idx, d, "field '%s': %s", f->key, e.msg[0] ? e.msg : "malformed id");
        return;
    }
    if (got != expected) {
        add_err(out, idx, TP_C0_ERR_ID_BAD_PREFIX, "field '%s' has the wrong id kind", f->key);
        return;
    }
    /* Reference existence (skip the create op's own freshly-minted primary id). */
    bool own_new = info->effect == TP_C0_OP_CLASS_CREATE && strcmp(f->key, kind_id_key(info->target_kind)) == 0;
    if (entity_count > 0 && !own_new && !entity_exists(entities, entity_count, expected, id)) {
        add_err(out, idx, TP_C0_ERR_SELECTOR_UNRESOLVED, "field '%s' references an unknown id", f->key);
    }
}

static void validate_op(tp_c0_txn_result *out, int idx, const tp_c0_op *op, const tp_c0_entity_ref *entities,
                        int entity_count) {
    if (op->kind == TP_C0_OP_INVALID) {
        add_err(out, idx, TP_C0_ERR_OP_UNKNOWN, "unknown operation '%s'", op->wire);
        return; /* unknown vocabulary: cannot check fields */
    }
    if (op->has_selector) {
        add_err(out, idx, TP_C0_ERR_SELECTOR_UNRESOLVED, "operation carries an unresolved selector (canonical is id-only)");
    }
    const tp_c0_op_info *info = tp_c0_op_info_by_kind(op->kind);
    for (int i = 0; i < op->field_count; i++) {
        const tp_c0_field *f = &op->fields[i];
        if (!tp_c0_op_field_allowed(op->kind, f->key)) {
            add_err(out, idx, TP_C0_ERR_UNKNOWN_FIELD, "unknown field '%s' for '%s'", f->key, op->wire);
            continue;
        }
        if (is_id_addressing_key(f->key)) {
            check_id_field(out, idx, info, f, entities, entity_count);
        }
    }
}

tp_c0_detail tp_c0_txn_validate(const tp_c0_txn_request *req, int64_t current_revision, const tp_c0_entity_ref *entities,
                                int entity_count, tp_c0_txn_result *out, tp_error *err) {
    if (!req || !out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null request/out");
    }
    memset(out, 0, sizeof *out);
    out->schema = TP_C0_TXN_SCHEMA;
    (void)snprintf(out->txn_id_hex, sizeof out->txn_id_hex, "%s", req->id_hex);

    /* 1. Revision precondition against the WHOLE batch, before any apply. */
    tp_error re = {0};
    tp_c0_detail rd = tp_c0_revision_check(req->expected_revision, current_revision, &re);
    if (rd != TP_C0_OK) {
        out->committed = false;
        out->revision = current_revision;
        add_err(out, -1, rd, "%s", re.msg);
        if (err) {
            *err = re;
        }
        return rd;
    }

    /* 2. Per-op semantic validation, collected in stable order. */
    for (int i = 0; i < req->op_count; i++) {
        validate_op(out, i, &req->ops[i], entities, entity_count);
    }

    if (out->error_count > 0) {
        out->committed = false;
        out->revision = current_revision;
        return out->errors[0].code;
    }

    /* 3. Committed stub: one new revision; addressing echoed; diffs are engine work. */
    out->committed = true;
    out->revision = current_revision + 1;
    out->op_count = req->op_count;
    for (int i = 0; i < req->op_count; i++) {
        const tp_c0_op *op = &req->ops[i];
        const tp_c0_op_info *info = tp_c0_op_info_by_kind(op->kind);
        tp_c0_result_op *ro = &out->ops[i];
        (void)snprintf(ro->wire, sizeof ro->wire, "%s", op->wire);
        ro->diff.cls = info ? info->effect : TP_C0_OP_CLASS_SET;
        ro->addr_count = 0;
        for (int f = 0; f < op->field_count && ro->addr_count < TP_C0_MAX_ADDR; f++) {
            if (is_id_addressing_key(op->fields[f].key)) {
                ro->addr[ro->addr_count] = op->fields[f];
                ro->addr[ro->addr_count].val.items = NULL; /* addr echoes are scalar ids, never arrays */
                ro->addr[ro->addr_count].val.item_count = 0;
                ro->addr_count++;
            }
        }
    }
    (void)err;
    return TP_C0_OK;
}

/* ---- revision precondition + idempotency retention set ------------------- */

tp_c0_detail tp_c0_revision_check(int64_t expected_revision, int64_t current_revision, tp_error *err) {
    if (expected_revision == current_revision) {
        return TP_C0_OK;
    }
    if (expected_revision < current_revision) {
        return tp_c0_fail(err, TP_C0_ERR_REVISION_CONFLICT, "expected_revision %" PRId64 " < current %" PRId64,
                          expected_revision, current_revision);
    }
    return tp_c0_fail(err, TP_C0_ERR_INVALID_REVISION, "expected_revision %" PRId64 " > current %" PRId64,
                      expected_revision, current_revision);
}

tp_c0_detail tp_c0_txn_idset_add(tp_c0_txn_idset *set, const char *id_hex, tp_error *err) {
    if (!set || !id_hex) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null idset/id");
    }
    if (!tpc0_is_hex32_lower(id_hex)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_ID, "transaction id must be 32 lowercase hex");
    }
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->ids[i], id_hex) == 0) {
            return tp_c0_fail(err, TP_C0_ERR_TXN_DUPLICATE_ID, "transaction id already applied");
        }
    }
    if (set->count >= TP_C0_IDSET_CAP) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "idempotency set full");
    }
    (void)snprintf(set->ids[set->count], sizeof set->ids[0], "%s", id_hex);
    set->count++;
    return TP_C0_OK;
}
