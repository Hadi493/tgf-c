#include "nob.h"

#define TD_JSON_CLIENT_HEADER <td/telegram/td_json_client.h>
#include TD_JSON_CLIENT_HEADER
#include <signal.h>
#include "tgf.h"
#include "config.h"

static Runtime rt = {
    .cfg.history_window_hours = 24,
    .src_name = "unknown",
    .keep_running = true,
};

static void sig_handler(int sig) {
    (void)sig;
    rt.keep_running = false;
}

static void on_response(Runtime *r, void *client, cJSON *root, const char *extra, char *argv[]);
static void on_error(Runtime *r, void *client, cJSON *root);

static int request_id_from_extra(const char *extra, RequestId *id) {
    if (!extra || !*extra) return 0;
    if (!strcmp(extra, "resolve_dest")) { id->kind = REQ_RESOLVE_DEST; id->chat_id = 0; id->msg_id = 0; return 1; }
    if (!strncmp(extra, "resolve_", 8)) { id->kind = REQ_RESOLVE_SOURCE; id->chat_id = atoll(extra + 8); id->msg_id = 0; return 1; }
    if (!strncmp(extra, "history_", 8)) { id->kind = REQ_HISTORY; id->chat_id = atoll(extra + 8); id->msg_id = 0; return 1; }
    if (!strncmp(extra, "fwd_", 4)) {
        if (!strncmp(extra + 4, "batch", 5) || strstr(extra + 4, "_batch")) { id->kind = REQ_FORWARD_BATCH; id->chat_id = atoll(extra + 4); id->msg_id = 0; return 1; }
        const char *p = strchr(extra + 4, '_');
        if (p) { id->kind = REQ_FORWARD_ONE; id->chat_id = atoll(extra + 4); id->msg_id = atoll(p + 1); return 1; }
    }
    if (!strcmp(extra, "getme")) { id->kind = REQ_GETME; id->chat_id = 0; id->msg_id = 0; return 1; }
    return 0;
}

typedef void (*ResponseHandler)(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);

static ResponseHandler handler_for(RequestKind kind) {
    switch (kind) {
    case REQ_GETME: return getme_handle;
    case REQ_RESOLVE_DEST: return resolve_dest_handle;
    case REQ_RESOLVE_SOURCE: return resolve_source_handle;
    case REQ_HISTORY: return history_handle;
    case REQ_FORWARD_ONE:
    case REQ_FORWARD_BATCH: return forward_handle;
    default: return NULL;
    }
}

static void on_response(Runtime *r, void *client, cJSON *root, const char *extra, char *argv[]) {
    RequestId id;
    if (!request_id_from_extra(extra, &id)) return;
    ResponseHandler handler = handler_for(id.kind);
    if (handler) handler(r, client, root, id, argv);
}

static void on_error(Runtime *r, void *client, cJSON *root) {
    const char *extra = cJSON_GetStr(root, "@extra");
    const char *msg   = cJSON_GetStr(root, "message");
    int code          = (int)cJSON_GetInt64(root, "code");
    { char b[128]; snprintf(b, sizeof(b), "ERROR [%s] code=%d: %s", extra ? extra : "?", code, msg ? msg : "?"); ui_status(b); }
    RequestId id;
    if (extra && request_id_from_extra(extra, &id)) {
        if (id.kind == REQ_RESOLVE_DEST || id.kind == REQ_GETME) {
            fprintf(stderr, "ERROR: Failed to resolve destination '%s'\n", r->cfg.dest_channel);
            transport_destroy(client);
            exit(1);
        }
        if (id.kind == REQ_RESOLVE_SOURCE) resolve_next(r, client);
    }
}

static int load_config(Runtime *r, const char *path) {
    return config_load(path, &r->cfg);
}

int main(int argc, char *argv[]) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    ui_init(&rt, argv[1] == NULL, argv[1] != NULL && strcmp(argv[1], "-d") == 0);

    if (load_config(&rt, "config.json") != 0) return 1;
    if (rt.cfg.num_sources == 0) { fprintf(stderr, "ERROR: No source channels configured\n"); return 1; }

    void *client = td_json_client_create();
    if (!client) { fprintf(stderr, "Failed to create TDLib client\n"); return 1; }
    transport_send(&rt, client, "setLogVerbosityLevel", "\"new_verbosity_level\":0", (RequestId){REQ_NONE, 0, 0});

    rt.source_chat_ids = calloc(rt.cfg.num_sources, sizeof(long long));
    if (!rt.source_chat_ids) { td_json_client_destroy(client); return 1; }

    history_store_load(&rt.history, &rt.cfg);
    if (rt.cfg.enable_sequential_forwarding) tracker_store_load(&rt.tracker);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    transport_send(&rt, client, "getAuthorizationState", "", (RequestId){REQ_NONE, 0, 0});

    time_t last_poll = 0;

    while (rt.keep_running) {
        const char *result = td_json_client_receive(client, 1.0);
        if (result) {
            cJSON *root = cJSON_Parse(result);
            if (root) {
                const char *type  = cJSON_GetStr(root, "@type");
                const char *extra = cJSON_GetStr(root, "@extra");
                if (!type) type = "";
                if (strcmp(type, "updateAuthorizationState") == 0)
                    auth_handle(&rt, client, result);
                else if (strcmp(type, "error") == 0)
                    on_error(&rt, client, root);
                else if (extra)
                    on_response(&rt, client, root, extra, argv);
                cJSON_Delete(root);

            }
        }

        if (rt.authorized && rt.dest_resolved && rt.source_count >= 1 && !rt.resolve_active) {
            time_t now = time(NULL);
            if (now - last_poll >= POLL_INTERVAL) {
                last_poll = now;
                poll_channels(&rt, client);
            }
        }
        if (argv[1] == NULL && rt.authorized && rt.dest_resolved && rt.source_count >= 1 && !rt.resolve_active) {
#ifndef TGF_NOGUI
            static int ncurses_inited = 0;
            if (!ncurses_inited) {
                ui_ncurses_init();
                ncurses_inited = 1;
            }
            static time_t last_dashboard_update = 0;
            time_t current_time = time(NULL);
            if (current_time - last_dashboard_update >= 1) {
                ui_refresh(rt.cfg.dest_channel, rt.src_name, rt.source_count, rt.cfg.forward_delay_sec);
                last_dashboard_update = current_time;
            }
#endif
        }
        process_fwd_queue(&rt, client);
        { static time_t last_prune = 0; time_t now = time(NULL);
          if (now - last_prune >= 1200) { history_store_prune(&rt.history, &rt.cfg); last_prune = now; } }
    }

    for (int i = 0; i < rt.history.set_size; i++) free(rt.history.set[i]);
    free(rt.history.set);
    free(rt.history.time);
    for (int i = 0; i < rt.fwd_queue_count; i++) {
        int idx = (rt.fwd_queue_head + i) % rt.fwd_queue_cap;
        free(rt.fwd_queue[idx].ids);
    }
    free(rt.fwd_queue);
    free(rt.source_chat_ids);
    for (int i = 0; i < rt.cfg.num_sources; i++) free(rt.cfg.source_channels[i]);
    free(rt.cfg.source_channels);
    free(rt.cfg.api_hash);
    free(rt.cfg.dest_channel);
    free(rt.cfg.history_file);
    td_json_client_destroy(client);
    return 0;
}
