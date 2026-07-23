#define NOB_IMPLEMENTATION
#include "nob.h"

#define TD_SOURCE "lib/td"
#define TD_BUILD  "lib/td/build"

static void clean(void)
{
    nob_log(NOB_INFO, "Cleaning...");
#ifdef _WIN32
    Cmd rm = {0};
    cmd_append(&rm, "rmdir", "/s", "/q", TD_BUILD);
    cmd_run(&rm);
    rm.count = 0;
    cmd_append(&rm, "del", "/f", "tgf.exe", "nob.old");
    cmd_run(&rm);
#else
    Cmd rm = {0};
    cmd_append(&rm, "rm", "-rf", TD_BUILD, "tgf", "nob.old");
    cmd_run(&rm);
#endif
}

static int check_system_td(void)
{
    return nob_file_exists("/usr/lib/libtdjson.so") ||
           nob_file_exists("/usr/local/lib/libtdjson.so") ||
           nob_file_exists("/usr/lib/x86_64-linux-gnu/libtdjson.so");
}

static int ensure_td_source(void)
{
    if (nob_file_exists(TD_SOURCE "/CMakeLists.txt")) return 1;

    if (nob_file_exists(TD_SOURCE)) {
        nob_log(NOB_INFO, "Initializing TDLib submodule...");
        Cmd init = {0};
        cmd_append(&init, "git", "submodule", "update", "--init", "--depth", "1", TD_SOURCE);
        return cmd_run(&init);
    }

    nob_log(NOB_INFO, "Downloading TDLib source to " TD_SOURCE " ...");
    Cmd clone = {0};
    cmd_append(&clone, "git", "clone", "--depth", "1",
               "https://github.com/tdlib/td.git", TD_SOURCE);
    return cmd_run(&clone);
}

static int cmake_available(void)
{
    Cmd c = {0};
    cmd_append(&c, "cmake", "--version");
    return cmd_run(&c, .stdout_path = "/dev/null", .stderr_path = "/dev/null");
}

static int build_td(void)
{
    nob_log(NOB_INFO, "Building TDLib from " TD_SOURCE " ...");
    if (!cmake_available()) {
        nob_log(NOB_ERROR, "cmake is required to build TDLib from source");
        return 0;
    }
    Cmd b = {0};
    cmd_append(&b, "cmake", "-B", TD_BUILD, "-S", TD_SOURCE,
               "-DCMAKE_BUILD_TYPE=Release");
    if (!cmd_run(&b)) return 0;
    b.count = 0;
    cmd_append(&b, "cmake", "--build", TD_BUILD, "--target", "tdjson", "--", "-j1");
    return cmd_run(&b);
}

static int td_already_built(void)
{
    return nob_file_exists(TD_BUILD "/libtdjson.so") ||
           nob_file_exists(TD_BUILD "/libtdjson.a")  ||
           nob_file_exists(TD_BUILD "/tdjson.dll");
}

int main(int argc, char **argv)
{
    GO_REBUILD_URSELF(argc, argv);

    if (argc > 1 && strcmp(argv[1], "clean") == 0) {
        clean();
        return 0;
    }

    Cmd cmd = {0};
    nob_cc(&cmd);
    nob_cc_flags(&cmd);

#ifdef _WIN32
    cmd_append(&cmd, "-DTGF_NOGUI");

    if (!nob_file_exists("tdjson.dll") && !nob_file_exists("tdjson.lib")) {
        if (!td_already_built()) {
            if (!ensure_td_source()) return 1;
            if (!build_td()) return 1;
        }
        cmd_append(&cmd, "-I" TD_SOURCE);
        cmd_append(&cmd, "-I" TD_BUILD);
        cmd_append(&cmd, "-L" TD_BUILD);
        cmd_append(&cmd, "-ltdjson");
    } else {
        cmd_append(&cmd, "-ltdjson");
    }
#else
    if (!check_system_td()) {
        if (!td_already_built()) {
            if (!ensure_td_source()) return 1;
            if (!build_td()) return 1;
        }
        cmd_append(&cmd, "-I" TD_SOURCE);
        cmd_append(&cmd, "-I" TD_BUILD);
        cmd_append(&cmd, "-L" TD_BUILD);
        cmd_append(&cmd, "-ltdjson");
        cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/" TD_BUILD);
    } else {
        cmd_append(&cmd, "-ltdjson");
    }
    cmd_append(&cmd, "-lncurses");
#endif

    cmd_append(&cmd, "cJSON.c");
    cmd_append(&cmd, "-O3");
    nob_cc_output(&cmd, "tgf");
    cmd_append(&cmd, "tracker.c");
    cmd_append(&cmd, "history.c");
    cmd_append(&cmd, "forward.c");
    cmd_append(&cmd, "tgf.c");

    if (!cmd_run(&cmd)) return 1;
    return 0;
}
