/* Negative fixture: a gui_view_* file that no declared role covers.
 *
 * The P1 review proved the old filename-prefix classification was
 * bypassable: role was inferred, so a file could carry view violations and
 * still be scanned as a non-view. Classification is declared now, and an
 * undeclared gui_view_* file must abort the checker BEFORE any rule runs.
 * The VIEW_ADMISSION violation below is what would fire once the file is
 * declared a view; the fixture asserts the abort, not the hit. */
typedef struct tp_session tp_session;

void ghost_view_admission(tp_session *session) {
    (void)tp_session_apply(session, 0, 0, 0);
}
