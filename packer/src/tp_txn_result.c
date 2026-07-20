#include "tp_core/tp_transaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_op_internal.h"
#include "tp_txn_internal.h"
/* ---- request / result lifecycle ------------------------------------------ */

void tp_txn_request_free(tp_txn_request *req) {
    if (!req) {
        return;
    }
    for (int i = 0; i < req->op_count; i++) {
        tp_operation_free(&req->ops[i]);
    }
    free(req->ops);
    free(req->label);
    free(req->author);
    free(req);
}

void tp_txn_result_free(tp_txn_result *res) {
    if (!res) {
        return;
    }
    free(res->ops);
    free(res->string_storage);
    free(res->errors);
    res->ops = NULL;
    res->string_storage = NULL;
    res->errors = NULL;
    res->op_count = res->error_count = 0;
}

/* ---- result assembly ----------------------------------------------------- */

void tp_txn__result_reset(tp_txn_result *out, const char *id_hex) {
    if (!out) {
        return;
    }
    out->schema = TP_TXN_SCHEMA;
    size_t id_len = 0U;
    if (id_hex) {
        while (id_len < sizeof out->transaction_id - 1U && id_hex[id_len]) {
            id_len++;
        }
        memcpy(out->transaction_id, id_hex, id_len);
    }
    out->transaction_id[id_len] = '\0';
    out->committed = false;
    out->no_change = false;
    out->revision = 0;
    out->ops = NULL;
    out->op_count = 0;
    out->string_storage = NULL;
    out->errors = NULL;
    out->error_count = 0;
}

/* Test-only fault seam for the add_error grow: -1 = off; N = the (N+1)th call's
 * grow fails once (then re-arms to off). Lets a test prove a dropped shape-error
 * record still forces a reject. */
static _Thread_local int s_add_error_fail = -1;
static _Thread_local size_t s_test_op_walk_steps = 0U;
static _Thread_local size_t s_test_error_allocations = 0U;
void tp_txn__test_set_add_error_fail(int nth) { s_add_error_fail = nth; }
void tp_txn__test_complexity_reset(void) {
    s_test_op_walk_steps = 0U;
    s_test_error_allocations = 0U;
}
size_t tp_txn__test_op_walk_steps(void) { return s_test_op_walk_steps; }
size_t tp_txn__test_error_allocations(void) { return s_test_error_allocations; }
void tp_txn__test_count_op_walk(size_t steps) { s_test_op_walk_steps += steps; }

/* Append one error (grows the dynamic error array). Returns true when stored; false
 * when the grow failed (OOM). A collect-all caller must treat a false as a forced
 * reject -- a shape-faulted batch must never falsely commit just because its error
 * record could not be stored. */
static bool result_error_store_allowed(void) {
    if (s_add_error_fail >= 0) { /* fault seam: fail this grow once */
        if (s_add_error_fail == 0) {
            s_add_error_fail = -1;
            return false;
        }
        s_add_error_fail--;
    }
    return true;
}

static void result_error_fill(tp_txn_error *e, int op_index, tp_status code,
                              const char *field, const char *msg) {
    e->op_index = op_index;
    e->code = code;
    (void)snprintf(e->field, sizeof e->field, "%s", field ? field : "");
    (void)snprintf(e->message, sizeof e->message, "%s", msg ? msg : "");
}

bool tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg) {
    if (!out || !result_error_store_allowed()) {
        return false;
    }
    tp_txn_error *n = (tp_txn_error *)realloc(out->errors, (size_t)(out->error_count + 1) * sizeof *n);
    if (!n) {
        return false;
    }
    s_test_error_allocations++;
    out->errors = n;
    tp_txn_error *e = &out->errors[out->error_count];
    result_error_fill(e, op_index, code, field, msg);
    out->error_count++;
    return true;
}

bool tp_txn__result_reserve_errors(tp_txn_result *out, int count) {
    if (!out || count < 0 || out->errors || out->error_count != 0) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if ((size_t)count > SIZE_MAX / sizeof *out->errors) {
        return false;
    }
    out->errors = (tp_txn_error *)malloc((size_t)count * sizeof *out->errors);
    if (!out->errors) {
        return false;
    }
    s_test_error_allocations++;
    return true;
}

bool tp_txn__result_add_error_reserved(tp_txn_result *out, int capacity, int op_index,
                                       tp_status code, const char *field, const char *msg) {
    if (!out || capacity < 0 || out->error_count >= capacity ||
        !result_error_store_allowed()) {
        return false;
    }
    result_error_fill(&out->errors[out->error_count], op_index, code, field, msg);
    out->error_count++;
    return true;
}

/* Add one id addressing echo to a committed result op. */
static void addr_id(tp_txn_result_op *ro, const char *key, tp_id_kind idk, tp_id128 id) {
    if (ro->addr_count >= 3) {
        return;
    }
    tp_txn_addr *a = &ro->addr[ro->addr_count++];
    (void)snprintf(a->key, sizeof a->key, "%s", key);
    a->idk = idk;
    a->id = id;
    a->str = NULL;
}

static void addr_str(tp_txn_result_op *ro, const char *key, const char *val,
                     char **storage) {
    if (ro->addr_count >= 3) {
        return;
    }
    const char *value = val ? val : "";
    const size_t length = strlen(value);
    tp_txn_addr *a = &ro->addr[ro->addr_count++];
    (void)snprintf(a->key, sizeof a->key, "%s", key);
    a->idk = TP_ID_KIND_INVALID;
    a->id = tp_id128_nil();
    a->str = *storage;
    memcpy(*storage, value, length + 1U);
    *storage += length + 1U;
}

/* Echo an op's wire + addressing ids on a committed result op (no diff). */
static void fill_result_op(tp_txn_result_op *ro, const tp_operation *op,
                           char **storage) {
    memset(ro, 0, sizeof *ro);
    (void)snprintf(ro->wire, sizeof ro->wire, "%s", tp_op_wire(op->kind));
    addr_id(ro, "atlas_id", TP_ID_KIND_ATLAS, op->atlas_id);
    switch (op->kind) {
        case TP_OP_SOURCE_ADD: addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.source_add.source_id); break;
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_SOURCE_REPLACE: addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.source_ref.source_id); break;
        case TP_OP_SPRITE_OVERRIDE_SET:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_set.source_id);
            addr_str(ro, "src_key", op->u.sprite_set.src_key, storage);
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_clear.source_id);
            addr_str(ro, "src_key", op->u.sprite_clear.src_key, storage);
            break;
        case TP_OP_SPRITE_NAME_SET:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_name.source_id);
            addr_str(ro, "src_key", op->u.sprite_name.src_key, storage);
            break;
        case TP_OP_ANIMATION_CREATE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_create.anim_id); break;
        case TP_OP_ANIMATION_REMOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_ref.anim_id); break;
        case TP_OP_ANIMATION_RENAME: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_rename.anim_id); break;
        case TP_OP_ANIMATION_SETTINGS_SET: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_settings.anim_id); break;
        case TP_OP_ANIMATION_FRAMES_SET: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frames_set.anim_id); break;
        case TP_OP_ANIMATION_FRAME_ADD: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_add.anim_id); break;
        case TP_OP_ANIMATION_FRAME_REMOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_rm.anim_id); break;
        case TP_OP_ANIMATION_FRAME_MOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_move.anim_id); break;
        case TP_OP_TARGET_CREATE: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_create.target_id); break;
        case TP_OP_TARGET_REMOVE: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_ref.target_id); break;
        case TP_OP_TARGET_SET: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_set.target_id); break;
        case TP_OP_ATLAS_CREATE:
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_RENAME:
        case TP_OP_ATLAS_SETTINGS_SET:
        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: break; /* atlas ops: atlas_id is the only addressing id */
    }
}

static _Thread_local int s_result_echo_fail = -1;
void tp_txn__test_set_result_echo_fail(int nth) {
    s_result_echo_fail = nth;
}

static bool result_echo_allocation_allowed(void) {
    if (s_result_echo_fail < 0) {
        return true;
    }
    if (s_result_echo_fail == 0) {
        s_result_echo_fail = -1;
        return false;
    }
    --s_result_echo_fail;
    return true;
}

static const char *result_op_string(const tp_operation *operation) {
    switch (operation->kind) {
        case TP_OP_SPRITE_OVERRIDE_SET:
            return operation->u.sprite_set.src_key;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            return operation->u.sprite_clear.src_key;
        case TP_OP_SPRITE_NAME_SET:
            return operation->u.sprite_name.src_key;
        default:
            return NULL;
    }
}

void tp_txn__result_echo_discard(tp_txn_result *out) {
    if (!out) {
        return;
    }
    free(out->ops);
    free(out->string_storage);
    out->ops = NULL;
    out->string_storage = NULL;
    out->op_count = 0;
}

bool tp_txn__result_echo_prepare(tp_txn_result *out,
                                const tp_txn_request *request) {
    if (!out || request->op_count == 0) {
        return true;
    }
    size_t string_bytes = 0U;
    for (int i = 0; i < request->op_count; ++i) {
        const char *value = result_op_string(&request->ops[i]);
        if (!value) {
            continue;
        }
        const size_t length = strlen(value) + 1U;
        if (string_bytes > SIZE_MAX - length) {
            return false;
        }
        string_bytes += length;
    }
    if (!result_echo_allocation_allowed()) {
        return false;
    }
    out->ops = calloc((size_t)request->op_count, sizeof *out->ops);
    if (!out->ops) {
        return false;
    }
    if (string_bytes > 0U) {
        if (!result_echo_allocation_allowed()) {
            tp_txn__result_echo_discard(out);
            return false;
        }
        out->string_storage = malloc(string_bytes);
        if (!out->string_storage) {
            tp_txn__result_echo_discard(out);
            return false;
        }
    }
    char *cursor = out->string_storage;
    for (int i = 0; i < request->op_count; ++i) {
        fill_result_op(&out->ops[i], &request->ops[i], &cursor);
    }
    out->op_count = request->op_count;
    return true;
}
