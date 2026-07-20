#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_source_path_text_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct path_case {
    const char *input;
    tp_status status;
    const char *normalized;
} path_case;

void test_source_path_text_table(void) {
    static const path_case cases[] = {
        {"sprites/hero.png", TP_STATUS_OK, "sprites/hero.png"},
        {"sprites\\hero.png", TP_STATUS_OK, "sprites/hero.png"},
        {"./sprites//hero.png", TP_STATUS_OK, "./sprites//hero.png"},
        {"../shared/hero.png", TP_STATUS_OK, "../shared/hero.png"},
        {"/opt/art/hero.png", TP_STATUS_OK, "/opt/art/hero.png"},
        {"C:\\art\\hero.png", TP_STATUS_OK, "C:/art/hero.png"},
        {"\\\\server\\share\\hero.png", TP_STATUS_OK,
         "//server/share/hero.png"},
        {"спрайты/герой.png", TP_STATUS_OK, "спрайты/герой.png"},
        {"", TP_STATUS_INVALID_ARGUMENT, NULL},
    };
    char normalized[TP_SOURCE_PATH_TEXT_CAP];
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT(cases[i].status,
                              tp_source_path_text_admit(cases[i].input));
        TEST_ASSERT_EQUAL_INT(
            cases[i].status,
            tp_source_path_text_normalize(cases[i].input, normalized,
                                          sizeof normalized));
        if (cases[i].normalized) {
            TEST_ASSERT_EQUAL_STRING(cases[i].normalized, normalized);
        } else {
            TEST_ASSERT_EQUAL_STRING("", normalized);
        }
    }
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_source_path_text_admit(NULL));

    const char invalid_utf8[] = {'x', (char)0xC0, (char)0xAF, '\0'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_source_path_text_admit(invalid_utf8));

    char longest[TP_SOURCE_PATH_TEXT_CAP];
    memset(longest, 'a', sizeof longest);
    longest[sizeof longest - 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_source_path_text_admit(longest));
    char too_long[TP_SOURCE_PATH_TEXT_CAP + 1U];
    memset(too_long, 'a', sizeof too_long);
    too_long[sizeof too_long - 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_source_path_text_admit(too_long));
}

typedef struct equality_case {
    const char *left;
    const char *right;
    bool equal;
} equality_case;

void test_source_path_hash_and_equality_invariants(void) {
    static const equality_case cases[] = {
        {"a/b", "a\\b", true},
        {"./a/b", "a/b", false},
        {"a/../b", "b", false},
        {"C:/Art", "c:/Art", false},
        {"//server/share/x", "\\\\server\\share\\x", true},
        {"арт/герой", "арт\\герой", true},
        {"é", "e\xCC\x81", false},
    };
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL(cases[i].equal,
                          tp_source_path_text_equal(cases[i].left,
                                                    cases[i].right));
        TEST_ASSERT_EQUAL(
            cases[i].equal,
            tp_source_path_text_hash(cases[i].left) ==
                tp_source_path_text_hash(cases[i].right));
    }
    TEST_ASSERT_FALSE(tp_source_path_text_equal(NULL, "a"));
    TEST_ASSERT_FALSE(tp_source_path_text_equal("", ""));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_source_path_text_table);
    RUN_TEST(test_source_path_hash_and_equality_invariants);
    return UNITY_END();
}
