#include "tp_core/tp_scan.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_cancel.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_srckey.h"
#include "tp_fs_internal.h"

// #region growable entry vector (scan-local)
typedef struct scan_vec {
    tp_scan_entry *data;
    int count;
    int cap;
} scan_vec;

/* Test-only allocation seam. Production leaves it disabled (-1). Keeping the
 * seam in this module lets the atomic-result contract be swept without replacing
 * the process allocator. */
static _Thread_local int s_scan_alloc_fail = -1;
static _Thread_local int s_scan_stat_error;
static _Thread_local bool s_scan_sort_started;
static _Thread_local bool s_scan_sort_finished;

void tp_scan__test_set_alloc_fail(int nth) { s_scan_alloc_fail = nth; }
void tp_scan__test_set_stat_error(int error) { s_scan_stat_error = error; }
void tp_scan__test_reset_sort_finished(void) {
    s_scan_sort_started = false;
    s_scan_sort_finished = false;
}
bool tp_scan__test_sort_started(void) { return s_scan_sort_started; }
bool tp_scan__test_sort_finished(void) { return s_scan_sort_finished; }

static bool scan_should_fail_alloc(void) {
    if (s_scan_alloc_fail < 0) {
        return false;
    }
    if (s_scan_alloc_fail == 0) {
        s_scan_alloc_fail = -1;
        return true;
    }
    s_scan_alloc_fail--;
    return false;
}

/* Test-only walk gate. When armed, the FIRST top-level walk publishes that it has
 * ENTERED the walk, then busy-waits until the test RELEASES it. Unlike the
 * thread-local alloc-fail seam this is CROSS-thread (global atomics): it lets an
 * async-job test prove the folder walk runs on the pack WORKER -- start() returns
 * while the worker is parked here -- and drive a mid-walk cancel deterministically.
 * Production never arms it (the atomics stay zero), so scan_gate_wait() is a no-op. */
static atomic_int s_scan_gate_armed;    /* 1 while a test wants the next walk to park */
static atomic_int s_scan_gate_entered;  /* set by the walk once it reaches the gate */
static atomic_int s_scan_gate_released; /* set by the test to let the walk proceed */

void tp_scan__test_arm_walk_gate(void) {
    atomic_store(&s_scan_gate_entered, 0);
    atomic_store(&s_scan_gate_released, 0);
    atomic_store(&s_scan_gate_armed, 1);
}

bool tp_scan__test_walk_gate_entered(void) {
    return atomic_load(&s_scan_gate_entered) != 0;
}

void tp_scan__test_release_walk_gate(void) {
    /* Also disarm so a test that armed but never walked cannot park a later scan. */
    atomic_store(&s_scan_gate_armed, 0);
    atomic_store(&s_scan_gate_released, 1);
}

static void scan_gate_wait(void) {
    if (atomic_load(&s_scan_gate_armed) == 0) {
        return; /* production / not armed: no-op */
    }
    atomic_store(&s_scan_gate_armed, 0); /* one-shot */
    atomic_store(&s_scan_gate_entered, 1);
    /* Busy-wait: the test releases within microseconds; on a single core the
     * scheduler quantum still breaks the spin. Reached only when a test armed it. */
    while (atomic_load(&s_scan_gate_released) == 0) {
    }
}

/* Test-only post-entry gate. It parks after the first visited entry and counts
 * later visits so cancellation tests can distinguish an early stop from a walk
 * that completed before observing cancellation. Production never arms it. */
static atomic_int s_scan_post_armed;    /* 1 = counting active for the armed scan */
static atomic_int s_scan_post_park;     /* 1 = park at the FIRST counted entry (one-shot) */
static atomic_int s_scan_post_entered;  /* set once the walk parks in the gate */
static atomic_int s_scan_post_released; /* set by the test to unpark the walk */
static atomic_int s_scan_visited;       /* entries visited since the last arm */

void tp_scan__test_arm_post_entry_gate(void) {
    atomic_store(&s_scan_visited, 0);
    atomic_store(&s_scan_post_entered, 0);
    atomic_store(&s_scan_post_released, 0);
    atomic_store(&s_scan_post_park, 1);
    atomic_store(&s_scan_post_armed, 1);
}

bool tp_scan__test_post_entry_gate_entered(void) {
    return atomic_load(&s_scan_post_entered) != 0;
}

void tp_scan__test_release_post_entry_gate(void) {
    /* Clear only the one-shot PARK (so a test that armed but never walked cannot park a
     * later scan); deliberately leave counting ARMED so that if the loop-top cancel poll
     * is ever deleted, the resumed walk keeps visiting and the counter climbs to N --
     * which is exactly what makes the mid-scan-cancel test fail. */
    atomic_store(&s_scan_post_park, 0);
    atomic_store(&s_scan_post_released, 1);
}

int tp_scan__test_visited_entries(void) {
    return atomic_load(&s_scan_visited);
}

static void scan_post_entry_gate(void) {
    if (atomic_load(&s_scan_post_armed) == 0) {
        return; /* production / not armed: no-op (mirrors scan_gate_wait) */
    }
    atomic_fetch_add(&s_scan_visited, 1);
    if (atomic_load(&s_scan_post_park) == 0) {
        return; /* park was one-shot on the first entry; keep only counting */
    }
    atomic_store(&s_scan_post_park, 0); /* one-shot */
    atomic_store(&s_scan_post_entered, 1);
    /* Busy-wait until the test releases (it requests cancel first, then releases). */
    while (atomic_load(&s_scan_post_released) == 0) {
    }
}

static void *scan_alloc(size_t size) {
    return scan_should_fail_alloc() ? NULL : malloc(size);
}

static void *scan_realloc(void *memory, size_t size) {
    return scan_should_fail_alloc() ? NULL : realloc(memory, size);
}

static tp_status scan_vec_push_owned(scan_vec *v, char **rel, char **abs,
                                     long long size, long long mtime,
                                     tp_error *err) {
    if (v->count == v->cap) {
        if (v->cap > INT_MAX / 2) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "directory scan has too many image entries");
        }
        int ncap = (v->cap == 0) ? 32 : v->cap * 2;
        if ((size_t)ncap > SIZE_MAX / sizeof *v->data) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "directory scan entry table overflows size_t");
        }
        tp_scan_entry *nd =
            (tp_scan_entry *)scan_realloc(v->data,
                                          (size_t)ncap * sizeof *nd);
        if (!nd) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "directory scan entry allocation failed");
        }
        v->data = nd;
        v->cap = ncap;
    }
    tp_scan_entry *e = &v->data[v->count];
    e->rel = *rel;
    e->abs = *abs;
    e->size = size;
    e->mtime = mtime;
    *rel = NULL;
    *abs = NULL;
    v->count++;
    return TP_STATUS_OK;
}

static void scan_vec_drop(scan_vec *v) {
    if (!v) {
        return;
    }
    for (int i = 0; i < v->count; ++i) {
        free(v->data[i].rel);
        free(v->data[i].abs);
    }
    free(v->data);
    memset(v, 0, sizeof *v);
}
// #endregion

// #region helpers
static bool has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    /* case-insensitive compare against the accepted set */
    static const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        const char *a = dot;
        const char *b = exts[i];
        bool eq = true;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') {
                ca = (char)(ca - 'A' + 'a');
            }
            if (ca != cb) {
                eq = false;
                break;
            }
            a++;
            b++;
        }
        if (eq && *a == '\0' && *b == '\0') {
            return true;
        }
    }
    return false;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const tp_scan_entry *)a)->rel, ((const tp_scan_entry *)b)->rel);
}

static void entry_swap(tp_scan_entry *a, tp_scan_entry *b) {
    const tp_scan_entry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void entry_heap_sift_down(tp_scan_entry *entries, int root, int count) {
    for (;;) {
        if (root >= count / 2) {
            return; /* Leaf: also keeps root * 2 + 1 inside the signed range. */
        }
        const int left = root * 2 + 1;
        int largest = left;
        const int right = left + 1;
        if (right < count &&
            entry_cmp(&entries[largest], &entries[right]) < 0) {
            largest = right;
        }
        if (entry_cmp(&entries[root], &entries[largest]) >= 0) {
            return;
        }
        entry_swap(&entries[root], &entries[largest]);
        root = largest;
    }
}

/* In-place heapsort preserves the scan's allocation behavior and exposes
 * cancellation points between bounded O(log n) sift operations. */
static tp_status entry_sort_cancellable(scan_vec *v,
                                        const tp_cancel_token *cancel,
                                        tp_error *err) {
    for (int start = v->count / 2; start > 0; --start) {
        if (tp_cancel_requested(cancel)) {
            return tp_error_set(err, TP_STATUS_CANCELLED,
                                "directory scan cancelled during sort");
        }
        entry_heap_sift_down(v->data, start - 1, v->count);
    }
    for (int end = v->count - 1; end > 0; --end) {
        if (tp_cancel_requested(cancel)) {
            return tp_error_set(err, TP_STATUS_CANCELLED,
                                "directory scan cancelled during sort");
        }
        entry_swap(&v->data[0], &v->data[end]);
        entry_heap_sift_down(v->data, 0, end);
    }
    return TP_STATUS_OK;
}

/* True iff `name` ends with `suffix` (case-sensitive byte compare; "" matches everything). */
static bool name_has_suffix(const char *name, const char *suffix) {
    size_t ln = strlen(name);
    size_t ls = strlen(suffix);
    if (ls == 0) {
        return true;
    }
    if (ln < ls) {
        return false;
    }
    return memcmp(name + (ln - ls), suffix, ls) == 0;
}

static tp_status scan_errno_status(int error, const char *operation,
                                   const char *path, tp_error *err) {
    tp_status status = TP_STATUS_PATH_RESOLVE_FAILED;
    switch (error) {
        case ENOENT:
            status = TP_STATUS_NOT_FOUND;
            break;
        case ENOMEM:
            status = TP_STATUS_OOM;
            break;
#ifdef ENAMETOOLONG
        case ENAMETOOLONG:
            status = TP_STATUS_OUT_OF_BOUNDS;
            break;
#endif
#ifdef ERANGE
        case ERANGE:
            status = TP_STATUS_OUT_OF_BOUNDS;
            break;
#endif
#ifdef EILSEQ
        case EILSEQ:
            return tp_error_set(
                err, TP_STATUS_INVALID_UTF8,
                "directory scan %s path or entry name is not valid UTF-8",
                operation ? operation : "filesystem");
#endif
        default:
            break;
    }
    return tp_error_set(err, status, "directory scan could not %s '%s' (errno %d)",
                        operation, path ? path : "", error);
}

static tp_status scan_join(const char *left, const char *right, size_t limit,
                           const char *kind, char **out, tp_error *err) {
    *out = NULL;
    const size_t left_length = strlen(left);
    const size_t right_length = strlen(right);
    const bool separator = left_length != 0U && left[left_length - 1U] != '/' &&
                           left[left_length - 1U] != '\\';
    const size_t separator_length = separator ? 1U : 0U;
    if (left_length > SIZE_MAX - separator_length ||
        left_length + separator_length > SIZE_MAX - right_length - 1U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "directory scan %s path arithmetic overflow", kind);
    }
    const size_t length = left_length + separator_length + right_length;
    if (length >= limit) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "directory scan %s path exceeds %zu bytes", kind,
                            limit - 1U);
    }
    char *joined = (char *)scan_alloc(length + 1U);
    if (!joined) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "directory scan %s path allocation failed", kind);
    }
    memcpy(joined, left, left_length);
    size_t position = left_length;
    if (separator) {
        joined[position++] = '/';
    }
    memcpy(joined + position, right, right_length + 1U);
    *out = joined;
    return TP_STATUS_OK;
}
// #endregion

// #region platform recursion
/* Recurse `abs_dir` (physical path) accumulating image files; `rel_prefix` is the
 * '/'-normalized path from the scan root (empty at the top). `cancel` is polled once
 * before each level's opendir and again at the top of the entry loop before each
 * readdir (NULL => never cancel); a cancelled walk aborts with TP_STATUS_CANCELLED
 * and the caller frees whatever was accumulated. */
static tp_status scan_dir(const char *abs_dir, const char *rel_prefix,
                          scan_vec *out, const tp_cancel_token *cancel,
                          tp_error *err) {
    /* Poll BEFORE the (recursive) opendir so an already-set cancel skips a fresh
     * blocking open at each level. This bounds latency but does NOT preempt a cancel
     * raised while already blocked inside opendir/readdir on a wedged mount -- that
     * needs killable/async I/O (out of scope). */
    if (tp_cancel_requested(cancel)) {
        return tp_error_set(err, TP_STATUS_CANCELLED, "directory scan cancelled");
    }
    tp_fs_dir *dir = tp_fs_dir_open(abs_dir);
    if (!dir) {
        const int error = errno;
        return scan_errno_status(error, "open", abs_dir, err);
    }
    tp_status status = TP_STATUS_OK;
    tp_fs_dir_entry entry;
    tp_fs_dir_result next = TP_FS_DIR_ENTRY;
    for (;;) {
        /* Poll at the loop TOP -- BEFORE each blocking tp_fs_dir_next, and before
         * recursing into a subdirectory below -- so cancel latency is bounded to one
         * entry per level and a cancel set while a child level was blocked is observed
         * before this level reads its next entry. */
        if (tp_cancel_requested(cancel)) {
            status = tp_error_set(err, TP_STATUS_CANCELLED,
                                  "directory scan cancelled");
            break;
        }
        next = tp_fs_dir_next(dir, &entry);
        if (next != TP_FS_DIR_ENTRY) {
            break;
        }
        scan_post_entry_gate(); /* test-only: count this entry; park after the first
                                 * (no-op in production) */
        if (entry.info.reparse) {
            continue; /* never recurse through links/junctions */
        }
        const bool directory = entry.info.kind == TP_FS_KIND_DIRECTORY;
        const bool image = entry.info.kind == TP_FS_KIND_REGULAR &&
                           has_image_ext(entry.name);
        if (!directory && !image) {
            continue;
        }
        char *child_abs = NULL;
        char *child_rel = NULL;
        status = scan_join(abs_dir, entry.name, TP_IDENTITY_PATH_MAX,
                           "absolute", &child_abs, err);
        if (status == TP_STATUS_OK) {
            status = scan_join(rel_prefix, entry.name, TP_SRCKEY_MAX,
                               "source-key", &child_rel, err);
        }
        if (status != TP_STATUS_OK) {
            free(child_abs);
            free(child_rel);
            break;
        }
        if (directory) {
            status = scan_dir(child_abs, child_rel, out, cancel, err);
        } else if (entry.info.size > (uint64_t)LLONG_MAX) {
            status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                  "directory scan file size exceeds LLONG_MAX");
        } else {
            status = scan_vec_push_owned(out, &child_rel, &child_abs,
                                         (long long)entry.info.size,
                                         (long long)entry.info.mtime, err);
        }
        free(child_abs);
        free(child_rel);
        if (status != TP_STATUS_OK) {
            break;
        }
    }
    if (status == TP_STATUS_OK && next == TP_FS_DIR_ERROR) {
        const int error = errno;
        status = scan_errno_status(error, "read", abs_dir, err);
    }
    tp_fs_dir_close(dir);
    return status;
}
// #endregion

// #region public API
tp_status tp_scan_dir_cancellable(const char *abs_dir, tp_scan_result *out,
                                  const tp_cancel_token *cancel, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "directory scan output is required");
    }
    out->entries = NULL;
    out->count = 0;
    if (!abs_dir || abs_dir[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "directory scan path is empty");
    }
    if (strlen(abs_dir) >= TP_IDENTITY_PATH_MAX) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "directory scan root exceeds %u bytes",
                            (unsigned)(TP_IDENTITY_PATH_MAX - 1));
    }
    /* Poll BEFORE the blocking root stat: a cancel raised while the caller resolved this
     * source (or finished the previous folder) skips a fresh stat on a slow/network mount.
     * *out was zeroed above, so a cancelled entry publishes nothing. */
    if (tp_cancel_requested(cancel)) {
        return tp_error_set(err, TP_STATUS_CANCELLED, "directory scan cancelled");
    }
    tp_fs_info root_info;
    if (!tp_fs_stat(abs_dir, &root_info)) {
        const int error = errno;
        return scan_errno_status(error, "stat", abs_dir, err);
    }
    if (root_info.kind != TP_FS_KIND_DIRECTORY || root_info.reparse) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "directory scan root is not a direct directory: '%s'",
                            abs_dir);
    }
    scan_gate_wait(); /* test-only: park here so a job test can prove the walk is
                       * off the caller thread / drive a mid-walk cancel (no-op otherwise) */
    scan_vec v = {0};
    tp_status status = scan_dir(abs_dir, "", &v, cancel, err);
    if (status != TP_STATUS_OK) {
        scan_vec_drop(&v);
        return status;
    }
    /* Poll after the walk before entering the bounded cancellable sort. */
    if (tp_cancel_requested(cancel)) {
        scan_vec_drop(&v);
        return tp_error_set(err, TP_STATUS_CANCELLED, "directory scan cancelled");
    }
    s_scan_sort_started = true; /* test observation seam; otherwise inert */
    if (v.count > 1) {
        status = entry_sort_cancellable(&v, cancel, err);
        if (status != TP_STATUS_OK) {
            scan_vec_drop(&v);
            return status;
        }
    }
    s_scan_sort_finished = true; /* test observation seam; otherwise inert */
    if (tp_cancel_requested(cancel)) {
        scan_vec_drop(&v);
        return tp_error_set(err, TP_STATUS_CANCELLED,
                            "directory scan cancelled");
    }
    out->entries = v.data;
    out->count = v.count;
    if (err) {
        err->msg[0] = '\0';
    }
    return TP_STATUS_OK;
}

tp_status tp_scan_dir(const char *abs_dir, tp_scan_result *out, tp_error *err) {
    return tp_scan_dir_cancellable(abs_dir, out, NULL, err);
}

void tp_scan_free(tp_scan_result *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; ++i) {
        free(out->entries[i].rel);
        free(out->entries[i].abs);
    }
    free(out->entries);
    out->entries = NULL;
    out->count = 0;
}

bool tp_scan_visit_dir(const char *dir, const char *suffix, tp_scan_name_visitor visit, void *ctx) {
    if (!dir || dir[0] == '\0' || !visit) {
        return false;
    }
    const char *suf = suffix ? suffix : "";
    tp_fs_dir *stream = tp_fs_dir_open(dir);
    if (!stream) {
        return false;
    }
    tp_fs_dir_entry entry;
    tp_fs_dir_result next;
    while ((next = tp_fs_dir_next(stream, &entry)) == TP_FS_DIR_ENTRY) {
        if (entry.info.kind != TP_FS_KIND_REGULAR || entry.info.reparse || !name_has_suffix(entry.name, suf)) {
            continue;
        }
        if (!visit(ctx, entry.name, entry.info.size)) {
            tp_fs_dir_close(stream);
            return true;
        }
    }
    tp_fs_dir_close(stream);
    return next == TP_FS_DIR_END;
}

bool tp_scan_is_dir(const char *abs) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
    return tp_fs_is_dir(abs);
}

bool tp_scan_exists(const char *abs) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
    return tp_fs_exists(abs);
}

tp_status tp_scan_classify_checked(const char *abs, tp_scan_kind *out,
                                   tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source classification output is required");
    }
    *out = TP_SCAN_KIND_MISSING;
    if (!abs || abs[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source classification path is empty");
    }
    if (s_scan_stat_error != 0) {
        errno = s_scan_stat_error;
        return scan_errno_status(errno, "stat", abs, err);
    }
    tp_fs_info info;
    if (!tp_fs_stat(abs, &info)) {
        const int error = errno;
        return scan_errno_status(error, "stat", abs, err);
    }
    *out = info.kind == TP_FS_KIND_DIRECTORY ? TP_SCAN_KIND_DIRECTORY
                                             : TP_SCAN_KIND_FILE;
    if (err) {
        err->msg[0] = '\0';
    }
    return TP_STATUS_OK;
}

tp_scan_kind tp_scan_classify(const char *abs) {
    tp_scan_kind kind = TP_SCAN_KIND_MISSING;
    (void)tp_scan_classify_checked(abs, &kind, NULL);
    return kind;
}

bool tp_scan_file_stat(const char *abs, long long *out_size,
                       long long *out_mtime) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
    tp_fs_info info;
    if (!tp_fs_stat(abs, &info) || info.kind != TP_FS_KIND_REGULAR ||
        info.size > (uint64_t)LLONG_MAX) {
        return false;
    }
    if (out_size) {
        *out_size = (long long)info.size;
    }
    if (out_mtime) {
        *out_mtime = (long long)info.mtime;
    }
    return true;
}

void tp_mkdirs(const char *dir) {
    if (!dir || dir[0] == '\0' || !tp_fs_path_is_valid_utf8(dir)) {
        return;
    }
    size_t input_length = strlen(dir);
    if (input_length == SIZE_MAX) {
        return;
    }
    char *tmp = (char *)malloc(input_length + 1U);
    if (!tmp) {
        return;
    }
    memcpy(tmp, dir, input_length + 1U);
    /* strip trailing separators (but keep a lone leading '/'). */
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    /* create each intermediate directory, then the leaf. `q != tmp` skips a
     * leading '/' (POSIX absolute) and never mkdir's the empty string. */
    for (char *q = tmp; *q != '\0'; q++) {
        if ((*q == '/' || *q == '\\') && q != tmp) {
            const char sep = *q;
            *q = '\0';
            (void)tp_fs_create_dir(tmp);
            *q = sep;
        }
    }
    (void)tp_fs_create_dir(tmp);
    free(tmp);
}

void tp_mkdirs_parent(const char *file_path) {
    if (!file_path || !tp_fs_path_is_valid_utf8(file_path)) {
        return;
    }
    size_t length = strlen(file_path);
    if (length == SIZE_MAX) {
        return;
    }
    char *tmp = (char *)malloc(length + 1U);
    if (!tmp) {
        return;
    }
    memcpy(tmp, file_path, length + 1U);
    char *last = strrchr(tmp, '/');
    char *lb = strrchr(tmp, '\\');
    if (lb && (!last || lb > last)) {
        last = lb;
    }
    if (!last) {
        free(tmp);
        return; /* no directory component */
    }
    *last = '\0';
    tp_mkdirs(tmp);
    free(tmp);
}
// #endregion
