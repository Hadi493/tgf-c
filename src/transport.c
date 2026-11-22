#include "tgf.h"

/* TDLib-free JSON request builder. Routes through the active transport vtable
 * (rt->tp), so the real app and the offline fake share this exact code path.
 * rt->tp must be set by the caller (main sets transport_tdlib; tests set fake). */
void transport_send(Runtime *rt, void *client, const char *type, const char *payload,
                    RequestId id) {
    char extra_buf[64];
    const char *extra = request_extra(id, extra_buf, sizeof(extra_buf));
    char buf[8192];
    int n;
    if (extra) {
        if (payload && payload[0])
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload,
                         extra);
        else
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",\"@extra\":\"%s\"}", type, extra);
    } else {
        if (payload && payload[0])
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s}", type, payload);
        else
            n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\"}", type);
    }
    if (n >= (int)sizeof(buf)) {
        ui_status("WARNING: request truncated");
        return;
    }
    rt->tp->send(client, buf);
}
