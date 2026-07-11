#include "tp_core/tp_names.h"

#include <stdio.h>
#include <string.h>

void tp_sprite_export_key(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    (void)snprintf(out, cap, "%s", raw ? raw : "");
    char *slash = strrchr(out, '/');
    char *base = slash ? slash + 1 : out;
    char *dot = strrchr(base, '.');
    if (dot && dot != base) { /* keep dotfiles like ".gitkeep" intact */
        *dot = '\0';
    }
}

int tp_nat_cmp(const char *a, const char *b) {
    while (*a && *b) {
        const int da = (*a >= '0' && *a <= '9');
        const int db = (*b >= '0' && *b <= '9');
        if (da && db) {
            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }
            const char *sa = a;
            const char *sb = b;
            while (*a >= '0' && *a <= '9') {
                a++;
            }
            while (*b >= '0' && *b <= '9') {
                b++;
            }
            const size_t la = (size_t)(a - sa);
            const size_t lb = (size_t)(b - sb);
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
            const int c = strncmp(sa, sb, la);
            if (c != 0) {
                return c;
            }
        } else {
            if (*a != *b) {
                return ((unsigned char)*a < (unsigned char)*b) ? -1 : 1;
            }
            a++;
            b++;
        }
    }
    if (*a) {
        return 1;
    }
    if (*b) {
        return -1;
    }
    return 0;
}

void tp_names_common_prefix(char names[][192], int count, char *out, size_t cap) {
    out[0] = '\0';
    if (count <= 0 || cap == 0) {
        return;
    }
    size_t pfx = strlen(names[0]);
    for (int i = 1; i < count; i++) {
        size_t k = 0;
        while (k < pfx && names[i][k] && names[0][k] == names[i][k]) {
            k++;
        }
        pfx = k;
    }
    while (pfx > 0) {
        const char c = names[0][pfx - 1];
        if ((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ' || c == '/') {
            pfx--;
        } else {
            break;
        }
    }
    if (pfx >= cap) {
        pfx = cap - 1;
    }
    memcpy(out, names[0], pfx);
    out[pfx] = '\0';
}
