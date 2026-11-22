#include "tgf.h"

#include <stdio.h>
#include <string.h>

static int handle_matches(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (!strncmp(a, "t.me/", 5)) a += 5;
    if (!strncmp(b, "t.me/", 5)) b += 5;
    if (*a == '@') a++;
    if (*b == '@') b++;
    return strcmp(a, b) == 0;
}

static int source_disabled(const Config *cfg, const char *handle) {
    for (int i = 0; i < cfg->num_disabled; i++)
        if (handle_matches(cfg->disabled_channels[i], handle)) return 1;
    return 0;
}

static void emit_source(Runtime *rt, int idx, long long chat_id, tgf_source_state st, int code) {
    if (!rt->emit) return;
    tgf_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TGF_EV_SOURCE_STATE;
    ev.id = chat_id;
    ev.src_state = st;
    ev.code = code;
    ev.total = rt->source_count;
    if (idx >= 0 && idx < rt->cfg.num_sources) ev.source = rt->cfg.source_channels[idx];
    rt->emit(rt, &ev);
}

void resolve_next(Runtime *rt, void *client) {
    while (rt->resolve_index < rt->cfg.num_sources) {
        int idx = rt->resolve_index;
        const char *name = rt->cfg.source_channels[idx];
        if (source_disabled(&rt->cfg, name)) {
            emit_source(rt, idx, 0, TGF_SRC_DISABLED, 0);
            rt->resolve_index++;
            continue;
        }
        if (name[0] == '@') name++;
        char payload[256];
        snprintf(payload, sizeof(payload), "\"username\":\"%s\"", name);
        transport_send(rt, client, "searchPublicChat", payload,
                       (RequestId){REQ_RESOLVE_SOURCE, idx, 0});
        rt->resolve_index++;
        rt->resolve_active = 1;
        return;
    }
    rt->resolve_active = 0;

    int expected = 0;
    for (int i = 0; i < rt->cfg.num_sources; i++)
        if (!source_disabled(&rt->cfg, rt->cfg.source_channels[i])) expected++;

    if (expected > 0 && rt->source_count == 0) {
        snprintf(rt->fatal_msg, sizeof(rt->fatal_msg), "ERROR: No source channels resolved");
        rt->fatal = 1;
        ui_status(rt->fatal_msg);
        return;
    }
    if (rt->source_count < expected) {
        char b[512];
        int pos = snprintf(b, sizeof(b), "WARNING: %d of %d source channels failed to resolve:",
                           expected - rt->source_count, expected);
        for (int i = 0; i < rt->cfg.num_sources && pos < (int)sizeof(b) - 2; i++) {
            if (rt->source_chat_ids[i] != 0) continue;
            if (source_disabled(&rt->cfg, rt->cfg.source_channels[i])) continue;
            pos += snprintf(b + pos, sizeof(b) - pos, " %s", rt->cfg.source_channels[i]);
        }
        ui_status(b);
    }
    if (rt->source_count > 0 && rt->emit) {
        tgf_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = TGF_EV_PIPELINE_STATE;
        ev.pipe = TGF_PIPE_RUNNING;
        rt->emit(rt, &ev);
    }
}

void resolve_destination(Runtime *rt, void *client) {
    const char *dest = rt->cfg.dest_channel ? rt->cfg.dest_channel : "@me";
    const char *name = dest[0] == '@' ? dest + 1 : dest;
    if (strcmp(name, "me") == 0 || strcmp(name, "Me") == 0) {
        transport_send(rt, client, "getMe", "", (RequestId){REQ_GETME, 0, 0});
    } else {
        char payload[256];
        snprintf(payload, sizeof(payload), "\"username\":\"%s\"", name);
        transport_send(rt, client, "searchPublicChat", payload,
                       (RequestId){REQ_RESOLVE_DEST, 0, 0});
    }
}

void getme_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)id;
    (void)argv;
    long long user_id = cJSON_GetInt64(root, "id");
    char payload[256];
    snprintf(payload, sizeof(payload), "\"user_id\":%lld,\"force\":true", user_id);
    transport_send(rt, client, "createPrivateChat", payload, (RequestId){REQ_RESOLVE_DEST, 0, 0});
}

void resolve_dest_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)id;
    (void)argv;
    long long chat_id = cJSON_GetInt64(root, "id");
    rt->dest_chat_id = chat_id;
    rt->dest_resolved = 1;
    rt->resolve_index = 0;
    resolve_next(rt, client);
}

void resolve_source_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)argv;
    int idx = (int)id.chat_id;
    if (idx < 0 || idx >= rt->cfg.num_sources || !rt->source_chat_ids) {
        resolve_next(rt, client);
        return;
    }
    long long chat_id = cJSON_GetInt64(root, "id");
    if (rt->source_chat_ids[idx] == 0) rt->source_count++;
    rt->source_chat_ids[idx] = chat_id;
    emit_source(rt, idx, chat_id, TGF_SRC_LIVE, 0);
    resolve_next(rt, client);
}

void resolve_source_failed(Runtime *rt, void *client, int idx, int code) {
    if (idx >= 0 && idx < rt->cfg.num_sources) {
        if (rt->source_chat_ids[idx] != 0) rt->source_count--;
        rt->source_chat_ids[idx] = 0;
        emit_source(rt, idx, 0, TGF_SRC_NO_ACCESS, code);
    }
    resolve_next(rt, client);
}
