#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* F2-05b-ii-B fix [1]: single-instance advisory lock on the recovery slot (GUI-layer, not core), so
 * a 2nd concurrent editor cannot adopt the 1st's LIVE session as "recovered" nor truncate its live
 * journal. Windows: an exclusive (share-mode 0) CreateFile handle; POSIX: flock(LOCK_EX|LOCK_NB). */
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "gui_scan.h"

#include "tp_core/tp_export.h"         /* TP_EXPORTER_ID_JSON_NEOTOLIS (default target exporter id) */
#include "tp_core/tp_id.h"             /* tp_rng_os + id128 generate/nil for op ids */
#include "tp_core/tp_operation.h"      /* the typed op the GUI mutators build */
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"    /* tp_model + tp_model_apply + tp_model_dirty/mark_saved + attach/recover */
#include "tp_core/tp_diff.h"           /* F2-03 undo/redo history (tp_model_enable_history/undo/redo) */
#include "tp_core/tp_journal.h"        /* F2-05b-ii-B recovery journal (io/create/recover sidecar) */

/* F2-05b-ii-A (docs/decisions/0015): GUI Undo/Redo now runs through the F2-03 diff history
 * (tp_model_enable_history + tp_model_undo/redo), NOT the retired 32 MB snapshot stack. Every
 * mutator BUILDS typed tp_operation(s) and commits them through a journal-LESS tp_model
 * (tp_model_wrap -> tp_model_apply, the F2-01/F2-02 clone-swap engine); the model OWNS the
 * project and captures one semantic diff per committed transaction (history is enabled at wrap).
 * s_proj is a read view of tp_model_project(s_model), refreshed after every committed
 * transaction and after every undo/redo (both clone-swap m->project). Dirty is IDENTITY-derived
 * (tp_model_dirty); a Save re-baselines via tp_model_mark_saved. The journal stays NULL (b-ii-B
 * owns the live journal + append-fail + recovery + D2).
 *
 * TRANSACTION-LEVEL COALESCING (b-ii-A crux): the F2-03 history has no built-in coalescing (one
 * commit = one undo step), so a raw slider drag would revert one tick at a time. A field-precise
 * debounce buffer holds ONE pending transaction; consecutive same-key edits replace its value
 * (latest wins), a different key flushes it first. The FLUSH TRIGGER is GESTURE-SCOPED (owner
 * decision, ADR 0015): the view layer commits one transaction per interaction on the widget's
 * gesture boundary (slider release / field Enter+blur / discrete click) via
 * gui_project_flush_pending; the 0.30 s time window is only a gated fallback
 * (gui_project_flush_elapsed). See the pending-buffer region below and decision 0015. */

// #region state
static tp_model *s_model;  /* owns s_proj; NULL until gui_project_init */
static tp_project *s_proj; /* == tp_model_project(s_model); refreshed after every commit/undo/redo */
static uint64_t s_txn_seq; /* monotonic transaction-id source (unique per commit) */
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_preview_stale;
static unsigned s_model_ver; /* bumped per real committed mutation (gui_project_model_version) */
static char s_name[256]; /* cached basename for the menu bar */
static double s_now;     /* coalescing clock (seconds), fed each frame by gui_project_tick */

/* F2-05b-ii-A #7: cached identity-derived dirty. tp_model_dirty walks the WHOLE project
 * (tp_semantic_identity) on every call; the menu-bar dot polls it ~60x/s, so re-hashing the whole
 * project each frame scaled O(project) per frame. The result changes ONLY when the model identity
 * changes -- a committed mutation (gui_project_touch), an undo/redo, or a mark_saved re-baseline
 * (promote_and_baseline / save) -- so cache it there and return the bool in the hot path. A buffered
 * (uncommitted) gesture is NOT yet in the identity, so buffering never needs to touch the cache. */
static bool s_dirty_cache;

/* Pending id-promotion failure from a void-context ensure_ids() (init/new/open). OS-RNG failure
 * would leave nil structural ids, which must never be silently swallowed. Drained + surfaced
 * once by the UI (gui_project_take_id_error). The save path fails closed on its own. */
static bool s_id_error;
static char s_id_error_msg[256];

/* Pending transaction REJECT (core rejected the op(s); model left byte-unchanged).
 * Surfaced once by the UI (gui_project_take_op_error) to the soft-error channel. */
static bool s_op_error;
static char s_op_error_msg[256];

/* F2-05b-ii-B crash-recovery slot (F2-04 §7.1). When set (gui_project_enable_recovery), EACH
 * installed model records a full-snapshot recovery journal at this DETERMINISTIC sidecar path: a
 * fresh checkpoint is written per New/Open (the slot always reflects the currently-open project) and
 * the slot is deleted on a CLEAN shutdown (only a crash -- shutdown never reached -- leaves it to
 * recover). Empty == journal DISABLED (the default; the headless selftest keeps it empty so it stays
 * deterministic + file-free). The journal is a SIDECAR -- it NEVER changes saved .ntpacker_project
 * bytes (the byte-parity goldens depend on this). See ADR 0015 for the keying/recovery-slot design.
 * fix [5]: sized >= every caller buffer (main.c 1152, selftest 1200) so a long exe path never
 * silently truncates the slot location. */
#define GUI_RECOVERY_PATH_MAX 1200
static char s_recovery_path[GUI_RECOVERY_PATH_MAX];
/* fix [1]: the single-instance lock handle + "do we OWN the slot" state. Recovery is ACTIVE for this
 * instance only when the slot is configured AND we hold the lock (recovery_active()); a 2nd instance
 * that cannot lock runs journal-less and never touches the slot. */
#ifdef _WIN32
static void *s_recovery_lock = NULL; /* HANDLE; NULL == not held (INVALID_HANDLE_VALUE also normalized to NULL) */
#else
static int s_recovery_lock = -1; /* fd; -1 == not held */
#endif
static bool s_recovery_locked;     /* true == this instance holds the slot lock (owns recovery) */
/* A one-shot notice that a crashed prior session's work was recovered at init (drained by the UI). */
static bool s_recovery_notice;
/* A one-shot notice that recovery is OFF because another instance holds the slot (drained by the UI). */
static bool s_recovery_busy_notice;
// #endregion

// #region transaction-level coalescing buffer (b-ii-A crux, decision 0015)
/* One pending transaction, keyed by (edit kind + atlas + EXACT target). Same key within the
 * window -> replace value (latest wins); different key / elapsed -> flush pending FIRST, then
 * buffer the new edit. Structural ops are non-coalescable (they flush pending, then commit
 * immediately). Field-precise keying is REQUIRED for correctness: keying slice9 by COMPONENT
 * makes a different-component edit flush the pending BEFORE its read-modify-write seed reads the
 * model, so each RMW read sees all prior edits committed (never merges two components against a
 * stale model -> no lost edit). */
#define GUI_COALESCE_SECS 0.30

typedef enum {
    CK_ATLAS_SETTING = 1,
    CK_SPRITE_ORIGIN,
    CK_SPRITE_SLICE9,   /* keyed by component (field = lrtb index) -- RMW correctness */
    CK_SPRITE_OVERRIDE,
    CK_ANIM_FPS,
    CK_ANIM_PLAYBACK,
    CK_ANIM_FLIP,
    CK_TARGET_OUTPATH   /* keyed by target index (field = index); H/G3 -- out-path text field coalesces */
} coalesce_kind;

typedef struct {
    coalesce_kind kind;
    int atlas;         /* atlas index */
    int field;         /* atlas field / sprite-override which / slice9 component; -1 if n/a */
    int anim;          /* animation index; -1 if n/a */
    char sprite[256];  /* sprite export key; "" if n/a */
} coalesce_key;

static bool s_pending_valid;        /* a coalescable edit is buffered (uncommitted) */
static coalesce_key s_pending_key;
static tp_operation s_pending_op;   /* owns its arms until committed/replaced/discarded */
static gui_action s_pending_act;    /* the touch tag to raise when it commits */
static double s_pending_time;       /* time of the last replace (coalesce window anchor) */
// #endregion

// #region helpers
static char *dupstr(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
}

static void recompute_name(void) {
    if (s_path[0] == '\0') {
        (void)snprintf(s_name, sizeof s_name, "untitled");
        return;
    }
    const char *base = s_path;
    for (const char *p = s_path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    (void)snprintf(s_name, sizeof s_name, "%s", base);
}

static void set_path(const char *path) {
    (void)snprintf(s_path, sizeof s_path, "%s", path ? path : "");
    recompute_name();
}

/* Assign a random persistent ID to any structural entity that lacks one -- nil (a freshly
 * created project/atlas/anim/target) OR loader-synthesized for a migrated legacy file (§5.5). A
 * real loaded ID (v3/v4) is preserved. Idempotent after the first call. Returns the promote
 * status. */
static tp_status ensure_ids(tp_error *err) {
    if (!s_proj) {
        return TP_STATUS_OK;
    }
    tp_rng rng = tp_rng_os();
    return tp_project_promote_ids(s_proj, &rng, err);
}

/* Record a void-context id-promotion failure so the UI can surface it. */
static void note_id_error(tp_status st, const tp_error *err) {
    s_id_error = true;
    (void)snprintf(s_id_error_msg, sizeof s_id_error_msg, "%s", (err && err->msg[0]) ? err->msg : tp_status_str(st));
}

bool gui_project_take_id_error(char *out, size_t cap) {
    if (!s_id_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_id_error_msg);
    }
    s_id_error = false;
    return true;
}

bool gui_project_take_op_error(char *out, size_t cap) {
    if (!s_op_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_op_error_msg);
    }
    s_op_error = false;
    return true;
}

/* fix3 [2]: fill `out` with the reason a flush's commit failed -- the drained op-error, else a NEUTRAL
 * fallback that fits save AND pack AND the dirty gate (the flush-failure abort paths share one wording,
 * no "saved"-specific verb). Consumes the op-error like gui_project_take_op_error. NULL-safe. */
void gui_project_flush_error(char *out, size_t cap) {
    if (!out || !cap) {
        return;
    }
    char m[256] = {0};
    if (!gui_project_take_op_error(m, sizeof m)) {
        (void)snprintf(m, sizeof m, "Your last edit could not be committed (disk full?) -- resolve it and try again.");
    }
    (void)snprintf(out, cap, "%s", m);
}

/* Seeds a fresh atlas with the default target (core helper owns the exporter id +
 * "out/<name>" path). LIFECYCLE, not a mutation op (the exporter id is core's, never a
 * frontend literal); mirrors the CLI do_new. Only a target-free atlas is seeded. */
static void seed_default_target(tp_project *p, int atlas_index) {
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a || a->target_count > 0) {
        return;
    }
    (void)tp_project_atlas_seed_default_target(p, atlas_index);
}

/* Drop a model's idempotency retention store (F2-05b-i F6). Idempotency is MOOT for the
 * single-threaded interactive GUI (unique monotonic ids, no retries), and the long-lived
 * journal-less model would otherwise accumulate one 33-byte id per commit FOREVER. The commit
 * path NULL-guards the idstore, so a NULL idstore simply skips id recording. Mirrors
 * tp_model_destroy's idstore cleanup exactly. */
static void drop_idstore(tp_model *m) {
    if (!m || !m->idstore) {
        return;
    }
    if (m->owns_idstore && m->idstore->destroy) {
        m->idstore->destroy(m->idstore->ctx);
    }
    free(m->idstore);
    m->idstore = NULL;
    m->owns_idstore = false;
}

/* F2-05b-ii-B: build/app-stable recovery-journal key. A fixed 128-bit tag identifying THIS app +
 * journal format, checked on recover so a foreign / old-format sidecar is rejected (in ADDITION to
 * the journal header's magic + version guard) rather than misapplied. Bump on any journal-format
 * change so old slots self-invalidate to a clean fresh init. */
static tp_id128 recovery_key(void) {
    tp_id128 k;
    static const uint8_t b[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(k.bytes, b, sizeof b);
    return k;
}

/* Raise the soft status-bar channel for a DEGRADED-durability notice (recovery journal could not be
 * attached). A missing recovery journal is never a crash or a blocked editor -- the model stays fully
 * usable journal-LESS; only crash-durability is lost until the next New/Open re-tries the slot. */
static void note_recovery_degraded(const char *msg) {
    s_op_error = true;
    (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "Recovery journal unavailable (%s) -- editing continues without crash recovery.",
                   msg ? msg : "unknown");
}

/* fix [1]: single-instance advisory lock on `<slot>.lock`. acquire returns true iff THIS process now
 * holds it (no other live instance does); it auto-releases on process death (crash-safe: a dead
 * instance never keeps the slot locked). release is idempotent. The lock file is a companion to the
 * journal slot -- never the journal file itself -- so it never interferes with journal I/O. */
static bool recovery_lock_acquire(const char *slot) {
    char lockpath[GUI_RECOVERY_PATH_MAX + 8];
    (void)snprintf(lockpath, sizeof lockpath, "%s.lock", slot);
#ifdef _WIN32
    HANDLE h = CreateFileA(lockpath, GENERIC_READ | GENERIC_WRITE, 0 /* exclusive: no sharing */, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false; /* another live instance holds it (ERROR_SHARING_VIOLATION) or the path is unwritable */
    }
    s_recovery_lock = (void *)h;
    return true;
#else
    int fd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        (void)close(fd); /* another live instance holds the exclusive lock */
        return false;
    }
    s_recovery_lock = fd;
    return true;
#endif
}
static void recovery_lock_release(void) {
#ifdef _WIN32
    if (s_recovery_lock != NULL) {
        (void)CloseHandle((HANDLE)s_recovery_lock); /* DELETE_ON_CLOSE removes the .lock file */
        s_recovery_lock = NULL;
    }
#else
    if (s_recovery_lock >= 0) {
        (void)flock(s_recovery_lock, LOCK_UN);
        (void)close(s_recovery_lock);
        s_recovery_lock = -1;
    }
#endif
    s_recovery_locked = false;
}

/* Recovery is ACTIVE for this instance only when a slot is configured AND we own the single-instance
 * lock. A 2nd concurrent instance (lock not held) is journal-less and must never touch the slot. */
static bool recovery_active(void) { return s_recovery_path[0] != '\0' && s_recovery_locked; }

/* fix [4]: the ONE owner of the create -> null-check -> attach -> destroy-on-fail ownership dance.
 * Creates a journal over `io` (TAKES OWNERSHIP of io, keyed with recovery_key) and attaches it to
 * `m`. On success `m` owns the journal. On ANY failure `io`/`j` are destroyed (never leaked) and a
 * non-OK status is returned. Shared by attach_recovery_journal and the append-fail test seam. */
static tp_status attach_journal_io(tp_model *m, tp_journal_io io, tp_error *err) {
    tp_journal *j = tp_journal_create(io, recovery_key()); /* TAKES OWNERSHIP of io; NULL destroys io */
    if (!j) {
        return TP_STATUS_OOM; /* io already destroyed by create */
    }
    tp_status st = tp_model_attach_journal(m, j, err);
    if (st != TP_STATUS_OK) {
        tp_journal_destroy(j); /* attach failed (checkpoint write) -> we still own j (and its io) */
    }
    return st;
}

/* Attach a FRESH file-backed recovery journal to `m` at the recovery slot, resetting the slot to
 * empty first so it reflects ONLY this model's project (one slot, always the current project -- no
 * accumulation across New/Open). No-op (leaves `m` journal-LESS, exactly the pre-B path) when
 * recovery is not ACTIVE (disabled, or a 2nd instance without the lock). PRECONDITION: any PREVIOUS
 * model (and thus the previous slot file handle) is already destroyed, so remove()+reopen never races
 * a live handle. On ANY failure the editor still runs journal-less and a degraded-durability notice
 * is raised (never a crash / blocked editor). */
#ifdef NTPACKER_GUI_SELFTEST
static bool s_test_skip_recovery_reset = false; /* selftest seam: one-shot skip of the slot reset (simulates a remove() that could not clear the slot) */
#endif
static void attach_recovery_journal(tp_model *m) {
#ifdef NTPACKER_GUI_SELFTEST
    const bool skip_reset = s_test_skip_recovery_reset;
    s_test_skip_recovery_reset = false; /* consume the one-shot arming ON ENTRY, so it never leaks past an early-return */
#endif
    if (!m || !recovery_active()) {
        return; /* recovery disabled / not owned -> journal-less (exactly the F2-05b-ii-A behavior) */
    }
#ifdef NTPACKER_GUI_SELFTEST
    if (!skip_reset) /* seam armed: leave the slot as-is so the fail-closed path is reachable */
#endif
        (void)remove(s_recovery_path); /* start a fresh slot: the old model's handle is already closed */
    tp_journal_io io = tp_journal_io_file(s_recovery_path);
    if (!io.ctx) {
        note_recovery_degraded("could not open the recovery journal file");
        return;
    }
    /* Fail closed if the slot is NOT actually empty. A failed remove() above (locked file / permission /
     * read-only dir) leaves stale-or-foreign bytes, and the journal layer accepts ANY >= header-length
     * store as a valid header -- magic/key are only checked on the recovery READ, never on this fresh
     * attach. Appending this session's edits after foreign content would silently lose them at recovery
     * time, so run journal-less with a degraded notice instead of building on a slot we could not reset. */
    if (io.length(io.ctx) != 0) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        note_recovery_degraded("could not reset the recovery slot -- a stale journal is present");
        return;
    }
    tp_error err = {0};
    if (attach_journal_io(m, io, &err) != TP_STATUS_OK) {
        note_recovery_degraded(err.msg[0] ? err.msg : "could not checkpoint the recovery journal");
    }
}

/* Wrap `p` (TAKES OWNERSHIP on success) in a fresh idstore-less model with the F2-03 undo/redo
 * history enabled + (when recovery is enabled) a fresh crash-recovery journal, and make it the live
 * model, refreshing the s_proj view. COMMIT-THEN-REPLACE (F2-05b-i F3): the replacement is wrapped
 * into a TEMP first and the OLD model is destroyed only AFTER the wrap succeeds -- so an OOM in the
 * wrap can never lose the open project or leave s_proj NULL. The journal is attached ONLY after the
 * old model is destroyed (its slot file handle closed), so the slot is never opened twice at once.
 * Returns true when `p` is installed; on failure `p` is freed (never leaked) and the CURRENT
 * model+project are kept intact. A NULL `p` returns false. */
static bool wrap_model(tp_project *p) {
    if (!p) {
        return false; /* nothing to install -> keep the current model (an upstream OOM must not clear it) */
    }
    tp_model *nm = tp_model_wrap(p);
    if (!nm) {
        tp_project_destroy(p); /* wrap did not take ownership on failure */
        return false;          /* keep the current model+project intact (F3) */
    }
    drop_idstore(nm);                  /* GUI carries NO idstore (F6); the journal is the idempotency authority */
    (void)tp_model_enable_history(nm); /* F2-05b-ii-A: undo/redo runs through this diff history */
    tp_model_destroy(s_model);         /* success: only now free the previous model (closes its slot handle) */
    s_model = nm;
    s_proj = tp_model_project(s_model);
    attach_recovery_journal(s_model);  /* F2-05b-ii-B: fresh recovery journal at the slot (no-op if disabled) */
    return true;
}

/* F2-05b-ii-A #7: refresh the cached dirty bool from the (possibly whole-project) identity walk.
 * Called ONLY at the identity-change choke points (commit/undo/redo/mark_saved), never per-frame. */
static void recompute_dirty(void) { s_dirty_cache = tp_model_dirty(s_model); }

/* Generates a fresh non-nil structural id via the OS RNG; false on an RNG fault. */
static bool gen_id(tp_id128 *out) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    return tp_id128_generate(&rng, out, &err) == TP_STATUS_OK;
}

static void ops_free(tp_operation *ops, int n) {
    for (int i = 0; i < n; i++) {
        tp_operation_free(&ops[i]);
    }
}

/* F2-05b-ii-A #8: fill `op` as the ONE default json-neotolis target.create for a GUI-added atlas/
 * target (exporter = TP_EXPORTER_ID_JSON_NEOTOLIS, out_path = "out/<name>", enabled). Extracted
 * from the two hand-written op sites (add_atlas + add_target) that had diverged buffer sizes
 * (out_path[576] each here vs the core seed's path[512]); one helper = one consistent size, no
 * divergence. `op` is zeroed first, so a false return (RNG/OOM) leaves it safe for ops_free /
 * tp_operation_free. Saved bytes stay byte-identical to the prior inline form (same fields, 576-byte
 * out_path -- unreachable overflow either way for a normal-length atlas name). */
static bool fill_default_target_op(tp_operation *op, tp_id128 atlas_id, const char *name) {
    memset(op, 0, sizeof *op);
    tp_id128 tgt_id;
    if (!gen_id(&tgt_id)) {
        return false;
    }
    char out_path[576];
    (void)snprintf(out_path, sizeof out_path, "out/%s", name ? name : "");
    op->kind = TP_OP_TARGET_CREATE;
    op->atlas_id = atlas_id;
    op->u.target_create.target_id = tgt_id;
    op->u.target_create.exporter_id = dupstr(TP_EXPORTER_ID_JSON_NEOTOLIS);
    op->u.target_create.out_path = dupstr(out_path);
    op->u.target_create.enabled = true;
    return op->u.target_create.exporter_id != NULL && op->u.target_create.out_path != NULL;
}

/* Commit `ops` as ONE atomic transaction on the persistent journal-less model NOW. On success
 * refreshes the s_proj view (the clone-swap replaced m->project; when history is enabled the
 * commit also captured a semantic diff and pushed one undo step, dropping any redo branch) and
 * returns true. On a reject the model is BYTE-UNCHANGED and the structured status is recorded for
 * the soft-error channel. ALWAYS frees the op arms and the result. Returns false on reject / no
 * model. */
static bool commit_txn_now(tp_operation *ops, int nops) {
    if (!s_model) {
        ops_free(ops, nops);
        return false;
    }
    tp_txn_request req;
    memset(&req, 0, sizeof req);
    req.schema = TP_TXN_SCHEMA;
    /* Unique 32-lowercase-hex per commit: the model persists across edits, so a fixed id
     * would trip idempotency (duplicate_id) on the second commit. A monotonic counter is
     * unique + never serialized. */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%032llx", (unsigned long long)(s_txn_seq++));
    req.expected_revision = tp_model_revision(s_model); /* single-threaded edits -> always matches */
    req.ops = ops;
    req.op_count = nops;

    tp_txn_result res;
    memset(&res, 0, sizeof res);
    tp_error err = {0};
    tp_status st = tp_model_apply(s_model, &req, &res, &err);

    bool ok = (st == TP_STATUS_OK);
    if (ok) {
        s_proj = tp_model_project(s_model); /* clone-swapped: old project freed, adopt the new view */
    } else {
        const char *msg = (err.msg[0]) ? err.msg : tp_status_str(st);
        if (res.error_count > 0 && res.errors[0].message[0]) {
            msg = res.errors[0].message;
        }
        /* F2-05b-ii-B append-fail UX: a recovery-journal append failure (full disk) rolls the store
         * back + rejects the commit inside tp_model_apply (the live model + revision + s_proj are
         * BYTE-UNCHANGED, the tx id stays retryable), so surface a clear, actionable status instead of
         * the internal gate prose. Never shows a false "saved"/clean state -- the model did not change. */
        if (st == TP_STATUS_JOURNAL_FAILED) {
            msg = "Could not journal the edit -- disk full? Your change was not applied.";
        }
        s_op_error = true;
        (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "%s", msg);
    }
    tp_txn_result_free(&res);
    ops_free(ops, nops);
    return ok;
}

/* Re-baseline the dirty anchor + history to the freshly installed project (init/new/open):
 * promote §5.5 ids (a direct mutation, so it is NOT an undo step) and mark_saved AFTER the
 * promotion, because ids participate in the semantic identity -- a promotion changes the
 * identity, so the clean baseline must be captured post-promotion. */
static void promote_and_baseline(void) {
    tp_error e = {0};
    tp_status st = ensure_ids(&e);
    if (st != TP_STATUS_OK) {
        note_id_error(st, &e); /* surface the RNG fault; never crash */
    }
    tp_model_mark_saved(s_model); /* clean baseline == the promoted identity */
    recompute_dirty();            /* #7: fresh baseline -> clean (cache the O(project) walk once) */
}
// #endregion

// #region pending-buffer primitives
static coalesce_key make_key(coalesce_kind kind, int atlas, int field, int anim, const char *sprite) {
    coalesce_key k;
    memset(&k, 0, sizeof k);
    k.kind = kind;
    k.atlas = atlas;
    k.field = field;
    k.anim = anim;
    (void)snprintf(k.sprite, sizeof k.sprite, "%s", sprite ? sprite : "");
    return k;
}

static bool key_eq(const coalesce_key *a, const coalesce_key *b) {
    return a->kind == b->kind && a->atlas == b->atlas && a->field == b->field && a->anim == b->anim &&
           strcmp(a->sprite, b->sprite) == 0;
}

/* Discard the buffered edit WITHOUT committing (new/open replace the whole project). */
static void pending_discard(void) {
    if (s_pending_valid) {
        tp_operation_free(&s_pending_op);
        memset(&s_pending_op, 0, sizeof s_pending_op);
        s_pending_valid = false;
    }
}

/* The committed atlas addressed by the buffered op, or NULL. */
static tp_project_atlas *atlas_of_pending(void) {
    int ai = tp_project_find_atlas_by_id(s_proj, s_pending_op.atlas_id);
    return ai < 0 ? NULL : tp_project_get_atlas(s_proj, ai);
}

/* F2-05b-ii-A #3: TRUE when committing the buffered op would not change the committed model (a
 * semantic no-op). The pre-cutover gui_project_touch memcmp-dedup suppressed no-op commits; the
 * read-only core commit path pushes an undo record + drops the redo branch UNCONDITIONALLY, so a
 * net-zero gesture (e.g. drag a knob out and back) would otherwise leave a phantom undo step AND
 * discard a live redo branch. We restore that parity GUI-side for the COALESCABLE op kinds only:
 * atlas.settings.set, animation.settings.set, sprite.override.set (absent sprite record ==
 * all-default/inherit, the same seed the setters use), and target.set (the out-path text field, H/G3
 * -- only out_path buffers; exporter/enabled are RMW-seeded, so all three are compared). Other
 * structural ops never buffer, so they always commit (a rename-to-same-name phantom stays a
 * pre-existing minor edge -- documented in ADR 0015).
 * Compares only the MASKED fields; an unmasked field is untouched by the op so it can't differ.
 * MAINTENANCE (review finding, ADR 0015 follow-up): this hand-mirrors each op's masked fields, so
 * adding a NEW masked field to a handled op kind REQUIRES extending the matching case below -- else a
 * gesture that changes ONLY that field is misclassified as a no-op and SILENTLY DISCARDED (data loss).
 * The robust fix is to derive the no-op verdict from the core semantic diff instead of re-listing
 * fields GUI-side; tracked as a separate hardening packet. */
static bool pending_is_noop(void) {
    if (!s_pending_valid || !s_proj) {
        return false;
    }
    const tp_operation *op = &s_pending_op;
    switch (op->kind) {
        case TP_OP_ATLAS_SETTINGS_SET: {
            const tp_project_atlas *a = atlas_of_pending();
            if (!a) {
                return false;
            }
            const tp_op_atlas_settings *s = &op->u.atlas_settings;
            const uint32_t m = s->mask;
            if ((m & TP_AF_MAX_SIZE) && a->max_size != s->max_size) return false;
            if ((m & TP_AF_PADDING) && a->padding != s->padding) return false;
            if ((m & TP_AF_MARGIN) && a->margin != s->margin) return false;
            if ((m & TP_AF_EXTRUDE) && a->extrude != s->extrude) return false;
            if ((m & TP_AF_ALPHA_THRESHOLD) && a->alpha_threshold != s->alpha_threshold) return false;
            if ((m & TP_AF_MAX_VERTICES) && a->max_vertices != s->max_vertices) return false;
            if ((m & TP_AF_SHAPE) && a->shape != s->shape) return false;
            if ((m & TP_AF_ALLOW_TRANSFORM) && a->allow_transform != s->allow_transform) return false;
            if ((m & TP_AF_POWER_OF_TWO) && a->power_of_two != s->power_of_two) return false;
            if ((m & TP_AF_PIXELS_PER_UNIT) && a->pixels_per_unit != s->pixels_per_unit) return false;
            return true;
        }
        case TP_OP_ANIMATION_SETTINGS_SET: {
            tp_project_atlas *a = atlas_of_pending();
            const tp_project_anim *an = a ? tp_project_atlas_find_animation_by_id(a, op->u.anim_settings.anim_id) : NULL;
            if (!an) {
                return false;
            }
            const tp_op_anim_settings *s = &op->u.anim_settings;
            const uint32_t m = s->mask;
            if ((m & TP_ANF_FPS) && an->fps != s->fps) return false;
            if ((m & TP_ANF_PLAYBACK) && an->playback != s->playback) return false;
            if ((m & TP_ANF_FLIP_H) && an->flip_h != s->flip_h) return false;
            if ((m & TP_ANF_FLIP_V) && an->flip_v != s->flip_v) return false;
            return true;
        }
        case TP_OP_SPRITE_OVERRIDE_SET: {
            tp_project_atlas *a = atlas_of_pending();
            if (!a) {
                return false;
            }
            const tp_op_sprite_set *s = &op->u.sprite_set;
            /* nil source_id -> the bridge record key IS the verbatim src_key (the export-key bridge). */
            const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, s->src_key ? s->src_key : "");
            const uint32_t m = s->mask;
            if (m & TP_SPF_ORIGIN) {
                const float ox = ov ? ov->origin_x : TP_PROJECT_ORIGIN_DEFAULT;
                const float oy = ov ? ov->origin_y : TP_PROJECT_ORIGIN_DEFAULT;
                if (ox != s->origin_x || oy != s->origin_y) return false;
            }
            if (m & TP_SPF_SLICE9) {
                for (int k = 0; k < 4; k++) {
                    const uint16_t cur = ov ? ov->slice9_lrtb[k] : 0;
                    if (cur != s->slice9[k]) return false;
                }
            }
            if ((m & TP_SPF_SHAPE) && (ov ? ov->ov_shape : TP_PROJECT_OV_INHERIT) != s->ov_shape) return false;
            if ((m & TP_SPF_ALLOW_ROTATE) && (ov ? ov->ov_allow_rotate : TP_PROJECT_OV_INHERIT) != s->ov_allow_rotate) return false;
            if ((m & TP_SPF_MAX_VERTICES) && (ov ? ov->ov_max_vertices : TP_PROJECT_OV_INHERIT) != s->ov_max_vertices) return false;
            if ((m & TP_SPF_MARGIN) && (ov ? ov->ov_margin : TP_PROJECT_OV_INHERIT) != s->ov_margin) return false;
            if ((m & TP_SPF_EXTRUDE) && (ov ? ov->ov_extrude : TP_PROJECT_OV_INHERIT) != s->ov_extrude) return false;
            return true;
        }
        case TP_OP_TARGET_SET: {
            /* H/G3: only the out-path text field BUFFERS a TP_OP_TARGET_SET (discrete exporter/enabled/
             * browse edits commit immediately via gui_project_set_target). That setter RMW-seeds
             * exporter_id + enabled from the committed record, so a buffered target op can differ ONLY in
             * out_path -- but the op sets all three unconditionally (no mask), so compare all three: this
             * stays COMPLETE (never a false no-op -> no silent discard) even if another target field ever
             * coalesces. Restores the net-zero parity the other coalescable kinds already have. */
            tp_project_atlas *a = atlas_of_pending();
            const tp_op_target_set *s = &op->u.target_set;
            const tp_project_target *t = a ? tp_project_atlas_find_target_by_id(a, s->target_id) : NULL;
            if (!t) {
                return false;
            }
            if (t->enabled != s->enabled) return false;
            if (strcmp(t->exporter_id ? t->exporter_id : "", s->exporter_id ? s->exporter_id : "") != 0) return false;
            if (strcmp(t->out_path ? t->out_path : "", s->out_path ? s->out_path : "") != 0) return false;
            return true;
        }
        default:
            return false; /* not a coalescable value edit -> always commit */
    }
}

/* fix [3]: returns FALSE iff a buffered gesture existed and its commit FAILED (e.g. a journal append
 * failure) -- i.e. an edit could NOT be made durable. Returns TRUE when nothing was pending, the
 * pending was a net-zero no-op, or it committed OK. Callers that persist or discard on the strength of
 * "no pending left" (save/save-as, undo/redo) MUST check this and ABORT on false so a journal-failed
 * flush is never mistaken for a clean state (no false "saved", no wrong undo target). */
bool gui_project_flush_pending(void) {
    if (!s_pending_valid) {
        return true;
    }
    if (pending_is_noop()) {
        /* #3: a gesture that nets back to the committed value commits NOTHING -- no phantom undo
         * step, no dropped redo branch, no dirty flip. pending_discard frees the arms + clears. */
        pending_discard();
        return true;
    }
    tp_operation op = s_pending_op; /* move: ownership of the arms transfers to the local */
    gui_action act = s_pending_act;
    memset(&s_pending_op, 0, sizeof s_pending_op);
    s_pending_valid = false;
    if (commit_txn_now(&op, 1)) { /* frees op's arms; refreshes s_proj + pushes one undo step */
        gui_project_touch(act);
        return true;
    }
    /* on reject commit_txn_now freed the arms + set the op-error; the edit could not be committed. */
    return false;
}

/* Called AT THE TOP of a coalescable mutator with its key, BEFORE the mutator reads the model:
 * flush the buffered edit when the incoming key DIFFERS, so a) distinct edits never merge and
 * b) an RMW read sees all prior edits committed. GESTURE-SCOPED (owner decision, ADR 0015): the
 * flush trigger is the gesture boundary (slider release / field Enter+blur / discrete click),
 * NOT a timer -- so a SAME-key edit never flushes here regardless of how long the gesture takes
 * (a slow drag stays one transaction). The time window is a FALLBACK only (gui_project_flush_elapsed).
 * After this returns, s_pending_valid is true IFF a same-key pending remains (the replace target). */
static void pending_route(const coalesce_key *k) {
    if (s_pending_valid && !key_eq(k, &s_pending_key)) {
        /* fix2: the bool is intentionally IGNORED here. On a journal-failed flush the different-key
         * gesture is dropped WITH the op-error surfaced (commit_txn_now set it); the caller then only
         * BUFFERS a new (uncommitted) edit -- there is no persist/discard "proceed as clean" decision to
         * abort. gui_project_flush_elapsed (the timer fallback) is the same case. Audited fix2 [3]. */
        (void)gui_project_flush_pending();
    }
}

/* Buffer `op` (TAKES OWNERSHIP of the arms) under `k`. Precondition (pending_route ran): a still-
 * valid pending is same-key -> replace its value (latest wins). Preview goes stale immediately;
 * the commit (and model_ver bump) is deferred to the flush. Always returns true. */
static bool pending_offer(const coalesce_key *k, tp_operation *op, gui_action act) {
    if (s_pending_valid) {
        tp_operation_free(&s_pending_op); /* same key: replace the value */
    }
    s_pending_op = *op; /* shallow move; caller must not free `op` after this */
    s_pending_key = *k;
    s_pending_act = act;
    s_pending_time = s_now;
    s_pending_valid = true;
    s_preview_stale = true; /* immediate stale feedback while the gesture buffers */
    return true;
}

/* FALLBACK ONLY (ADR 0015): commit a buffered gesture that never received a release/blur/discrete
 * boundary (e.g. a control that streams values with no pointer/focus edge). The caller (main.c)
 * gates this on NO active gesture (no held pointer, no focused input) so it can never split a live
 * drag or a mid-typing field. Primary commits are gesture-scoped; this only backstops a missed edge. */
void gui_project_flush_elapsed(void) {
    if (s_pending_valid && (s_now - s_pending_time) > GUI_COALESCE_SECS) {
        gui_project_flush_pending();
    }
}

/* F2-05b-ii-A #5: EFFECTIVE slice9 peek for the on-canvas guide lines. slice9 edits now BUFFER until
 * the gesture boundary, so the committed record is FROZEN mid-typing -- the guides would freeze with
 * it (regressing "typing in the Region panel moves the lines this same frame"). Returns true + fills
 * out_lrtb[4] with the BUFFERED slice9 when a slice9 gesture is buffered for this atlas+sprite (the
 * pending op already seeds all four components from the committed record, so it is the full effective
 * value); false otherwise, so the caller falls back to the committed record. No commit, read-only. */
bool gui_project_peek_pending_slice9(int atlas_index, const char *sprite_key, int out_lrtb[4]) {
    if (!s_pending_valid || s_pending_key.kind != CK_SPRITE_SLICE9 || s_pending_key.atlas != atlas_index) {
        return false;
    }
    if (strcmp(s_pending_key.sprite, sprite_key ? sprite_key : "") != 0) {
        return false;
    }
    for (int k = 0; k < 4; k++) {
        out_lrtb[k] = (int)s_pending_op.u.sprite_set.slice9[k];
    }
    return true;
}
// #endregion

// #region lifecycle
/* F2-05b-ii-B crash recovery (F2-04 §7.1/§22.3). If recovery is ACTIVE (slot owned) and the slot
 * holds a usable journal from a CRASHED prior session, rebuild + install its last committed STATE,
 * kept DIRTY so the user is prompted to Save the recovered work. Returns true when a model was
 * adopted; a stale / empty / foreign / corrupt / torn-first slot returns false -> normal fresh init.
 *
 * fix [0]+[2] REDESIGN: recover only the STATE, then rebuild a FRESH model + FRESH journal around it.
 * The recovered journal returned by tp_model_recover cannot be adopted directly because: [0] its
 * retained-id index already holds the crashed session's ids 0..K-1, and s_txn_seq restarts at 0 each
 * launch, so the first post-recovery commits would collide as DUPLICATE_ID -> the project would look
 * frozen; [2] it may be POISONED (mid-stream corruption, F2-04 C2/C3) -> every append fails. So we
 * tp_project_clone the recovered project OFF rm, tp_model_destroy(rm) (drops the old/poisoned journal
 * + closes the slot handle), then wrap_model the clone -> a FRESH journal with an EMPTY, non-poisoned
 * retained-id index at the reset slot. tp_model_recover TAKES OWNERSHIP of the io on every path
 * (recovered model owns it, else destroyed), so the io is never leaked. */
static bool try_adopt_recovered(void) {
    if (!recovery_active()) {
        return false; /* recovery disabled, or a 2nd instance without the slot lock -> fresh init */
    }
    tp_journal_io io = tp_journal_io_file(s_recovery_path);
    if (!io.ctx) {
        return false; /* cannot open the slot (or none exists) -> fresh init */
    }
    tp_model *rm = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err = {0};
    tp_status st = tp_model_recover(io, recovery_key(), &rm, &info, &err); /* TAKES OWNERSHIP of io */
    tp_journal_recovery_free(&info);                                       /* frees the recovered snapshot copy */
    if (st != TP_STATUS_OK || !rm) {
        return false; /* nothing recoverable (empty/bad-header/stale-key/torn-first) -> fresh init */
    }
    /* Clone the recovered STATE, then discard rm (its journal may be poisoned + its index stale). */
    tp_project *recovered = tp_project_clone(tp_model_project(rm));
    tp_model_destroy(rm); /* drops the recovered journal (poisoned or not) + closes the slot handle */
    if (!recovered) {
        return false; /* clone OOM -> fall back to a fresh init (never crash on recovery) */
    }
    if (!wrap_model(recovered)) {
        return false; /* wrap OOM -> recovered freed by wrap_model; fall back to a fresh init */
    }
    /* wrap_model attached a FRESH journal at the reset slot (empty id-index, not poisoned) with a
     * checkpoint of the recovered project, and left the model CLEAN (saved_identity == identity). The
     * recovered content is UNSAVED work ahead of any on-disk file -> flag it dirty via the F2-04 C5
     * recovered_unsaved field (the model's own "dirty vs the project file" mechanism, tp_transaction.h):
     * gui_project_is_dirty() reads true WITHOUT a spurious extra commit, and a later Save re-baselines +
     * clears it. (This writes a tp_model runtime field, not the project -- outside the R7 project-write
     * boundary rules.) */
    s_model->recovered_unsaved = true;
    set_path("");        /* recovered work is untitled -> a deliberate Save As is required */
    s_preview_stale = true;
    recompute_dirty();   /* recovered_unsaved == true -> dirty (independent of identity) */
    s_recovery_notice = true; /* surfaced once by the UI (gui_project_take_recovery_notice) */
    return true;
}

void gui_project_init(void) {
    if (s_model) {
        return;
    }
    pending_discard();
    if (try_adopt_recovered()) {
        return; /* F2-05b-ii-B: adopted a crash-recovered model (dirty) -- do NOT overwrite with a fresh one */
    }
    tp_project *p = tp_project_create();
    seed_default_target(p, 0); /* clean baseline includes it (I1) -- lifecycle, direct */
    (void)wrap_model(p);       /* first model: on OOM s_model/s_proj stay NULL; attaches a fresh recovery journal */
    set_path("");
    s_preview_stale = false;
    promote_and_baseline();
}

void gui_project_shutdown(void) {
    pending_discard();
    tp_model_destroy(s_model); /* frees the model + its owned project + history + recovery journal (closes the slot file) */
    s_model = NULL;
    s_proj = NULL;
    /* F2-05b-ii-B clean-exit reset: a cleanly-exited session leaves NO journal to recover, so the next
     * launch starts fresh (no spurious "recovered" prompt). Only a CRASH -- which never reaches this
     * shutdown -- leaves the slot on disk. Delete the slot ONLY while we own it (recovery_active), then
     * release the single-instance lock so a relaunch can re-acquire it. */
    if (recovery_active()) {
        (void)remove(s_recovery_path);
    }
    recovery_lock_release();
}

/* F2-05b-ii-B: enable/configure crash recovery. `slot_path` is a DETERMINISTIC sidecar journal path
 * (a stable temp/app-data location the next launch reconstructs WITHOUT a random session id, per the
 * owner's requirement); NULL/"" DISABLES recovery (journal-less, the default). Call ONCE before the
 * first gui_project_init in the interactive app.
 *
 * fix [1]: acquires a single-instance advisory lock on the slot. If another live instance already
 * holds it, THIS instance runs journal-less (recovery INACTIVE) and never touches the slot -- so it
 * neither adopts the other instance's LIVE session as "recovered" nor truncates its live journal --
 * and raises a one-shot "another instance" notice. Re-enabling always releases any prior lock first. */
void gui_project_enable_recovery(const char *slot_path) {
    recovery_lock_release(); /* drop any prior lock so a re-enable (or disable) is clean */
    (void)snprintf(s_recovery_path, sizeof s_recovery_path, "%s", slot_path ? slot_path : "");
    if (s_recovery_path[0] == '\0') {
        return; /* disabled */
    }
    s_recovery_locked = recovery_lock_acquire(s_recovery_path);
    if (!s_recovery_locked) {
        s_recovery_busy_notice = true; /* another instance owns the slot -> crash recovery off this window */
    }
}

/* Drains the one-shot "recovered unsaved changes" notice (true once after a crash-recovery adopt at
 * init, then cleared). The UI polls this to surface the recovery to the user. */
bool gui_project_take_recovery_notice(char *out, size_t cap) {
    if (!s_recovery_notice) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "Recovered unsaved changes from a previous session. Save to keep them.");
    }
    s_recovery_notice = false;
    return true;
}

/* Drains the one-shot "another instance is running -> crash recovery is off for this window" notice
 * (fix [1]). Returns true once when a 2nd instance could not acquire the slot lock. */
bool gui_project_take_recovery_busy_notice(char *out, size_t cap) {
    if (!s_recovery_busy_notice) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "Another ntpacker window is open -- crash recovery is off for this one.");
    }
    s_recovery_busy_notice = false;
    return true;
}

#ifdef NTPACKER_GUI_SELFTEST
/* Dev seam (selftest only): attach a FRESH in-memory recovery journal to the CURRENT model and return
 * its io handle (BORROWED -- the model owns the real io) so the fault suite can arm a deterministic
 * write failure (tp_journal_io_memory__fail_next_writes) and exercise the append-fail commit path.
 * Returns a NULL-ctx io on failure. Never used in production (production uses tp_journal_io_file). */
tp_journal_io gui_project__test_attach_memory_journal(void) {
    tp_journal_io io = tp_journal_io_memory();
    if (!io.ctx) {
        return io; /* OOM -> NULL-ctx io */
    }
    tp_error err = {0};
    if (attach_journal_io(s_model, io, &err) != TP_STATUS_OK) { /* fix [4]: the shared ownership dance */
        tp_journal_io none;
        memset(&none, 0, sizeof none);
        return none; /* io/j already destroyed by attach_journal_io */
    }
    return io; /* borrowed handle: arming faults via io.ctx drives the model's owned journal */
}

/* Dev seam (selftest only, fix [1] regression): hold a FOREIGN single-instance lock on `slot` from a
 * SEPARATE handle, simulating another live editor, so a following gui_project_enable_recovery(slot)
 * sees the slot busy and runs journal-less. Returns true if the foreign lock was taken. */
#ifdef _WIN32
static void *s_test_foreign_lock = NULL;
#else
static int s_test_foreign_lock = -1;
#endif
bool gui_project__test_hold_foreign_lock(const char *slot) {
    char lockpath[GUI_RECOVERY_PATH_MAX + 8];
    (void)snprintf(lockpath, sizeof lockpath, "%s.lock", slot);
#ifdef _WIN32
    HANDLE h = CreateFileA(lockpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    s_test_foreign_lock = (void *)h;
    return true;
#else
    int fd = open(lockpath, O_CREAT | O_RDWR, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return false;
    }
    s_test_foreign_lock = fd;
    return true;
#endif
}
void gui_project__test_release_foreign_lock(void) {
#ifdef _WIN32
    if (s_test_foreign_lock != NULL) {
        (void)CloseHandle((HANDLE)s_test_foreign_lock);
        s_test_foreign_lock = NULL;
    }
#else
    if (s_test_foreign_lock >= 0) {
        (void)flock(s_test_foreign_lock, LOCK_UN);
        (void)close(s_test_foreign_lock);
        s_test_foreign_lock = -1;
    }
#endif
}
/* True iff recovery is ACTIVE (slot configured AND this instance owns the lock). */
bool gui_project__test_recovery_active(void) { return recovery_active(); }
/* Dev seam (selftest only): arm a one-shot skip of the next slot reset in attach_recovery_journal, so a
 * pre-seeded stale slot survives to the fresh-attach and the fail-closed empty-check path is reachable
 * (simulates a remove() the OS could not honor -- locked file / read-only dir). */
void gui_project__test_skip_next_recovery_reset(void) { s_test_skip_recovery_reset = true; }
#endif
// #endregion

// #region accessors
tp_project *gui_project_get(void) { return s_proj; }
const char *gui_project_path(void) { return s_path; }
const char *gui_project_display_name(void) { return s_name; }
bool gui_project_has_path(void) { return s_path[0] != '\0'; }
/* Identity-derived dirty, CACHED (#7). tp_model_dirty walks the whole project; this returns the
 * bool recomputed at the identity-change choke points instead of re-hashing every frame. A
 * buffered-but-uncommitted edit is NOT yet in the model identity; the destructive gates
 * (new/open/exit) flush the pending buffer BEFORE calling this (see gui_actions.c) so a pending
 * edit can never be silently discarded (and the flush's commit refreshes the cache). */
bool gui_project_is_dirty(void) { return s_dirty_cache; }
bool gui_project_is_stale(void) { return s_preview_stale; }
/* H/P1-8: true while the live model is crash-recovered unsaved work adopted at init (try_adopt_recovered
 * set recovered_unsaved) and not yet Saved; a Save clears it (tp_model_mark_saved). The queryable form of
 * the condition the startup arg-open guard keys off -- so a stale CLI file arg defers instead of silently
 * discarding the recovered work (J13). More precise than gui_project_is_dirty (recovered work only). */
bool gui_project_has_recovered_unsaved(void) { return s_model && s_model->recovered_unsaved; }
// #endregion

// #region dirty/stale choke point
/* Post-commit choke point: a REAL committed mutation makes the preview stale and bumps the
 * model-version counter. Undo history + dirty are core's now (F2-03 diff + identity), so this no
 * longer serializes a snapshot or recomputes dirty. `act` is vestigial (coalescing moved to the
 * transaction buffer) but kept for call-site clarity + the dev-seam signature. */
void gui_project_touch(gui_action act) {
    (void)act;
    s_preview_stale = true;
    s_model_ver++;
    recompute_dirty(); /* #7: a real committed mutation is the only thing that can flip dirty here */
}

void gui_project_touch_setting(void) { gui_project_touch(GUI_ACT_SET_SETTING); }

void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
unsigned gui_project_model_version(void) { return s_model_ver; }
// #endregion

// #region mutation wrappers (each builds typed op(s) + commits through the model)
/* CONVENTION (UAF class, ADR 0015): these wrappers call gui_project_flush_pending() first, which may
 * commit a buffered gesture whose clone-swap FREES the current project. Any wrapper that then reads a
 * caller-supplied `const char *` which a caller might source FROM the live project (a->name,
 * target.exporter_id, animation.name, ...) MUST dup that string into a local BEFORE the flush -- see
 * gui_project_set_target / gui_project_remove_animation. Callers passing UI buffers, drain-queue copies,
 * or literals are already safe; the sink-side dup makes it not matter which. */
/* True iff any atlas in `p` already carries `name` (exact match) -- the auto-name uniqueness scan. */
static bool atlas_name_in_use(const tp_project *p, const char *name) {
    for (int i = 0; i < p->atlas_count; i++) {
        if (p->atlases[i].name && strcmp(p->atlases[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* True iff any target of any atlas in `p` already writes to `out_path` (exact match). The auto-name
 * scan avoids a NAME whose derived default out_path (out/<name>) is still held by another atlas's
 * target -- core does not validate out_path uniqueness, so a collision is a SILENT export overwrite. */
static bool target_out_path_in_use(const tp_project *p, const char *out_path) {
    for (int i = 0; i < p->atlas_count; i++) {
        const tp_project_atlas *a = &p->atlases[i];
        for (int t = 0; t < a->target_count; t++) {
            if (a->targets[t].out_path && strcmp(a->targets[t].out_path, out_path) == 0) {
                return true;
            }
        }
    }
    return false;
}

int gui_project_add_atlas(void) {
    /* fix2 [3]: a journal-failed flush dropped the buffered gesture (op-error already surfaced) -> abort
     * this structural op too, never pair a silent lost edit with an unrelated committed change. */
    if (!gui_project_flush_pending()) {
        return -1;
    }
    if (!s_proj) {
        return -1;
    }
    /* Lowest FREE atlasN whose NAME and default out_path (out/atlasN) are BOTH unused. atlas_count+1
     * alone collides after a remove (atlas1/2/3, remove atlas1 -> count 2 -> "atlas3" still present) and
     * core rejects the dup name -> Add Atlas would wedge (H/P2-14). But merely reclaiming a freed NAME is
     * not enough: if that atlasN was RENAMED (name freed, its target still out/atlasN), a new atlasN would
     * seed a second target at out/atlasN -- core does not validate out_path uniqueness, so both atlases
     * export to one file (silent overwrite). Scanning both avoids it. Terminates: only finitely many
     * atlasN names / out/atlasN paths exist (matches gui_project_create_animation's auto-name idiom). */
    char name[64];
    for (int n = 1;; n++) {
        (void)snprintf(name, sizeof name, "atlas%d", n);
        char probe_out[576];
        (void)snprintf(probe_out, sizeof probe_out, "out/%s", name);
        if (!atlas_name_in_use(s_proj, name) && !target_out_path_in_use(s_proj, probe_out)) {
            break;
        }
    }
    tp_id128 new_id;
    if (!gen_id(&new_id)) {
        return -1;
    }
    /* ONE transaction: create the atlas AND seed its default json-neotolis target (I1). Both ops go
     * through the diff history so undo removes the whole atlas and redo restores its target too -- the
     * old direct seed_default_target (a non-op mutation) would be LOST on redo (decision 0015). The
     * target op is built by the shared fill_default_target_op helper (#8). */
    tp_operation ops[2];
    memset(ops, 0, sizeof ops);
    ops[0].kind = TP_OP_ATLAS_CREATE;
    ops[0].atlas_id = new_id;
    ops[0].u.atlas_create.name = dupstr(name);
    if (!ops[0].u.atlas_create.name || !fill_default_target_op(&ops[1], new_id, name)) {
        ops_free(ops, 2);
        return -1;
    }
    if (!commit_txn_now(ops, 2)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_ATLAS);
    return tp_project_find_atlas_by_id(s_proj, new_id);
}

/* fix3 [0]: returns TRUE iff the removal actually committed (false on the flush-fail abort, an
 * invalid index, or a commit reject) so the deferred handler shows "Removed X (Ctrl+Z)" + resets
 * selection ONLY on a real removal -- never a false "Removed" over a dropped gesture. */
bool gui_project_remove_atlas(int index) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, index);
    if (!a) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a->id;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_REMOVE_ATLAS);
    return true;
}

/* Build one TP_OP_SOURCE_ADD op (zeroes `op` first, so a false return is safe for tp_operation_free).
 * `path` is stored verbatim -- callers pass a '/'-normalized path (core dedups on the normalized form).
 * Shared by the single- and batch-add wrappers so the two op-build sites never drift (mirrors
 * fill_default_target_op; #8). */
static bool fill_source_add_op(tp_operation *op, tp_id128 atlas_id, const char *path, tp_source_kind kind) {
    memset(op, 0, sizeof *op);
    op->kind = TP_OP_SOURCE_ADD;
    op->atlas_id = atlas_id;
    op->u.source_add.kind = kind;
    op->u.source_add.key = dupstr(path);
    return op->u.source_add.key != NULL && gen_id(&op->u.source_add.source_id);
}

gui_add_status gui_project_add_source_kind(int atlas_index, const char *path, tp_source_kind kind) {
    if (!gui_project_flush_pending()) {
        return GUI_ADD_FAILED; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !path || path[0] == '\0') {
        return GUI_ADD_FAILED;
    }
    if (tp_project_atlas_has_source_path(a, path)) {
        return GUI_ADD_DUPLICATE; /* core rejects a dup path; catch it here (no op) -- no touch, no dirty */
    }
    tp_operation op;
    if (!fill_source_add_op(&op, a->id, path, kind)) {
        tp_operation_free(&op);
        return GUI_ADD_FAILED;
    }
    if (!commit_txn_now(&op, 1)) {
        return GUI_ADD_FAILED;
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_ADD_SOURCE);
    return GUI_ADD_ADDED;
}

gui_add_status gui_project_add_source(int atlas_index, const char *path) {
    /* Folder default: the "Add Folder" dialog and other kind-agnostic callers. The
     * "Add Files" dialog records TP_SOURCE_KIND_FILE via add_source_kind directly. */
    return gui_project_add_source_kind(atlas_index, path, TP_SOURCE_KIND_FOLDER);
}

/* Batch-add multiple sources as ONE atomic transaction (H/P2-13) -- the "Add Files" multi-select path,
 * which previously committed one txn PER file (N undo steps + a mid-batch failure left a partial add).
 * Skips empty paths and any path already in the atlas OR already queued in THIS batch (both counted into
 * *out_dup), so the committed txn holds only DISTINCT new sources and never self-rejects on a duplicate.
 * Commits nothing when nothing is new. Returns true iff the txn committed (or was a clean no-op); false
 * on flush-fail / OOM / a core reject (the model is then byte-unchanged). Both out-counts are always set
 * (0 on early failure). One commit -> ONE undo step for the whole multi-select. */
bool gui_project_add_sources(int atlas_index, const char *const *paths, int n_paths, tp_source_kind kind,
                             int *out_added, int *out_dup) {
    if (out_added) {
        *out_added = 0;
    }
    if (out_dup) {
        *out_dup = 0;
    }
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: a journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || n_paths <= 0 || !paths) {
        return false;
    }
    tp_operation *ops = (tp_operation *)calloc((size_t)n_paths, sizeof *ops);
    if (!ops) {
        return false;
    }
    int m = 0; /* distinct new sources queued */
    int dup = 0;
    for (int i = 0; i < n_paths; i++) {
        const char *path = paths[i];
        if (!path || path[0] == '\0') {
            continue;
        }
        if (tp_project_atlas_has_source_path(a, path)) {
            dup++;
            continue; /* already in the atlas (core would reject the dup) */
        }
        bool queued = false; /* de-dup WITHIN the batch: two identical selections must not both add */
        for (int j = 0; j < m; j++) {
            if (strcmp(ops[j].u.source_add.key, path) == 0) {
                queued = true;
                break;
            }
        }
        if (queued) {
            dup++;
            continue;
        }
        if (!fill_source_add_op(&ops[m], a->id, path, kind)) {
            ops_free(ops, m + 1); /* free the fully-built arms + this partial one */
            free(ops);
            if (out_dup) {
                *out_dup = dup; /* preserve the dup tally counted before this OOM/RNG fault */
            }
            return false;
        }
        m++;
    }
    bool ok = true;
    if (m > 0) {
        ok = commit_txn_now(ops, m); /* ONE transaction -> ONE undo step; ALWAYS frees the op arms */
        if (ok) {
            gui_scan_invalidate_all();
            gui_project_touch(GUI_ACT_ADD_SOURCE);
        }
    } else {
        ops_free(ops, 0); /* nothing queued (all dup/empty): no commit, no dirty */
    }
    free(ops); /* commit_txn_now consumed the arms; the array itself is ours */
    if (out_added) {
        *out_added = ok ? m : 0;
    }
    if (out_dup) {
        *out_dup = dup;
    }
    return ok;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_source(int atlas_index, int source_index) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || source_index < 0 || source_index >= a->source_count) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a->id;
    op.u.source_ref.source_id = a->sources[source_index].id;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_REMOVE_SOURCE);
    return true;
}

bool gui_project_set_atlas_name(int atlas_index, const char *name) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !name || name[0] == '\0') {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a->id;
    op.u.atlas_rename.name = dupstr(name);
    if (!op.u.atlas_rename.name) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_ATLAS);
    return true;
}

bool gui_project_set_sprite_rename(int atlas_index, const char *sprite_name, const char *rename) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    /* PENDING (name-keyed) override: nil source_id + verbatim key, exactly the CLI's
     * source-less sprite path (decision 0014 F2). The GUI's sprite_name IS the ext-stripped
     * export key, so verbatim == the export-key bridge. An empty/NULL rename clears it. */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_NAME_SET;
    op.atlas_id = a->id;
    op.u.sprite_name.source_id = tp_id128_nil();
    op.u.sprite_name.src_key = dupstr(sprite_name);
    op.u.sprite_name.name = (rename && rename[0]) ? dupstr(rename) : NULL;
    if (!op.u.sprite_name.src_key || ((rename && rename[0]) && !op.u.sprite_name.name)) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_SPRITE);
    return true;
}

/* Maps a gui_atlas_field to its op mask bit + fills the matching payload field. */
static bool fill_atlas_knob(tp_op_atlas_settings *s, gui_atlas_field f, int iv, float fv) {
    switch (f) {
        case GUI_ATLAS_MAX_SIZE: s->max_size = iv; s->mask = TP_AF_MAX_SIZE; return true;
        case GUI_ATLAS_PADDING: s->padding = iv; s->mask = TP_AF_PADDING; return true;
        case GUI_ATLAS_MARGIN: s->margin = iv; s->mask = TP_AF_MARGIN; return true;
        case GUI_ATLAS_EXTRUDE: s->extrude = iv; s->mask = TP_AF_EXTRUDE; return true;
        case GUI_ATLAS_ALPHA_THRESHOLD: s->alpha_threshold = iv; s->mask = TP_AF_ALPHA_THRESHOLD; return true;
        case GUI_ATLAS_MAX_VERTICES: s->max_vertices = iv; s->mask = TP_AF_MAX_VERTICES; return true;
        case GUI_ATLAS_SHAPE: s->shape = iv; s->mask = TP_AF_SHAPE; return true;
        case GUI_ATLAS_ALLOW_TRANSFORM: s->allow_transform = (iv != 0); s->mask = TP_AF_ALLOW_TRANSFORM; return true;
        case GUI_ATLAS_POWER_OF_TWO: s->power_of_two = (iv != 0); s->mask = TP_AF_POWER_OF_TWO; return true;
        case GUI_ATLAS_PIXELS_PER_UNIT: s->pixels_per_unit = fv; s->mask = TP_AF_PIXELS_PER_UNIT; return true;
    }
    return false;
}

bool gui_project_set_atlas_setting(int atlas_index, gui_atlas_field field, int ivalue, float fvalue) {
    coalesce_key ck = make_key(CK_ATLAS_SETTING, atlas_index, (int)field, -1, "");
    pending_route(&ck); /* flush a different knob's pending BEFORE reading this atlas */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a->id;
    if (!fill_atlas_knob(&op.u.atlas_settings, field, ivalue, fvalue)) {
        tp_operation_free(&op);
        return false;
    }
    return pending_offer(&ck, &op, GUI_ACT_SET_SETTING);
}

/* Buffers a sprite.override.set on the PENDING (name-keyed) record for `sprite_name`: nil
 * source_id + verbatim key (== the export-key bridge). Core applies the masked fields on commit
 * then prunes an all-default record. The caller has already run pending_route(k). */
static bool sprite_override_offer(int atlas_index, const char *sprite_name, tp_op_sprite_set payload,
                                  const coalesce_key *k) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = a->id;
    op.u.sprite_set = payload;
    op.u.sprite_set.source_id = tp_id128_nil();
    op.u.sprite_set.src_key = dupstr(sprite_name);
    if (!op.u.sprite_set.src_key) {
        tp_operation_free(&op);
        return false;
    }
    return pending_offer(k, &op, GUI_ACT_SET_SETTING);
}

bool gui_project_set_sprite_origin(int atlas_index, const char *sprite_name, int axis, float value) {
    if (axis < 0 || axis > 1) {
        return false; /* 0 = Pivot X, 1 = Pivot Y */
    }
    /* Component-precise key (mirror slice9): X and Y are DIFFERENT keys, so editing the OTHER axis
     * flushes the buffered one FIRST -- then the read-modify-write seed below reads the COMMITTED
     * value of the non-edited component and the two components can never merge against a stale model.
     * (The pre-fix code keyed both axes the same AND seeded from a view-side committed read, so a
     * back-to-back X then Y replaced {x=new,y=old} with {x=old,y=new} and silently lost the X edit.) */
    coalesce_key ck = make_key(CK_SPRITE_ORIGIN, atlas_index, axis, -1, sprite_name);
    pending_route(&ck);
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_ORIGIN;
    p.origin_x = (axis == 0) ? value : (ov ? ov->origin_x : TP_PROJECT_ORIGIN_DEFAULT);
    p.origin_y = (axis == 1) ? value : (ov ? ov->origin_y : TP_PROJECT_ORIGIN_DEFAULT);
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

bool gui_project_set_sprite_slice9(int atlas_index, const char *sprite_name, int lrtb_index, int value) {
    if (lrtb_index < 0 || lrtb_index >= 4) {
        return false;
    }
    /* Field-precise key: the component index. A different-component edit therefore has a
     * different key, so pending_route flushes the prior component's pending BEFORE the RMW seed
     * below reads the model -> the seed carries the committed value of every OTHER component and
     * two components can never merge against a stale model (the RMW lost-edit is impossible). */
    coalesce_key ck = make_key(CK_SPRITE_SLICE9, atlas_index, lrtb_index, -1, sprite_name);
    pending_route(&ck);
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_SLICE9;
    for (int comp = 0; comp < 4; comp++) {
        p.slice9[comp] = ov ? ov->slice9_lrtb[comp] : 0;
    }
    p.slice9[lrtb_index] = (uint16_t)(value < 0 ? 0 : value);
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

bool gui_project_set_sprite_override(int atlas_index, const char *sprite_name, gui_sprite_ov which, int value) {
    coalesce_key ck = make_key(CK_SPRITE_OVERRIDE, atlas_index, (int)which, -1, sprite_name);
    pending_route(&ck);
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    const int16_t v = (int16_t)value; /* value may be TP_PROJECT_OV_INHERIT to clear the field */
    switch (which) {
        case GUI_SPRITE_OV_SHAPE: p.mask = TP_SPF_SHAPE; p.ov_shape = v; break;
        case GUI_SPRITE_OV_ROTATE: p.mask = TP_SPF_ALLOW_ROTATE; p.ov_allow_rotate = v; break;
        case GUI_SPRITE_OV_MAXVERT: p.mask = TP_SPF_MAX_VERTICES; p.ov_max_vertices = v; break;
        case GUI_SPRITE_OV_MARGIN: p.mask = TP_SPF_MARGIN; p.ov_margin = v; break;
        case GUI_SPRITE_OV_EXTRUDE: p.mask = TP_SPF_EXTRUDE; p.ov_extrude = v; break;
        default: return false;
    }
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

int gui_project_add_target(int atlas_index) {
    if (!gui_project_flush_pending()) {
        return -1; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    /* target.create op for the default json-neotolis target (mirrors seed_default_target's exporter +
     * "out/<name>" path). An OP (not the lifecycle seed) so the added target is captured in the diff
     * history and Undo removes exactly this target -- a direct seed leaves no undo step, so Ctrl+Z would
     * revert the WRONG (prior) edit (decision 0015). */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return -1;
    }
    tp_operation op;
    if (!fill_default_target_op(&op, a->id, a->name)) { /* shared default-target builder (#8) */
        tp_operation_free(&op);
        return -1;
    }
    if (!commit_txn_now(&op, 1)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_TARGET);
    a = tp_project_get_atlas(s_proj, atlas_index);
    return a ? a->target_count - 1 : -1;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_target(int atlas_index, int index) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_REMOVE;
    op.atlas_id = a->id;
    op.u.target_ref.target_id = a->targets[index].id;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_REMOVE_TARGET);
    return true;
}

bool gui_project_set_target(int atlas_index, int index, const char *exporter_id, const char *out_path, bool enabled) {
    /* F2-05b-ii-A fix (browse-target UAF): dup the caller's strings BEFORE flushing. exporter_id/out_path
     * may point INTO the live project (do_browse_target_at passes t->exporter_id, the export-dialog toggle
     * passes t0->exporter_id/out_path); gui_project_flush_pending() can commit a buffered gesture whose
     * clone-swap frees that project, dangling the pointers before dupstr would read them. Capturing at the
     * sink first inoculates EVERY caller, not just the one path the reviewer flagged. */
    char *exp = dupstr(exporter_id);
    char *outp = dupstr(out_path);
    /* fix2 [3]: same class as the other structural wrappers -- a journal-failed flush dropped the
     * buffered gesture (op-error surfaced), so abort THIS target edit too (freeing the pre-dup'd
     * strings) instead of pairing a lost edit with an unrelated target change. */
    if (!gui_project_flush_pending()) {
        free(exp);
        free(outp);
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count || !exp || !outp) {
        free(exp);
        free(outp);
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = a->id;
    op.u.target_set.target_id = a->targets[index].id;
    op.u.target_set.enabled = enabled;
    op.u.target_set.exporter_id = exp;  /* ownership transfers to op -> commit_txn_now frees it */
    op.u.target_set.out_path = outp;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_TARGET);
    return true;
}

/* H/G3: COALESCABLE out-path-only setter for the export-target path text field. Discrete target edits
 * (enabled checkbox / exporter dropdown / browse pick) stay immediate via gui_project_set_target above;
 * the free-text out-path field, however, fired one gui_project_set_target per keystroke -> one committed
 * TP_OP_TARGET_SET per keystroke = undo spam. Buffering it under a per-target key (field = index) makes
 * the field's existing Enter/blur gesture-commit flush the whole edit as ONE undo step -- mirrors the
 * atlas-settings path (gui_project_set_atlas_setting). TP_OP_TARGET_SET replaces exporter_id+out_path+
 * enabled ATOMICALLY, so RMW-seed exporter_id + enabled from the COMMITTED record read AFTER pending_route
 * (post-route -> no UAF; contrast gui_project_set_target, which had to dup CALLER pointers BEFORE its
 * flush). Switching to a different target index is a different key -> pending_route flushes -> a correct
 * one-undo-per-target boundary. */
bool gui_project_set_target_out_path(int atlas_index, int index, const char *out_path) {
    coalesce_key ck = make_key(CK_TARGET_OUTPATH, atlas_index, index, -1, "");
    pending_route(&ck); /* flush a DIFFERENT key (other target / other knob) BEFORE reading this target */
    /* NEVER buffer an EMPTY out_path: core (tp_op_validate) REJECTS out_path=="" -- buffering it would (a)
     * flush-reject at the gesture boundary and (b) break a following discrete edit whose flush-first would
     * then fail. The committed record must stay non-empty, exactly as pre-G3 (every empty keystroke's commit
     * was rejected). Clearing the field DISCARDS any same-key pending -> the field resyncs to the last
     * committed path; a discrete edit then always reads a valid record. After pending_route a still-valid
     * pending is necessarily THIS same key (a different key was just flushed). */
    if (!out_path || out_path[0] == '\0') {
        if (s_pending_valid) {
            pending_discard();
        }
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index); /* read AFTER route */
    if (!a || index < 0 || index >= a->target_count) {
        return false;
    }
    tp_project_target *t = &a->targets[index];
    char *exp = dupstr(t->exporter_id);        /* RMW seed: unchanged exporter (dup: commit/replace frees the arms) */
    char *outp = dupstr(out_path);             /* guaranteed non-empty by the guard above */
    if (!exp || !outp) {
        free(exp);
        free(outp);
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = a->id;
    op.u.target_set.target_id = t->id;
    op.u.target_set.enabled = t->enabled;      /* RMW seed: unchanged */
    op.u.target_set.exporter_id = exp;         /* ownership transfers to op */
    op.u.target_set.out_path = outp;
    return pending_offer(&ck, &op, GUI_ACT_SET_TARGET);
}

/* H/G3: discrete target-field setters (enabled toggle / exporter change). IMMEDIATE (one undo step each).
 * Unlike a raw gui_project_set_target from the view, they do NOT take the un-edited fields from a STALE
 * view snapshot: they FLUSH any buffered out-path gesture FIRST (so the just-typed path is committed), then
 * read the un-edited fields from the NOW-committed record and delegate to gui_project_set_target. This closes
 * the hazard G3 introduced -- a discrete widget re-sending the stale committed out_path would commit the
 * buffered typed value then overwrite it back, silently reverting the user's typed path. gui_project_set_
 * target's own pre-flush dups run against a stable s_proj (nothing is pending after our flush) so it is
 * UAF-safe and its internal flush is a no-op. An empty out_path is never buffered (gui_project_set_target_
 * out_path guards it away), so this flush never rejects on emptiness -- only a genuine journal failure returns
 * false, which correctly aborts the discrete edit too. */
bool gui_project_set_target_enabled(int atlas_index, int index, bool enabled) {
    if (!gui_project_flush_pending()) { /* commit any buffered out-path gesture FIRST (fresh committed read) */
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return false;
    }
    const tp_project_target *t = &a->targets[index]; /* read AFTER flush -> current exporter + out_path */
    return gui_project_set_target(atlas_index, index, t->exporter_id, t->out_path, enabled);
}

bool gui_project_set_target_exporter(int atlas_index, int index, const char *exporter_id) {
    if (!gui_project_flush_pending()) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return false;
    }
    const tp_project_target *t = &a->targets[index]; /* read AFTER flush -> current out_path + enabled */
    return gui_project_set_target(atlas_index, index, exporter_id, t->out_path, t->enabled);
}
// #endregion

// #region animations
static tp_project_anim *anim_at(int atlas_index, int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return NULL;
    }
    return &a->animations[anim_index];
}

/* The atlas's animation named `name` (exact), or NULL. */
static tp_project_anim *find_anim_by_name(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return NULL;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (a->animations[i].name && strcmp(a->animations[i].name, name) == 0) {
            return &a->animations[i];
        }
    }
    return NULL;
}

bool gui_project_anim_id_exists(int atlas_index, const char *id) {
    return find_anim_by_name(tp_project_get_atlas(s_proj, atlas_index), id) != NULL;
}

int gui_project_create_animation(int atlas_index, const char *base, const char *const *frames, int frame_count) {
    if (!gui_project_flush_pending()) {
        return -1; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return -1;
    }
    /* unique id (CLIENT naming policy): prefer `base` verbatim, else base"2"/"3"...; a
     * NULL/empty base auto-names "animN". Core has no dup-name rejection for animations. */
    char id[128];
    if (base && base[0]) {
        (void)snprintf(id, sizeof id, "%s", base);
        for (int n = 2; gui_project_anim_id_exists(atlas_index, id); n++) {
            (void)snprintf(id, sizeof id, "%s%d", base, n);
        }
    } else {
        for (int n = 1;; n++) {
            (void)snprintf(id, sizeof id, "anim%d", n);
            if (!gui_project_anim_id_exists(atlas_index, id)) {
                break;
            }
        }
    }
    tp_id128 anim_id;
    if (!gen_id(&anim_id)) {
        return -1;
    }
    int nframes = 0;
    for (int i = 0; frames && i < frame_count; i++) {
        if (frames[i] && frames[i][0]) {
            nframes++;
        }
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = a->id;
    op.u.anim_create.anim_id = anim_id;
    op.u.anim_create.name = dupstr(id);
    op.u.anim_create.fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    op.u.anim_create.playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    op.u.anim_create.flip_h = false;
    op.u.anim_create.flip_v = false;
    op.u.anim_create.frame_count = nframes;
    bool bad = (op.u.anim_create.name == NULL);
    if (!bad && nframes > 0) {
        op.u.anim_create.frames = (char **)calloc((size_t)nframes, sizeof(char *));
        if (!op.u.anim_create.frames) {
            bad = true;
        } else {
            int w = 0;
            for (int i = 0; frames && i < frame_count && !bad; i++) {
                if (frames[i] && frames[i][0]) {
                    op.u.anim_create.frames[w] = dupstr(frames[i]);
                    if (!op.u.anim_create.frames[w]) {
                        bad = true;
                    }
                    w++;
                }
            }
        }
    }
    if (bad) {
        tp_operation_free(&op);
        return -1;
    }
    if (!commit_txn_now(&op, 1)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_ANIM);
    /* Re-fetch the (clone-swapped) atlas to report the appended animation's index. */
    a = tp_project_get_atlas(s_proj, atlas_index);
    return (a && tp_project_atlas_find_animation_by_id(a, anim_id)) ? (a->animation_count - 1) : -1;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). The deferred
 * handler guards preview_stop + the s_sel_anim reset + "Removed" message on this. */
bool gui_project_remove_animation(int atlas_index, const char *id) {
    /* F2-05b-ii-A fix (sibling-sink UAF, same class as gui_project_set_target): dup `id` BEFORE the
     * flush. The production caller (the do-remove-animation handler) passes a->animations[i].name -- a
     * pointer INTO the live project; gui_project_flush_pending() can commit a buffered gesture whose
     * clone-swap frees that project, dangling `id` before find_anim_by_name reads it. `an->id` (used to
     * build the op) is re-resolved from the post-flush project, so only the incoming `id` needs saving. */
    char *idc = dupstr(id);
    if (!gui_project_flush_pending()) {
        free(idc); /* fix2 [3]: journal-failed flush dropped the gesture -> abort (free the pre-dup, op-error surfaced) */
        return false;
    }
    bool ok = false;
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (a && idc) {
        tp_project_anim *an = find_anim_by_name(a, idc);
        if (an) {
            tp_operation op;
            memset(&op, 0, sizeof op);
            op.kind = TP_OP_ANIMATION_REMOVE;
            op.atlas_id = a->id;
            op.u.anim_ref.anim_id = an->id;
            if (commit_txn_now(&op, 1)) {
                gui_project_touch(GUI_ACT_REMOVE_ANIM);
                ok = true;
            }
        }
    }
    free(idc);
    return ok;
}

bool gui_project_set_anim_id(int atlas_index, int anim_index, const char *new_id) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !new_id || new_id[0] == '\0') {
        return false;
    }
    if (an->name && strcmp(an->name, new_id) == 0) {
        return true; /* no-op: avoid a needless undo step (rename-to-own-name) */
    }
    /* H/P1-2: animation rename is now a first-class op -- undoable + journaled + crash-safe.
     * Mirrors gui_project_set_atlas_name; the name-uniqueness policy lives in core validate now,
     * so a collision surfaces through commit_txn_now's op-error channel (no client clash check). */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_RENAME;
    op.atlas_id = a->id;
    op.u.anim_rename.anim_id = an->id;
    op.u.anim_rename.name = dupstr(new_id);
    if (!op.u.anim_rename.name) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_ANIM);
    return true;
}

/* Buffers one animation.settings.set under `k` (the caller has run pending_route). */
static bool anim_settings_offer(int atlas_index, int anim_index, uint32_t mask, float fps, int playback, bool flip_h,
                                bool flip_v, const coalesce_key *k) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = a->id;
    op.u.anim_settings.anim_id = an->id;
    op.u.anim_settings.mask = mask;
    op.u.anim_settings.fps = fps;
    op.u.anim_settings.playback = playback;
    op.u.anim_settings.flip_h = flip_h;
    op.u.anim_settings.flip_v = flip_v;
    return pending_offer(k, &op, GUI_ACT_SET_ANIM);
}

bool gui_project_set_anim_fps(int atlas_index, int anim_index, float fps) {
    coalesce_key ck = make_key(CK_ANIM_FPS, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!(fps >= 1.0F)) {
        fps = 1.0F; /* widget parse-clamp (>=1); core also range-checks (>0 finite) on commit */
    }
    if (!s_pending_valid && an->fps == fps) {
        return true; /* safe no-op: nothing buffered for this key and the committed value matches */
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_FPS, fps, an->playback, an->flip_h, an->flip_v, &ck);
}

bool gui_project_set_anim_playback(int atlas_index, int anim_index, int playback) {
    coalesce_key ck = make_key(CK_ANIM_PLAYBACK, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (playback < 0) {
        playback = 0;
    }
    if (playback > 6) {
        playback = 6;
    }
    if (!s_pending_valid && an->playback == playback) {
        return true;
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_PLAYBACK, an->fps, playback, an->flip_h, an->flip_v, &ck);
}

bool gui_project_set_anim_flip(int atlas_index, int anim_index, bool flip_h, bool flip_v) {
    coalesce_key ck = make_key(CK_ANIM_FLIP, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!s_pending_valid && an->flip_h == flip_h && an->flip_v == flip_v) {
        return true;
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_FLIP_H | TP_ANF_FLIP_V, an->fps, an->playback, flip_h,
                               flip_v, &ck);
}

bool gui_project_anim_add_frames(int atlas_index, int anim_index, const char *const *frames, int count) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !frames || count <= 0) {
        return false;
    }
    tp_id128 anim_id = an->id;
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_id128 aid = a->id;
    tp_operation *ops = (tp_operation *)calloc((size_t)count, sizeof *ops);
    if (!ops) {
        return false;
    }
    int n = 0;
    bool oom = false;
    for (int i = 0; i < count && !oom; i++) {
        if (!(frames[i] && frames[i][0])) {
            continue;
        }
        tp_operation *op = &ops[n];
        op->kind = TP_OP_ANIMATION_FRAME_ADD;
        op->atlas_id = aid;
        op->u.anim_frame_add.anim_id = anim_id;
        op->u.anim_frame_add.frame = dupstr(frames[i]);
        op->u.anim_frame_add.index = -1; /* append */
        if (!op->u.anim_frame_add.frame) {
            oom = true;
            break;
        }
        n++;
    }
    if (oom || n == 0) {
        ops_free(ops, n);
        free(ops);
        return false;
    }
    bool ok = commit_txn_now(ops, n); /* frees the op arms */
    free(ops);
    if (ok) {
        gui_project_touch(GUI_ACT_ANIM_FRAMES);
    }
    return ok;
}

bool gui_project_anim_remove_frame(int atlas_index, int anim_index, int frame_index) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || frame_index < 0 || frame_index >= an->frame_count) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = a->id;
    op.u.anim_frame_rm.anim_id = an->id;
    op.u.anim_frame_rm.index = frame_index;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}

bool gui_project_anim_move_frame(int atlas_index, int anim_index, int frame_index, int delta) {
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || frame_index < 0 || frame_index >= an->frame_count) {
        return false;
    }
    int to = frame_index + delta; /* op addresses (from,to) absolute; core clamps `to` into range */
    if (to < 0) {
        to = 0;
    }
    if (to >= an->frame_count) {
        to = an->frame_count - 1;
    }
    if (to == frame_index) {
        return true; /* no-op move (edge button): skip commit, as before */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = a->id;
    op.u.anim_frame_move.anim_id = an->id;
    op.u.anim_frame_move.from_index = frame_index;
    op.u.anim_frame_move.to_index = to;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}
// #endregion

// #region undo / redo (F2-03 diff history)
/* A pending buffered edit counts as undoable (undo flushes it into a step, then reverts it). */
bool gui_project_can_undo(void) { return s_pending_valid || tp_model_can_undo(s_model); }
bool gui_project_can_redo(void) { return tp_model_can_redo(s_model); }
int gui_project_undo_depth(void) { return tp_model_undo_depth(s_model); }
int gui_project_redo_depth(void) { return tp_model_redo_depth(s_model); }

/* Undo reverses the most recent committed transaction via its captured semantic diff; the model
 * clone-swaps m->project, so refresh s_proj + re-resolve cached pointers exactly as commit does.
 * A buffered gesture is committed FIRST (its own step) so Ctrl+Z reverts the in-flight drag.
 * Dirty is identity-derived, so an undo back to the saved baseline reads clean. */
bool gui_project_undo(void) {
    if (!gui_project_flush_pending()) {
        return false; /* fix [3]: the buffered gesture could not commit (journal failed) -- do NOT
                       * then undo a DIFFERENT (older) step; the op-error is already surfaced. */
    }
    tp_error e = {0};
    if (tp_model_undo(s_model, &e) != TP_STATUS_OK) {
        return false;
    }
    s_proj = tp_model_project(s_model); /* the model swapped its project on undo */
    s_preview_stale = true;             /* restored model != last-packed; packing is blocked -> always stale */
    recompute_dirty();                  /* #7: undo back to the saved baseline reads clean by identity */
    gui_scan_invalidate_all();
    return true;
}

bool gui_project_redo(void) {
    if (!gui_project_flush_pending()) {
        return false; /* fix [3]: a journal-failed flush must not silently proceed into a redo */
    }
    tp_error e = {0};
    if (tp_model_redo(s_model, &e) != TP_STATUS_OK) {
        return false;
    }
    s_proj = tp_model_project(s_model);
    s_preview_stale = true;
    recompute_dirty(); /* #7: identity moved -> refresh the cached dirty */
    gui_scan_invalidate_all();
    return true;
}
// #endregion

// #region file operations
bool gui_project_new(void) {
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    tp_project *fresh = tp_project_create();
    if (!fresh) {
        return false;
    }
    seed_default_target(fresh, 0); /* fresh GUI project exports something (I1) -- lifecycle, direct */
    if (!wrap_model(fresh)) {
        return false; /* OOM: wrap_model freed fresh + kept the CURRENT project intact (F3) -- no data loss */
    }
    set_path("");
    s_preview_stale = false;
    gui_scan_invalidate_all();
    promote_and_baseline();
    return true;
}

tp_status gui_project_open(const char *path, char *err_out, size_t err_cap) {
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    tp_error err = {0};
    tp_project *loaded = NULL;
    tp_status st = tp_project_load(path, &loaded, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    if (!wrap_model(loaded)) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "out of memory wrapping the loaded project");
        }
        return TP_STATUS_OOM;
    }
    set_path(path);
    s_preview_stale = true; /* nothing packed this session yet */
    gui_scan_invalidate_all();
    promote_and_baseline();
    return TP_STATUS_OK;
}

tp_status gui_project_save(char *err_out, size_t err_cap) {
    if (s_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no path (use Save As)");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return gui_project_save_as(s_path, err_out, err_cap);
}

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    /* fix [3]: the buffered gesture must be DURABLY committed before we persist. If its commit fails
     * (e.g. a journal append failure -- full disk), the edit is NOT in the model, so writing the file
     * + mark_saved would produce a file missing the edit AND a false "saved"/clean state (silent data
     * loss). ABORT: do not save, do not mark_saved; surface the reason. The model stays dirty. */
    if (!gui_project_flush_pending()) {
        gui_project_flush_error(err_out, err_cap); /* fix3 [2]: shared neutral wording (save/pack/gate) */
        return TP_STATUS_JOURNAL_FAILED;
    }
    tp_error err = {0};
    /* Promote to final random ids BEFORE writing (§5.5); on OS-RNG failure promote returns
     * RNG_FAILED with every id left nil, so persisting now would write a nil-id file that
     * fails on reload. Fail closed -- report and return WITHOUT saving. */
    tp_status ids = ensure_ids(&err);
    if (ids != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(ids));
        }
        return ids;
    }
    tp_status st = tp_project_save(s_proj, path, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    set_path(path);
    /* Re-baseline the identity anchor to the just-saved (possibly relativized) in-memory form.
     * Save may relativize absolute sources in place -> that identity IS the clean baseline; the
     * revision + undo history are preserved (§8/§420). */
    tp_model_mark_saved(s_model);
    recompute_dirty(); /* #7: the just-saved identity is the new clean baseline */
    return TP_STATUS_OK;
}
// #endregion
