/* Lexer fixture: a `'"'` character literal must not open a string.
 * When comments and literals are removed in ordered passes instead of one
 * alternation, the quote INSIDE this character literal opens a string that runs
 * to the next quote anywhere in the file -- blanking the two forbidden calls
 * below along with everything between them. That is how gui_view_chrome.c hid
 * two real `system()` calls from every whole-file scan. The registering ctest
 * asserts two hits; an ordered-pass lexer produces zero. */
void bad_view_platform_after_quote_char(void) {
    char quote = '"';
    (void)quote;
    (void)system("forbidden-a");
    (void)system("forbidden-b");
}
