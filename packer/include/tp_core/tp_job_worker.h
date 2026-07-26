#ifndef TP_CORE_TP_JOB_WORKER_H
#define TP_CORE_TP_JOB_WORKER_H

/* Marker header for the private outer Pack/Export worker boundary.
 *
 * It intentionally exposes no ordinary client command or a second hidden argv
 * mode. Outer jobs reuse TP_BUILD_WORKER_ARGV1; the existing early build-worker
 * dispatch selects the inner-build or outer-job protocol by request wire magic.
 * All transport and worker entry points remain private to tp_build. */

#endif /* TP_CORE_TP_JOB_WORKER_H */
