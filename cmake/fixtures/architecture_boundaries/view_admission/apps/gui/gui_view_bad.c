void bad_view_admission(tp_session *session,
                        const tp_txn_request *request,
                        tp_txn_result *result,
                        tp_error *error) {
    (void)tp_session_apply(session, request, result, error);
}
