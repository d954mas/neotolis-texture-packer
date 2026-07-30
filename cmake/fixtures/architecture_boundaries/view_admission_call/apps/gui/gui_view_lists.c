typedef struct tp_session tp_session;

void bad_view_admission(tp_session *session) {
    (void)tp_session_apply
        (
            session, 0, 0, 0
        );
}
