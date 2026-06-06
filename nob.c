#define NOB_IMPLEMENTATION
#include "nob.h"

int main(int argc, char **argv) {
    GO_REBUILD_URSELF(argc, argv);

    Cmd cmd = {0};
    cmd_append(&cmd, "gcc");
    cmd_append(&cmd, "-Wall", "-Wextra");
    cmd_append(&cmd, "-ltdjson");
    cmd_append(&cmd, "cJSON.c");
    cmd_append(&cmd, "-O3");
    cmd_append(&cmd, "-o", "tgf");
    cmd_append(&cmd, "tgf.c");
    if (!cmd_run(&cmd)) return 1;

    cmd_append(&cmd, "./tgf");
    if (!cmd_run(&cmd)) return 1;

    return 0;
}
