#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_out.h"

int main(void) {
    int ok = 1;
    if (SIZE_MAX > UINT32_MAX) {
        cli_sb size = {0};
        cli_sb_size(&size, (size_t)UINT32_MAX + 1U);
        ok = !size.oom && size.buf &&
             strcmp(size.buf, "4294967296") == 0;
        cli_sb_free(&size);
    }

    const char invalid[] = {'a', (char)0xc3, 'x', '\0'};
    cli_sb repaired = {0};
    cli_sb_json_str(&repaired, invalid);
    ok = ok && !repaired.oom && repaired.buf &&
         strcmp(repaired.buf, "\"a\\ufffdx\"") == 0;
    cli_sb_free(&repaired);

    const char valid[] = {(char)0xd0, (char)0xb6, (char)0xf0,
                          (char)0x9f, (char)0x99, (char)0x82, '\0'};
    const char expected[] = {'"', (char)0xd0, (char)0xb6, (char)0xf0,
                             (char)0x9f, (char)0x99, (char)0x82, '"', '\0'};
    cli_sb preserved = {0};
    cli_sb_json_str(&preserved, valid);
    ok = ok && !preserved.oom && preserved.buf &&
         strcmp(preserved.buf, expected) == 0;
    cli_sb_free(&preserved);

    return ok ? 0 : 1;
}
