#include "cli_exit.h"
#include "cli_out.h"

int main(void) {
    static const char path[] = "/workspace/project.ntpacker_project";
    tp_error error = {{0}};
    (void)tp_error_set_file_io(
        &error, TP_FILE_IO_PHASE_ATOMIC_REPLACE, path, 13,
        "could not atomically replace project");
    cli_emit_file_io_error(true, false, &error);
    return CLI_EXIT_FILE_IO;
}
