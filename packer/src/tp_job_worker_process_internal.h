#ifndef TP_JOB_WORKER_PROCESS_INTERNAL_H
#define TP_JOB_WORKER_PROCESS_INTERNAL_H

#include "tp_job_worker_internal.h"

typedef struct tp_job_worker_process tp_job_worker_process;

tp_status tp_job_worker_process_start(
    const tp_job_worker_proto_request *request,
    tp_job_worker_process **out, tp_error *err);
void tp_job_worker_process_request_cancel(tp_job_worker_process *process);
void tp_job_worker_process_pump(tp_job_worker_process *process);
tp_job_worker_proto_progress tp_job_worker_process_progress(
    const tp_job_worker_process *process);
bool tp_job_worker_process_terminal(
    const tp_job_worker_process *process);
const tp_job_worker_proto_response *tp_job_worker_process_response(
    const tp_job_worker_process *process);
void tp_job_worker_process_destroy(tp_job_worker_process *process);

#ifdef TP_ENABLE_TEST_SEAMS
void tp_job_worker__test_set_timeout_ms(int timeout_ms);
bool tp_job_worker__test_request_backpressured(
    const tp_job_worker_process *process);
void tp_job_worker__test_reset(void);
#endif

#endif /* TP_JOB_WORKER_PROCESS_INTERNAL_H */
