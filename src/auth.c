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
            rt->cfg.api_id, rt->cfg.api_hash);
        transport_send(rt, client, "setTdlibParameters", buf, (RequestId){REQ_AUTH_PARAMS, 0, 0});
    } else if (strstr(json, "authorizationStateWaitPhoneNumber")) {
        char phone[32] = {0};
        printf("\033[2J\033[H");
        printf("Welcome to the TGF\n");
        printf("Enter your phone number with country code\n");
        printf("Phone (+xxx): "); fflush(stdout);
        if (fgets(phone, sizeof(phone), stdin)) phone[strcspn(phone, "\r\n")] = 0;
        char payload[256] = {0};
        json_escape_build(payload, sizeof(payload), "phone_number", phone);
        transport_send(rt, client, "setAuthenticationPhoneNumber", payload, (RequestId){REQ_AUTH_PHONE, 0, 0});
    } else if (strstr(json, "authorizationStateWaitCode")) {
        char code[32] = {0};
        printf("Code: "); fflush(stdout);
        if (fgets(code, sizeof(code), stdin)) code[strcspn(code, "\r\n")] = 0;
        char payload[256] = {0};
        json_escape_build(payload, sizeof(payload), "code", code);
        transport_send(rt, client, "checkAuthenticationCode", payload, (RequestId){REQ_AUTH_CODE, 0, 0});
    } else if (strstr(json, "authorizationStateWaitPassword")) {
        char pw[128] = {0};
        printf("2FA password: "); fflush(stdout);
        if (fgets(pw, sizeof(pw), stdin)) pw[strcspn(pw, "\r\n")] = 0;
        char payload[256] = {0};
        json_escape_build(payload, sizeof(payload), "password", pw);
        transport_send(rt, client, "checkAuthenticationPassword", payload, (RequestId){REQ_AUTH_PW, 0, 0});
    } else if (strstr(json, "authorizationStateReady")) {
        rt->authorized = 1;
        printf("\n✓ Authorized!\n");
        const char *dest_name = rt->cfg.dest_channel;
        if (dest_name[0] == '@') dest_name++;
        if (strcmp(dest_name, "me") == 0 || strcmp(dest_name, "Me") == 0) {
            transport_send(rt, client, "getMe", "", (RequestId){REQ_GETME, 0, 0});
        } else {
            char payload[256];
            snprintf(payload, sizeof(payload), "\"username\":\"%s\"", dest_name);
            transport_send(rt, client, "searchPublicChat", payload, (RequestId){REQ_RESOLVE_DEST, 0, 0});
        }
    }
}
