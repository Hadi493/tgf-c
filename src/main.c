#include "nob.h"

#include <signal.h>
#include "tgf.h"
#include "config.h"
#include "loop.h"

static Runtime rt = {
    .cfg.history_window_hours = 24,
    .src_name = "unknown",
    .keep_running = true,
    .tp = &transport_tdlib,
};

static void sig_handler(int sig) {
    (void)sig;
    rt.keep_running = false;
}

static int load_config(Runtime *r, const char *path) {
    return config_load(path, &r->cfg);
}

static int dash_inited = 0;
static time_t dash_last = 0;
static void dashboard_tick(Runtime *rt, void *ud) {
    (void)ud;
#ifndef TGF_NOGUI
    if (!rt->authorized || !rt->dest_resolved || rt->source_count < 1 || rt->resolve_active) return;
    if (!dash_inited) { ui_ncurses_init(); dash_inited = 1; }
    time_t now = time(NULL);
    if (now - dash_last >= 1) {
        ui_refresh(rt->cfg.dest_channel, rt->src_name, rt->source_count, rt->cfg.forward_delay_sec);
        dash_last = now;
    }
#else
    (void)rt;
#endif
}

int main(int argc, char *argv[]) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-d") != 0)) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    ui_init(&rt, argv[1] == NULL, argv[1] != NULL && strcmp(argv[1], "-d") == 0);
    rt.debug = (argv[1] != NULL && strcmp(argv[1], "-d") == 0);

    if (load_config(&rt, "config.json") != 0) return 1;
    if (rt.cfg.num_sources == 0) { fprintf(stderr, "ERROR: No source channels configured\n"); return 1; }

    void *client = rt.tp->create();
    if (!client) { fprintf(stderr, "Failed to create TDLib client\n"); return 1; }
    transport_send(&rt, client, "setLogVerbosityLevel", "\"new_verbosity_level\":0", (RequestId){REQ_NONE, 0, 0});

    rt.source_chat_ids = calloc(rt.cfg.num_sources, sizeof(long long));
    if (!rt.source_chat_ids) { rt.tp->destroy(client); return 1; }

    history_store_load(&rt.history, &rt.cfg);
    fwd_queue_load(&rt);
    if (rt.cfg.enable_sequential_forwarding) { tracker_store_load(&rt.tracker); seq_pending_load(&rt); }
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    transport_send(&rt, client, "getAuthorizationState", "", (RequestId){REQ_NONE, 0, 0});

    LoopState ls = {0};
    while (rt.keep_running) loop_tick(&rt, client, &ls, argv[1] == NULL ? dashboard_tick : NULL, NULL);

    for (int i = 0; i < rt.history.set_size; i++) free(rt.history.set[i]);
    free(rt.history.set);
    free(rt.history.time);
    for (int i = 0; i < rt.fwd_queue_count; i++) {
        int idx = (rt.fwd_queue_head + i) % rt.fwd_queue_cap;
        free(rt.fwd_queue[idx].ids);
    }
    free(rt.fwd_queue);
    free(rt.fwd_inflight.ids);
    free(rt.source_chat_ids);
    for (int i = 0; i < rt.cfg.num_sources; i++) free(rt.cfg.source_channels[i]);
    free(rt.cfg.source_channels);
    free(rt.cfg.api_hash);
    free(rt.cfg.dest_channel);
    free(rt.cfg.history_file);
    rt.tp->destroy(client);
    return 0;
}
