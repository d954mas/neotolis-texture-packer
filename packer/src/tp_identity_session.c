#include "tp_core/tp_identity.h"

#include <stdio.h>
#include <string.h>

#include "tp_hex.h" /* shared lowercase-hex encoder (same code the drift-guard tests call) */

/* ======================================================================== */
/* Session identity DTO + Save-As transition.                               */
/*                                                                          */
/* The atomic-transition guarantee is structural: every transition          */
/* canonicalizes the destination into a LOCAL buffer first and overwrites    */
/* *id only after full success, so any failure leaves the OLD identity       */
/* byte-for-byte intact (rollback). Identity lives ONLY here as a runtime    */
/* DTO -- it is never serialized into `.ntpacker_project` (§59 item 2).      */
/* ======================================================================== */

tp_status tp_session_identity_init_unsaved(tp_session_identity *id, const tp_rng *rng, tp_error *err) {
    if (!id) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "identity out pointer is NULL");
    }
    memset(id, 0, sizeof *id);
    id->kind = TP_IDENTITY_UNSAVED;
    id->canonical_path[0] = '\0';
    tp_id128 sid;
    tp_status st = tp_id128_generate(rng, &sid, err);
    if (st != TP_STATUS_OK) {
        /* On RNG failure leave *id zeroed: is_valid() == false, so a failed init
         * is never mistaken for a real unsaved identity. */
        id->session_id = tp_id128_nil();
        return st;
    }
    id->session_id = sid;
    return TP_STATUS_OK;
}

tp_status tp_session_identity_claim_path(tp_session_identity *id, const char *dest_path,
                                         const char *const *claimed_keys, size_t claimed_count, tp_error *err) {
    if (!id) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "identity out pointer is NULL");
    }
    /* Canonicalize into a LOCAL buffer -- *id is untouched until we fully succeed. */
    char key[TP_IDENTITY_PATH_MAX];
    tp_status st = tp_identity_path_canonical(dest_path, key, sizeof key, err);
    if (st != TP_STATUS_OK) {
        return st; /* rollback: old identity intact */
    }
    for (size_t i = 0; i < claimed_count; i++) {
        if (claimed_keys && claimed_keys[i] && tp_identity_path_equal(key, claimed_keys[i])) {
            return tp_error_set(err, TP_STATUS_IDENTITY_COLLISION,
                                "destination '%s' is already claimed by another session", key);
        }
    }
    /* Commit the transition. */
    id->kind = TP_IDENTITY_SAVED;
    id->session_id = tp_id128_nil();
    memcpy(id->canonical_path, key, strlen(key) + 1U);
    return TP_STATUS_OK;
}

tp_status tp_session_identity_transition_to_path(tp_session_identity *id, const char *dest_path, tp_error *err) {
    return tp_session_identity_claim_path(id, dest_path, NULL, 0, err);
}

bool tp_session_identity_equal(const tp_session_identity *a, const tp_session_identity *b) {
    if (!a || !b || a->kind != b->kind) {
        return false;
    }
    if (a->kind == TP_IDENTITY_UNSAVED) {
        return tp_id128_eq(a->session_id, b->session_id);
    }
    return tp_identity_path_equal(a->canonical_path, b->canonical_path);
}

bool tp_session_identity_is_valid(const tp_session_identity *id) {
    if (!id) {
        return false;
    }
    if (id->kind == TP_IDENTITY_UNSAVED) {
        return !tp_id128_is_nil(id->session_id);
    }
    return id->canonical_path[0] != '\0';
}

tp_status tp_session_identity_key(const tp_session_identity *id, char *out, size_t cap, tp_error *err) {
    if (!id || !out || cap == 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "identity/out is NULL or cap is 0");
    }
    if (!tp_session_identity_is_valid(id)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "identity is not initialized");
    }
    if (id->kind == TP_IDENTITY_SAVED) {
        size_t n = strlen(id->canonical_path);
        if (n >= cap) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical key exceeds out buffer");
        }
        memcpy(out, id->canonical_path, n + 1U);
        return TP_STATUS_OK;
    }
    /* UNSAVED -> "session:" + 32 lowercase hex of the runtime session ID. A plain
     * hex form (NOT the "atlas_/source_/..." shape-ID format, which lives
     * elsewhere); shares tp_hex_encode_lower with the drift-guard tests so the encoding
     * cannot silently diverge between production and the vectors that pin it. */
    enum { PREFIX_LEN = 8, HEX_LEN = 32 }; /* strlen("session:") + 16*2 */
    if (cap < PREFIX_LEN + HEX_LEN + 1U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "session key needs %d bytes", PREFIX_LEN + HEX_LEN + 1);
    }
    memcpy(out, "session:", PREFIX_LEN);
    tp_hex_encode_lower(id->session_id.bytes, 16U, out + PREFIX_LEN);
    return TP_STATUS_OK;
}
