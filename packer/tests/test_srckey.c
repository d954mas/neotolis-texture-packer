/* F1-02: production source-key normalization + portability validation on the
 * tp_status model. Separators ('/' and '\\'), '.'/repeated-sep collapse,
 * '..' reject, absolute reject, Unicode NFC (composed/decomposed equivalence),
 * preserved case, Windows reserved names / invalid chars / trailing dot-space,
 * and Unicode case-fold collision detection (incl. the ss/ss fold of "straße"
 * vs "STRASSE").
 *
 * NOTE on \x escapes: a hex escape greedily eats following hex digits, so byte
 * sequences whose next char is a hex digit are split across adjacent literals. */

#include "tp_core/tp_srckey.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *norm(const char *in) {
    static char buf[TP_SRCKEY_MAX];
    tp_error err;
    tp_status st = tp_srckey_normalize(in, buf, sizeof buf, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, in);
    return buf;
}

static void norm_err(const char *in, tp_status want) {
    char buf[TP_SRCKEY_MAX];
    tp_status st = tp_srckey_normalize(in, buf, sizeof buf, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, st, in);
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
    char a[TP_SRCKEY_MAX], b[TP_SRCKEY_MAX];
    (void)tp_srckey_normalize(decomposed, a, sizeof a, NULL);
    (void)tp_srckey_normalize(composed, b, sizeof b, NULL);
    TEST_ASSERT_EQUAL_STRING(a, b);
}

void test_rejects(void) {
    norm_err("/etc/x.png", TP_STATUS_KEY_ABSOLUTE);
    norm_err("\\x.png", TP_STATUS_KEY_ABSOLUTE);
    norm_err("C:/x.png", TP_STATUS_KEY_ABSOLUTE);      /* drive-prefixed = absolute */
    norm_err("a/../b.png", TP_STATUS_KEY_TRAVERSAL);
    norm_err("..", TP_STATUS_KEY_TRAVERSAL);
    norm_err(".", TP_STATUS_INVALID_ARGUMENT);
    norm_err("./", TP_STATUS_INVALID_ARGUMENT);
    norm_err("///", TP_STATUS_KEY_ABSOLUTE); /* leading separator = absolute */
    norm_err("", TP_STATUS_INVALID_ARGUMENT);
}

void test_invalid_utf8(void) {
    /* 0xFF 0xFE is not a valid UTF-8 lead sequence. */
    norm_err("bad\xff\xfe.png", TP_STATUS_INVALID_UTF8);
}

void test_drive_prefix_after_dotstrip(void) {
    /* A drive prefix revealed only after '.'-component stripping must still be
     * rejected as absolute -- otherwise "./C:/x" would normalize to the accepted
     * "C:/x" and re-normalizing the stored key would fail (non-idempotent). */
    norm_err("./C:/x.png", TP_STATUS_KEY_ABSOLUTE);
    norm_err(".\\C:\\x.png", TP_STATUS_KEY_ABSOLUTE);
    /* a drive-looking spelling in a LATER component is not absolute; the
     * portability scan flags the ':' instead of rejecting the key. */
    TEST_ASSERT_EQUAL_STRING("a/C:/b", norm("a/C:/b"));
    unsigned flags = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("a/C:/b", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_INVALID_CHAR) != 0);
}

void test_normalize_idempotent(void) {
    /* Guarantee: normalize(normalize(x)) == normalize(x) for every accepted x. */
    static const char *accepted[] = {
        "sub/button.png", "sub\\btn.png", "a//b/./c.png",     "./a/b/c.png",
        "x.png",          "Button.PNG",   "a/C:/b",           "caf\xc3\xa9/x.png",
    };
    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
        char k1[TP_SRCKEY_MAX];
        char k2[TP_SRCKEY_MAX];
        TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_srckey_normalize(accepted[i], k1, sizeof k1, NULL), accepted[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_srckey_normalize(k1, k2, sizeof k2, NULL), accepted[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k1, k2, accepted[i]);
    }
}

void test_buffer_too_small(void) {
    /* A result that will not fit the caller buffer is a structured OUT_OF_BOUNDS,
     * never a crash. "abcdef" needs 7 bytes incl NUL; give it 4. */
    char tiny[4];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_srckey_normalize("abcdef", tiny, sizeof tiny, NULL));
}

void test_portability_reserved_names(void) {
    unsigned flags = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("con/x.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_RESERVED_NAME) != 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("CON.txt", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_RESERVED_NAME) != 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("com1.dat", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_RESERVED_NAME) != 0);
    /* not reserved: longer base name */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("community.png", &flags, NULL));
    TEST_ASSERT_EQUAL_UINT(TP_SRCKEY_PORT_OK, flags);
}

void test_portability_invalid_char_and_trailing(void) {
    unsigned flags = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("sub/a?b.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_INVALID_CHAR) != 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("sub/a:b.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_INVALID_CHAR) != 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("folder./x.png", &flags, NULL));
    TEST_ASSERT_TRUE((flags & TP_SRCKEY_PORT_TRAILING_DOT_SPACE) != 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_portability("clean/name.png", &flags, NULL));
    TEST_ASSERT_EQUAL_UINT(TP_SRCKEY_PORT_OK, flags);
}

/* Normalize both inputs into SEPARATE buffers (norm() returns one static buffer,
 * which would alias if used twice in one expression) then test for collision. */
static bool collide_norm(const char *a_in, const char *b_in) {
    char a[TP_SRCKEY_MAX];
    char b[TP_SRCKEY_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(a_in, a, sizeof a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(b_in, b, sizeof b, NULL));
    bool c = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_collides(a, b, &c, NULL));
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

void test_collides_near_limit(void) {
    /* collides must handle keys whose case-fold expands past the length limit.
     * U+0130 (İ, C4 B0) case-folds to "i" + U+0307 (2 bytes -> 3), so ~1000 İ plus
     * an ASCII tail folds well past TP_SRCKEY_MAX. A fixed-size fold buffer would
     * return buffer_too_small here. (Static buffers keep the frame small.) */
    static char x_in[TP_SRCKEY_MAX];
    static char y_in[TP_SRCKEY_MAX];
    size_t p = 0;
    for (int i = 0; i < 1000; i++) {
        x_in[p++] = (char)0xC4;
        x_in[p++] = (char)0xB0;
    }
    for (int i = 0; i < 1400; i++) {
        x_in[p++] = 'A';
    }
    x_in[p] = '\0';
    p = 0;
    for (int i = 0; i < 1000; i++) {
        y_in[p++] = (char)0xC4;
        y_in[p++] = (char)0xB0;
    }
    for (int i = 0; i < 1400; i++) {
        y_in[p++] = 'a';
    }
    y_in[p] = '\0';

    static char x[TP_SRCKEY_MAX];
    static char y[TP_SRCKEY_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(x_in, x, sizeof x, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(y_in, y, sizeof y, NULL));

    bool c = true;
    /* colliding pair (differ only by ASCII case), fold ~4400 bytes > 4096 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_collides(x, y, &c, NULL));
    TEST_ASSERT_TRUE(c);
    /* non-colliding pair, still no buffer_too_small */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_collides(x, "plain.png", &c, NULL));
    TEST_ASSERT_FALSE(c);

    /* ß-heavy near-limit colliding pair: "ß"xN folds to "ss"xN, matching "SS"xN. */
    static char lo[TP_SRCKEY_MAX];
    static char up[TP_SRCKEY_MAX];
    p = 0;
    for (int i = 0; i < 1500; i++) {
        lo[p++] = (char)0xC3;
        lo[p++] = (char)0x9F;
    }
    lo[p] = '\0';
    p = 0;
    for (int i = 0; i < 1500; i++) {
        up[p++] = 'S';
        up[p++] = 'S';
    }
    up[p] = '\0';
    static char lon[TP_SRCKEY_MAX];
    static char upn[TP_SRCKEY_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(lo, lon, sizeof lon, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_normalize(up, upn, sizeof upn, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_srckey_collides(lon, upn, &c, NULL));
    TEST_ASSERT_TRUE(c);
}

void test_status_tokens_stable(void) {
    TEST_ASSERT_EQUAL_STRING("key_absolute", tp_status_id(TP_STATUS_KEY_ABSOLUTE));
    TEST_ASSERT_EQUAL_STRING("key_traversal", tp_status_id(TP_STATUS_KEY_TRAVERSAL));
    TEST_ASSERT_EQUAL_STRING("invalid_utf8", tp_status_id(TP_STATUS_INVALID_UTF8));
}

void test_canonical_validation_distinguishes_valid_and_normalizable_keys(void) {
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_srckey_validate_canonical("sprites/hero.png", &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_srckey_validate_canonical("sprites//hero.png", &error));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "sprites/hero.png"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_KEY_TRAVERSAL,
        tp_srckey_validate_canonical("../hero.png", &error));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_structure_normalization);
    RUN_TEST(test_preserved_case);
    RUN_TEST(test_nfc_compose);
    RUN_TEST(test_rejects);
    RUN_TEST(test_invalid_utf8);
    RUN_TEST(test_drive_prefix_after_dotstrip);
    RUN_TEST(test_normalize_idempotent);
    RUN_TEST(test_buffer_too_small);
    RUN_TEST(test_portability_reserved_names);
    RUN_TEST(test_portability_invalid_char_and_trailing);
    RUN_TEST(test_casefold_collisions);
    RUN_TEST(test_collides_near_limit);
    RUN_TEST(test_status_tokens_stable);
    RUN_TEST(test_canonical_validation_distinguishes_valid_and_normalizable_keys);
    return UNITY_END();
}
