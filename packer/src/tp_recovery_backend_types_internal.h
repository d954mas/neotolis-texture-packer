#ifndef TP_RECOVERY_BACKEND_TYPES_INTERNAL_H
#define TP_RECOVERY_BACKEND_TYPES_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_journal.h"
#include "tp_core/tp_recovery.h"

/* Platform-neutral storage for the two native resources held by recovery.
 * Behavior stays in the platform backend; domain objects only own these values. */
typedef struct tp_recovery_lock_pin {
    intptr_t native_handle;
} tp_recovery_lock_pin;

typedef struct tp_recovery_file_pin {
    intptr_t native_handle;
    uint64_t identity_high;
    uint64_t identity_low;
    bool has_identity;
} tp_recovery_file_pin;

#define TP_RECOVERY_LOCK_PIN_INIT ((tp_recovery_lock_pin){.native_handle = -1})
#define TP_RECOVERY_FILE_PIN_INIT ((tp_recovery_file_pin){.native_handle = -1})

tp_journal_io tp_recovery_backend_journal_read(const char *path);
tp_journal_io tp_recovery_backend_live_create(
    const char *journal_path, tp_recovery_file_pin *pin,
    tp_status *status_out, tp_error *err);
void tp_recovery_backend_live_close(tp_recovery_file_pin *pin);
tp_status tp_recovery_backend_live_delete(
    const char *journal_path, tp_recovery_file_pin *pin, tp_error *err);

tp_status tp_recovery_backend_lock_open(
    tp_recovery_lock_pin *lock, const char *lock_path, tp_error *err);
void tp_recovery_backend_lock_release(tp_recovery_lock_pin *lock);
bool tp_recovery_backend_lock_is_unowned(const char *lock_path);

tp_journal_io tp_recovery_backend_candidate_pin(
    tp_recovery_file_pin *pin, const char *path,
    tp_status *status_out, tp_error *err);
void tp_recovery_backend_candidate_close(tp_recovery_file_pin *pin);
tp_status tp_recovery_backend_candidate_delete(
    tp_recovery_file_pin *pin, const char *journal_path, tp_error *err);

#ifndef _WIN32
void tp_recovery__test_fail_next_quarantine_unlink(void);
#endif

#endif
