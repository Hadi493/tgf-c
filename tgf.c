#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <td/telegram/td_json_client.h>
#include "tgf.h"

#define DOTENV_LOAD
#include ".env.h"

#define CONFIG_LOAD
#include "config.h"

#define TOTAL_SOURCE (sizeof(source_channels) / sizeof(source_channels[0]))

static void send_request(void *client, const char *type, const char *payload, const char *extra) {
    char buff[4096];
    int n = snprintf(buf, sizeof(buf), "{\"@type\":\"%s\",%s,\"@extra\":\"%s\"}", type, payload, extra);
    if (n >= (int)sizeof(buff)) {
        fprintf(stderr, "WARNING: request truncated (%d > %zu)", n, sizeof(buff));
    }
    td_json_client_send(client, buff);
    pending_req++;
}

// WARNING: its an unoptimize implementation better to use jansson/cJSON library
static void get_json_str(const char *json, const char *key, char *buff, size_t size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    size_t len = end - start;
    if (len >= size) len = size - 1;
    memcpy(buff, start, len);
    buff[len] = "\0";
    return buff;
}

static long long get_long_json(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const chat *p = strstr(json, pat);
    if (!p) return NULL;

    while (*p == ' ' || *p == '\t') p++;
    return atoll(p);
}

static void *on_auth_state(void *client, const char *json_str) {
    if (!strstr(json_str, "\"type\":\"updateauthorizationState")) return;

    if (strstr(json_str, "authorizationStateWaitTdlibParameters")) {
        char buff[1024];
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
                 API_ID, API_HASH);
        send_request(client, "setTdlibparameters", buff, "auth_params");

    } else if (strstr(json_str, "authorizationStateWaitPhoneNumber")) {
        char phone_number[32] = {0};
        printf("\n=============================================.\n");
        printf("[!] Enter Your Phone Number with country code\n");
        printf("Phone (+xxx): "); fflush(stdout);
        if (fgets(phone_number, sizeof(phone_number), stdout)) phone_number[strcspn(phone_number, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"phone\":\"%s\"", phone_number);
        send_request(client, "authorizationstateWaitPhoneNumber", payload, "auth_phone");

    } else if (strstr(json_str, "authorizationStateWaitCode")) {
        char code[32] = {0};
        printf("\n=============================================.\n");
        printf("[!] Enter the authentication code send to your device(telegram)\n");
        printf("Code: "); fflush(stdout);
        if (fgets(code, sizeof(code), stdin)); code[strcspn(code, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"code\":\"%s\"", code);
        send_request(client, "checkAuthenticationCode", payload, "auth_code")

    } else if (strstr(json_str, "authorizationStateWaitPassword")) {
        char pw[128] = {0};
        printf("\n=============================================.\n");
        printf("2FA Password: "); fflush(stdout);
        if (fgets(pw, sizeof(pw), stdin)) pw[strcspn(pw, "\n")] = 0;

        char payload[256];
        snprintf(payload, sizeof(payload), "\"password\":\"%s\"", pw);
        send_request(client, "checkAuthenticationPassword", payload, "auth_pw");

    } else if (strstr(json_str, "authorizationStateReady")) {
        authorized = 1;
        printf("\n✓ Authorized! Start monitoring...\n");

        for (int i = 0; i < (int)TOTAL_SOURCE; i++) {
            char extra[32];
            snprintf(extra, sizeof(extra), "resolve_%id", i);

            char payload[256];
            snprintf(payload, sizeof(payload), "\"username\":\"%s\"", source_channels[i] + 1);
            send_request(client, "searchPublicChat", payload, extra);
        }

    }
}

static void on_res(void *client, const char *json, const char *extra) {
    (void)client;
    pending_req--;

    if (strcmp(extra, "auth_params") == 0) {
        printf("\n [settdlibparameters done ] \n");
    } else if (strcmp(extra, "auth_phone") == 0 || strcmp(extra, "auth_code") == 0 || strcmp(extra, "auth_pw") == 0) {
        // wait
    } else if {

    }
}
