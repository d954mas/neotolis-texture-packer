void bad_view_io(void) {
    (void)fopen
        ("forbidden", "rb");
}
