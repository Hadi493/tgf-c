#include "tgf.h"

#define TD_JSON_CLIENT_HEADER <td/telegram/td_json_client.h>
#include TD_JSON_CLIENT_HEADER

void transport_send(Runtime *rt, void *client, const char *type, const char *payload, RequestId id) {
    (void)rt;
    char extra_buf[64];
    const char *extra = request_extra(id, extra_buf, sizeof(extra_buf));
    char buf[8192];
    int n;
    if (extra) {
        if (payload && payload[0])
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload, extra);
        else
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",\"@extra\":\"%s\"}", type, extra);
    } else {
        if (payload && payload[0])
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s}", type, payload);
        else
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\"}", type);
    }
    if (n >= (int)sizeof(buf)) ui_status("WARNING: request truncated");
    td_json_client_send(client, buf);
}

void transport_destroy(void *client) {
    td_json_client_destroy(client);
}
