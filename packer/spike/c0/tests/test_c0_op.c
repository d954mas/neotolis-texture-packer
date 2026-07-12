/* C0-02 task 1/2/4: the append-only operation catalog, the verb->operation
 * mapping (no raw field patch), the closed field vocabulary, the selector
 * resolution boundary (incl the ambiguous-selector fault), and the semantic
 * state field partition. These pins are the executable form of C0-02-contract.md
 * §1, §2, §4. */

#include "tp_c0/tp_c0_op.h"
#include "tp_c0/tp_c0_semantic.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- catalog + wire round-trip ------------------------------------------- */

void test_catalog_wire_roundtrip(void) {
    for (int k = TP_C0_OP_INVALID + 1; k < TP_C0_OP_KIND_COUNT; k++) {
        const tp_c0_op_info *info = tp_c0_op_info_by_kind((tp_c0_op_kind)k);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(k, info->kind);
        TEST_ASSERT_TRUE(info->wire[0] != '\0');
        /* wire -> kind -> wire is stable */
        TEST_ASSERT_EQUAL_INT(k, tp_c0_op_kind_from_wire(info->wire));
        TEST_ASSERT_EQUAL_STRING(info->wire, tp_c0_op_wire((tp_c0_op_kind)k));
    }
}

void test_unknown_wire_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_INVALID, tp_c0_op_kind_from_wire("atlas.explode"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_INVALID, tp_c0_op_kind_from_wire(""));
    TEST_ASSERT_NULL(tp_c0_op_info_by_wire("nope"));
    TEST_ASSERT_NULL(tp_c0_op_info_by_kind(TP_C0_OP_INVALID));
    TEST_ASSERT_NULL(tp_c0_op_info_by_kind(TP_C0_OP_KIND_COUNT));
}

/* ---- verb -> operation mapping (no raw field patch) ---------------------- */

static tp_c0_op_kind by_verb(const char *verb) {
    for (int k = TP_C0_OP_INVALID + 1; k < TP_C0_OP_KIND_COUNT; k++) {
        const tp_c0_op_info *info = tp_c0_op_info_by_kind((tp_c0_op_kind)k);
        if (info->cli_verb && strcmp(info->cli_verb, verb) == 0) {
            return info->kind;
        }
    }
    return TP_C0_OP_INVALID;
}

void test_every_cli_verb_maps(void) {
    /* Each current ntpacker mutation verb lowers to a catalog op. `sprite set`
     * lowers to TWO ops (override + name); by_verb finds the first, which is
     * enough to prove the verb is represented. */
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ATLAS_CREATE, by_verb("atlas add"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ATLAS_REMOVE, by_verb("atlas remove"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ATLAS_RENAME, by_verb("atlas rename"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ATLAS_SETTINGS_SET, by_verb("set"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_SOURCE_ADD, by_verb("add"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_SOURCE_REMOVE, by_verb("remove"));
    TEST_ASSERT_TRUE(by_verb("sprite set") != TP_C0_OP_INVALID);
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_SPRITE_OVERRIDE_CLEAR, by_verb("sprite unset"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_CREATE, by_verb("anim create"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_REMOVE, by_verb("anim remove"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_SETTINGS_SET, by_verb("anim set"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_FRAME_ADD, by_verb("anim add-frame"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_FRAME_REMOVE, by_verb("anim remove-frame"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_ANIMATION_FRAME_MOVE, by_verb("anim move-frame"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_TARGET_CREATE, by_verb("target add"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_TARGET_REMOVE, by_verb("target remove"));
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_TARGET_SET, by_verb("target set"));
}

void test_reserved_ops_have_no_verb(void) {
    /* Spec-listed ops with no current CLI verb (Epic B / MCP bulk). */
    TEST_ASSERT_NULL(tp_c0_op_info_by_kind(TP_C0_OP_SOURCE_REPLACE)->cli_verb);
    TEST_ASSERT_NULL(tp_c0_op_info_by_kind(TP_C0_OP_ANIMATION_FRAMES_SET)->cli_verb);
}

/* ---- effect classes ------------------------------------------------------ */

void test_effect_classes(void) {
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_CLASS_CREATE, tp_c0_op_info_by_kind(TP_C0_OP_ATLAS_CREATE)->effect);
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_CLASS_REMOVE, tp_c0_op_info_by_kind(TP_C0_OP_ATLAS_REMOVE)->effect);
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_CLASS_SET, tp_c0_op_info_by_kind(TP_C0_OP_ATLAS_SETTINGS_SET)->effect);
    /* MOVE class has a real member: animation.frame.move (task 6). */
    TEST_ASSERT_EQUAL_INT(TP_C0_OP_CLASS_MOVE, tp_c0_op_info_by_kind(TP_C0_OP_ANIMATION_FRAME_MOVE)->effect);
    TEST_ASSERT_EQUAL_STRING("create", tp_c0_op_class_name(TP_C0_OP_CLASS_CREATE));
    TEST_ASSERT_EQUAL_STRING("move", tp_c0_op_class_name(TP_C0_OP_CLASS_MOVE));
    /* Every class is represented by at least one op. */
    bool seen[4] = {false, false, false, false};
    for (int k = TP_C0_OP_INVALID + 1; k < TP_C0_OP_KIND_COUNT; k++) {
        seen[tp_c0_op_info_by_kind((tp_c0_op_kind)k)->effect] = true;
    }
    for (int c = 0; c < 4; c++) {
        TEST_ASSERT_TRUE(seen[c]);
    }
}

/* ---- closed field vocabulary (no raw field patch) ------------------------ */

void test_field_vocabulary(void) {
    TEST_ASSERT_TRUE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "op"));
    TEST_ASSERT_TRUE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "atlas_id"));
    TEST_ASSERT_TRUE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "max_size"));
    TEST_ASSERT_TRUE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "pixels_per_unit"));
    /* An arbitrary path/field is NOT patchable -- there is no escape hatch. */
    TEST_ASSERT_FALSE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "atlases/0/secret"));
    TEST_ASSERT_FALSE(tp_c0_op_field_allowed(TP_C0_OP_ATLAS_SETTINGS_SET, "name")); /* rename is its own op */
    int n = 0;
    const char *const *keys = tp_c0_op_fields(TP_C0_OP_ANIMATION_FRAME_MOVE, &n);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("anim_id", keys[0]);
    TEST_ASSERT_EQUAL_STRING("from_index", keys[1]);
    TEST_ASSERT_EQUAL_STRING("to_index", keys[2]);
}

/* ---- selector resolution boundary ---------------------------------------- */

static tp_c0_id128 mk_id(uint8_t b) {
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[15] = b;
    return id;
}

void test_selector_resolves_unique_name(void) {
    tp_c0_entity_ref ents[] = {
        {TP_C0_ID_KIND_ATLAS, mk_id(1), "hud", 0},
        {TP_C0_ID_KIND_ATLAS, mk_id(2), "world", 1},
        {TP_C0_ID_KIND_ANIM, mk_id(3), "hud", 0}, /* same name, different kind: no clash */
    };
    tp_c0_selector sel = {0};
    sel.kind = TP_C0_SEL_NAME;
    sel.target_kind = TP_C0_ID_KIND_ATLAS;
    (void)snprintf(sel.name, sizeof sel.name, "%s", "world");
    tp_c0_id128 out;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_selector_resolve(&sel, ents, 3, &out, &err));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(out, mk_id(2)));
}

void test_selector_index_and_id(void) {
    tp_c0_entity_ref ents[] = {
        {TP_C0_ID_KIND_TARGET, mk_id(10), "json-neotolis", 0},
        {TP_C0_ID_KIND_TARGET, mk_id(11), "defold", 1},
    };
    tp_c0_id128 out;
    tp_error err = {0};
    tp_c0_selector byidx = {0};
    byidx.kind = TP_C0_SEL_INDEX;
    byidx.target_kind = TP_C0_ID_KIND_TARGET;
    byidx.index = 1;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_selector_resolve(&byidx, ents, 2, &out, &err));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(out, mk_id(11)));

    tp_c0_selector byid = {0};
    byid.kind = TP_C0_SEL_ID;
    byid.target_kind = TP_C0_ID_KIND_TARGET;
    byid.id = mk_id(10);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_selector_resolve(&byid, ents, 2, &out, &err));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(out, mk_id(10)));
}

void test_selector_ambiguous_and_unresolved(void) {
    tp_c0_entity_ref ents[] = {
        {TP_C0_ID_KIND_ATLAS, mk_id(1), "hud", 0},
        {TP_C0_ID_KIND_ATLAS, mk_id(2), "hud", 1}, /* duplicate name -> ambiguous */
    };
    tp_c0_id128 out;
    tp_error err = {0};
    tp_c0_selector sel = {0};
    sel.kind = TP_C0_SEL_NAME;
    sel.target_kind = TP_C0_ID_KIND_ATLAS;
    (void)snprintf(sel.name, sizeof sel.name, "%s", "hud");
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_AMBIGUOUS, tp_c0_selector_resolve(&sel, ents, 2, &out, &err));
    TEST_ASSERT_TRUE(tp_c0_id128_is_nil(out));
    /* prose lists the candidate ids */
    TEST_ASSERT_TRUE(strstr(err.msg, "atlas_") != NULL);

    (void)snprintf(sel.name, sizeof sel.name, "%s", "missing");
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_UNRESOLVED, tp_c0_selector_resolve(&sel, ents, 2, &out, &err));

    tp_c0_selector byid = {0};
    byid.kind = TP_C0_SEL_ID;
    byid.target_kind = TP_C0_ID_KIND_ATLAS;
    byid.id = mk_id(99); /* not in table */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_SELECTOR_UNRESOLVED, tp_c0_selector_resolve(&byid, ents, 2, &out, &err));
}

/* ---- semantic state partition -------------------------------------------- */

static bool semantic_has(const char *entity, const char *field, bool *ordered_out) {
    for (int i = 0; i < tp_c0_semantic_field_count(); i++) {
        const tp_c0_semantic_field *f = tp_c0_semantic_field_at(i);
        if (strcmp(f->entity, entity) == 0 && strcmp(f->field, field) == 0) {
            if (ordered_out) {
                *ordered_out = f->ordered;
            }
            return true;
        }
    }
    return false;
}

static bool excluded_has(const char *cat) {
    for (int i = 0; i < tp_c0_runtime_excluded_count(); i++) {
        if (strcmp(tp_c0_runtime_excluded_at(i), cat) == 0) {
            return true;
        }
    }
    return false;
}

void test_semantic_partition(void) {
    TEST_ASSERT_TRUE(tp_c0_semantic_field_count() > 0);
    TEST_ASSERT_TRUE(tp_c0_runtime_excluded_count() > 0);
    bool ordered = false;
    /* persistent content participates */
    TEST_ASSERT_TRUE(semantic_has("atlas", "max_size", &ordered));
    TEST_ASSERT_FALSE(ordered);
    TEST_ASSERT_TRUE(semantic_has("target", "enabled", NULL));
    TEST_ASSERT_TRUE(semantic_has("sprite", "rename", NULL)); /* logical name is semantic */
    /* the ONLY order-semantic collection is animation frames */
    TEST_ASSERT_TRUE(semantic_has("animation", "frames", &ordered));
    TEST_ASSERT_TRUE(ordered);
    TEST_ASSERT_TRUE(semantic_has("atlas", "sources", &ordered));
    TEST_ASSERT_FALSE(ordered); /* sources are ID-keyed, order-normalized */
    /* runtime state is excluded, and dirty is NOT derived from revision */
    TEST_ASSERT_TRUE(excluded_has("revision_counter"));
    TEST_ASSERT_TRUE(excluded_has("pack_result"));
    TEST_ASSERT_TRUE(excluded_has("source_runtime_status"));
    TEST_ASSERT_TRUE(excluded_has("gui_model_version_counter"));
    TEST_ASSERT_TRUE(excluded_has("schema_version"));
    /* partition is disjoint: no excluded category is a semantic field name */
    for (int i = 0; i < tp_c0_semantic_field_count(); i++) {
        TEST_ASSERT_FALSE(excluded_has(tp_c0_semantic_field_at(i)->field));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_catalog_wire_roundtrip);
    RUN_TEST(test_unknown_wire_is_invalid);
    RUN_TEST(test_every_cli_verb_maps);
    RUN_TEST(test_reserved_ops_have_no_verb);
    RUN_TEST(test_effect_classes);
    RUN_TEST(test_field_vocabulary);
    RUN_TEST(test_selector_resolves_unique_name);
    RUN_TEST(test_selector_index_and_id);
    RUN_TEST(test_selector_ambiguous_and_unresolved);
    RUN_TEST(test_semantic_partition);
    return UNITY_END();
}
