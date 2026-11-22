#include "tgf.h"

#include <stdio.h>
#include <string.h>

static void json_escape_build(char *buf, size_t size, const char *key, const char *val) {
    if (size == 0) return;
    int pos = snprintf(buf, size, "\"%s\":\"", key);
    if (pos < 0 || pos >= (int)size) pos = (int)size - 1;
    while (*val && pos < (int)size - 4) {
        if ((*val == '"' || *val == '\\') && pos < (int)size - 3) buf[pos++] = '\\';
        buf[pos++] = *val++;
    }
    if (pos < (int)size - 1) buf[pos++] = '"';
    buf[pos < (int)size ? pos : (int)size - 1] = 0;
}

static void emit_auth(Runtime *rt, tgf_auth_state st) {
    if (!rt->emit) return;
    tgf_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TGF_EV_AUTH_STATE;
    ev.auth = st;
    rt->emit(rt, &ev);
}

static void emit_conn(Runtime *rt, tgf_conn_state st) {
    if (!rt->emit) return;
    tgf_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TGF_EV_CONNECTION_STATE;
    ev.conn = st;
    rt->emit(rt, &ev);
}

/* Core command handler: the frontend submits phone/code/password; the core
 * builds the request, sends it, then wipes the payload buffer. */
void auth_submit(Runtime *rt, void *client, int kind, const char *value) {
    char payload[256] = {0};
    const char *key, *type;
    RequestId id;
    if (kind == TGF_AUTH_PHONE) {
        key = "phone_number";
        type = "setAuthenticationPhoneNumber";
        id = (RequestId){REQ_AUTH_PHONE, 0, 0};
    } else if (kind == TGF_AUTH_CODE) {
        key = "code";
        type = "checkAuthenticationCode";
        id = (RequestId){REQ_AUTH_CODE, 0, 0};
    } else {
        key = "password";
        type = "checkAuthenticationPassword";
        id = (RequestId){REQ_AUTH_PW, 0, 0};
    }
    json_escape_build(payload, sizeof(payload), key, value);
    transport_send(rt, client, type, payload, id);
    memset(payload, 0, sizeof(payload));
}

void auth_handle(Runtime *rt, void *client, const char *json) {
    if (!strstr(json, "\"@type\":\"updateAuthorizationState\"")) return;

    if (strstr(json, "authorizationStateWaitTdlibParameters")) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "\"database_directory\":\"tdlib_db\","
                 "\"files_directory\":\"tdlib_files\","
                 "\"use_message_database\":true,"
                 "\"use_secret_chats\":false,"
                 "\"api_id\":%d,"
                 "\"api_hash\":\"%s\","
                 "\"system_language_code\":\"en\","
                 "\"device_model\":\"Desktop\","
                 "\"application_version\":\"1.0\"",
                 rt->cfg.api_id, rt->cfg.api_hash ? rt->cfg.api_hash : "");
        transport_send(rt, client, "setTdlibParameters", buf, (RequestId){REQ_AUTH_PARAMS, 0, 0});
        memset(buf, 0, sizeof(buf));
        emit_auth(rt, TGF_AUTH_PARAMS);
        emit_conn(rt, TGF_CONN_CONNECTING);
    } else if (strstr(json, "authorizationStateWaitPhoneNumber")) {
        emit_auth(rt, TGF_AUTH_PHONE);
    } else if (strstr(json, "authorizationStateWaitCode")) {
        emit_auth(rt, TGF_AUTH_CODE);
    } else if (strstr(json, "authorizationStateWaitPassword")) {
        emit_auth(rt, TGF_AUTH_PASSWORD);
    } else if (strstr(json, "authorizationStateReady")) {
        rt->authorized = 1;
        emit_auth(rt, TGF_AUTH_READY);
        emit_conn(rt, TGF_CONN_READY);
        resolve_destination(rt, client);
    } else if (strstr(json, "authorizationStateLoggingOut")) {
        rt->authorized = 0;
        emit_conn(rt, TGF_CONN_LOST);
        emit_auth(rt, TGF_AUTH_LOGGING_OUT);
    } else if (strstr(json, "authorizationStateClosed")) {
        rt->authorized = 0;
        emit_conn(rt, TGF_CONN_LOST);
        emit_auth(rt, TGF_AUTH_CLOSING);
    }
}
