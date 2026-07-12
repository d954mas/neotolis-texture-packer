/* C0-01 task 6: source-key normalization + portability validation.
 * Separators ('/' and '\\'), '.'/repeated-sep collapse, '..' reject, absolute
 * reject, Unicode NFC (composed/decomposed equivalence), preserved case,
 * Windows reserved names / invalid chars / trailing dot-space, and Unicode
 * case-fold collision detection (incl. the ss/ss fold of "straße" vs "STRASSE").
 *
 * NOTE on \x escapes: a hex escape greedily eats following hex digits, so byte
 * sequences whose next char is a hex digit are split across adjacent literals. */

#include "tp_c0/tp_c0_srckey.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *norm(const char *in) {
    static char buf[TP_C0_SRCKEY_MAX];
    tp_error err;
    tp_c0_detail d = tp_c0_srckey_normalize(in, buf, sizeof buf, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, in);
    return buf;
}

static void norm_err(const char *in, tp_c0_detail want) {
    char buf[TP_C0_SRCKEY_MAX];
    tp_c0_detail d = tp_c0_srckey_normalize(in, buf, sizeof buf, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, d, in);
}

void test_structure_normalization(void) {
    TEST_ASSERT_EQUAL_STRING("sub/button.png", norm("sub/button.png"));
    TEST_ASSERT_EQUAL_STRING("sub/btn.png", norm("sub\\btn.png"));      /* '\\' separator */
    TEST_ASSERT_EQUAL_STRING("a/b/c.png", norm("a//b/./c.png"));        /* repeated sep + '.' */
    TEST_ASSERT_EQUAL_STRING("a/b/c.png", norm("./a/b/c.png"));
    TEST_ASSERT_EQUAL_STRING("x.png", norm("x.png"));
}

void test_preserved_case(void) {
    TEST_ASSERT_EQUAL_STRING("Button.PNG", norm("Button.PNG")); /* case is NOT folded */
}

void test_nfc_compose(void) {
    /* "cafe" + U+0301 (combining acute) -> "caf" + U+00E9 (é, C3 A9) */
    const char *decomposed = "cafe\xcc\x81/x.png";
    const char *composed = "caf\xc3\xa9/x.png";
    TEST_ASSERT_EQUAL_STRING(composed, norm(decomposed));
    TEST_ASSERT_EQUAL_STRING(composed, norm(composed));
    /* NFC equivalence: both forms normalize to the same key. */
    char a[TP_C0_SRCKEY_MAX], b[TP_C0_SRCKEY_MAX];
    (void)tp_c0_srckey_normalize(decomposed, a, sizeof a, NULL);
    (void)tp_c0_srckey_normalize(composed, b, sizeof b, NULL);
    TEST_ASSERT_EQUAL_STRING(a, b);
}

void test_rejects(void) {
    norm_err("/etc/x.png", TP_C0_ERR_KEY_ABSOLUTE);
    norm_err("\\x.png", TP_C0_ERR_KEY_ABSOLUTE);
    norm_err("C:/x.png", TP_C0_ERR_KEY_ABSOLUTE);      /* drive-prefixed = absolute */
    norm_err("a/../b.png", TP_C0_ERR_KEY_TRAVERSAL);
    norm_err("..", TP_C0_ERR_KEY_TRAVERSAL);
    norm_err(".", TP_C0_ERR_EMPTY);
    norm_err("./", TP_C0_ERR_EMPTY);
    norm_err("///", TP_C0_ERR_KEY_ABSOLUTE); /* leading separator = absolute */
    norm_err("", TP_C0_ERR_EMPTY);
}

void test_invalid_utf8(void) {
    /* 0xFF 0xFE is not a valid UTF-8 lead sequence. */
    norm_err("bad\xff\xfe.png", TP_C0_ERR_INVALID_UTF8);
}

void test_portability_reserved_names(void) {
    unsigned flags = 0;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("con/x.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_RESERVED_NAME) != 0);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("CON.txt", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_RESERVED_NAME) != 0);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("com1.dat", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_RESERVED_NAME) != 0);
    /* not reserved: longer base name */
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("community.png", &flags, NULL));
    TEST_ASSERT_EQUAL_UINT(TP_C0_PORT_OK, flags);
}

void test_portability_invalid_char_and_trailing(void) {
    unsigned flags = 0;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("sub/a?b.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_INVALID_CHAR) != 0);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("sub/a:b.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_INVALID_CHAR) != 0);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("folder./x.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_C0_PORT_TRAILING_DOT_SPACE) != 0);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_portability("clean/name.png", &flags, NULL));
    TEST_ASSERT_EQUAL_UINT(TP_C0_PORT_OK, flags);
}

/* Normalize both inputs into SEPARATE buffers (norm() returns one static buffer,
 * which would alias if used twice in one expression) then test for collision. */
static bool collide_norm(const char *a_in, const char *b_in) {
    char a[TP_C0_SRCKEY_MAX];
    char b[TP_C0_SRCKEY_MAX];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_normalize(a_in, a, sizeof a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_normalize(b_in, b, sizeof b, NULL));
    bool c = false;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_srckey_collides(a, b, &c, NULL));
    return c;
}

void test_casefold_collisions(void) {
    /* ASCII case collision -- reported, never silently merged. */
    TEST_ASSERT_TRUE(collide_norm("Button.png", "button.png"));
    /* Unicode full case-fold: ß folds to "ss", so these collide. */
    TEST_ASSERT_TRUE(collide_norm("STRASSE.png", "stra\xc3\x9f" "e.png"));
    /* distinct names do not collide */
    TEST_ASSERT_FALSE(collide_norm("a.png", "b.png"));
    /* NFC-equivalent inputs normalize to identical keys -> not a "collision" */
    TEST_ASSERT_FALSE(collide_norm("caf\xc3\xa9.png", "cafe\xcc\x81.png"));
}

void test_detail_tokens_stable(void) {
    TEST_ASSERT_EQUAL_STRING("key_absolute", tp_c0_detail_id(TP_C0_ERR_KEY_ABSOLUTE));
    TEST_ASSERT_EQUAL_STRING("key_traversal", tp_c0_detail_id(TP_C0_ERR_KEY_TRAVERSAL));
    TEST_ASSERT_EQUAL_STRING("invalid_utf8", tp_c0_detail_id(TP_C0_ERR_INVALID_UTF8));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_structure_normalization);
    RUN_TEST(test_preserved_case);
    RUN_TEST(test_nfc_compose);
    RUN_TEST(test_rejects);
    RUN_TEST(test_invalid_utf8);
    RUN_TEST(test_portability_reserved_names);
    RUN_TEST(test_portability_invalid_char_and_trailing);
    RUN_TEST(test_casefold_collisions);
    RUN_TEST(test_detail_tokens_stable);
    return UNITY_END();
}
