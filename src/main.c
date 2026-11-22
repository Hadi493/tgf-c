#include "nob.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "tgf.h"
#include "tgf_core.h"

static tgf_ctx *g_ctx = NULL;
static volatile sig_atomic_t g_stop = 0;
static int g_exit = 0;
static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

static void ask(const char *label, char *buf, size_t n) {
    printf("%s", label);
    fflush(stdout);
    if (fgets(buf, n, stdin)) buf[strcspn(buf, "\r\n")] = 0;
}

int main(int argc, char *argv[]) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-d") != 0)) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return 1;
    }
    bool debug = (argc == 2 && strcmp(argv[1], "-d") == 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    tgf_config cfg;
    if (tgf_config_load("config.json", &cfg) != TGF_OK) {
        const char *err = tgf_config_error();
        fprintf(stderr, "%s\n", err ? err : "Cannot load config.json");
        return 1;
    }
    if (cfg.num_sources == 0) {
        fprintf(stderr, "ERROR: No source channels configured\n");
        tgf_config_free(&cfg);
        return 1;
    }
    char *dest = strdup(cfg.dest_channel ? cfg.dest_channel : "");
    int delay = cfg.forward_delay_sec;

    tgf_ctx *ctx = tgf_create(&cfg, &transport_tdlib);
    tgf_config_free(&cfg);
    if (!ctx) {
        free(dest);
        return 1;
    }
    g_ctx = ctx;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    if (tgf_start(ctx) != TGF_OK) {
        tgf_destroy(ctx);
        free(dest);
        return 1;
    }

    bool dash_begun = false;
    char fatal_msg[256] = "";
    char last_log[256] = "";
    tgf_event ev;
    while (!g_stop) {
        bool got = false;
        while (tgf_next_event(ctx, &ev)) {
            got = true;
            switch (ev.kind) {
            case TGF_EV_AUTH_STATE:
                if (ev.auth == TGF_AUTH_PHONE) {
                    char b[32];
                    ask("Phone (+xxx): ", b, sizeof b);
                    tgf_submit_phone(ctx, b);
                    memset(b, 0, sizeof b);
                } else if (ev.auth == TGF_AUTH_CODE) {
                    char b[32];
                    ask("Code: ", b, sizeof b);
                    tgf_submit_code(ctx, b);
                    memset(b, 0, sizeof b);
                } else if (ev.auth == TGF_AUTH_PASSWORD) {
                    char b[128];
                    ask("2FA password: ", b, sizeof b);
                    tgf_submit_password(ctx, b);
                    memset(b, 0, sizeof b);
                } else if (ev.auth == TGF_AUTH_READY) {
                    printf("\nAuthorized!\n");
                }
                break;
            case TGF_EV_PIPELINE_STATE:
                if (ev.pipe == TGF_PIPE_RUNNING && !debug && !dash_begun) {
                    ui_dashboard_begin(dest, delay);
                    dash_begun = true;
                }
                break;
            case TGF_EV_FORWARDED:
                if (debug)
                    printf(" FORWARDED from %s -> %s\n", ev.source ? ev.source : "?",
                           ev.dest ? ev.dest : "?");
                break;
            case TGF_EV_LOG:
                if (ev.text) {
                    snprintf(last_log, sizeof(last_log), "%s", ev.text);
                    if (debug || !dash_begun) fprintf(stderr, "%s\n", ev.text);
                }
                break;
            case TGF_EV_ERROR:
                if (ev.text) snprintf(fatal_msg, sizeof(fatal_msg), "%s", ev.text);
                g_exit = 1;
                g_stop = 1;
                break;
            default:
                break;
            }
            if (!debug && (dash_begun || ev.kind == TGF_EV_SOURCE_STATE)) ui_dashboard_event(&ev);
            tgf_event_free(&ev);
        }
        if (!debug && dash_begun) ui_dashboard_draw();
        if (!got) usleep(20000);
    }

    if (!debug && dash_begun) ui_dashboard_end();
    if (fatal_msg[0] && (debug || strcmp(fatal_msg, last_log) != 0))
        fprintf(stderr, "%s\n", fatal_msg);
    tgf_stop(ctx);
    tgf_destroy(ctx);
    free(dest);
    return g_exit;
}
