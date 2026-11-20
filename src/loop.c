#include "tgf.h"
#include "loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        if (id.kind == REQ_FORWARD_ONE || id.kind == REQ_FORWARD_BATCH)
            forward_error_handle(r, id, code, msg);
    }
}

void loop_tick(Runtime *rt, void *client, LoopState *ls, char *argv[]) {
    const char *result = rt->tp->receive(client, 1.0);
    if (result) {
        cJSON *root = cJSON_Parse(result);
        if (root) {
            const char *type  = cJSON_GetStr(root, "@type");
            const char *extra = cJSON_GetStr(root, "@extra");
            if (!type) type = "";
            if (strcmp(type, "updateAuthorizationState") == 0)
                auth_handle(rt, client, result);
            else if (strcmp(type, "error") == 0)
                on_error(rt, client, root);
            else if (extra)
                on_response(rt, client, root, extra, argv);
            cJSON_Delete(root);
        }
    }

    if (rt->authorized && rt->dest_resolved && rt->source_count >= 1 && !rt->resolve_active) {
        time_t now = time(NULL);
        if (now - ls->last_poll >= POLL_INTERVAL) {
            ls->last_poll = now;
            poll_channels(rt, client);
        }
    }
    if (argv[1] == NULL && rt->authorized && rt->dest_resolved && rt->source_count >= 1 && !rt->resolve_active) {
#ifndef TGF_NOGUI
        if (!ls->ncurses_inited) {
            ui_ncurses_init();
            ls->ncurses_inited = 1;
        }
        time_t current_time = time(NULL);
        if (current_time - ls->last_dashboard >= 1) {
            ui_refresh(rt->cfg.dest_channel, rt->src_name, rt->source_count, rt->cfg.forward_delay_sec);
            ls->last_dashboard = current_time;
        }
#endif
    }
    process_fwd_queue(rt, client);
    { time_t now = time(NULL);
      if (now - ls->last_prune >= 1200) { history_store_prune(&rt->history, &rt->cfg); ls->last_prune = now; } }
}
