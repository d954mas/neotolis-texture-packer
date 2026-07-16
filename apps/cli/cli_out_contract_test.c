#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_out.h"

int main(void) {
    if (SIZE_MAX <= UINT32_MAX) {
        return 0;
    }
    cli_sb sb = {0};
    cli_sb_size(&sb, (size_t)UINT32_MAX + 1U);
    const int ok = !sb.oom && sb.buf && strcmp(sb.buf, "4294967296") == 0;
    cli_sb_free(&sb);
    return ok ? 0 : 1;
}
