/* F1-00 tasks 1(b) + 3: the FILESYSTEM-DEPENDENT half -- realpath/symlink
 * resolution and the atomic Save-As identity transition. Structured CI-portable:
 * everything happens under a created scratch dir (argv[1]); assertions are on
 * equivalence/relationships, never baked machine-absolute strings. Symlink
 * equivalence is created where the platform allows and SKIPPED-with-note where it
 * needs privilege (Windows). */

#define _CRT_SECURE_NO_WARNINGS /* fopen()/_mkdir() without the MSVC deprecation error */

#include "tp_core/tp_identity.h"
#include "tp_hex.h" /* shared lowercase-hex encoder -- same code production uses (drift guard) */

#include <stdio.h>
#include <string.h>

/* Platform headers BEFORE unity.h: unity_internals.h pulls <stdnoreturn.h>, whose
 * `noreturn` macro breaks windows.h's DECLSPEC_NORETURN (__declspec(noreturn)). */
#if defined(_WIN32)
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* match tp_identity.c: Win7+ (GetFinalPathNameByHandleW, symlink API) */
#endif
#include <windows.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(p) mkdir((p), 0777)
#endif

#include "unity.h"

static const char *g_dir; /* scratch dir (argv[1]); absolute, created by CMake */

void setUp(void) {}
void tearDown(void) {}

/* --- tiny filesystem helpers (test-local) --- */
static void joinp(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < cap);
}
static void touch(const char *path) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fputs("x", f);
    fclose(f);
}
static void ensure_dir(const char *path) {
    (void)make_dir(path); /* ignore EEXIST */
}

/* fixed RNG so an unsaved identity is deterministic across a test. */
static int fixed_fill(void *ctx, uint8_t *out, size_t len) {
    memcpy(out, (const uint8_t *)ctx, len);
    return (int)len;
}
static int fail_fill(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    (void)out;
    (void)len;
    return -1;
}
static tp_rng seeded_rng(uint8_t *seed16) {
    tp_rng r = {fixed_fill, seed16};
    return r;
}

/* ---------------------------------------------------------------------- */

void test_init_unsaved_and_key(void) {
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) {
        seed[i] = (uint8_t)(0xA0 + i);
    }
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, &err));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_UNSAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_is_valid(&id));

    char key[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_key(&id, key, sizeof key, &err));
    /* "session:" + 32 lowercase hex of the seed, built via the SHARED encoder so
     * this drift-guard and production cannot diverge (the raw literal is pinned in
     * test_identity_id.c against a known seed). */
    char expect[64];
    memcpy(expect, "session:", 8);
    tp_hex_encode_lower(seed, 16U, expect + 8);
    TEST_ASSERT_EQUAL_STRING(expect, key);
    TEST_ASSERT_EQUAL_STRING("session:a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", key);
}

void test_init_unsaved_rng_failure(void) {
    tp_rng rng = {fail_fill, NULL};
    tp_session_identity id;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RNG_FAILED, tp_session_identity_init_unsaved(&id, &rng, &err));
    TEST_ASSERT_FALSE(tp_session_identity_is_valid(&id)); /* failed init is not usable */
    char key[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_session_identity_key(&id, key, sizeof key, &err));
}

void test_first_save_transition(void) {
    uint8_t seed[16] = {1, 2, 3};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));

    char dest[TP_IDENTITY_PATH_MAX];
    joinp(dest, sizeof dest, g_dir, "first_save.ntpacker_project"); /* parent exists, file does not */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_transition_to_path(&id, dest, NULL));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_SAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_is_valid(&id));

    /* The identity's key equals the canonical of the destination computed directly. */
    char k1[TP_IDENTITY_PATH_MAX];
    char k2[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_key(&id, k1, sizeof k1, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(dest, k2, sizeof k2, NULL));
    TEST_ASSERT_TRUE(tp_identity_path_equal(k1, k2));
}

void test_not_yet_created_canonicalizes_via_existing_parent(void) {
    char parent[TP_IDENTITY_PATH_MAX];
    joinp(parent, sizeof parent, g_dir, "created_parent");
    ensure_dir(parent);

    char dest[TP_IDENTITY_PATH_MAX];
    joinp(dest, sizeof dest, parent, "not_created_yet.ntpacker_project"); /* file absent, parent present */

    char dest_canon[TP_IDENTITY_PATH_MAX];
    char parent_canon[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(dest, dest_canon, sizeof dest_canon, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(parent, parent_canon, sizeof parent_canon, NULL));

    /* dest resolves as <resolved-parent>/<final-component>. */
    size_t pl = strlen(parent_canon);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(dest_canon, parent_canon, pl), dest_canon);
    TEST_ASSERT_EQUAL_INT('/', dest_canon[pl]);
    TEST_ASSERT_EQUAL_STRING("not_created_yet.ntpacker_project", dest_canon + pl + 1);
}

void test_missing_destination_parent(void) {
    uint8_t seed[16] = {9};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));
    tp_session_identity before = id;

    char dest[TP_IDENTITY_PATH_MAX];
    joinp(dest, sizeof dest, g_dir, "no_such_parent_dir/proj.ntpacker_project");
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PATH_RESOLVE_FAILED, tp_session_identity_transition_to_path(&id, dest, &err));
    /* Rollback: still the original unsaved identity. */
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_UNSAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_equal(&before, &id));
}

void test_first_save_failure_rollback(void) {
    uint8_t seed[16] = {7, 7, 7};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));
    tp_session_identity before = id;

    /* A non-absolute destination fails lexically, before the realpath layer. */
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PATH_NOT_ABSOLUTE,
                          tp_session_identity_transition_to_path(&id, "relative/not/absolute.ntpacker_project", &err));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_UNSAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_equal(&before, &id));
}

void test_save_as_path_to_path(void) {
    uint8_t seed[16] = {5};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));

    char destA[TP_IDENTITY_PATH_MAX];
    char destB[TP_IDENTITY_PATH_MAX];
    joinp(destA, sizeof destA, g_dir, "saveas_a.ntpacker_project");
    joinp(destB, sizeof destB, g_dir, "saveas_b.ntpacker_project");

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_transition_to_path(&id, destA, NULL));
    char keyA[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_key(&id, keyA, sizeof keyA, NULL));

    /* Save As: path -> path. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_transition_to_path(&id, destB, NULL));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_SAVED, id.kind);
    char keyB[TP_IDENTITY_PATH_MAX];
    char canonB[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_key(&id, keyB, sizeof keyB, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(destB, canonB, sizeof canonB, NULL));
    TEST_ASSERT_TRUE(tp_identity_path_equal(keyB, canonB));   /* now B */
    TEST_ASSERT_FALSE(tp_identity_path_equal(keyB, keyA));    /* copy at another path = another identity */
}

void test_save_as_rollback_preserves_old_identity(void) {
    uint8_t seed[16] = {6};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));

    char destA[TP_IDENTITY_PATH_MAX];
    joinp(destA, sizeof destA, g_dir, "rollback_a.ntpacker_project");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_transition_to_path(&id, destA, NULL));
    tp_session_identity saved_a = id; /* snapshot of the SAVED-A identity */

    /* A failing Save As (missing parent) must leave identity == SAVED path A. */
    char badB[TP_IDENTITY_PATH_MAX];
    joinp(badB, sizeof badB, g_dir, "nope_missing_dir/rollback_b.ntpacker_project");
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PATH_RESOLVE_FAILED, tp_session_identity_transition_to_path(&id, badB, &err));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_SAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_equal(&saved_a, &id));
}

void test_destination_collision(void) {
    char destC[TP_IDENTITY_PATH_MAX];
    joinp(destC, sizeof destC, g_dir, "claimed.ntpacker_project");
    char claimed_key[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(destC, claimed_key, sizeof claimed_key, NULL));
    const char *claimed[] = {claimed_key};

    uint8_t seed[16] = {2, 2};
    tp_rng rng = seeded_rng(seed);
    tp_session_identity id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_init_unsaved(&id, &rng, NULL));
    tp_session_identity before = id;

    /* Claiming the already-claimed key collides and rolls back. */
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_IDENTITY_COLLISION,
                          tp_session_identity_claim_path(&id, destC, claimed, 1, &err));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_UNSAVED, id.kind);
    TEST_ASSERT_TRUE(tp_session_identity_equal(&before, &id));

    /* A different destination against the same claim set succeeds. */
    char destD[TP_IDENTITY_PATH_MAX];
    joinp(destD, sizeof destD, g_dir, "unclaimed.ntpacker_project");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_identity_claim_path(&id, destD, claimed, 1, NULL));
    TEST_ASSERT_EQUAL_INT(TP_IDENTITY_SAVED, id.kind);
}

void test_equivalent_path_vectors(void) {
    /* Create a real file; two equivalent spellings canonicalize to one identity. */
    char eqdir[TP_IDENTITY_PATH_MAX];
    char eqsub[TP_IDENTITY_PATH_MAX];
    joinp(eqdir, sizeof eqdir, g_dir, "eq");
    ensure_dir(eqdir);
    joinp(eqsub, sizeof eqsub, eqdir, "sub");
    ensure_dir(eqsub);

    char file[TP_IDENTITY_PATH_MAX];
    joinp(file, sizeof file, eqdir, "equiv.ntpacker_project");
    touch(file);

    char spell1[TP_IDENTITY_PATH_MAX]; /* .../eq/equiv... */
    char spell2[TP_IDENTITY_PATH_MAX]; /* .../eq/sub/../equiv... (same file) */
    joinp(spell2, sizeof spell2, eqsub, "../equiv.ntpacker_project");

    char c1[TP_IDENTITY_PATH_MAX];
    char c2[TP_IDENTITY_PATH_MAX];
    memcpy(spell1, file, strlen(file) + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(spell1, c1, sizeof c1, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(spell2, c2, sizeof c2, NULL));
    TEST_ASSERT_TRUE_MESSAGE(tp_identity_path_equal(c1, c2), c2);
}

/* Symlink/junction equivalence -- created where the platform allows, skipped
 * (with a note) where it needs privilege. */
void test_symlink_equivalence(void) {
    char target[TP_IDENTITY_PATH_MAX];
    char link[TP_IDENTITY_PATH_MAX];
    joinp(target, sizeof target, g_dir, "sym_target.ntpacker_project");
    joinp(link, sizeof link, g_dir, "sym_link.ntpacker_project");
    touch(target);

#if defined(_WIN32)
    /* Privilege-free FS-resolution proof (always runs): a case-different spelling
     * of an existing file resolves to the same identity (case-insensitive volume
     * + Windows ASCII case-fold). */
    char casedir[TP_IDENTITY_PATH_MAX];
    joinp(casedir, sizeof casedir, g_dir, "CaseDir");
    ensure_dir(casedir);
    char cfile[TP_IDENTITY_PATH_MAX];
    joinp(cfile, sizeof cfile, casedir, "File.ntpacker_project");
    touch(cfile);
    char lower[TP_IDENTITY_PATH_MAX];
    joinp(lower, sizeof lower, g_dir, "casedir/file.ntpacker_project");
    char ca[TP_IDENTITY_PATH_MAX];
    char cb[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(cfile, ca, sizeof ca, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(lower, cb, sizeof cb, NULL));
    TEST_ASSERT_TRUE(tp_identity_path_equal(ca, cb));

    /* A genuine symlink needs SeCreateSymbolicLink (privilege / Developer Mode). */
    wchar_t wlink[TP_IDENTITY_PATH_MAX];
    wchar_t wtarget[TP_IDENTITY_PATH_MAX];
    MultiByteToWideChar(CP_UTF8, 0, link, -1, wlink, (int)(sizeof wlink / sizeof wlink[0]));
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, (int)(sizeof wtarget / sizeof wtarget[0]));
    if (!CreateSymbolicLinkW(wlink, wtarget, 0)) {
        DWORD e = GetLastError();
        if (e == ERROR_PRIVILEGE_NOT_HELD) {
            TEST_IGNORE_MESSAGE("SKIP: Windows symlink creation needs privilege/Developer Mode "
                                "(case-insensitive equivalence above already proves FS resolution)");
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)e, "CreateSymbolicLinkW failed for another reason");
    }
#else
    if (symlink(target, link) != 0) {
        TEST_IGNORE_MESSAGE("SKIP: symlink() failed on this platform/filesystem");
    }
#endif

    /* The symlink resolves to the same canonical identity as its target. */
    char ct[TP_IDENTITY_PATH_MAX];
    char cl[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(target, ct, sizeof ct, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_canonical(link, cl, sizeof cl, NULL));
    TEST_ASSERT_TRUE_MESSAGE(tp_identity_path_equal(ct, cl), cl);
}

/* A DANGLING symlink (the link exists, its target does not) must resolve to
 * path_resolve_failed -- NOT the phantom <parent>/<linkname> identity. Otherwise
 * the SAME file would acquire a second, different canonical identity the moment
 * its target is created (realpath would then follow the link to the target),
 * splitting one project into two in the journal/session registry keyed on this
 * string. Created where the platform allows; skipped-with-note where a symlink
 * needs privilege (Windows), exactly like test_symlink_equivalence. */
void test_dangling_symlink_is_resolve_failed(void) {
    char link[TP_IDENTITY_PATH_MAX];
    char missing[TP_IDENTITY_PATH_MAX];
    joinp(link, sizeof link, g_dir, "dangling_link.ntpacker_project");
    joinp(missing, sizeof missing, g_dir, "no_such_target.ntpacker_project"); /* never created */

#if defined(_WIN32)
    wchar_t wlink[TP_IDENTITY_PATH_MAX];
    wchar_t wtarget[TP_IDENTITY_PATH_MAX];
    MultiByteToWideChar(CP_UTF8, 0, link, -1, wlink, (int)(sizeof wlink / sizeof wlink[0]));
    MultiByteToWideChar(CP_UTF8, 0, missing, -1, wtarget, (int)(sizeof wtarget / sizeof wtarget[0]));
    if (!CreateSymbolicLinkW(wlink, wtarget, 0)) {
        DWORD e = GetLastError();
        if (e == ERROR_PRIVILEGE_NOT_HELD) {
            TEST_IGNORE_MESSAGE("SKIP: Windows symlink creation needs privilege/Developer Mode");
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)e, "CreateSymbolicLinkW failed for another reason");
    }
#else
    if (symlink(missing, link) != 0) {
        TEST_IGNORE_MESSAGE("SKIP: symlink() failed on this platform/filesystem");
    }
#endif

    char canon[TP_IDENTITY_PATH_MAX];
    tp_error err;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_PATH_RESOLVE_FAILED,
                                  tp_identity_path_canonical(link, canon, sizeof canon, &err), link);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    ensure_dir(g_dir);

    UNITY_BEGIN();
    RUN_TEST(test_init_unsaved_and_key);
    RUN_TEST(test_init_unsaved_rng_failure);
    RUN_TEST(test_first_save_transition);
    RUN_TEST(test_not_yet_created_canonicalizes_via_existing_parent);
    RUN_TEST(test_missing_destination_parent);
    RUN_TEST(test_first_save_failure_rollback);
    RUN_TEST(test_save_as_path_to_path);
    RUN_TEST(test_save_as_rollback_preserves_old_identity);
    RUN_TEST(test_destination_collision);
    RUN_TEST(test_equivalent_path_vectors);
    RUN_TEST(test_symlink_equivalence);
    RUN_TEST(test_dangling_symlink_is_resolve_failed);
    return UNITY_END();
}
