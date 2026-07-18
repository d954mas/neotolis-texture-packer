#ifndef TP_RECOVERY_BACKEND_TYPES_INTERNAL_H
#define TP_RECOVERY_BACKEND_TYPES_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

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

#endif
