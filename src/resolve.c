#include "tgf.h"

#include <stdio.h>
#include <string.h>

void resolve_next(Runtime *rt, void *client) {
    while (rt->resolve_index < rt->cfg.num_sources) {
        const char *name = rt->cfg.source_channels[rt->resolve_index];
        if (name[0] == '@') name++;
        char payload[256];
        snprintf(payload, sizeof(payload), "\"username\":\"%s\"", name);
        transport_send(rt, client, "searchPublicChat", payload, (RequestId){REQ_RESOLVE_SOURCE, rt->resolve_index, 0});
        rt->resolve_index++;
        rt->resolve_active = 1;
        return;
    }
    rt->resolve_active = 0;
    if (rt->source_count == 0) {
        fprintf(stderr, "ERROR: No source channels resolved\n");
        transport_destroy(client);
        exit(1);
    }
    if (rt->source_count < rt->cfg.num_sources) {
        fprintf(stderr, "WARNING: %d of %d source channels failed to resolve:",
                rt->cfg.num_sources - rt->source_count, rt->cfg.num_sources);
        for (int i = 0; i < rt->cfg.num_sources; i++)
            if (rt->source_chat_ids[i] == 0) fprintf(stderr, " %s", rt->cfg.source_channels[i]);
        fprintf(stderr, "\n");
    }
}

void getme_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)id; (void)argv;
    long long user_id = cJSON_GetInt64(root, "id");
    char payload[256];
    snprintf(payload, sizeof(payload), "\"user_id\":%lld,\"force\":true", user_id);
    transport_send(rt, client, "createPrivateChat", payload, (RequestId){REQ_RESOLVE_DEST, 0, 0});
}

void resolve_dest_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    (void)id; (void)argv;
    long long chat_id = cJSON_GetInt64(root, "id");
    rt->dest_chat_id = chat_id;
    rt->dest_resolved = 1;
    rt->resolve_index = 0;
    resolve_next(rt, client);
}

void resolve_source_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]) {
    long long chat_id = cJSON_GetInt64(root, "id");
    int idx = (int)id.chat_id;
    rt->source_chat_ids[idx] = chat_id;
    rt->source_count++;
    if (argv[1] != NULL && strcmp(argv[1], "-d") == 0) {
        printf("\r  Resolving channels... %d/%d\n", rt->source_count, rt->cfg.num_sources);
    }
    fflush(stdout);
    resolve_next(rt, client);
}
