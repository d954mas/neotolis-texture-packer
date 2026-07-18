#ifndef NTPACKER_CLI_MUTATE_INTERNAL_H
#define NTPACKER_CLI_MUTATE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"

#define CLI_PATH_MAX TP_IDENTITY_PATH_MAX

extern const char *const k_atlas_knobs;
extern const char *const k_sprite_fields;

const char *split_kv(const char *tok, char *kbuf, size_t kcap);
bool to_long(const char *s, long *out);
bool to_int(const char *s, int *out);
bool to_bool(const char *s, bool *out);
bool to_float(const char *s, float *out);
bool to_longs_csv(const char *s, long *out, int want);
bool to_floats_csv(const char *s, float *out, int want);
bool parse_playback(const char *s, int *out);
char *cli_strdup(const char *s);
bool cli_gen_id(tp_id128 *out);
bool status_is_internal_fault(tp_status status);

typedef struct cli_edit {
    tp_session *session;
    tp_session_snapshot *snapshot;
    const char *path;
} cli_edit;

void edit_close(cli_edit *edit);
int edit_open(cli_edit *edit, const char *path, bool json, bool quiet);
int edit_resolve(cli_edit *edit, tp_id128 atlas_scope,
                 tp_selector_kind kind, const char *selector,
                 tp_selector_result *result, bool json, bool quiet);
int edit_resolve_sprite(cli_edit *edit, tp_id128 atlas_id,
                        const char *selector, tp_selector_result *result,
                        tp_id128 *source_id, char *source_key,
                        size_t source_key_capacity, bool json, bool quiet);
int edit_open_atlas(cli_edit *edit, const char *path, const char *selector,
                    const tp_snapshot_atlas **atlas, bool json, bool quiet);
int edit_fail_usage(cli_edit *edit, bool json, bool quiet,
                    const char *id, const char *msg);
int commit_session_ops(cli_edit *edit, tp_operation *ops, int nops,
                       const char *verb, int count, const char *human,
                       bool json, bool quiet);
void free_ops(tp_operation *ops, int n);

int do_add(const char *const *pos, int npos, bool json, bool quiet);
int do_remove_source(const char *const *pos, int npos, bool json, bool quiet);

#endif /* NTPACKER_CLI_MUTATE_INTERNAL_H */
