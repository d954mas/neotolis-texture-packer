/* Unit test for gui_shell_quote.h's gui_shell_squote(): the single source of truth for wrapping a
 * USER-DERIVED path as one POSIX shell word (home dirs, imported asset filenames), so an embedded
 * apostrophe or shell metacharacter cannot inject into a system()-dispatched "open in the OS file
 * manager" command. The function is compiled ONLY on POSIX (Windows takes the UTF-16 path straight to
 * ShellExecuteW, never a shell), so the real assertions are #ifndef _WIN32; the Windows build compiles
 * a trivially-passing stub to stay green, and Linux CI runs the real body under ASan+UBSan. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "gui_shell_quote.h"

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#ifndef _WIN32

/* (a) a plain path is wrapped verbatim in single quotes. */
void test_shell_squote_plain_path_round_trips(void) {
    char out[64];
    TEST_ASSERT_TRUE(gui_shell_squote("/home/user/atlas.png", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("'/home/user/atlas.png'", out);
}

/* (b) an embedded apostrophe expands to the '\'' close/escape/reopen sequence. */
void test_shell_squote_apostrophe_expands_to_escape_sequence(void) {
    char out[64];
    TEST_ASSERT_TRUE(gui_shell_squote("o'brien", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("'o'\\''brien'", out);
}

/* (d) an empty string yields the empty shell word ''. */
void test_shell_squote_empty_string_is_empty_quotes(void) {
    char out[8];
    TEST_ASSERT_TRUE(gui_shell_squote("", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("''", out);
}

/* (c) an out_size too small to hold the quoted word returns false and never writes at or past
 *     out_size -- guard bytes filled with a sentinel must stay intact. */
void test_shell_squote_too_small_returns_false_without_overflow(void) {
    char buf[16];
    memset(buf, 0x7F, sizeof buf);
    /* "abcdef" needs 9 bytes ('abcdef' + NUL); 5 is too small. */
    TEST_ASSERT_FALSE(gui_shell_squote("abcdef", buf, 5));
    for (size_t i = 5; i < sizeof buf; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)buf[i]);
    }
}

/* (c') the wider '\'' branch (6 bytes headroom) must also fail cleanly when capacity is tight. */
void test_shell_squote_apostrophe_respects_tight_capacity(void) {
    char buf[16];
    memset(buf, 0x7F, sizeof buf);
    TEST_ASSERT_FALSE(gui_shell_squote("a'b", buf, 4));
    for (size_t i = 4; i < sizeof buf; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)buf[i]);
    }
}

/* (e) out_size < 3 (can't even hold '' + NUL) is rejected before a single byte is written. */
void test_shell_squote_out_size_below_three_returns_false(void) {
    char buf[8];
    memset(buf, 0x7F, sizeof buf);
    TEST_ASSERT_FALSE(gui_shell_squote("x", buf, 2));
    TEST_ASSERT_FALSE(gui_shell_squote("x", buf, 1));
    TEST_ASSERT_FALSE(gui_shell_squote("x", buf, 0));
    for (size_t i = 0; i < sizeof buf; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)buf[i]);
    }
}

#else /* _WIN32 */

/* gui_shell_squote does not exist on Windows (POSIX-only); keep the local build green. */
void test_shell_squote_windows_is_posix_only_noop(void) {
    TEST_PASS();
}

#endif /* !_WIN32 */

int main(void) {
    UNITY_BEGIN();
#ifndef _WIN32
    RUN_TEST(test_shell_squote_plain_path_round_trips);
    RUN_TEST(test_shell_squote_apostrophe_expands_to_escape_sequence);
    RUN_TEST(test_shell_squote_empty_string_is_empty_quotes);
    RUN_TEST(test_shell_squote_too_small_returns_false_without_overflow);
    RUN_TEST(test_shell_squote_apostrophe_respects_tight_capacity);
    RUN_TEST(test_shell_squote_out_size_below_three_returns_false);
#else
    RUN_TEST(test_shell_squote_windows_is_posix_only_noop);
#endif
    return UNITY_END();
}
