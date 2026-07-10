#include "gui_history.h"

#include <stdlib.h>
#include <string.h>

// #region config + stacks
#define GUI_HISTORY_BUDGET_BYTES ((size_t)32U * 1024U * 1024U)
#define GUI_HISTORY_COALESCE_SECS 0.30

typedef struct snap {
    char *buf;
    size_t len;
    uint32_t tag;
} snap;

typedef struct stack {
    snap *items;
    int count;
    int cap;
    size_t bytes;
} stack;

static stack s_undo;
static stack s_redo;
static uint32_t s_last_tag;   /* tag of the last real push (coalesce key) */
static double s_last_time;    /* time of the last push (coalesce window) */
static bool s_have_last;      /* a coalescable top exists */
// #endregion

// #region stack primitives
static void stack_free(stack *s) {
    for (int i = 0; i < s->count; i++) {
        free(s->items[i].buf);
    }
    free(s->items);
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
    s->bytes = 0;
}

static bool stack_push(stack *s, const char *buf, size_t len, uint32_t tag) {
    if (s->count == s->cap) {
        int ncap = (s->cap == 0) ? 16 : s->cap * 2;
        snap *ni = (snap *)realloc(s->items, (size_t)ncap * sizeof *ni);
        if (!ni) {
            return false;
        }
        s->items = ni;
        s->cap = ncap;
    }
    char *copy = (char *)malloc(len ? len : 1U);
    if (!copy) {
        return false;
    }
    if (len) {
        memcpy(copy, buf, len);
    }
    s->items[s->count].buf = copy;
    s->items[s->count].len = len;
    s->items[s->count].tag = tag;
    s->count++;
    s->bytes += len;
    return true;
}

static void stack_drop_oldest(stack *s) {
    if (s->count == 0) {
        return;
    }
    s->bytes -= s->items[0].len;
    free(s->items[0].buf);
    memmove(&s->items[0], &s->items[1], (size_t)(s->count - 1) * sizeof s->items[0]);
    s->count--;
}

/* Transfers ownership of the popped buffer to *out (caller frees). */
static bool stack_pop(stack *s, char **out, size_t *out_len) {
    if (s->count == 0) {
        *out = NULL;
        *out_len = 0;
        return false;
    }
    s->count--;
    *out = s->items[s->count].buf;
    *out_len = s->items[s->count].len;
    s->bytes -= *out_len;
    return true;
}
// #endregion

// #region public API
void gui_history_init(void) {
    memset(&s_undo, 0, sizeof s_undo);
    memset(&s_redo, 0, sizeof s_redo);
    s_last_tag = 0;
    s_last_time = 0.0;
    s_have_last = false;
}

void gui_history_reset(void) {
    stack_free(&s_undo);
    stack_free(&s_redo);
    s_last_tag = 0;
    s_last_time = 0.0;
    s_have_last = false;
}

void gui_history_shutdown(void) { gui_history_reset(); }

void gui_history_push(const char *buf, size_t len, uint32_t action_tag, double now) {
    if (!buf) {
        return;
    }
    const bool coalesce = s_have_last && s_undo.count > 0 && action_tag != 0U && action_tag == s_last_tag &&
                          (now - s_last_time) <= GUI_HISTORY_COALESCE_SECS;
    stack_free(&s_redo); /* any new mutation invalidates redo, coalesced or not */
    if (coalesce) {
        s_last_time = now; /* extend the gesture window; keep the single pre-gesture entry */
        return;
    }
    if (!stack_push(&s_undo, buf, len, action_tag)) {
        return; /* best-effort: on OOM we simply lose the history step, never crash */
    }
    while (s_undo.bytes > GUI_HISTORY_BUDGET_BYTES && s_undo.count > 1) {
        stack_drop_oldest(&s_undo);
    }
    s_last_tag = action_tag;
    s_last_time = now;
    s_have_last = true;
}

bool gui_history_can_undo(void) { return s_undo.count > 0; }
bool gui_history_can_redo(void) { return s_redo.count > 0; }

bool gui_history_undo(const char *cur_buf, size_t cur_len, char **out, size_t *out_len) {
    if (s_undo.count == 0) {
        *out = NULL;
        *out_len = 0;
        return false;
    }
    (void)stack_push(&s_redo, cur_buf, cur_len, 0U); /* current model -> redo (best-effort) */
    (void)stack_pop(&s_undo, out, out_len);
    s_have_last = false; /* an undo breaks the coalescing chain */
    return true;
}

bool gui_history_redo(const char *cur_buf, size_t cur_len, char **out, size_t *out_len) {
    if (s_redo.count == 0) {
        *out = NULL;
        *out_len = 0;
        return false;
    }
    (void)stack_push(&s_undo, cur_buf, cur_len, 0U);
    (void)stack_pop(&s_redo, out, out_len);
    s_have_last = false;
    return true;
}

int gui_history_undo_depth(void) { return s_undo.count; }
int gui_history_redo_depth(void) { return s_redo.count; }
size_t gui_history_bytes(void) { return s_undo.bytes + s_redo.bytes; }
// #endregion
