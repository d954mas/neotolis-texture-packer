/*
 * F2-04 journal I/O seams: an in-memory backing store (deterministic; drives the
 * fault suite) and a real file backing store (production durability, F2-05). All
 * durability the journal core needs goes through the tp_journal_io vtable, so the
 * core is testable on crafted/corrupt bytes without real-fs flakiness. Every path
 * closes its resources (no fd/buffer leak under LSan).
 */

#include "tp_core/tp_journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_journal_internal.h"

#if defined(_WIN32)
#include <io.h> /* _chsize_s, _fileno */
#else
#include <unistd.h> /* ftruncate, fileno */
#endif

/* ---- in-memory backing store --------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    int fail_writes;    /* fail the next N write() calls entirely */
    int64_t short_next; /* >=0: next write is a short write of this many bytes */
    bool fail_truncate; /* fail the next truncate() call */
} journal_mem;

static int64_t mem_write(void *ctx, const uint8_t *data, size_t len) {
    journal_mem *m = (journal_mem *)ctx;
    if (m->fail_writes > 0) {
        m->fail_writes--;
        return 0; /* injected total failure: nothing written */
    }
    size_t n = len;
    if (m->short_next >= 0) {
        size_t cap_short = (size_t)m->short_next;
        n = (cap_short < len) ? cap_short : len; /* injected short write */
        m->short_next = -1;
    }
    if (n > 0) {
        if (m->len > SIZE_MAX - n) {
            return -1;
        }
        size_t need = m->len + n;
        if (need > m->cap) {
            size_t ncap = (m->cap == 0) ? 256 : m->cap;
            while (ncap < need) {
                if (ncap > SIZE_MAX / 2) {
                    ncap = need;
                    break;
                }
                ncap *= 2;
            }
            uint8_t *nb = (uint8_t *)realloc(m->buf, ncap);
            if (!nb) {
                return -1;
            }
            m->buf = nb;
            m->cap = ncap;
        }
        memcpy(m->buf + m->len, data, n);
        m->len += n;
    }
    return (int64_t)n;
}

static int64_t mem_length(void *ctx) {
    const journal_mem *m = (const journal_mem *)ctx;
    return (int64_t)m->len;
}

static int mem_truncate(void *ctx, size_t len) {
    journal_mem *m = (journal_mem *)ctx;
    if (m->fail_truncate) {
        m->fail_truncate = false;
        return -1;
    }
    if (len > m->len) {
        return -1; /* this seam never extends */
    }
    m->len = len;
    return 0;
}

static int mem_read_all(void *ctx, uint8_t **out, size_t *out_len) {
    const journal_mem *m = (const journal_mem *)ctx;
    if (m->len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    uint8_t *cp = (uint8_t *)malloc(m->len);
    if (!cp) {
        return -1;
    }
    memcpy(cp, m->buf, m->len);
    *out = cp;
    *out_len = m->len;
    return 0;
}

static int mem_sync(void *ctx) {
    (void)ctx;
    return 0;
}

static void mem_destroy(void *ctx) {
    journal_mem *m = (journal_mem *)ctx;
    if (m) {
        free(m->buf);
        free(m);
    }
}

tp_journal_io tp_journal_io_memory(void) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    journal_mem *m = (journal_mem *)calloc(1, sizeof *m);
    if (!m) {
        return io; /* ctx == NULL => tp_journal_create fails cleanly */
    }
    m->short_next = -1;
    io.ctx = m;
    io.write = mem_write;
    io.length = mem_length;
    io.truncate = mem_truncate;
    io.read_all = mem_read_all;
    io.sync = mem_sync;
    io.destroy = mem_destroy;
    return io;
}

/* ---- memory-io fault seams (test-only) ----------------------------------- */

static journal_mem *mem_of(tp_journal_io io) {
    return (io.write == mem_write) ? (journal_mem *)io.ctx : NULL;
}

void tp_journal_io_memory__fail_next_writes(tp_journal_io io, int n) {
    journal_mem *m = mem_of(io);
    if (m) {
        m->fail_writes = n;
    }
}
void tp_journal_io_memory__short_next_write(tp_journal_io io, int64_t n) {
    journal_mem *m = mem_of(io);
    if (m) {
        m->short_next = n;
    }
}
void tp_journal_io_memory__fail_next_truncate(tp_journal_io io) {
    journal_mem *m = mem_of(io);
    if (m) {
        m->fail_truncate = true;
    }
}
void tp_journal_io_memory__poke(tp_journal_io io, size_t at, uint8_t val) {
    journal_mem *m = mem_of(io);
    if (m && at < m->len) {
        m->buf[at] = val;
    }
}
void tp_journal_io_memory__set_len(tp_journal_io io, size_t len) {
    journal_mem *m = mem_of(io);
    if (m && len <= m->len) {
        m->len = len;
    }
}

/* ---- file backing store -------------------------------------------------- */

typedef struct {
    FILE *fp;
} journal_file;

static int64_t file_write(void *ctx, const uint8_t *data, size_t len) {
    journal_file *f = (journal_file *)ctx;
    if (fseek(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    size_t n = fwrite(data, 1, len, f->fp);
    if (fflush(f->fp) != 0) {
        return -1;
    }
    return (int64_t)n;
}

static int64_t file_length(void *ctx) {
    journal_file *f = (journal_file *)ctx;
    if (fseek(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    long pos = ftell(f->fp);
    return (pos < 0) ? -1 : (int64_t)pos;
}

static int file_truncate(void *ctx, size_t len) {
    journal_file *f = (journal_file *)ctx;
    if (fflush(f->fp) != 0) {
        return -1;
    }
#if defined(_WIN32)
    int rc = _chsize_s(_fileno(f->fp), (long long)len);
#else
    int rc = ftruncate(fileno(f->fp), (long)len);
#endif
    (void)fseek(f->fp, 0, SEEK_END);
    return (rc == 0) ? 0 : -1;
}

static int file_read_all(void *ctx, uint8_t **out, size_t *out_len) {
    journal_file *f = (journal_file *)ctx;
    if (fseek(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    long sz = ftell(f->fp);
    if (sz < 0) {
        return -1;
    }
    if (sz == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    if (fseek(f->fp, 0, SEEK_SET) != 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)sz, f->fp);
    if (got != (size_t)sz) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = got;
    return 0;
}

static int file_sync(void *ctx) {
    journal_file *f = (journal_file *)ctx;
    return (fflush(f->fp) == 0) ? 0 : -1;
}

static void file_destroy(void *ctx) {
    journal_file *f = (journal_file *)ctx;
    if (f) {
        if (f->fp) {
            (void)fclose(f->fp);
        }
        free(f);
    }
}

tp_journal_io tp_journal_io_file(const char *path) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    if (!path) {
        return io;
    }
    FILE *fp = fopen(path, "r+b"); /* existing journal */
    if (!fp) {
        fp = fopen(path, "w+b"); /* create fresh */
    }
    if (!fp) {
        return io; /* ctx == NULL => create fails */
    }
    journal_file *f = (journal_file *)calloc(1, sizeof *f);
    if (!f) {
        (void)fclose(fp);
        return io;
    }
    f->fp = fp;
    io.ctx = f;
    io.write = file_write;
    io.length = file_length;
    io.truncate = file_truncate;
    io.read_all = file_read_all;
    io.sync = file_sync;
    io.destroy = file_destroy;
    return io;
}
