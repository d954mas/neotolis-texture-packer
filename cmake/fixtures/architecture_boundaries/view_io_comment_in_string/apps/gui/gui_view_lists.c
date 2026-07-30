/* Lexer fixture: a `//` inside a STRING must not open a comment.
 * When comments are stripped before literals, the `//` in the URL below is read
 * as a line comment and erases the rest of that real line -- including the
 * forbidden call on it. The registering ctest asserts two hits; a
 * comments-first lexer produces one. */
void bad_view_io_after_url(void) {
    const char *url = "http://example"; (void)fopen("forbidden", "rb");
    (void)url;
    (void)opendir("forbidden");
}
