/* SEAM_FENCE negative fixture (never compiled). A COMPOUND guard condition is
 * not a seam fence: the || arm can be always-true, which would compile the
 * job-owner test seam into every shipping build. The A6 walker must report the
 * symbol below as unfenced. */
#if defined(TP_ENABLE_TEST_SEAMS) || 1
void tp_session_job_attach_internal(void);
#endif
